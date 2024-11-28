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
 * Guard BIF calls using the generic bif1, bif2, and bif3 instructions
 * are expensive. Not only are there two indirect calls (one to the
 * fragment, one to the BIF itself), but the caller-saved X registers
 * must also be saved and restored, and the BIF operands that are
 * usually in CPU registers must be written out to memory.
 *
 * Therefore, guard BIFs that are used fairly frequently and can
 * be implemented entirely in assembly language without any calls to
 * C function are implemented in this source file.
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
#include "erl_map.h"
}

using namespace asmjit;
