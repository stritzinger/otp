/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2020-2023. All Rights Reserved.
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

#include <algorithm>
#include <sstream>
#include <float.h>

#include "beam_asm.hpp"
using namespace asmjit;

#ifdef BEAMASM_DUMP_SIZES
#    include <mutex>

typedef std::pair<Uint64, Uint64> op_stats;

static std::unordered_map<char *, op_stats> sizes;
static std::mutex size_lock;

extern "C" void beamasm_dump_sizes() {
    std::lock_guard<std::mutex> lock(size_lock);

    std::vector<std::pair<char *, op_stats>> flat(sizes.cbegin(), sizes.cend());
    double total_size = 0.0;

    for (const auto &op : flat) {
        total_size += op.second.second;
    }

    /* Sort instructions by total size, in descending order. */
    std::sort(
            flat.begin(),
            flat.end(),
            [](std::pair<char *, op_stats> &a, std::pair<char *, op_stats> &b) {
                return a.second.second > b.second.second;
            });

    for (const auto &op : flat) {
        fprintf(stderr,
                "%34s:\t%zu\t%f\t%zu\t%zu\r\n",
                op.first,
                op.second.second,
                op.second.second / total_size,
                op.second.first,
                op.second.first ? (op.second.second / op.second.first) : 0);
    }
}
#endif

ErtsCodePtr BeamModuleAssembler::getCode(BeamLabel label) {
    ASSERT(label < rawLabels.size() + 1);
    return (ErtsCodePtr)getCode(rawLabels[label]);
}

ErtsCodePtr BeamModuleAssembler::getLambda(unsigned index) {
    const auto &lambda = lambdas[index];
    return (ErtsCodePtr)getCode(lambda.trampoline);
}

BeamModuleAssembler::BeamModuleAssembler(BeamGlobalAssembler *ga,
                                         Eterm mod,
                                         int num_labels,
                                         int num_functions,
                                         const BeamFile *file)
        : BeamModuleAssembler(ga, mod, num_labels, file) {

}


// void BeamModuleAssembler::emit_i_nif_padding() {
//     const size_t minimum_size = sizeof(UWord[BEAM_NATIVE_MIN_FUNC_SZ]);
//     size_t prev_func_start, diff;

//     prev_func_start = code.labelOffsetFromBase(rawLabels[functions.back() + 1]);
//     diff = a.offset() - prev_func_start;

//     if (diff < minimum_size) {
//         embed_zeros(minimum_size - diff);
//     }
// }

// static void i_emit_nyi(char *msg) {
//     erts_exit(ERTS_ERROR_EXIT, "NYI: %s\n", msg);
// }

// void BeamModuleAssembler::emit_nyi(const char *msg) {
//     emit_enter_runtime(0);

//     a.mov(ARG1, imm(msg));
//     runtime_call<1>(i_emit_nyi);

//     /* Never returns */
// }

// void BeamModuleAssembler::emit_nyi() {
//     emit_nyi("<unspecified>");
// }

bool BeamModuleAssembler::emit(unsigned specific_op, const Span<ArgVal> &args) {
    // check_pending_stubs();

#ifdef BEAMASM_DUMP_SIZES
    size_t before = a.offset();
#endif

    comment(opc[specific_op].name);

#define InstrCnt()
//     switch (specific_op) {
// #include "beamasm_emit.h"
//     default:
//         ERTS_ASSERT(0 && "Invalid instruction");
//         break;
//     }

#ifdef BEAMASM_DUMP_SIZES
    {
        std::lock_guard<std::mutex> lock(size_lock);

        sizes[opc[specific_op].name].first++;
        sizes[opc[specific_op].name].second += a.offset() - before;
    }
#endif

    return true;
}

// /*
//  * Here follows meta instructions.
//  */

// // void BeamGlobalAssembler::emit_i_func_info_shared() {}

// // void BeamModuleAssembler::emit_i_func_info(const ArgWord &Label,
// //                                            const ArgAtom &Module,
// //                                            const ArgAtom &Function,
// //                                            const ArgWord &Arity) {
// // }

// void BeamModuleAssembler::emit_label(const ArgLabel &Label) {
//     ASSERT(Label.isLabel());

//     current_label = rawLabels[Label.get()];
//     bind_veneer_target(current_label);

//     last_destination_offset = ~0;
// }

// void BeamModuleAssembler::emit_aligned_label(const ArgLabel &Label,
//                                              const ArgWord &Alignment) {
//     a.align(AlignMode::kCode, Alignment.get());
//     emit_label(Label);
// }

// void BeamModuleAssembler::emit_on_load() {
//     on_load = current_label;
// }


// void BeamModuleAssembler::emit_line(const ArgWord &Loc) {
//     /* There is no need to align the line instruction. In the loaded code, the
//      * type of the pointer will be void* and that pointer will only be used in
//      * comparisons.
//      *
//      * We only need to do something when there's a possibility of raising an
//      * exception at the very end of the preceding instruction (and thus
//      * pointing at the start of this one). If we were to do nothing, the error
//      * would erroneously refer to this instead of the preceding line.
//      *
//      * Since line addresses are taken _after_ line instructions we can avoid
//      * this by adding a nop when we detect this condition. */
//     if (a.offset() == last_error_offset) {
//         a.nop();
//     }
// }

// void BeamModuleAssembler::emit_func_line(const ArgWord &Loc) {
//     emit_line(Loc);
// }

// void BeamModuleAssembler::emit_empty_func_line() {
// }

/*
 * Here follows stubs for instructions that should never be called.
 */

// void BeamModuleAssembler::emit_i_debug_breakpoint() {
//     emit_nyi("i_debug_breakpoint should never be called");
// }

// void BeamModuleAssembler::emit_i_generic_breakpoint() {
//     emit_nyi("i_generic_breakpoint should never be called");
// }

// void BeamModuleAssembler::emit_trace_jump(const ArgWord &) {
//     emit_nyi("trace_jump should never be called");
// }

// void BeamModuleAssembler::emit_call_error_handler() {
//     emit_nyi("call_error_handler should never be called");
// }

// const Label &BeamModuleAssembler::resolve_beam_label(const ArgLabel &Lbl,
//                                                      enum Displacement disp) {
//     ASSERT(Lbl.isLabel());

//     const Label &beamLabel = rawLabels.at(Lbl.get());
//     const auto &labelEntry = code.labelEntry(beamLabel);

//     if (labelEntry->hasName()) {
//         return resolve_label(rawLabels.at(Lbl.get()), disp, labelEntry->name());
//     } else {
//         return resolve_label(rawLabels.at(Lbl.get()), disp);
//     }
// }

// const Label &BeamModuleAssembler::resolve_label(const Label &target,
//                                                 enum Displacement disp,
//                                                 const char *labelName) {
//     ssize_t currOffset = a.offset();

//     ssize_t minOffset = currOffset - disp;
//     ssize_t maxOffset = currOffset + disp;

//     ASSERT(disp >= dispMin && disp <= dispMax);
//     ASSERT(target.isValid());

//     if (code.isLabelBound(target)) {
//         ssize_t targetOffset = code.labelOffsetFromBase(target);

//         /* Backward reference: skip veneers if it's already in range. */
//         if (targetOffset >= minOffset) {
//             return target;
//         }
//     }

//     /* If a previously created veneer is reachable from this point, we can use
//      * it instead of creating a new one. */
//     auto range = _veneers.equal_range(target.id());
//     for (auto it = range.first; it != range.second; it++) {
//         const Veneer &veneer = it->second;

//         if (code.isLabelBound(veneer.anchor)) {
//             ssize_t veneerOffset = code.labelOffsetFromBase(veneer.anchor);

//             if (veneerOffset >= minOffset && veneerOffset <= maxOffset) {
//                 return veneer.anchor;
//             }
//         } else if (veneer.latestOffset <= maxOffset) {
//             return veneer.anchor;
//         }
//     }

//     Label anchor;

//     if (!labelName) {
//         anchor = a.newLabel();
//     } else {
//         /* This is the entry label for a function. Create an unique
//          * name for the anchor label. It is necessary to include a
//          * sequence number in the label name because if the module is
//          * huge more than one veneer can be created for each entry
//          * label. */
//         std::stringstream name;
//         name << '@' << labelName << '-' << labelSeq++;
//         anchor = a.newNamedLabel(name.str().c_str());
//     }

//     auto it = _veneers.emplace(target.id(),
//                                Veneer{.latestOffset = maxOffset,
//                                       .anchor = anchor,
//                                       .target = target});

//     const Veneer &veneer = it->second;
//     _pending_veneers.emplace(veneer);

//     return veneer.anchor;
// }

// const Label &BeamModuleAssembler::resolve_fragment(void (*fragment)(),
//                                                    enum Displacement disp) {
//     auto it = _dispatchTable.find(fragment);

//     if (it == _dispatchTable.end()) {
//         it = _dispatchTable.emplace(fragment, a.newLabel()).first;
//     }

//     return resolve_label(it->second, disp);
// }

// arm::Mem BeamModuleAssembler::embed_constant(const ArgVal &value,
//                                              enum Displacement disp) {
//     ssize_t currOffset = a.offset();

//     ssize_t minOffset = currOffset - disp;
//     ssize_t maxOffset = currOffset + disp;

//     ASSERT(disp >= dispMin && disp <= dispMax);
//     ASSERT(!value.isRegister());

//     /* If a previously embedded constant is reachable from this point, we
//      * can use it instead of creating a new one. */
//     auto range = _constants.equal_range(value);
//     for (auto it = range.first; it != range.second; it++) {
//         const Constant &constant = it->second;

//         if (code.isLabelBound(constant.anchor)) {
//             ssize_t constOffset = code.labelOffsetFromBase(constant.anchor);

//             if (constOffset >= minOffset && constOffset <= maxOffset) {
//                 return arm::Mem(constant.anchor);
//             }
//         } else if (constant.latestOffset <= maxOffset) {
//             return arm::Mem(constant.anchor);
//         }
//     }

//     auto it = _constants.emplace(value,
//                                  Constant{.latestOffset = maxOffset,
//                                           .anchor = a.newLabel(),
//                                           .value = value});

//     const Constant &constant = it->second;
//     _pending_constants.emplace(constant);

//     return arm::Mem(constant.anchor);
// }

// void BeamModuleAssembler::emit_i_flush_stubs() {
//     /* Flush all stubs that are due within the next two check intervals
//      * to prevent them from being emitted inside function prologues or
//      * NIF padding. */
//     flush_pending_stubs(STUB_CHECK_INTERVAL * 2);
//     last_stub_check_offset = a.offset();
// }

// void BeamModuleAssembler::check_pending_stubs() {
//     size_t currOffset = a.offset();

//     /* We shouldn't let too much space pass between checks. */
//     ASSERT((last_stub_check_offset + dispMin) >= currOffset);

//     if ((last_stub_check_offset + STUB_CHECK_INTERVAL) < currOffset) {
//         last_stub_check_offset = currOffset;

//         flush_pending_stubs(STUB_CHECK_INTERVAL * 2);
//     }
// }

// void BeamModuleAssembler::flush_pending_stubs(size_t range) {
//     ssize_t effective_offset = a.offset() + range;
//     Label next;

//     while (!_pending_veneers.empty()) {
//         const Veneer &veneer = _pending_veneers.top();

//         if (veneer.latestOffset > effective_offset) {
//             break;
//         }

//         if (!code.isLabelBound(veneer.anchor)) {
//             if (!next.isValid()) {
//                 next = a.newLabel();

//                 comment("Begin stub section");
//                 a.b(next);
//             }

//             emit_veneer(veneer);

//             effective_offset = a.offset() + range;
//         }

//         _pending_veneers.pop();
//     }

//     while (!_pending_constants.empty()) {
//         const Constant &constant = _pending_constants.top();

//         if (constant.latestOffset > effective_offset) {
//             break;
//         }

//         /* Unlike veneers, we never bind constants ahead of time. */
//         ASSERT(!code.isLabelBound(constant.anchor));

//         if (!next.isValid()) {
//             next = a.newLabel();

//             comment("Begin stub section");
//             a.b(next);
//         }

//         emit_constant(constant);

//         effective_offset = a.offset() + range;

//         _pending_constants.pop();
//     }

//     if (next.isValid()) {
//         comment("End stub section");
//         a.bind(next);
//     }
// }

// void BeamModuleAssembler::emit_veneer(const Veneer &veneer) {
//     const Label &anchor = veneer.anchor;
//     const Label &target = veneer.target;
//     bool directBranch;

//     ASSERT(!code.isLabelBound(anchor));
//     a.bind(anchor);

//     /* Prefer direct branches when possible. */
//     if (code.isLabelBound(target)) {
//         auto targetOffset = code.labelOffsetFromBase(target);
//         directBranch = (a.offset() - targetOffset) <= disp128MB;
//     } else {
//         directBranch = false;
//     }

// #ifdef DEBUG
//     directBranch &= (a.offset() % 512) >= 256;
// #endif

//     if (ERTS_LIKELY(directBranch)) {
//         a.b(target);
//     } else {
//         Label pointer = a.newLabel();

//         a.ldr(SUPER_TMP, arm::Mem(pointer));
//         a.br(SUPER_TMP);

//         a.align(AlignMode::kCode, 8);
//         a.bind(pointer);
//         a.embedLabel(veneer.target);
//     }
// }

// void BeamModuleAssembler::emit_constant(const Constant &constant) {
//     const Label &anchor = constant.anchor;
//     const ArgVal &value = constant.value;

//     ASSERT(!code.isLabelBound(anchor));
//     a.align(AlignMode::kData, 8);
//     a.bind(anchor);

//     ASSERT(!value.isRegister());

//     if (value.isImmed()) {
//         a.embedUInt64(value.as<ArgImmed>().get());
//     } else if (value.isWord()) {
//         a.embedUInt64(value.as<ArgWord>().get());
//     } else if (value.isLabel()) {
//         a.embedLabel(rawLabels.at(value.as<ArgLabel>().get()));
//     } else {
//         switch (value.getType()) {
//         case ArgVal::BytePtr:
//             strings.push_back({anchor, 0, value.as<ArgBytePtr>().get()});
//             a.embedUInt64(LLONG_MAX);
//             break;
//         case ArgVal::Catch: {
//             auto handler = rawLabels[value.as<ArgCatch>().get()];
//             catches.push_back({{anchor, 0, 0}, handler});

//             /* Catches are limited to 32 bits, but since we don't want to load
//              * 32-bit argument values due to displacement limits, we'll store
//              * this as a 64-bit value with the upper bits cleared. */
//             a.embedUInt64(INT_MAX);
//             break;
//         }
//         case ArgVal::Export: {
//             auto index = value.as<ArgExport>().get();
//             imports[index].patches.push_back({anchor, 0, 0});
//             a.embedUInt64(LLONG_MAX);
//             break;
//         }
//         case ArgVal::FunEntry: {
//             auto index = value.as<ArgLambda>().get();
//             lambdas[index].patches.push_back({anchor, 0, 0});
//             a.embedUInt64(LLONG_MAX);
//             break;
//         }
//         case ArgVal::Literal: {
//             auto index = value.as<ArgLiteral>().get();
//             literals[index].patches.push_back({anchor, 0, 0});
//             a.embedUInt64(LLONG_MAX);
//             break;
//         }
//         default:
//             ASSERT(!"error");
//         }
//     }
// }
