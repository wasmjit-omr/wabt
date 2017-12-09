/*
 * Copyright 2017 wasmjit-omr project participants
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
 */

#include "wabtjit.h"
#include "type-dictionary.h"
#include "function-builder.h"

#include "Jit.hpp"

wabt::jit::JITedFunction wabt::jit::compile(wabt::interp::Thread* thread, wabt::interp::IstreamOffset offset) {
  TypeDictionary types;
  FunctionBuilder builder{thread, offset, &types};
  uint8_t* function = nullptr;

  if (compileMethodBuilder(&builder, &function) == 0) {
    return reinterpret_cast<JITedFunction>(function);
  } else {
    return nullptr;
  }
}
