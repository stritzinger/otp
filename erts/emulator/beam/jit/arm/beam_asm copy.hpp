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

#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <map>
#include <functional>
#include <algorithm>
#include <cmath>

#ifndef ASMJIT_ASMJIT_H_INCLUDED
#    include <asmjit/asmjit.hpp>
#endif

#include <asmjit/a32.h>

extern "C"
{
#ifdef HAVE_CONFIG_H
#    include "config.h"
#endif

#include "sys.h"
#include "erl_vm.h"
#include "global.h"
#include "beam_catches.h"
#include "big.h"

#include "beam_asm.h"
}

#include "beam_jit_common.hpp"

/* On Windows, the min and max macros may be defined. */
#undef min
#undef max

using namespace asmjit;

struct BeamAssembler : public BeamAssemblerCommon {
    BeamAssembler() : BeamAssemblerCommon(a) {
        Error err = code.attach(&a);
        ERTS_ASSERT(!err && "Failed to attach codeHolder");
    }

    BeamAssembler(const std::string &log) : BeamAssembler() {
        if (erts_jit_asm_dump) {
            setLogger(log + ".asm");
        }
    }

protected:
    a32::Assembler a;

    /* * * * * * * * * */

    /* Points at x_reg_array inside an ErtsSchedulerRegisters struct, allowing
     * the aux_regs field to be addressed with an 8-bit displacement. */
    const a32::Gp scheduler_registers = a32::r10;

    const a32::Gp E = a32::fp;

    const a32::Gp c_p = a32::r7;
    const a32::Gp FCALLS = a32::r8;
    const a32::Gp HTOP = a32::r9;

    /* Local copy of the active code index.
     *
     * This is set to ERTS_SAVE_CALLS_CODE_IX when save_calls is active, which
     * routes us to a common handler routine that calls save_calls before
     * jumping to the actual code. */
    const a32::Gp active_code_ix = a32::r6;

#ifdef ERTS_MSACC_EXTENDED_STATES
    const a32::Mem erts_msacc_cache = getSchedulerRegRef(
            offsetof(ErtsSchedulerRegisters, aux_regs.d.erts_msacc_cache));
#endif


    const a32::Gp ARG1 = a32::r1;
    const a32::Gp ARG2 = a32::r2;
    const a32::Gp ARG3 = a32::r3;
    const a32::Gp ARG4 = a32::r4;
    const a32::Gp ARG5 = a32::r5;

    const a32::Gp RET = a32::r0;

    constexpr arm::Mem getSchedulerRegRef(int offset) const {
        ASSERT((offset & (sizeof(Eterm) - 1)) == 0);
        return arm::Mem(scheduler_registers, offset);
    }
};

#include "beam_asm_global.hpp"

class BeamModuleAssembler : public BeamAssembler,
                            public BeamModuleAssemblerCommon {
    BeamGlobalAssembler *ga;

    /* Sequence number used to create unique named labels by
     * resolve_label(). Only used when assembly output has been
     * requested. */
    long labelSeq = 0;

    /* Save the last PC for an error. */
    size_t last_error_offset = 0;

    static constexpr ptrdiff_t STUB_CHECK_INTERVAL = 4 << 10;
    static constexpr ptrdiff_t STUB_CHECK_INTERVAL_UNREACHABLE =
            (4 << 10) - 128;
    size_t last_stub_check_offset = 0;

    /* Save the last known unreachable position. */
    size_t last_unreachable_offset = 0;

    /* Mark this point unreachable. This must be placed at the very end when
     * used in a BEAM instruction, and should not be used in helper
     * functions. */
    void mark_unreachable() {
        last_unreachable_offset = a.offset();
    }

    /* Use within BEAM instructions. */
    void mark_unreachable_check_pending_stubs() {
        mark_unreachable();
        check_pending_stubs();
    }

    bool is_unreachable() {
        return a.offset() == last_unreachable_offset;
    }

    enum Displacement : size_t {
        /* Pessimistic estimate for helper functions, where we don't know the
         * branch displacement or whether it will be used near label
         * resolution.
         *
         * Note that we subtract the size of one instruction to handle
         * backward displacements. */
        dispUnknown = (32 << 10) - sizeof(Uint32) - STUB_CHECK_INTERVAL,

        /* +- 32KB: `tbz`, `tbnz`, `ldr` of 8-byte literal. */
        disp32K = (32 << 10) - sizeof(Uint32),

        /* +- 1MB: `adr`, `b.cond`, `cb.cond` */
        disp1MB = (1 << 20) - sizeof(Uint32),

        /* +- 128MB: `b`, `blr` */
        disp128MB = (128 << 20) - sizeof(Uint32),

        dispMin = dispUnknown,
        dispMax = disp128MB
    };

    static_assert(dispMin <= dispUnknown && dispMax >= disp128MB);
    static_assert(STUB_CHECK_INTERVAL < dispMin / 2);

    struct Veneer {
        ssize_t latestOffset;
        Label anchor;

        Label target;

        constexpr bool operator>(const Veneer &other) const {
            return latestOffset > other.latestOffset;
        }
    };

    struct Constant {
        ssize_t latestOffset;
        Label anchor;

        ArgVal value;

        constexpr bool operator>(const Constant &other) const {
            return latestOffset > other.latestOffset;
        }
    };

    struct EmbeddedLabel {
        ssize_t latestOffset;
        Label anchor;

        Label label;

        constexpr bool operator>(const EmbeddedLabel &other) const {
            return latestOffset > other.latestOffset;
        }
    };

    /* ArgVal -> Constant
     *
     * `_pending_constants` points directly into this container, which is
     * documented to be safe as long as we only insert elements. */
    std::unordered_multimap<ArgVal, const Constant, ArgVal::Hash> _constants;

    /* Label::id() -> Veneer
     *
     * `_pending_veneers` points directly into this container. */
    std::unordered_multimap<uint32_t, const Veneer> _veneers;

    template<typename T>
    using PendingStubs =
            std::priority_queue<std::reference_wrapper<const T>,
                                std::deque<std::reference_wrapper<const T>>,
                                std::greater<const T &>>;

    /* Index of Label -> EmbeddedLabel
     *
     * `_pending_labels` points directly into this container. */
    std::unordered_map<uint32_t, EmbeddedLabel> _embedded_labels;

    /* All pending stubs, segregated by type and sorted by `latestOffset` in
     * ascending order.
     *
     * We use separate queues to avoid interleaving them, as they have
     * different sizes and alignment requirements. */
    PendingStubs<Constant> _pending_constants;
    PendingStubs<Veneer> _pending_veneers;
    PendingStubs<EmbeddedLabel> _pending_labels;

    /* Maps code pointers to thunks that jump to them, letting us treat global
     * fragments as if they were local. */
    std::unordered_map<void (*)(), Label> _dispatchTable;

    RegisterCache<16, arm::Mem, a32::Gp> reg_cache =
            RegisterCache<16, arm::Mem, a32::Gp>(scheduler_registers, E, {});

    void reg_cache_put(arm::Mem mem, a32::Gp src) {

    }

    a32::Gp find_cache(arm::Mem mem) {
    }

    /* Works as the STR instruction, but also updates the cache. */
    void str_cache(a32::Gp src, arm::Mem mem_dst) {

    }

    /* Works as the STP instruction, but also updates the cache. */
    void stp_cache(a32::Gp src1, a32::Gp src2, arm::Mem mem_dst) {

    }

    /* Works like LDR, but looks in the cache first. */
    void ldr_cached(a32::Gp dst, arm::Mem mem) {

    }

    template<typename L, typename... Any>
    void preserve_cache(L generate, Any... clobber) {

    }

    void trim_preserve_cache(const ArgWord &Words) {

    }

    void mov_preserve_cache(a32::VecD dst, a32::VecD src) {

    }

    void mov_preserve_cache(a32::Gp dst, a32::Gp src) {

    }

    void untag_ptr_preserve_cache(a32::Gp dst, a32::Gp src) {

    }

    arm::Mem embed_label(const Label &label, enum Displacement disp);

public:
    BeamModuleAssembler(BeamGlobalAssembler *ga,
                        Eterm mod,
                        int num_labels,
                        const BeamFile *file = NULL);
    BeamModuleAssembler(BeamGlobalAssembler *ga,
                        Eterm mod,
                        int num_labels,
                        int num_functions,
                        const BeamFile *file = NULL);

    bool emit(unsigned op, const Span<ArgVal> &args);

    void emit_coverage(void *coverage, Uint index, Uint size);

    void codegen(JitAllocator *allocator,
                 const void **executable_ptr,
                 void **writable_ptr,
                 const BeamCodeHeader *in_hdr,
                 const BeamCodeHeader **out_exec_hdr,
                 BeamCodeHeader **out_rw_hdr);

    void codegen(JitAllocator *allocator,
                 const void **executable_ptr,
                 void **writable_ptr);

    void codegen(char *buff, size_t len);

    void *register_metadata(const BeamCodeHeader *header);

    ErtsCodePtr getCode(unsigned label);
    ErtsCodePtr getLambda(unsigned index);

    void *getCode(Label label) {
        return BeamAssembler::getCode(label);
    }

    byte *getCode(char *labelName) {
        return BeamAssembler::getCode(labelName);
    }

    void embed_vararg_rodata(const Span<ArgVal> &args, a32::Gp reg);

    unsigned getCodeSize() {
        ASSERT(code.hasBaseAddress());
        return code.codeSize();
    }

    void copyCodeHeader(BeamCodeHeader *hdr);
    BeamCodeHeader *getCodeHeader(void);
    const ErtsCodeInfo *getOnLoad(void);

    unsigned patchCatches(char *rw_base);
    void patchLambda(char *rw_base, unsigned index, const ErlFunEntry *fe);
    void patchLiteral(char *rw_base, unsigned index, Eterm lit);
    void patchImport(char *rw_base, unsigned index, const Export *import);
    void patchStrings(char *rw_base, const byte *string);

protected:
    void emit_gc_test(const ArgWord &Stack,
                      const ArgWord &Heap,
                      const ArgWord &Live);
    void emit_gc_test_preserve(const ArgWord &Need,
                               const ArgWord &Live,
                               const ArgSource &Preserve,
                               a32::Gp preserve_reg);

    arm::Mem emit_variable_apply(bool includeI);
    arm::Mem emit_fixed_apply(const ArgWord &arity, bool includeI);

    a32::Gp emit_call_fun(bool skip_box_test = false,
                          bool skip_header_test = false);

    void emit_is_cons(Label Fail, a32::Gp Src) {
        preserve_cache([&]() {
            BeamAssembler::emit_is_cons(Fail, Src);
        });
    }

    void emit_is_not_cons(Label Fail, a32::Gp Src) {

    }

    void emit_is_list(Label Fail, a32::Gp Src) {
    }

    void emit_is_boxed(Label Fail, a32::Gp Src) {

    }

    void emit_is_boxed(Label Fail, const ArgVal &Arg, a32::Gp Src) {

    }

    /* Copies `count` words from the address at `from`, to the address at `to`.
     *
     * Clobbers v30 and v31. */
    void emit_copy_words_increment(a32::Gp from, a32::Gp to, size_t count);

    void emit_get_list(const a32::Gp boxed_ptr,
                       const ArgRegister &Hd,
                       const ArgRegister &Tl);

    void emit_add_sub_types(bool is_small_result,
                            const ArgSource &LHS,
                            const a32::Gp lhs_reg,
                            const ArgSource &RHS,
                            const a32::Gp rhs_reg,
                            const Label next);

    void emit_are_both_small(const ArgSource &LHS,
                             const a32::Gp lhs_reg,
                             const ArgSource &RHS,
                             const a32::Gp rhs_reg,
                             const Label next);

    void emit_div_rem_literal(Sint divisor,
                              const ArgSource &Dividend,
                              a32::Gp dividend,
                              a32::Gp quotient,
                              a32::Gp remainder,
                              const Label &generic,
                              bool need_div,
                              bool need_rem);

    void emit_div_rem(const ArgLabel &Fail,
                      const ArgSource &LHS,
                      const ArgSource &RHS,
                      const ErtsCodeMFA *error_mfa,
                      const ArgRegister &Quotient,
                      const ArgRegister &Remainder,
                      bool need_div,
                      bool need_rem);

    void emit_i_bif(const ArgLabel &Fail,
                    const ArgWord &Bif,
                    const ArgRegister &Dst);

    void emit_error(int code);
    void emit_error(int reason, const ArgSource &Src);

    int emit_bs_get_field_size(const ArgSource &Size,
                               int unit,
                               Label Fail,
                               const a32::Gp &out);

    void emit_bs_get_utf8(const ArgRegister &Ctx, const ArgLabel &Fail);
    void emit_bs_get_utf16(const ArgRegister &Ctx,
                           const ArgLabel &Fail,
                           const ArgWord &Flags);
    void update_bin_state(a32::Gp bin_offset,
                          Sint bit_offset,
                          Sint size,
                          a32::Gp size_reg);
    void set_zero(Sint effectiveSize);
    void emit_construct_utf8(const ArgVal &Src,
                             Sint bit_offset,
                             bool is_byte_aligned);

    void emit_read_bits(Uint bits,
                        const a32::Gp bin_offset,
                        const a32::Gp bin_base,
                        const a32::Gp bitdata);

    void emit_extract_integer(const a32::Gp &bitdata,
                              const a32::Gp &small_tag,
                              Uint flags,
                              Uint position,
                              Uint bits,
                              const ArgRegister &Dst);

    void emit_extract_bitstring(const a32::Gp bitdata,
                                Uint position,
                                Uint bits,
                                const ArgRegister &Dst);

    UWord bs_get_flags(const ArgVal &val);

    void emit_raise_exception();
    void emit_raise_exception(const ErtsCodeMFA *exp);
    void emit_raise_exception(Label I, const ErtsCodeMFA *exp);

    void emit_validate(const ArgWord &Arity);
    void emit_bs_skip_bits(const ArgLabel &Fail, const ArgRegister &Ctx);

    void emit_linear_search(a32::Gp val, Label fail, const Span<ArgVal> &args);

    void emit_float_instr(uint32_t instId,
                          const ArgFRegister &LHS,
                          const ArgFRegister &RHS,
                          const ArgFRegister &Dst);

    void emit_validate_unicode(Label next, Label fail, a32::Gp value);

    void ubif_comment(const ArgWord &Bif);

    void emit_cmp_immed_to_bool(arm::CondCode cc,
                                const ArgSource &LHS,
                                const ArgSource &RHS,
                                const ArgRegister &Dst);

    void emit_cond_to_bool(arm::CondCode cc, const ArgRegister &Dst);
    void emit_bif_is_ge_lt(arm::CondCode cc,
                           const ArgSource &LHS,
                           const ArgSource &RHS,
                           const ArgRegister &Dst);
    void emit_bif_min_max(arm::CondCode cc,
                          const ArgSource &LHS,
                          const ArgSource &RHS,
                          const ArgRegister &Dst);

    void emit_proc_lc_unrequire(void);
    void emit_proc_lc_require(void);

    void emit_nyi(const char *msg);
    void emit_nyi(void);

    /* Returns a vector of the untagged and rebased `args`. The adjusted
     * `comparand` is stored in ARG1. */
    const std::vector<ArgVal> emit_select_untag(const ArgSource &Src,
                                                const Span<ArgVal> &args,
                                                a32::Gp comparand,
                                                Label fail,
                                                UWord base,
                                                int shift);

    void emit_binsearch_nodes(a32::Gp reg,
                              size_t Left,
                              size_t Right,
                              Label fail,
                              const Span<ArgVal> &args);

    void emit_optimized_two_way_select(a32::Gp reg,
                                       const ArgVal &value1,
                                       const ArgVal &value2,
                                       const ArgVal &label);

#ifdef DEBUG
    void emit_tuple_assertion(const ArgSource &Src, a32::Gp tuple_reg);
#endif

    void emit_dispatch_return();

#include "beamasm_protos.h"

    /* Resolves a BEAM label.
     *
     * When the branch type is not `dispUnknown`, this must be used
     * _IMMEDIATELY BEFORE_ the instruction that the label is used in. */
    const Label &resolve_beam_label(const ArgLabel &Label,
                                    enum Displacement disp);
    const Label &resolve_label(const Label &target,
                               enum Displacement disp,
                               const char *name = nullptr);

    /* Resolves a shared fragment, creating a trampoline that loads the
     * appropriate address before jumping there.
     *
     * When the branch type is not `dispUnknown`, this must be used
     * _IMMEDIATELY BEFORE_ the instruction that the label is used in. */
    const Label &resolve_fragment(void (*fragment)(), enum Displacement disp);

    /* Embeds a constant argument and returns its address. All kinds of
     * constants are accepted, including labels and export entries.
     *
     * When the branch type is not `dispUnknown`, this must be used
     * _IMMEDIATELY BEFORE_ the instruction that the label is used in. */
    arm::Mem embed_constant(const ArgVal &value, enum Displacement disp);

    /* Convenience wrapper for embedding raw pointers or immediates. */
    template<typename T,
             std::enable_if_t<std::is_integral<T>::value ||
                                      std::is_pointer<T>::value,
                              bool> = true>
    arm::Mem embed_constant(T data, enum Displacement disp) {
        return embed_constant(ArgWord((UWord)data), disp);
    }

    /* Binds a label and all related veneers that are within reach of it. */
    void bind_veneer_target(const Label &target);

    void emit_constant(const Constant &constant);
    void emit_veneer(const Veneer &veneer);

    /* Unconditionally emits all veneers and constants that are due within
     * `range` bytes. */
    void flush_pending_stubs(size_t range);

    /* Emits pending veneers when appropriate. Must be called at least once
     * every `STUB_CHECK_INTERVAL` bytes for veneers and constants to work. */
    void check_pending_stubs();

    /* Unconditionally emits all pending labels. Must only be called when
     * the current code position is unreachable. */
    void flush_pending_labels();

    /* Calls the given shared fragment, ensuring that the redzone is unused and
     * that the return address forms a valid CP. */
    template<typename Any>
    void fragment_call(Any target) {
        emit_assert_redzone_unused();

#if defined(JIT_HARD_DEBUG)
        /* Verify that the stack has not grown. */
        Label next = a.newLabel();
        a.ldr(SUPER_TMP, getInitialSPRef());
        a.cmp(a32::sp, SUPER_TMP);
        a.b_eq(next);
        a.udf(0xdead);
        a.bind(next);
#endif

        a.bl(resolve_fragment((void (*)())target, disp128MB));
    }

    template<typename T>
    struct function_arity;
    template<typename T, typename... Args>
    struct function_arity<T(Args...)>
            : std::integral_constant<int, sizeof...(Args)> {};

    template<int expected_arity, typename T>
    void runtime_call(T(*func)) {
        static_assert(expected_arity == function_arity<T>());

        a.bl(resolve_fragment((void (*)())func, disp128MB));
    }

    bool isRegisterBacked(const ArgVal &arg) {
        if (arg.isXRegister()) {
            return arg.as<ArgXRegister>().get() < num_register_backed_xregs;
        } else if (arg.isFRegister()) {
            return arg.as<ArgFRegister>().get() < num_register_backed_fregs;
        }

        return false;
    }

    template<typename RegType = a32::Gp>
    struct Variable {
        RegType reg;
        arm::Mem mem;

        Variable(RegType _r) : Variable(_r, arm::Mem()) {
        }
        Variable(RegType _r, arm::Mem _mem) : reg(_r), mem(_mem) {
        }
    };

    Variable<a32::Gp> init_destination(const ArgVal &arg, a32::Gp tmp) {
        /* We don't support storing into GpW since their maximum displacement
         * is 16K, which means we have to check stubs far more often. */
        ASSERT(tmp.isGpX());

        if (isRegisterBacked(arg)) {
            auto index = arg.as<ArgXRegister>().get();
            return Variable(register_backed_xregs[index]);
        } else {
            return Variable(tmp, getArgRef(arg));
        }
    }

    Variable<a32::VecD> init_destination(const ArgVal &arg, a32::VecD tmp) {
        if (isRegisterBacked(arg)) {
            auto index = arg.as<ArgFRegister>().get();
            return Variable(register_backed_fregs[index]);
        } else {
            return Variable(tmp, getArgRef(arg));
        }
    }

    Variable<a32::Gp> load_source(const ArgVal &arg, a32::Gp tmp) {
        /* We don't support loading into GpW since their maximum displacement
         * is 16K, which means we have to check stubs far more often. */
        ASSERT(tmp.isGpX());

        if (arg.isLiteral()) {
            preserve_cache(
                    [&]() {
                        a.ldr(tmp, embed_constant(arg, disp32K));
                    },
                    tmp);
            return Variable(tmp);
        } else if (arg.isRegister()) {
            if (isRegisterBacked(arg)) {
                auto index = arg.as<ArgXRegister>().get();
                a32::Gp reg = register_backed_xregs[index];
                reg_cache.invalidate(reg);
                return Variable(reg);
            }

            auto ref = getArgRef(arg);
            ldr_cached(tmp, ref);
            return Variable(tmp, ref);
        } else {
            if (arg.isImmed() || arg.isWord()) {
                auto val = arg.isImmed() ? arg.as<ArgImmed>().get()
                                         : arg.as<ArgWord>().get();

                if (Support::isIntOrUInt32(val)) {
                    preserve_cache(
                            [&]() {
                                mov_imm(tmp, val);
                            },
                            tmp);
                    return Variable(tmp);
                }
            }

            preserve_cache(
                    [&]() {
                        a.ldr(tmp, embed_constant(arg, disp32K));
                    },
                    tmp);
            return Variable(tmp);
        }
    }

    /*
     * Load the argument into ANY register, using the
     * cache to avoid reloading the value.
     *
     * Because it is not possible to predict into which register
     * the value will end up, the following code is UNSAFE:
     *
     *    auto src = load_source(Src);
     *    a.tst(src.reg, ...);
     *    a.mov(TMP2, NIL);
     *    a.ccmp(src.reg, TMP2, ..., ...);
     *
     * If the value of Src happens to end up in TMP2, it will be
     * overwritten before its second use.
     *
     * Basically, the only safe way to use this function is when the
     * register is used immediately and only once. For example:
     *
     *    a.and_(TMP1, load_source(Src), imm(...));
     *    a.cmp(TMP1, imm(...));
     */
    Variable<a32::Gp> load_source(const ArgVal &arg) {
        if (!arg.isRegister()) {
            return load_source(arg, TMP1);
        } else {
            a32::Gp cached_reg = find_cache(getArgRef(arg));

            if (cached_reg.isValid()) {
                return load_source(arg, cached_reg);
            } else {
                return load_source(arg, TMP1);
            }
        }
    }

    auto load_sources(const ArgVal &Src1,
                      a32::Gp tmp1,
                      const ArgVal &Src2,
                      a32::Gp tmp2) {
        if (!isRegisterBacked(Src1) && !isRegisterBacked(Src2)) {
            switch (ArgVal::memory_relation(Src1, Src2)) {
            case ArgVal::Relation::consecutive:
                safe_ldp(tmp1, tmp2, Src1, Src2);
                return std::make_pair(Variable(tmp1, getArgRef(Src1)),
                                      Variable(tmp2, getArgRef(Src2)));
            case ArgVal::Relation::reverse_consecutive:
                safe_ldp(tmp2, tmp1, Src2, Src1);
                return std::make_pair(Variable(tmp1, getArgRef(Src1)),
                                      Variable(tmp2, getArgRef(Src2)));
            case ArgVal::Relation::none:
                break;
            }
        }

        return std::make_pair(load_source(Src1, tmp1), load_source(Src2, tmp2));
    }

    Variable<a32::VecD> load_source(const ArgVal &arg, a32::VecD tmp) {
        if (isRegisterBacked(arg)) {
            auto index = arg.as<ArgFRegister>().get();
            return Variable<a32::VecD>(register_backed_fregs[index]);
        }

        a.ldr(tmp, getArgRef(arg));
        return Variable<a32::VecD>(tmp);
    }

    void emit_load_args(const ArgSource &Src1,
                        a32::Gp src1_default,
                        const ArgSource &Src2,
                        a32::Gp src2_default,
                        const ArgSource &Src3,
                        a32::Gp src3_default) {

    }

    template<typename Reg>
    void mov_var(const Variable<Reg> &to, const Variable<Reg> &from) {

    }

    template<typename Reg>
    void mov_var(const Variable<Reg> &to, Reg from) {

    }

    template<typename Reg>
    void mov_var(Reg to, const Variable<Reg> &from) {

    }

    void flush_var(const Variable<a32::Gp> &to) {

    }

    void flush_var(const Variable<a32::VecD> &to) {

    }

    enum Relation { none, consecutive, reverse_consecutive };

    static Relation memory_relation(const arm::Mem &mem1,
                                    const arm::Mem &mem2) {

    }

    void flush_vars(const Variable<a32::Gp> &to1,
                    const Variable<a32::Gp> &to2) {

    }

    void flush_vars(const Variable<a32::Gp> &to1,
                    const Variable<a32::Gp> &to2,
                    const Variable<a32::Gp> &to3) {

    }

    void mov_arg(const ArgRegister &To, const ArgVal &From) {

    }

    void mov_arg(const ArgRegister &To, arm::Mem From) {

    }

    void mov_arg(arm::Mem To, const ArgVal &From) {

    }

    void mov_arg(a32::Gp to, const ArgVal &from) {

    }

    void mov_arg(const ArgVal &to, a32::Gp from) {

    }

    void cmp_arg(a32::Gp gp, const ArgVal &arg) {

    }

    void safe_str(a32::Gp gp, arm::Mem mem) {

    }

    void safe_stp(a32::Gp gp1,
                  a32::Gp gp2,

                  const ArgVal &Dst1,
                  const ArgVal &Dst2) {
        ASSERT(ArgVal::memory_relation(Dst1, Dst2) ==
               ArgVal::Relation::consecutive);
        safe_stp(gp1, gp2, getArgRef(Dst1));
    }

    void safe_stp(a32::Gp gp1, a32::Gp gp2, arm::Mem mem) {

    }

    void safe_ldr(a32::Gp gp, arm::Mem mem) {

    }

    void safe_ldur(a32::Gp gp, arm::Mem mem) {

    }

    void safe_ldp(a32::Gp gp1,
                  a32::Gp gp2,
                  const ArgVal &Src1,
                  const ArgVal &Src2) {

    }

    void safe_ldp(a32::Gp gp1, a32::Gp gp2, arm::Mem mem) {

    }

    /* Set the Z flag if Reg1 and Reg2 are definitely not equal based
     * on their tags alone. (They may still be equal if both are
     * immediates and all other bits are equal too.) */
    void emit_is_unequal_based_on_tags(Label Unequal,
                                       const ArgVal &Src1,
                                       a32::Gp Reg1,
                                       const ArgVal &Src2,
                                       a32::Gp Reg2) {

    }

    /* Set the Z flag if Reg1 and Reg2 are both immediates. */
    void emit_are_both_immediate(const ArgVal &Src1,
                                 a32::Gp Reg1,
                                 const ArgVal &Src2,
                                 a32::Gp Reg2) {

};

void *beamasm_metadata_insert(std::string module_name,
                              ErtsCodePtr base_address,
                              size_t code_size,
                              const std::vector<AsmRange> &ranges);
void beamasm_metadata_early_init();
void beamasm_metadata_late_init();
