/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2002-2025. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * %CopyrightEnd%
 */
#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "sys.h"
#include "erl_mmap.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#ifdef HAVE_SYS_MMAN_H
#  include <sys/mman.h>
#endif

#if HAVE_ERTS_MMAP

#define ERTS_RECORD_ARENA_SIZE (UWORD_CONSTANT(100) * 1024 * 1024)

typedef struct ErtsMMapRecordChunk_ ErtsMMapRecordChunk;
struct ErtsMMapRecordChunk_ {
    char *ptr;
    UWord size;
    int free;
    ErtsMMapRecordChunk *prev;
    ErtsMMapRecordChunk *next;
};

static int record_enabled = 0;
static int replay_enabled = 0;
static int record_initialized = 0;
static int record_fd = -1;
static char *record_base = NULL;
static char *record_path = NULL;
static char *replay_path = NULL;
static ErtsMMapRecordChunk *record_chunks = NULL;
static erts_mtx_t record_mtx;
static int record_mtx_inited = 0;

/*
 * Literal super-carrier snapshot tracking.
 *
 * On 64-bit, the literal allocator has its own mmapper (erts_literal_mmapper)
 * reserved as a 1 GB virtual range. Allocations inside it do NOT go through
 * mseg_create() and therefore do NOT reach erts_mmap_record_alloc() above.
 *
 * To replay correctly we track every live (ptr, size) region handed out by
 * erts_alcu_mmapper_mseg_alloc / _realloc, and at process exit we dump those
 * regions (their raw bytes) to a sidecar file next to the main record arena
 * (<record-arena>.literals). On replay, after the literal mmapper has been
 * set up (so the same virtual range is reserved), we read the sidecar and
 * memcpy bytes back at their original addresses.
 */
typedef struct ErtsLiteralSnapshotRegion_ ErtsLiteralSnapshotRegion;
struct ErtsLiteralSnapshotRegion_ {
    char *ptr;
    UWord size;
    ErtsLiteralSnapshotRegion *next;
};

static ErtsLiteralSnapshotRegion *literal_regions = NULL;
static erts_mtx_t literal_mtx;
static int literal_mtx_inited = 0;

#define ERTS_LITERAL_SNAPSHOT_MAGIC  0x4C49544C55 /* "LITL\0" */
#define ERTS_LITERAL_SNAPSHOT_VERSION 1

static void
literal_mtx_ensure_inited(void)
{
    if (!literal_mtx_inited) {
        erts_mtx_init(&literal_mtx, "mmap_record_literal", NIL,
                      ERTS_LOCK_FLAGS_PROPERTY_STATIC
                      | ERTS_LOCK_FLAGS_CATEGORY_ALLOCATOR);
        literal_mtx_inited = 1;
    }
}

static const char *
literal_sidecar_path_for_record(void)
{
    static char buf[1024];
    const char *base;
    int len;

    if (replay_enabled) {
        base = replay_path;
    } else {
        base = record_path;
    }
    if (!base) {
        return NULL;
    }
    len = snprintf(buf, sizeof(buf), "%s.literals", base);
    if (len <= 0 || len >= (int) sizeof(buf)) {
        return NULL;
    }
    return buf;
}

static UWord
record_align(UWord size, Uint32 mmap_flags)
{
    UWord align = ERTS_PAGEALIGNED_SIZE;
    if (mmap_flags & ERTS_MMAPFLG_SUPERALIGNED) {
        align = ERTS_SUPERALIGNED_SIZE;
    }
    return (size + (align - 1)) & ~(align - 1);
}

static char *
record_align_ptr(char *ptr, UWord align)
{
    UWord v = (UWord) ptr;
    UWord a = (v + (align - 1)) & ~(align - 1);
    return (char *) a;
}

static ErtsMMapRecordChunk *
record_new_chunk(char *ptr, UWord size, int free)
{
    ErtsMMapRecordChunk *c = (ErtsMMapRecordChunk *) malloc(sizeof(*c));
    if (!c) {
        return NULL;
    }
    c->ptr = ptr;
    c->size = size;
    c->free = free;
    c->prev = NULL;
    c->next = NULL;
    return c;
}

static void
record_merge_with_neighbors(ErtsMMapRecordChunk *c)
{
    if (c->next && c->next->free) {
        ErtsMMapRecordChunk *n = c->next;
        c->size += n->size;
        c->next = n->next;
        if (c->next) {
            c->next->prev = c;
        }
        free(n);
    }
    if (c->prev && c->prev->free) {
        ErtsMMapRecordChunk *p = c->prev;
        p->size += c->size;
        p->next = c->next;
        if (c->next) {
            c->next->prev = p;
        }
        free(c);
    }
}

int
erts_mmap_record_option_record(const char *path)
{
    char *copy;
    size_t len;

    if (!path || !path[0] || replay_enabled) {
        return 0;
    }

    len = strlen(path);
    copy = (char *) malloc(len + 1);
    if (!copy) {
        return 0;
    }
    memcpy(copy, path, len + 1);

    if (record_path) {
        free(record_path);
    }
    record_path = copy;

    record_enabled = 1;
    return 1;
}

int
erts_mmap_record_option_replay(const char *path)
{
    char *copy;
    size_t len;

    if (!path || !path[0] || record_enabled) {
        return 0;
    }

    len = strlen(path);
    copy = (char *) malloc(len + 1);
    if (!copy) {
        return 0;
    }
    memcpy(copy, path, len + 1);

    if (replay_path) {
        free(replay_path);
    }
    replay_path = copy;
    replay_enabled = 1;
    return 1;
}

int
erts_mmap_record_option_record_enabled(void)
{
    return record_enabled;
}

int
erts_mmap_record_option_replay_enabled(void)
{
    return replay_enabled;
}

int
erts_mmap_record_option_enabled(void)
{
    return record_enabled || replay_enabled;
}

int
erts_mmap_record_arena_contains(const void *ptr)
{
    if (!record_base) {
        return 0;
    }
    return (const char *) ptr >= record_base
        && (const char *) ptr < record_base + ERTS_RECORD_ARENA_SIZE;
}

void
erts_mmap_record_arena_bounds(const char **base_out, UWord *size_out)
{
    if (base_out) {
        *base_out = record_base;
    }
    if (size_out) {
        *size_out = ERTS_RECORD_ARENA_SIZE;
    }
}

int
erts_mmap_record_init(void)
{
    const char *path = NULL;
    ErtsMMapRecordChunk *c;
    struct stat st;

    if (!record_enabled && !replay_enabled) {
        return 1;
    }
    if (record_initialized) {
        return 1;
    }

    if (replay_enabled) {
        /*
         * Open the arena read-only during replay so the OS will not let us
         * mutate the on-disk snapshot, and map it MAP_PRIVATE (copy-on-write)
         * so the VM can still write into restored memory without propagating
         * those writes back to the file. Without this, a crash mid-replay
         * leaves a partially-modified arena on disk and subsequent replays
         * observe a different (corrupted) snapshot.
         */
        path = replay_path;
        record_fd = open(path, O_RDONLY, 0);
    } else {
        path = record_path;
        if (!path) {
            return 0;
        }
        record_fd = open(path, O_RDWR | O_CREAT, 0666);
    }
    if (record_fd < 0) {
        return 0;
    }

    if (fstat(record_fd, &st) != 0) {
        close(record_fd);
        record_fd = -1;
        return 0;
    }
    if (replay_enabled) {
        if ((UWord) st.st_size < ERTS_RECORD_ARENA_SIZE) {
            close(record_fd);
            record_fd = -1;
            return 0;
        }
    } else if (st.st_size != (off_t) ERTS_RECORD_ARENA_SIZE) {
        if (ftruncate(record_fd, (off_t) ERTS_RECORD_ARENA_SIZE) != 0) {
            close(record_fd);
            record_fd = -1;
            return 0;
        }
    }

    record_base = (char *) mmap(NULL,
                                ERTS_RECORD_ARENA_SIZE,
                                PROT_READ | PROT_WRITE,
                                replay_enabled ? MAP_PRIVATE : MAP_SHARED,
                                record_fd,
                                0);
    if (record_base == MAP_FAILED) {
        record_base = NULL;
        close(record_fd);
        record_fd = -1;
        return 0;
    }

    c = record_new_chunk(record_base, ERTS_RECORD_ARENA_SIZE, 1);
    if (!c) {
        munmap(record_base, ERTS_RECORD_ARENA_SIZE);
        record_base = NULL;
        close(record_fd);
        record_fd = -1;
        return 0;
    }

    if (!record_mtx_inited) {
        erts_mtx_init(&record_mtx, "mmap_record", NIL,
                      ERTS_LOCK_FLAGS_PROPERTY_STATIC
                      | ERTS_LOCK_FLAGS_CATEGORY_ALLOCATOR);
        record_mtx_inited = 1;
    }

    record_chunks = c;
    record_initialized = 1;
    return 1;
}

void *
erts_mmap_record_alloc(UWord *sizep, Uint32 mmap_flags)
{
    UWord need;
    UWord align;
    ErtsMMapRecordChunk *c;
    void *res = NULL;

    if (!record_initialized || !sizep) {
        return NULL;
    }

    align = ERTS_PAGEALIGNED_SIZE;
    if (mmap_flags & ERTS_MMAPFLG_SUPERALIGNED) {
        align = ERTS_SUPERALIGNED_SIZE;
    }
    need = record_align(*sizep, mmap_flags);

    erts_mtx_lock(&record_mtx);
    for (c = record_chunks; c; c = c->next) {
        if (c->free) {
            char *ret_ptr = record_align_ptr(c->ptr, align);
            UWord prefix = (UWord) (ret_ptr - c->ptr);
            UWord total_need = prefix + need;
            if (c->size < total_need) {
                continue;
            }

            if (prefix > 0) {
                ErtsMMapRecordChunk *pre = record_new_chunk(c->ptr, prefix, 1);
                if (!pre) {
                    break;
                }
                pre->prev = c->prev;
                pre->next = c;
                if (pre->prev) {
                    pre->prev->next = pre;
                } else {
                    record_chunks = pre;
                }
                c->prev = pre;
                c->ptr = ret_ptr;
                c->size -= prefix;
            }

            if (c->size > need) {
                ErtsMMapRecordChunk *tail = record_new_chunk(c->ptr + need,
                                                             c->size - need,
                                                             1);
                if (!tail) {
                    break;
                }
                tail->prev = c;
                tail->next = c->next;
                if (tail->next) {
                    tail->next->prev = tail;
                }
                c->next = tail;
                c->size = need;
            }
            c->free = 0;
            *sizep = c->size;
            res = c->ptr;
            break;
        }
    }
    erts_mtx_unlock(&record_mtx);

    return res;
}

void
erts_mmap_record_free(void *ptr, UWord size)
{
    ErtsMMapRecordChunk *c;
    (void) size;

    if (!record_initialized || !ptr) {
        return;
    }

    erts_mtx_lock(&record_mtx);
    for (c = record_chunks; c; c = c->next) {
        if (c->ptr == (char *) ptr) {
            c->free = 1;
            record_merge_with_neighbors(c);
            break;
        }
    }
    erts_mtx_unlock(&record_mtx);
}

void *
erts_mmap_record_realloc(void *ptr, UWord old_size, UWord *sizep, Uint32 mmap_flags)
{
    void *new_ptr;
    UWord copy_sz;

    if (!record_initialized || !sizep) {
        return NULL;
    }
    if (!ptr) {
        return erts_mmap_record_alloc(sizep, mmap_flags);
    }
    if (*sizep <= old_size) {
        return ptr;
    }

    new_ptr = erts_mmap_record_alloc(sizep, mmap_flags);
    if (!new_ptr) {
        return NULL;
    }

    copy_sz = old_size < *sizep ? old_size : *sizep;
    sys_memcpy(new_ptr, ptr, copy_sz);
    erts_mmap_record_free(ptr, old_size);
    return new_ptr;
}

/*
 * ---------------------------------------------------------------------------
 * Literal super-carrier snapshot tracking.
 * ---------------------------------------------------------------------------
 */

void
erts_mmap_record_literal_alloc(void *ptr, UWord size)
{
    ErtsLiteralSnapshotRegion *r;

    if (!record_enabled || !ptr || !size) {
        return;
    }
    r = (ErtsLiteralSnapshotRegion *) malloc(sizeof(*r));
    if (!r) {
        return;
    }
    r->ptr = (char *) ptr;
    r->size = size;

    literal_mtx_ensure_inited();
    erts_mtx_lock(&literal_mtx);
    r->next = literal_regions;
    literal_regions = r;
    erts_mtx_unlock(&literal_mtx);
}

void
erts_mmap_record_literal_free(void *ptr, UWord size)
{
    ErtsLiteralSnapshotRegion **pp;
    (void) size;

    if (!record_enabled || !ptr) {
        return;
    }

    literal_mtx_ensure_inited();
    erts_mtx_lock(&literal_mtx);
    for (pp = &literal_regions; *pp; pp = &(*pp)->next) {
        if ((*pp)->ptr == (char *) ptr) {
            ErtsLiteralSnapshotRegion *r = *pp;
            *pp = r->next;
            free(r);
            break;
        }
    }
    erts_mtx_unlock(&literal_mtx);
}

void
erts_mmap_record_literal_realloc(void *old_ptr, UWord old_size,
                                 void *new_ptr, UWord new_size)
{
    if (!record_enabled) {
        return;
    }
    if (old_ptr) {
        erts_mmap_record_literal_free(old_ptr, old_size);
    }
    if (new_ptr && new_size) {
        erts_mmap_record_literal_alloc(new_ptr, new_size);
    }
}

/*
 * Sidecar file format (little-endian, host-size UWord):
 *
 *   UWord magic      (ERTS_LITERAL_SNAPSHOT_MAGIC)
 *   UWord version    (ERTS_LITERAL_SNAPSHOT_VERSION)
 *   UWord count      (number of regions)
 *   for each region:
 *       UWord ptr    (virtual address)
 *       UWord size   (bytes)
 *       byte  data[size]
 */

static int
write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *) buf;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

static int
read_all(int fd, void *buf, size_t len)
{
    char *p = (char *) buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t) n;
    }
    return 0;
}

void
erts_mmap_record_literal_dump_on_exit(void)
{
    const char *path;
    int fd;
    UWord header[3];
    ErtsLiteralSnapshotRegion *r;
    UWord count = 0;

    if (!record_enabled) {
        return;
    }
    path = literal_sidecar_path_for_record();
    if (!path) {
        return;
    }

    literal_mtx_ensure_inited();
    erts_mtx_lock(&literal_mtx);

    for (r = literal_regions; r; r = r->next) {
        count++;
    }

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        erts_mtx_unlock(&literal_mtx);
        return;
    }

    header[0] = (UWord) ERTS_LITERAL_SNAPSHOT_MAGIC;
    header[1] = (UWord) ERTS_LITERAL_SNAPSHOT_VERSION;
    header[2] = count;
    if (write_all(fd, header, sizeof(header)) != 0) {
        goto done;
    }

    for (r = literal_regions; r; r = r->next) {
        UWord rec[2];
        rec[0] = (UWord) r->ptr;
        rec[1] = r->size;
        if (write_all(fd, rec, sizeof(rec)) != 0) {
            goto done;
        }
        if (r->size > 0) {
            if (write_all(fd, r->ptr, (size_t) r->size) != 0) {
                goto done;
            }
        }
    }

done:
    close(fd);
    erts_mtx_unlock(&literal_mtx);
}

/*
 * Restore the literal super-carrier contents from the sidecar file.
 *
 * Must be called AFTER erts_mmap_init(&erts_literal_mmapper, ...) so that
 * the 1 GB virtual range is reserved at the same address that it was during
 * record (ASLR is required to be off). For each recorded region we:
 *   1. Ensure physical memory is reserved on the target pages via the
 *      mmapper's reserve_physical callback.
 *   2. memcpy the recorded bytes.
 *
 * NOTE: After this call the literal mmapper's free-list does NOT know that
 * these regions are in use. That's OK for replay because replay skips
 * load_preloaded() and therefore never asks the literal allocator for
 * fresh memory; existing code already baked-in pointers into these
 * addresses.
 */
int
erts_mmap_record_literal_restore(ErtsMemMapper *mm)
{
    const char *path;
    int fd;
    UWord header[3];
    UWord count, i;
    int ok = 0;
    (void) mm;

    if (!replay_enabled) {
        return 1;
    }
    path = literal_sidecar_path_for_record();
    if (!path) {
        return 0;
    }

    fd = open(path, O_RDONLY, 0);
    if (fd < 0) {
        /* Missing sidecar: not fatal, but callers likely can't boot. */
        return 0;
    }

    if (read_all(fd, header, sizeof(header)) != 0) {
        goto out;
    }
    if (header[0] != (UWord) ERTS_LITERAL_SNAPSHOT_MAGIC
        || header[1] != (UWord) ERTS_LITERAL_SNAPSHOT_VERSION) {
        goto out;
    }
    count = header[2];

    for (i = 0; i < count; i++) {
        UWord rec[2];
        char *ptr;
        UWord size;

        if (read_all(fd, rec, sizeof(rec)) != 0) {
            goto out;
        }
        ptr = (char *) rec[0];
        size = rec[1];

        /*
         * Reserve physical memory on the target region so that the
         * upcoming writes land on real pages. The super-carrier was
         * reserved with os_mmap_virtual() and is PROT_NONE until this
         * call flips the pages to PROT_READ|PROT_WRITE.
         *
         * We use erts_mmap_reserve_physical(), a small wrapper in
         * erl_mmap.c, because ErtsMemMapper is only forward-declared
         * outside that file.
         */
        if (mm) {
            if (!erts_mmap_reserve_physical(mm, ptr, size)) {
                goto out;
            }
            /*
             * Tell the mmapper these pages are now in-use so subsequent
             * erts_mmap() calls (e.g. when the literal allocator grows
             * its carriers) don't hand them out and overwrite the bytes
             * we are about to memcpy in.
             */
            if (!erts_mmap_mark_allocated(mm, ptr, size)) {
                fprintf(stderr,
                        "replay_root_debug: WARNING mark_allocated failed "
                        "for [%p..+0x%lx); later literal allocations may "
                        "clobber restored bytes\n",
                        (void *) ptr, (unsigned long) size);
            }
        }
        if (size > 0) {
            if (read_all(fd, ptr, (size_t) size) != 0) {
                goto out;
            }
        }
    }
    ok = 1;

out:
    close(fd);
    return ok;
}

#endif /* HAVE_ERTS_MMAP */
