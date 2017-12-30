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

#include "function-builder.h"
#include "src/interp.h"
#include "ilgen/VirtualMachineState.hpp"
#include <type_traits>

namespace wabt {
namespace jit {

FunctionBuilder::FunctionBuilder(interp::Thread* thread, interp::IstreamOffset const offset, TypeDictionary* types)
    : TR::MethodBuilder(types),
      thread_(thread),
      offset_(offset),
      valueType_(types->LookupUnion("Value")),
      pValueType_(types->PointerTo(types->LookupUnion("Value"))) {
  DefineLine(__LINE__);
  DefineFile(__FILE__);
  DefineName("WASM_Function");

  DefineReturnType(types->toIlType<std::underlying_type<wabt::interp::Result>::type>());
}

bool FunctionBuilder::buildIL() {
  using ValueEnum = std::underlying_type<wabt::interp::Result>::type;
  setVMState(new OMR::VirtualMachineState{});

  // generate pop
  Pop(this, "i32");

  // generate push 3
  Push(this, "i32", Const(3));

  // generate return to the interpreter
  Return(Const(static_cast<ValueEnum>(interp::Result::Ok)));

  // return from IL gen signalling success
  return true;
}

/**
 * @brief Generate push to the interpreter stack
 *
 * The generated code should be equivalent to:
 *
 * auto stack_top = *stack_top_addr;
 * stack_base_addr[stack_top] = value;
 * *stack_top_addr = stack_top + 1;
 */
void FunctionBuilder::Push(TR::IlBuilder* b, const char* type, TR::IlValue* value) {
  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);
  b->StoreIndirect("Value", type,
  b->              IndexAt(pValueType_,
                           stack_base_addr,
                           stack_top),
                   value);
  b->StoreAt(stack_top_addr,
  b->        Add(
                 stack_top,
  b->            Const(1)));
}

/**
 * @brief Generate pop from the interpreter stack
 *
 * The generated code should be equivalent to:
 *
 * auto new_stack_top = *stack_top_addr - 1;
 * *stack_top_addr = new_stack_top;
 * return stack_base_addr[new_stack_top];
 */
TR::IlValue* FunctionBuilder::Pop(TR::IlBuilder* b, const char* type) {
  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* new_stack_top = b->Sub(
                        b->    LoadAt(pInt32, stack_top_addr),
                        b->    Const(1));
  b->StoreAt(stack_top_addr, new_stack_top);
  return b->LoadIndirect("Value", type,
         b->             IndexAt(pValueType_,
                                 stack_base_addr,
                                 new_stack_top));
}

}
}
