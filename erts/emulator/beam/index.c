/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 * 
 * Copyright Ericsson AB 1996-2025. All Rights Reserved.
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
#include "erl_vm.h"
#include "global.h"
#include "index.h"

void index_info(fmtfn_t to, void *arg, IndexTable *t)
{
    hash_info(to, arg, &t->htable);
    erts_print(to, arg, "=index_table:%s\n", t->htable.name);
    erts_print(to, arg, "size: %d\n",	t->size);
    erts_print(to, arg, "limit: %d\n",	t->limit);
    erts_print(to, arg, "entries: %d\n",t->entries);
}


/*
 * Returns size of table in bytes. Stored objects not included.
 */
int 
index_table_sz(IndexTable *t)
{
  return (sizeof(IndexTable)
          - sizeof(Hash)
          + t->size*sizeof(IndexSlot*)
          +  hash_table_sz(&(t->htable)));
}


/*
** init a pre allocated or static hash structure
** and allocate buckets.
*/
IndexTable*
erts_index_init(ErtsAlcType_t type, IndexTable* t, char* name,
		int size, int limit, HashFunctions fun)
{
    Uint base_size = (((Uint)limit+INDEX_PAGE_SIZE-1)/INDEX_PAGE_SIZE)*sizeof(IndexSlot*);
    hash_init(type, &t->htable, name, 3*size/4, fun);

    t->size = 0;
    t->limit = limit;
    t->entries = 0;
    t->type = type;
    t->seg_table = (IndexSlot***) erts_alloc(type, base_size);
    return t;
}

IndexSlot*
index_put_entry(IndexTable* t, void* tmpl)
{
    int ix;
    IndexSlot* p = (IndexSlot*) hash_put(&t->htable, tmpl);

    if (p->index >= 0) {
	return p;
    }

    ix = t->entries;
    if (ix >= t->size) {
	Uint sz;
	if (ix >= t->limit) {
	    /* A core dump is unnecessary */
	    erts_exit(ERTS_DUMP_EXIT, "no more index entries in %s (max=%d)\n",
		     t->htable.name, t->limit);
	}
	sz = INDEX_PAGE_SIZE*sizeof(IndexSlot*);
	t->seg_table[ix>>INDEX_PAGE_SHIFT] = erts_alloc(t->type, sz);
	t->size += INDEX_PAGE_SIZE;
    }
    p->index = ix;
    t->seg_table[ix>>INDEX_PAGE_SHIFT][ix&INDEX_PAGE_MASK] = p;

    /*
     * Do a write barrier here to allow readers to do lock free iteration.
     * erts_index_num_entries() does matching read barrier.
     */
    ERTS_THR_WRITE_MEMORY_BARRIER;
    t->entries++;

    return p;
}

int index_get(IndexTable* t, void* tmpl)
{
    IndexSlot* p = (IndexSlot*) hash_get(&t->htable, tmpl);

    if (p != NULL) {
	return p->index;
    }
    return -1;
}

void
erts_index_rebuild_hash_buckets(IndexTable *t)
{
    int i;
    int bits = ERTS_SIZEOF_TERM * 8;
    Uint slots;
    Uint sz;
    int nobjs = 0;
    HashBucket **new_bucket;
    Hash *h;

    ASSERT(t != NULL);

    h = &t->htable;
    ASSERT(h->shift > 0 && h->shift < bits);
    ASSERT(bits - h->shift > 0);

    slots = UWORD_CONSTANT(1) << (bits - h->shift);
    sz = slots * sizeof(HashBucket *);

    new_bucket = (HashBucket **) h->fun.meta_alloc(h->meta_alloc_type, sz);
    memzero(new_bucket, sz);

    for (i = 0; i < t->entries; i++) {
        HashBucket *b = (HashBucket *) erts_index_lookup(t, i);
        if (b) {
            Uint ix = hash_get_slot(h, b->hvalue);
            b->next = new_bucket[ix];
            new_bucket[ix] = b;
            nobjs++;
        }
    }

    h->bucket = new_bucket;
    h->nobjs = nobjs;
    h->grow_threshold = (8 * (int) slots) / 5;
    if (h->shift < h->max_shift) {
        h->shrink_threshold = ((int) slots) / 5;
    } else {
        h->shrink_threshold = -1;
    }
}

void index_erase_latest_from(IndexTable* t, Uint from_ix)
{
    if(from_ix < (Uint)t->entries) {
	int ix;
	for (ix = from_ix; ix < t->entries; ix++)  {
	    IndexSlot* obj = t->seg_table[ix>>INDEX_PAGE_SHIFT][ix&INDEX_PAGE_MASK];
	    hash_erase(&t->htable, obj);
	}
	t->entries = from_ix;
    }
    else {
	ASSERT(from_ix == t->entries);
    }
}
