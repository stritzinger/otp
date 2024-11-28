/*
 * %CopyrightBegin%
 *
 * Copyright Ericsson AB 2021-2023. All Rights Reserved.
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


/* Calls to functions that are being purged (but haven't finished) land here.
 *
 *
 * */
void BeamGlobalAssembler::emit_unloaded_fun() {
    // TODO
}


/* Handles save_calls for local funs, which is a side-effect of our calling
 * convention.
 *
 * When the active code index is ERTS_SAVE_CALLS_CODE_IX, all local fun calls
 * will land here. */
void BeamGlobalAssembler::emit_dispatch_save_calls_fun() {
   
   // TODO
}
