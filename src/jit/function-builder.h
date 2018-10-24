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
#include "ilgen/BytecodeBuilder.hpp"
#include "ilgen/MethodBuilder.hpp"
#include "ilgen/VirtualMachineOperandStack.hpp"

#include "src/interp.h"

#include <type_traits>

namespace wabt {
namespace jit {

class FunctionBuilder : public TR::MethodBuilder {
 public:
  FunctionBuilder(interp::Thread* thread, interp::DefinedFunc* fn, TypeDictionary* types);
  bool buildIL() override;

  /**
   * @brief Generate push to the interpreter stack
   * @param b is the builder object used to generate the code
   * @param type is the name of the field in the Value union corresponding to the type of the value being pushed
   * @param value is the IlValue representing the value being pushed
   * @param pc is a pointer the instruction performing the push
   *
   * If the push results in a stack overflow as trap condition is generated for the instruction pointed to by `pc`.
   */
  void Push(TR::IlBuilder* b, const char* type, TR::IlValue* value, const uint8_t* pc);

  /**
   * @brief Overload of `Push()` for 32-bit integer values
   * @param b is the builder object used to generate the code
   * @param value is the IlValue representing the value being pushed
   * @param pc is a pointer the instruction performing the push
   */
  void PushI32(TR::IlBuilder* b, TR::IlValue* value, const uint8_t* pc);

  /**
   * @brief Generate pop from the interpreter stack
   * @param b is the builder object used to generate the code
   * @param type is the name of the field in the Value union corresponding to the type of the value being popped
   * @return an IlValue representing the popped value
   */
  TR::IlValue* Pop(TR::IlBuilder* b, const char* type);

  /**
   * @brief Overload of `Pop()` for 32-bit integer values
   * @param b is the builder object used to generate the code
   * @return an IlValue representing the popped value
   */
  TR::IlValue* PopI32(TR::IlBuilder* b);

  /**
   * @brief Drop a number of values from the interpreter stack, optionally keeping the top value of the stack
   * @param b is the builder object used to generate the code
   * @param drop_count is the number of values to drop from the stack
   * @param keep_count is 1 to keep the top value intact and 0 otherwise
   */
  void DropKeep(TR::IlBuilder* b, uint32_t drop_count, uint8_t keep_count);

  /**
   * @brief Generate load of pointer to a vlue on the interpreter stack by an index
   * @param b is the builder object used to generate the code
   * @param depth is the index from the top of the stack
   * @return and IlValue representing a pointer to the value on the stack
   *
   * JitBuilder does not currently represent unions as value types. This a problem
   * for this function because it cannot simply return an IlValue representing
   * the union. As workaround, it will generate a load of the *base address* of
   * the union, instead of loading the union directly. This behaviour differs
   * from `Thread::Pick()` and users must take this into account.
   */
  TR::IlValue* Pick(TR::IlBuilder* b, Index depth);

 private:
  struct BytecodeWorkItem {
    TR::BytecodeBuilder* builder;
    const uint8_t* pc;

    BytecodeWorkItem(TR::BytecodeBuilder* builder, const uint8_t* pc)
      : builder(builder), pc(pc) {}
  };

  template <typename T>
  const char* TypeFieldName() const;

  const char* TypeFieldName(Type t) const;

  TR::IlValue* Const(TR::IlBuilder* b, const interp::TypedValue* v) const;

  template <typename T, typename TResult = T, typename TOpHandler>
  void EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h);

  template <typename T, typename TResult = T, typename TOpHandler>
  void EmitUnaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h);

  template <typename T>
  void EmitIntDivide(TR::IlBuilder* b, const uint8_t* pc);

  template <typename T>
  void EmitIntRemainder(TR::IlBuilder* b, const uint8_t* pc);

  template <typename T>
  TR::IlValue* EmitMemoryPreAccess(TR::IlBuilder* b, const uint8_t** pc);

  void EmitTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc);
  void EmitCheckTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc);
  void EmitTrapIf(TR::IlBuilder* b, TR::IlValue* condition, TR::IlValue* result, const uint8_t* pc);

  template <typename F>
  TR::IlValue* EmitIsNan(TR::IlBuilder* b, TR::IlValue* value);

  template <typename ToType, typename FromType>
  void EmitTruncation(TR::IlBuilder* b, const uint8_t* pc);
  template <typename ToType, typename FromType>
  void EmitUnsignedTruncation(TR::IlBuilder* b, const uint8_t* pc);

  template <typename>
  TR::IlValue* CalculateShiftAmount(TR::IlBuilder* b, TR::IlValue* amount);

  using Result_t = std::underlying_type<wabt::interp::Result>::type;

  static Result_t CallHelper(wabt::interp::Thread* th, wabt::interp::IstreamOffset offset, uint8_t* current_pc);

  static Result_t CallIndirectHelper(wabt::interp::Thread* th, Index table_index, Index sig_index, Index entry_index, uint8_t* current_pc);

  static Result_t CallHostHelper(wabt::interp::Thread* th, Index func_index);

  static void* MemoryTranslationHelper(interp::Thread* th, uint32_t memory_id, uint64_t address, uint32_t size);

  std::vector<BytecodeWorkItem> workItems_;

  interp::Thread* thread_;
  interp::DefinedFunc* fn_;

  TR::IlType* const valueType_;
  TR::IlType* const pValueType_;

  bool Emit(TR::BytecodeBuilder* b, const uint8_t* istream, const uint8_t* pc);
};

}
}

#endif // FUNCTIONBUILDER_HPP
