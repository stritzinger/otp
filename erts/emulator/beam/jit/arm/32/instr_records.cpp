/*
 * %CopyrightBegin%
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright Ericsson AB 2025-2026. All Rights Reserved.
 * SPDX-FileCopyrightText: Copyright 2024-2026 Stritzinger GmbH
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

#include "beam_asm.hpp"

extern "C"
{
#include "erl_record.h"
}

void BeamModuleAssembler::emit_is_any_native_record(const ArgLabel &Fail,
                                                    const ArgRegister &Src) {
    auto src = load_source(Src, ARG3);

    emit_is_boxed(resolve_beam_label(Fail, dispUnknown), Src, src.reg);

    preserve_cache(
            [&]() {
                a32::Gp boxed_ptr = emit_ptr_val(TMP, src.reg);
                a.ldr(TMP, emit_boxed_val(boxed_ptr));
                a.and_(TMP, TMP, imm(_TAG_HEADER_MASK));
                a.cmp(TMP, imm(_TAG_HEADER_RECORD));
                a.b_ne(resolve_beam_label(Fail, disp32MB));
            },
            TMP);
}

void BeamModuleAssembler::emit_is_native_record(const ArgLabel &Fail,
                                                const ArgRegister &Src,
                                                const ArgAtom &Module,
                                                const ArgAtom &Name) {
    auto src = load_source(Src, ARG3);

    preserve_cache(
            [&]() {
                a32::Gp boxed_ptr = emit_ptr_val(TMP, src.reg);
                a.ldr(TMP,
                      emit_boxed_val(boxed_ptr,
                                     offsetof(ErtsRecordInstance,
                                              record_definition)));
                boxed_ptr = emit_ptr_val(TMP, TMP);
                ERTS_CT_ASSERT_FIELD_PAIR(ErtsRecordDefinition, module, name);
                lea(ARG4,
                    emit_boxed_val(boxed_ptr,
                                   offsetof(ErtsRecordDefinition, module)));
                a.ldmia(a32::Mem(ARG4), a32::GpList({VAR, TMP}));

                mov_imm(ARG3, Module.get());
                a.cmp(VAR, ARG3);
                a.b_ne(resolve_beam_label(Fail, disp32MB));
                mov_imm(ARG3, Name.get());
                a.cmp(TMP, ARG3);
                a.b_ne(resolve_beam_label(Fail, disp32MB));
            },
            TMP,
            VAR,
            ARG3,
            ARG4);
}

void BeamModuleAssembler::emit_is_record_accessible(const ArgLabel &Fail,
                                                    const ArgRegister &Src,
                                                    const ArgAtom &Scope) {
    auto src = load_source(Src, ARG3);

    preserve_cache(
            [&]() {
                a32::Gp boxed_ptr = emit_ptr_val(TMP, src.reg);
                a.ldr(TMP,
                      emit_boxed_val(boxed_ptr,
                                     offsetof(ErtsRecordInstance,
                                              record_definition)));
                boxed_ptr = emit_ptr_val(TMP, TMP);
                a.ldr(VAR,
                      emit_boxed_val(
                              boxed_ptr,
                              offsetof(ErtsRecordDefinition, is_exported)));

                if (Scope.get() == am_external) {
                    const Uint bit_num = _TAG_IMMED2_SIZE;
                    ERTS_CT_ASSERT(am_false == make_atom(0));
                    ERTS_CT_ASSERT(am_true == make_atom(1));
                    ERTS_CT_ASSERT((1 << bit_num) ==
                                   (am_true - am_false));

                    comment("external operation");
                    a.tst(VAR, imm(1 << bit_num));
                    a.b_eq(resolve_beam_label(Fail, disp32MB));
                } else {
                    Label next = a.new_label();

                    comment("auto_local operation");
                    mov_imm(ARG3, am_true);
                    a.cmp(VAR, ARG3);
                    a.b_eq(next);

                    a.ldr(VAR,
                          emit_boxed_val(
                                  boxed_ptr,
                                  offsetof(ErtsRecordDefinition, module)));
                    mov_imm(ARG3, mod);
                    a.cmp(VAR, ARG3);
                    a.b_ne(resolve_beam_label(Fail, disp32MB));

                    a.bind(next);
                }
            },
            TMP,
            VAR,
            ARG3);
}

void BeamModuleAssembler::emit_i_get_record_elements(
        const ArgLabel &Fail,
        const ArgRegister &Src,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    mov_arg(ARG3, Src);
    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_imm(ARG4, args.size());
    embed_vararg_rodata(args, TMP);

    emit_enter_runtime<Update::eStack>();

    a.sub(a32::sp, a32::sp, imm(8)); /* keep AAPCS alignment */
    a.str(TMP, a32::Mem(a32::sp, 0)); /* arg5: elements */
    runtime_call<bool (*)(Process *, Eterm *, Eterm, Uint, const Eterm *),
                 erl_get_record_elements>();
    a.add(a32::sp, a32::sp, imm(8));

    emit_leave_runtime<Update::eStack>();

    a.tst(ARG1, ARG1);
    a.b_eq(resolve_beam_label(Fail, dispUnknown));
}

void BeamModuleAssembler::emit_i_create_local_native_record(
        const ArgLiteral &Def,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    Label next = a.new_label();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_arg(ARG3, Def);
    mov_arg(ARG4, Live);

    emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

    a.sub(a32::sp, a32::sp, imm(8));
    mov_imm(TMP, args.size());
    a.str(TMP, a32::Mem(a32::sp, 0)); /* arg5: number of updates */
    embed_vararg_rodata(args, TMP);
    a.str(TMP, a32::Mem(a32::sp, 4)); /* arg6: updates */
    runtime_call<
            Eterm (*)(Process *, Eterm *, Eterm, Uint, Uint, const Eterm *),
            erl_create_local_native_record>();
    a.add(a32::sp, a32::sp, imm(8));

    emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

    emit_branch_if_value(ARG1, next);
    emit_raise_exception();

    a.bind(next);
    mov_arg(Dst, ARG1);
}

void BeamModuleAssembler::emit_i_create_native_record(
        const ArgConstant &Id,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    Label next = a.new_label();

    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_arg(ARG3, Id);
    mov_arg(ARG4, Live);

    emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

    a.sub(a32::sp, a32::sp, imm(8));
    mov_imm(TMP, args.size());
    a.str(TMP, a32::Mem(a32::sp, 0)); /* arg5: number of updates */
    embed_vararg_rodata(args, TMP);
    a.str(TMP, a32::Mem(a32::sp, 4)); /* arg6: updates */
    runtime_call<
            Eterm (*)(Process *, Eterm *, Eterm, Uint, Uint, const Eterm *),
            erl_create_native_record>();
    a.add(a32::sp, a32::sp, imm(8));

    emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

    emit_branch_if_value(ARG1, next);
    emit_raise_exception();

    a.bind(next);
    mov_arg(Dst, ARG1);
}

void BeamModuleAssembler::emit_i_update_native_record(
        const ArgSource &Src,
        const ArgRegister &Dst,
        const ArgWord &Live,
        const ArgWord &Size,
        const Span<const ArgVal> &args) {
    Label next = a.new_label();

    mov_arg(ARG3, Src);
    a.mov(ARG1, c_p);
    load_x_reg_array(ARG2);
    mov_arg(ARG4, Live);

    emit_enter_runtime<Update::eHeapAlloc | Update::eReductions>();

    a.sub(a32::sp, a32::sp, imm(8));
    mov_imm(TMP, args.size());
    a.str(TMP, a32::Mem(a32::sp, 0)); /* arg5: number of updates */
    embed_vararg_rodata(args, TMP);
    a.str(TMP, a32::Mem(a32::sp, 4)); /* arg6: updates */
    runtime_call<
            Eterm (*)(Process *, Eterm *, Eterm, Uint, Uint, const Eterm *),
            erl_update_native_record>();
    a.add(a32::sp, a32::sp, imm(8));

    emit_leave_runtime<Update::eHeapAlloc | Update::eReductions>();

    emit_branch_if_value(ARG1, next);
    emit_raise_exception();

    a.bind(next);
    mov_arg(Dst, ARG1);
}

void BeamModuleAssembler::emit_get_record_field(const ArgLabel &Fail,
                                                const ArgRegister &Src,
                                                const ArgConstant &Id,
                                                const ArgAtom &Name,
                                                const ArgRegister &Dst) {
    a.mov(ARG1, c_p);
    mov_arg(ARG2, Src);
    mov_arg(ARG3, Id);
    mov_arg(ARG4, Name);

    emit_enter_runtime<Update::eHeapAlloc>();
    if (Id.isImmed()) {
        comment("local record");
        runtime_call<Eterm (*)(Process *, Eterm, Eterm, Eterm),
                     erl_get_local_record_field>();
    } else {
        comment("external record");
        runtime_call<Eterm (*)(Process *, Eterm, Eterm, Eterm),
                     erl_get_record_field>();
    }
    emit_leave_runtime<Update::eHeapAlloc>();

    if (Fail.get() != 0) {
        emit_branch_if_not_value(ARG1,
                                 resolve_beam_label(Fail, dispUnknown));
    } else {
        Label next = a.new_label();

        emit_branch_if_value(ARG1, next);
        emit_raise_exception();

        a.bind(next);
    }

    mov_arg(Dst, ARG1);
}
