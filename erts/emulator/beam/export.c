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
#include "export.h"
#include "hash.h"
#include "jit/beam_asm.h"
#include "erl_global_literals.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#define EXPORT_INITIAL_SIZE   4000
#define EXPORT_LIMIT          (512*1024)

#ifdef DEBUG
#  define IF_DEBUG(x) x
#else
#  define IF_DEBUG(x)
#endif

static void create_shared_lambda(Export *export)
{
    ErlFunThing *lambda;
    struct erl_off_heap_header **ohp;

    lambda = (ErlFunThing*)erts_global_literal_allocate(ERL_FUN_SIZE, &ohp);

    lambda->thing_word = MAKE_FUN_HEADER(export->info.mfa.arity, 0, 1);
    lambda->entry.exp = export;

    export->lambda = make_fun(lambda);

    erts_global_literal_register(&export->lambda);
}

static HashValue export_hash(const Export *export)
{
    return (atom_val(export->info.mfa.module) *
            atom_val(export->info.mfa.function)) ^
           export->info.mfa.arity;
}

static int export_cmp(const Export *lhs, const Export *rhs)
{
    return !(lhs->info.mfa.module == rhs->info.mfa.module &&
             lhs->info.mfa.function == rhs->info.mfa.function &&
             lhs->info.mfa.arity == rhs->info.mfa.arity);
}

static void export_init(Export *dst, const Export *template)
{
    sys_memset(&dst->info.u, 0, sizeof(dst->info.u));
    dst->info.gen_bp = NULL;
    dst->info.mfa.module = template->info.mfa.module;
    dst->info.mfa.function = template->info.mfa.function;
    dst->info.mfa.arity = template->info.mfa.arity;
    dst->bif_number = -1;
    dst->is_bif_traced = 0;

    create_shared_lambda(dst);

    sys_memset(&dst->trampoline, 0, sizeof(dst->trampoline));

    if (BeamOpsAreInitialized()) {
        dst->trampoline.common.op = BeamOpCodeAddr(op_call_error_handler);
    }

    for (int ix = 0; ix < ERTS_NUM_CODE_IX; ix++) {
        erts_activate_export_trampoline(dst, ix);
    }

#ifdef BEAMASM
    dst->dispatch.addresses[ERTS_SAVE_CALLS_CODE_IX] =
        beam_save_calls_export;
#endif
}

static void export_stage(Export *export,
                         ErtsCodeIndex src_ix,
                         ErtsCodeIndex dst_ix)
{
    ErtsDispatchable *dispatch = &export->dispatch;
    dispatch->addresses[dst_ix] = dispatch->addresses[src_ix];
}

#define ERTS_CODE_STAGED_PREFIX export
#define ERTS_CODE_STAGED_OBJECT_TYPE Export
#define ERTS_CODE_STAGED_OBJECT_HASH export_hash
#define ERTS_CODE_STAGED_OBJECT_COMPARE export_cmp
#define ERTS_CODE_STAGED_OBJECT_INITIALIZE export_init
#define ERTS_CODE_STAGED_OBJECT_STAGE export_stage
#define ERTS_CODE_STAGED_OBJECT_ALLOC_TYPE ERTS_ALC_T_EXPORT
#define ERTS_CODE_STAGED_TABLE_ALLOC_TYPE ERTS_ALC_T_EXPORT_TABLE
#define ERTS_CODE_STAGED_TABLE_INITIAL_SIZE EXPORT_INITIAL_SIZE
#define ERTS_CODE_STAGED_TABLE_LIMIT EXPORT_LIMIT

#define ERTS_CODE_STAGED_WANT_GET
#define ERTS_CODE_STAGED_WANT_PUT
#define ERTS_CODE_STAGED_WANT_LIST
#define ERTS_CODE_STAGED_WANT_LIST_SIZE
#define ERTS_CODE_STAGED_WANT_ENTRY_BYTES
#define ERTS_CODE_STAGED_WANT_TABLE_SIZE

#include "erl_code_staged.h"

void
init_export_table(void)
{
    int i;

    export_staged_init();

    for (i = 0; i < ERTS_NUM_CODE_IX; i++) {
        erts_alloc_trace_note_alloc("export_table.index_root",
                                    &export_tables[i],
                                    sizeof(export_tables[i]));
    }
}

void
init_export_table_replay(IndexTable *roots, int no_roots)
{
    HashFunctions f;
    erts_rwmtx_opt_t rwmtx_opt = ERTS_RWMTX_OPT_DEFAULT_INITER;
    int i;

    ASSERT(roots != NULL);
    ASSERT(no_roots == ERTS_NUM_CODE_IX);
    (void) no_roots;

    rwmtx_opt.type = ERTS_RWMTX_TYPE_FREQUENT_READ;
    rwmtx_opt.lived = ERTS_RWMTX_LONG_LIVED;

    erts_rwmtx_init_opt(&export_rwmutex,
                        &rwmtx_opt,
                        "export_staging_lock",
                        NIL,
                        (ERTS_LOCK_FLAGS_PROPERTY_STATIC |
                         ERTS_LOCK_FLAGS_CATEGORY_GENERIC));

    erts_atomic_init_nob(&export_total_entries_bytes, 0);

    f.hash = (H_FUN) export_staged_hash;
    f.cmp = (HCMP_FUN) export_staged_cmp;
    f.alloc = (HALLOC_FUN) export_staged_alloc;
    f.free = (HFREE_FUN) export_staged_free;
    f.meta_alloc = (HMALLOC_FUN) erts_alloc;
    f.meta_free = (HMFREE_FUN) erts_free;
    f.meta_print = (HMPRINT_FUN) erts_print;

    for (i = 0; i < ERTS_NUM_CODE_IX; i++) {
        export_tables[i] = roots[i];
        export_tables[i].htable.fun = f;
        erts_index_rebuild_hash_buckets(&export_tables[i]);
    }

    /*
     * In debug builds the staged-table template tracks whether staging is
     * active via export_debug_stage_ix (~0 = idle). BSS leaves it at 0,
     * which is not the idle sentinel; replay skips the normal staging cycle
     * so we must reset it here to prevent spurious assertion failures when
     * compile:file later triggers erts_start_staging_code_ix.
     */
#ifdef DEBUG
    export_debug_stage_ix = ~0;
#endif
}

void
export_info(fmtfn_t to, void *to_arg)
{
    export_staged_info(to, to_arg);
}

/*
 * Return a pointer to the export entry for the given function,
 * or NULL otherwise.  Notes:
 *
 * 1) BIFs have export entries and can be called through
 *    a wrapper in the export entry.
 * 2) Functions referenced by a loaded module, but not yet loaded
 *    also have export entries.  The export entry contains
 *    a wrapper which invokes the error handler if a function is
 *    called through such an export entry.
 * 3) This function is suitable for the implementation of erlang:apply/3.
 */
const Export *erts_find_export_entry(Eterm m, Eterm f, unsigned a,
                                     ErtsCodeIndex code_ix);

const Export *erts_find_export_entry(Eterm m, Eterm f, unsigned a,
                                     ErtsCodeIndex code_ix)
{
    export_template_t template;
    Export *object;

    object = export_staged_init_template(&template);
    object->info.mfa.module = m;
    object->info.mfa.function = f;
    object->info.mfa.arity = a;

    return export_staged_get(&template, code_ix);
}

/*
 * Find the export entry for a loaded function.
 * Returns a NULL pointer if the given function is not loaded, or
 * a pointer to the export entry.
 *
 * Note: This function never returns export entries for BIFs
 * or functions which are not yet loaded.  This makes it suitable
 * for use by the erlang:function_exported/3 BIF or whenever you
 * cannot depend on the error_handler.
 */
const Export *erts_find_function(Eterm m, Eterm f, unsigned int a,
                                 ErtsCodeIndex code_ix)
{
    const Export *export = erts_find_export_entry(m, f, a, code_ix);

    if (export == NULL
        || (erts_is_export_trampoline_active(export, code_ix) &&
            !BeamIsOpCode(export->trampoline.common.op,
                          op_i_generic_breakpoint))) {
        return NULL;
    }

    return export;
}

/*
 * Returns a pointer to an existing export entry for a MFA,
 * or creates a new one and returns the pointer.
 *
 * This function acts on the staging export table. It should only be used
 * to load new code.
 */

Export *erts_export_put(Eterm mod, Eterm func, unsigned int arity)
{
    export_template_t template;
    Export *object;

    ASSERT(is_atom(mod));
    ASSERT(is_atom(func));

    object = export_staged_init_template(&template);
    object->info.mfa.module = mod;
    object->info.mfa.function = func;
    object->info.mfa.arity = arity;

    return export_staged_put(&template);
}

/*
 * Find the existing export entry for M:F/A. Failing that, create a stub
 * export entry (making a call through it will cause the error_handler to
 * be called).
 *
 * Stub export entries will be placed in the staging export table.
 */

Export *erts_export_get_or_make_stub(Eterm mod, Eterm func, unsigned int arity)
{
    export_template_t template;
    Export *object;

    ASSERT(is_atom(mod));
    ASSERT(is_atom(func));

    object = export_staged_init_template(&template);
    object->info.mfa.module = mod;
    object->info.mfa.function = func;
    object->info.mfa.arity = arity;

    return export_staged_upsert(&template);
}

Export *export_list(int i, ErtsCodeIndex code_ix)
{
    return export_staged_list(i, code_ix);
}

int export_list_size(ErtsCodeIndex code_ix)
{
    return export_staged_list_size(code_ix);
}

int export_table_sz(void)
{
    return export_staged_table_size();
}

int export_entries_sz(void)
{
    return export_staged_entry_bytes();
}

const Export *export_get(const Export *e)
{
    export_template_t template;
    Export *object;

    object = export_staged_init_template(&template);
    object->info.mfa.module = e->info.mfa.module;
    object->info.mfa.function = e->info.mfa.function;
    object->info.mfa.arity = e->info.mfa.arity;

    return export_staged_get(&template, erts_active_code_ix());
}

void export_start_staging(void)
{
    export_staged_start_staging();
}

void export_end_staging(int commit)
{
    export_staged_end_staging(commit);
}

void erts_export_replay_repair_all_lambdas(void)
{
    ErtsCodeIndex code_ix;
    int count, i;

    if (!erts_mmap_record_option_replay_enabled()) {
        return;
    }

    code_ix = erts_active_code_ix();

    count = export_list_size(code_ix);
    for (i = 0; i < count; i++) {
        Export *ep = export_list(i, code_ix);

        if (!ep) {
            continue;
        }

        /*
         * Do not reuse replay-snapshot lambda objects. They may carry stale
         * runtime state in their backing memory. Rebuild a canonical shared
         * lambda from current export metadata instead.
         */
        create_shared_lambda(ep);
    }
}
