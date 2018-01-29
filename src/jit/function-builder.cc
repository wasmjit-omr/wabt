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
#include "infra/Assert.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

namespace wabt {
namespace jit {

using ResultEnum = std::underlying_type<wabt::interp::Result>::type;

// The following functions are required to be able to properly parse opcodes. However, their
// original definitions are defined with static linkage in src/interp.cc. Because of this, the only
// way to use them is to simply copy their definitions here.

template <typename T>
inline T ReadUxAt(const uint8_t* pc) {
  T result;
  memcpy(&result, pc, sizeof(T));
  return result;
}

template <typename T>
inline T ReadUx(const uint8_t** pc) {
  T result = ReadUxAt<T>(*pc);
  *pc += sizeof(T);
  return result;
}

inline uint8_t ReadU8(const uint8_t** pc) {
  return ReadUx<uint8_t>(pc);
}

inline uint32_t ReadU32(const uint8_t** pc) {
  return ReadUx<uint32_t>(pc);
}

inline uint64_t ReadU64(const uint8_t** pc) {
  return ReadUx<uint64_t>(pc);
}

inline Opcode ReadOpcode(const uint8_t** pc) {
  uint8_t value = ReadU8(pc);
  if (Opcode::IsPrefixByte(value)) {
    // For now, assume all instructions are encoded with just one extra byte
    // so we don't have to decode LEB128 here.
    uint32_t code = ReadU8(pc);
    return Opcode::FromCode(value, code);
  } else {
    // TODO(binji): Optimize if needed; Opcode::FromCode does a log2(n) lookup
    // from the encoding.
    return Opcode::FromCode(value);
  }
}

inline Opcode ReadOpcodeAt(const uint8_t* pc) {
  return ReadOpcode(&pc);
}

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

  DefineFunction("f32_sqrt", __FILE__, "0",
                 reinterpret_cast<void*>(static_cast<float (*)(float)>(std::sqrt)),
                 Float,
                 1,
                 Float);
  DefineFunction("f32_copysign", __FILE__, "0",
                 reinterpret_cast<void*>(static_cast<float (*)(float, float)>(std::copysign)),
                 Float,
                 2,
                 Float,
                 Float);
}

bool FunctionBuilder::buildIL() {
  setVMState(new OMR::VirtualMachineState());

  const uint8_t* istream = thread_->GetIstream();

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(ReadOpcodeAt(&istream[offset_]).GetName())),
                          &istream[offset_]);
  AppendBuilder(workItems_[0].builder);

  int32_t next_index;

  while ((next_index = GetNextBytecodeFromWorklist()) != -1) {
    auto& work_item = workItems_[next_index];

    if (!Emit(work_item.builder, istream, work_item.pc))
      return false;
  }

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

  TR::IlBuilder* overflow_handler = nullptr;

  b->IfThen(&overflow_handler,
  b->       UnsignedGreaterOrEqualTo(
                stack_top,
  b->           Const(static_cast<int32_t>(thread_->value_stack_.size()))));
  overflow_handler->Return(
  overflow_handler->    Const(static_cast<ResultEnum>(interp::Result::TrapValueStackExhausted)));

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

/**
 * @brief Generate a drop-x from the interpreter stack, optionally keeping the top value
 *
 * The generated code should be equivalent to:
 *
 * auto stack_top = *stack_top_addr;
 * auto new_stack_top = stack_top - drop_count;
 *
 * if (keep_count == 1) {
 *   stack_base_addr[new_stack_top - 1] = stack_base_addr[stack_top - 1];
 * }
 *
 * *stack_top_addr = new_stack_top;
 */
void FunctionBuilder::DropKeep(TR::IlBuilder* b, uint32_t drop_count, uint8_t keep_count) {
  TR_ASSERT(keep_count <= 1, "Invalid keep count");

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* new_stack_top = b->Sub(stack_top, b->Const(static_cast<int32_t>(drop_count)));

  if (keep_count == 1) {
    auto* old_top_value = b->LoadAt(pValueType_,
                          b->       IndexAt(pValueType_,
                                            stack_base_addr,
                          b->               Sub(stack_top, b->Const(1))));

    b->StoreAt(
    b->        IndexAt(pValueType_,
                       stack_base_addr,
    b->                Sub(new_stack_top, b->Const(1))),
               old_top_value);
  }

  b->StoreAt(stack_top_addr, new_stack_top);
}

/**
 * @brief Generate load from the interpreter stack by an index
 *
 * The generate code should be equivalent to:
 *
 * return &value_stack_[value_stack_top_ - depth];
 */
TR::IlValue* FunctionBuilder::Pick(TR::IlBuilder* b, Index depth) {
  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* offset = b->Sub(
                 b->    LoadAt(pInt32, stack_top_addr),
                 b->    ConstInt32(depth));
  return b->IndexAt(pValueType_,
                    stack_base_addr,
                    offset);
}

template <>
const char* FunctionBuilder::TypeFieldName<int32_t>() const {
  return "i32";
}

template <>
const char* FunctionBuilder::TypeFieldName<uint32_t>() const {
  return "i32";
}

template <>
const char* FunctionBuilder::TypeFieldName<int64_t>() const {
  return "i64";
}

template <>
const char* FunctionBuilder::TypeFieldName<uint64_t>() const {
  return "i64";
}

template <>
const char* FunctionBuilder::TypeFieldName<float>() const {
  return "f32";
}

template <>
const char* FunctionBuilder::TypeFieldName<double>() const {
  return "f64";
}

template <typename T, typename TOpHandler>
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, TOpHandler h) {
  auto* rhs = Pop(b, TypeFieldName<T>());
  auto* lhs = Pop(b, TypeFieldName<T>());

  Push(b, TypeFieldName<T>(), h(lhs, rhs));
}

template <typename T, typename TOpHandler>
void FunctionBuilder::EmitUnaryOp(TR::IlBuilder* b, TOpHandler h) {
  Push(b, TypeFieldName<T>(), h(Pop(b, TypeFieldName<T>())));
}

template <typename T>
void FunctionBuilder::EmitIntDivide(TR::IlBuilder* b) {
  static_assert(std::is_integral<T>::value,
                "EmitIntDivide only works on integral types");

  EmitBinaryOp<T>(b, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
    TR::IlBuilder* div_zero_path = nullptr;

    b->IfThen(&div_zero_path, b->EqualTo(divisor, b->Const(static_cast<T>(0))));
    div_zero_path->Return(div_zero_path->Const(
        static_cast<ResultEnum>(interp::Result::TrapIntegerDivideByZero)));

    TR::IlBuilder* div_ovf_path = nullptr;

    b->IfThen(&div_ovf_path,
    b->       And(
    b->           EqualTo(dividend, b->Const(std::numeric_limits<T>::min())),
    b->           EqualTo(divisor, b->Const(static_cast<T>(-1)))));
    div_ovf_path->Return(div_ovf_path->Const(
        static_cast<ResultEnum>(interp::Result::TrapIntegerOverflow)));

    return b->Div(dividend, divisor);
  });
}

template <typename T>
void FunctionBuilder::EmitIntRemainder(TR::IlBuilder* b) {
  static_assert(std::is_integral<T>::value,
                "EmitIntRemainder only works on integral types");

  EmitBinaryOp<T>(b, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
    TR::IlBuilder* div_zero_path = nullptr;

    b->IfThen(&div_zero_path, b->EqualTo(divisor, b->Const(static_cast<T>(0))));
    div_zero_path->Return(div_zero_path->Const(
        static_cast<ResultEnum>(interp::Result::TrapIntegerDivideByZero)));

    TR::IlValue* return_value = b->Const(static_cast<T>(0));

    TR::IlBuilder* div_no_ovf_path = nullptr;
    b->IfThen(&div_no_ovf_path,
    b->       Or(
    b->           NotEqualTo(dividend, b->Const(std::numeric_limits<T>::min())),
    b->           NotEqualTo(divisor, b->Const(static_cast<T>(-1)))));
    div_no_ovf_path->StoreOver(return_value,
                               div_no_ovf_path->Rem(dividend, divisor));

    return return_value;
  });
}

template <typename T>
TR::IlValue* FunctionBuilder::CalculateShiftAmount(TR::IlBuilder* b, TR::IlValue* amount) {
  return b->And(amount, b->Const(static_cast<T>(sizeof(T) * 8 - 1)));
}

bool FunctionBuilder::Emit(TR::BytecodeBuilder* b,
                           const uint8_t* istream,
                           const uint8_t* pc) {
  Opcode opcode = ReadOpcode(&pc);
  TR_ASSERT(!opcode.IsInvalid(), "Invalid opcode");

  switch (opcode) {
    case Opcode::Select: {
      TR::IlBuilder* true_path = nullptr;
      TR::IlBuilder* false_path = nullptr;

      b->IfThenElse(&true_path, &false_path, Pop(b, "i32"));
      DropKeep(true_path, 1, 0);
      DropKeep(false_path, 1, 1);
      break;
    }

    case Opcode::Return:
      b->Return(b->Const(static_cast<ResultEnum>(interp::Result::Ok)));
      return true;

    case Opcode::Unreachable:
      b->Return(b->Const(static_cast<ResultEnum>(interp::Result::TrapUnreachable)));
      return true;

    case Opcode::I32Const:
      Push(b, "i32", b->ConstInt32(ReadU32(&pc)));
      break;

    case Opcode::I64Const:
      Push(b, "i64", b->ConstInt64(ReadU64(&pc)));
      break;

    case Opcode::F32Const:
      Push(b, "f32", b->ConstFloat(ReadUx<float>(&pc)));
      break;

    case Opcode::F64Const:
      Push(b, "f64", b->ConstDouble(ReadUx<double>(&pc)));
      break;

    case Opcode::GetLocal:
      // note: to work around JitBuilder's lack of support unions as value types,
      // just copy a field that's the size of the entire union
      Push(b, "i64", b->LoadIndirect("Value", "i64", Pick(b, ReadU32(&pc))));
      break;

    case Opcode::SetLocal: {
      // see note for GetLocal
      auto* value = Pop(b, "i64");
      auto* local_addr = Pick(b, ReadU32(&pc));
      b->StoreIndirect("Value", "i64", local_addr, value);
      break;
    }

    case Opcode::TeeLocal:
      // see note for GetLocal
      b->StoreIndirect("Value", "i64", Pick(b, ReadU32(&pc)), b->LoadIndirect("Value", "i64", Pick(b, 1)));
      break;

    case Opcode::I32Add:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::I32Sub:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I32Mul:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I32DivS:
      EmitIntDivide<int32_t>(b);
      break;

    case Opcode::I32RemS:
      EmitIntRemainder<int32_t>(b);
      break;

    case Opcode::I32And:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I32Or:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I32Xor:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I32Shl:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrS:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrU:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32Rotl:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I32Rotr:
      EmitBinaryOp<int32_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I64Add:
        EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
          return b->Add(lhs, rhs);
        });
        break;

    case Opcode::I64Sub:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I64Mul:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I64DivS:
      EmitIntDivide<int64_t>(b);
      break;

    case Opcode::I64RemS:
      EmitIntRemainder<int64_t>(b);
      break;

    case Opcode::F32Abs:
      EmitUnaryOp<float>(b, [&](TR::IlValue* value) {
        auto* return_value = b->Copy(value);

        TR::IlBuilder* zero_path = nullptr;
        TR::IlBuilder* nonzero_path = nullptr;
        TR::IlBuilder* neg_path = nullptr;

        // We have to check explicitly for 0.0, since abs(-0.0) is 0.0.
        b->IfThenElse(&zero_path, &nonzero_path, b->EqualTo(value, b->ConstFloat(0)));
        zero_path->StoreOver(return_value, zero_path->ConstFloat(0));

        nonzero_path->IfThen(&neg_path, nonzero_path->LessThan(value, nonzero_path->ConstFloat(0)));
        neg_path->StoreOver(return_value, neg_path->Mul(value, neg_path->ConstFloat(-1)));

        return return_value;
      });
      break;

    case Opcode::F32Neg:
      EmitUnaryOp<float>(b, [&](TR::IlValue* value) {
        return b->Mul(value, b->ConstFloat(-1));
      });
      break;

    case Opcode::F32Sqrt:
      EmitUnaryOp<float>(b, [&](TR::IlValue* value) {
        return b->Call("f32_sqrt", 1, value);
      });
      break;

    case Opcode::F32Add:
      EmitBinaryOp<float>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::F32Sub:
      EmitBinaryOp<float>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::F32Mul:
      EmitBinaryOp<float>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::F32Div:
      EmitBinaryOp<float>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Div(lhs, rhs);
      });
      break;

    case Opcode::F32Copysign:
      EmitBinaryOp<float>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Call("f32_copysign", 2, lhs, rhs);
      });
      break;

    case Opcode::InterpAlloca: {
      auto pInt32 = typeDictionary()->PointerTo(Int32);
      auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
      auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

      auto* old_value_stack_top = b->LoadAt(pInt32, stack_top_addr);
      auto* count = b->ConstInt32(ReadU32(&pc));
      auto* stack_top =  b->Add(old_value_stack_top, count);
      b->StoreAt(stack_top_addr, stack_top);

      TR::IlBuilder* overflow_handler = nullptr;

      b->IfThen(&overflow_handler,
      b->       UnsignedGreaterOrEqualTo(
                    stack_top,
      b->           Const(static_cast<int32_t>(thread_->value_stack_.size()))));
      overflow_handler->Return(
      overflow_handler->    Const(static_cast<ResultEnum>(interp::Result::TrapValueStackExhausted)));

      TR::IlBuilder* set_zero = nullptr;
      b->ForLoopUp("i", &set_zero, old_value_stack_top, stack_top, b->Const(1));
      set_zero->StoreIndirect("Value", "i64",
      set_zero->              IndexAt(pValueType_, stack_base_addr,
      set_zero->                      Load("i")),
      set_zero->              ConstInt64(0));

      break;
    }

    case Opcode::Drop:
      DropKeep(b, 1, 0);
      break;

    case Opcode::InterpDropKeep: {
      uint32_t drop_count = ReadU32(&pc);
      uint8_t keep_count = *pc++;
      DropKeep(b, drop_count, keep_count);
      break;
    }

    case Opcode::Nop:
      break;

    default:
      return false;
  }

  int32_t next_index = static_cast<int32_t>(workItems_.size());

  workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                const_cast<char*>(ReadOpcodeAt(pc).GetName())),
                          pc);
  b->AddFallThroughBuilder(workItems_[next_index].builder);
  return true;
}

}
}
