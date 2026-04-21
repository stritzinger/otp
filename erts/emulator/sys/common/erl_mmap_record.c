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
erts_mmap_record_option_enabled(void)
{
    return record_enabled || replay_enabled;
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
        path = replay_path;
        record_fd = open(path, O_RDWR, 0);
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
                                MAP_SHARED,
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

#endif /* HAVE_ERTS_MMAP */
