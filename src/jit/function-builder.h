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

#ifndef FUNCTIONBUILDER_HPP
#define FUNCTIONBUILDER_HPP

#include "type-dictionary.h"
#include "stack.h"
#include "ilgen/BytecodeBuilder.hpp"
#include "ilgen/MethodBuilder.hpp"
#include "ilgen/VirtualMachineState.hpp"

#include "src/interp/interp.h"

#include <type_traits>

#define CHECK_TRAP_HELPER(...)                \
  do {                                           \
    wabt::interp::Result result = (__VA_ARGS__); \
    if (result != wabt::interp::Result::Ok) {    \
      return static_cast<Result_t>(result);      \
    }                                            \
  } while (0)
#define TRAP_HELPER(type) return static_cast<Result_t>(wabt::interp::Result::Trap##type)
#define TRAP_UNLESS_HELPER(cond, type) TRAP_IF_HELPER(!(cond), type)
#define TRAP_IF_HELPER(cond, type)  \
  do {                       \
    if (WABT_UNLIKELY(cond)) \
      TRAP_HELPER(type);            \
  } while (0)

namespace wabt {
namespace jit {

// Workaround for broken JitBuilder callbacks. See vms_hack.cc for more details.
void initializeVmsHack(TR::VirtualMachineState* vms);

class WabtState : public TR::VirtualMachineState {
public:
  VirtualStack stack;

  WabtState() { initializeVmsHack(this); }

  VirtualMachineState* MakeCopy() override {
    return new WabtState(*this);
  }

  void MergeInto(TR::VirtualMachineState* other, TR::IlBuilder* b) override {
    this->stack.MergeInto(&dynamic_cast<WabtState&>(*other).stack, b);
  }
};

class FunctionBuilder : public TR::MethodBuilder {
 public:
  FunctionBuilder(interp::Thread* thread, interp::DefinedFunc* fn, TypeDictionary* types);
  bool buildIL() override;

 private:
  struct BytecodeWorkItem {
    TR::BytecodeBuilder* builder;
    const uint8_t* pc;

    BytecodeWorkItem(TR::BytecodeBuilder* builder, const uint8_t* pc)
      : builder(builder), pc(pc) {}
  };

  void SetUpLocals(TR::IlBuilder* b, const uint8_t** pc, VirtualStack* stack);
  void TearDownLocals(TR::IlBuilder* b);
  uint32_t GetLocalOffset(VirtualStack* stack, Type* type, uint32_t depth);

  void MoveToPhysStack(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, uint32_t depth);
  void MoveFromPhysStack(TR::IlBuilder* b, VirtualStack* stack, const std::vector<Type>& types);
  TR::IlValue* PickPhys(TR::IlBuilder* b, uint32_t depth);

  template <typename T>
  const char* TypeFieldName() const;
  const char* TypeFieldName(Type t) const;
  const char* TypeFieldName(TR::DataType dt) const;

  TR::IlValue* Const(TR::IlBuilder* b, const interp::TypedValue* v) const;

  template <typename T, typename TResult = T, typename TOpHandler>
  void EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, TOpHandler h);

  template <typename T, typename TResult = T, typename TOpHandler>
  void EmitUnaryOp(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, TOpHandler h);

  template <typename T>
  void EmitIntDivide(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack);

  template <typename T>
  void EmitIntRemainder(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack);

  template <typename T>
  TR::IlValue* EmitMemoryPreAccess(TR::IlBuilder* b, const uint8_t** pc, VirtualStack* stack);

  void EmitTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc);
  void EmitCheckTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc);
  void EmitTrapIf(TR::IlBuilder* b, TR::IlValue* condition, TR::IlValue* result, const uint8_t* pc);

  template <typename F>
  TR::IlValue* EmitIsNan(TR::IlBuilder* b, TR::IlValue* value);

  template <typename ToType, typename FromType>
  void EmitTruncation(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack);
  template <typename ToType, typename FromType>
  void EmitUnsignedTruncation(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack);

  template <typename ToType, typename FromType>
  void EmitSaturatingTruncation(TR::IlBuilder* b, VirtualStack* stack);
  template <typename ToType, typename FromType>
  void EmitUnsignedSaturatingTruncation(TR::IlBuilder* b, VirtualStack* stack);

  template <typename>
  TR::IlValue* CalculateShiftAmount(TR::IlBuilder* b, TR::IlValue* amount);

  static Result_t CallIndirectHelper(ThreadInfo* th, Index table_index, Index sig_index, Index entry_index);

  static void* MemoryTranslationHelper(interp::Thread* th, uint32_t memory_id, uint64_t address, uint32_t size);

  std::vector<BytecodeWorkItem> workItems_;

  interp::Thread* thread_;
  interp::DefinedFunc* fn_;

  TR::IlType* const valueType_;
  TR::IlType* const pValueType_;

  bool Emit(TR::BytecodeBuilder* b, const uint8_t* istream, const uint8_t* pc, VirtualStack& stack);
};

}
}

#endif // FUNCTIONBUILDER_HPP
