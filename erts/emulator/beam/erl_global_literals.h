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

/* Global literals are used to store Erlang terms that are never modified or 
 * deleted. They are commonly-used constants at compile or run-time. This is 
 * similar in spirit to persistent_term but for internal usage.
 *
 * Examples include lambdas associated with export entries, the bitstring
 * representation of atoms, and certain constants.
 */

#ifndef __ERL_GLOBAL_LITERALS_H__
#define __ERL_GLOBAL_LITERALS_H__

extern Eterm ERTS_GLOBAL_LIT_OS_TYPE;
extern Eterm ERTS_GLOBAL_LIT_OS_VERSION;
extern Eterm ERTS_GLOBAL_LIT_DFLAGS_RECORD;
extern Eterm ERTS_GLOBAL_LIT_ERL_FILE_SUFFIX;
extern Eterm ERTS_GLOBAL_LIT_EMPTY_TUPLE;

/* Initializes global literals. Note that the literals terms mentioned in the 
 * examples above may be created elsewhere, and are only kept here for clarity.
 */
void init_global_literals(void);

/*
 * Replay-only: restore the snapshotted ERTS_GLOBAL_LIT_EMPTY_TUPLE term and
 * the global_literal_chunk linked-list head from struct-root-dumps.
 * Returns 1 on success (snapshot loaded and globals updated), 0 if the
 * snapshot is unavailable.
 *
 * When the empty tuple snapshot is restored its boxed pointer references an
 * address in the record-time arena; the arena is mapped MAP_PRIVATE at
 * replay so the bytes [0,0] of the empty tuple header survive at the same
 * virtual address. Without this restore, init_empty_tuple() would create a
 * fresh empty tuple at a new literal-mmapper address, but every literal map
 * loaded from beam files still has its `keys` field pointing at the
 * record-time empty tuple address, which would cause ets:insert deep-copy
 * to assert (obj == ERTS_GLOBAL_LIT_EMPTY_TUPLE) and crash.
 */
int erts_global_literals_apply_replay_root(void);

/* Allocates space for global literals. Users must call erts_global_literal_register
 * when done creating the literal. 
 */
Eterm *erts_global_literal_allocate(Uint sz, struct erl_off_heap_header ***ohp);

/* Registers the pointed-to term as a global literal. Must be called for terms 
 * allocated using erts_global_literal_allocate.*/
void erts_global_literal_register(Eterm *variable);
int erts_global_literal_is_in_range(void *ptr);

/* Iterates between global literal areas. Can only be used when crash dumping. 
 * Iteration is started by passing NULL, then successively calling this function
 * until it returns NULL.
 */
ErtsLiteralArea *erts_global_literal_iterate_area(ErtsLiteralArea *prev);

#endif
