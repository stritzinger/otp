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

/*
 * Notes.
 *
 * The immediate operand for the and, orr, eor, and tst instructions
 * follow special rules.
 *
 * For our purposes, only bit patterns consisting of 1 through 63 ones
 * at any position in a word are possible to encode as an
 * immediate. Other patterns must be loaded into a tempoary register.
 *
 * Here are some examples of possible immediate values:
 *
 *    0b00000011
 *    0b00001111
 *    0b00111100
 *
 *    0xFFFFFFFFFFFFFFF0
 *    0x100000000000000F
 *
 * The last one is possible because it is the pattern 0x1F
 * (0b00011111) rotated right one position.
 *
 * Here is an example of mask that is not a possible to encode as an
 * immediate:
 *
 *    0b111011
 *
 * For more about the encoding rules, see:
 *
 * https://stackoverflow.com/questions/30904718/range-of-immediate-values-in-armv8-a64-assembly
 *
 */

#include <algorithm>
#include <numeric>
#include "beam_asm.hpp"

extern "C"
{
#include "erl_bif_table.h"
#include "big.h"
#include "beam_catches.h"
#include "beam_common.h"
#include "code_ix.h"
#include "erl_binary.h"
}

using namespace asmjit;


void BeamModuleAssembler::emit_coverage(void *coverage, Uint index, Uint size) {
    ASSERT(0);
}
