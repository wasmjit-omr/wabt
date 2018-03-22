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
#include "wabtjit.h"
#include "src/cast.h"
#include "src/interp.h"
#include "ilgen/VirtualMachineState.hpp"
#include "infra/Assert.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

namespace wabt {

namespace jit {

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

#define CHECK_TRAP_IN_HELPER(...)                \
  do {                                           \
    wabt::interp::Result result = (__VA_ARGS__); \
    if (result != wabt::interp::Result::Ok) {    \
      return static_cast<Result_t>(result);      \
    }                                            \
  } while (0)
#define TRAP(type) return static_cast<Result_t>(wabt::interp::Result::Trap##type)
#define TRAP_UNLESS(cond, type) TRAP_IF(!(cond), type)
#define TRAP_IF(cond, type)  \
  do {                       \
    if (WABT_UNLIKELY(cond)) \
      TRAP(type);            \
  } while (0)

FunctionBuilder::Result_t FunctionBuilder::CallHelper(wabt::interp::Thread* th, wabt::interp::IstreamOffset offset, uint8_t* current_pc) {
  // no need to check if JIT was enabled since we can only get here it was
  auto meta_it = th->env_->jit_meta_.find(offset);

  auto call_interp = [&]() {
    th->set_pc(offset);
    auto last_jit_frame = th->last_jit_frame_;
    th->last_jit_frame_ = th->call_stack_top_;
    wabt::interp::Result result = wabt::interp::Result::Ok;
    while (result == wabt::interp::Result::Ok) {
      result = th->Run(1000);
    }
    th->last_jit_frame_ = last_jit_frame;
    return result;
  };

  CHECK_TRAP_IN_HELPER(th->PushCall(current_pc));
  if (meta_it != th->env_->jit_meta_.end()) {
    auto meta = &meta_it->second;
    if (!meta->tried_jit) {
      meta->num_calls++;

      if (meta->num_calls >= th->env_->jit_threshold) {
        meta->jit_fn = jit::compile(th, meta->wasm_fn);
        meta->tried_jit = true;

        if (th->env_->trap_on_failed_comp && meta->jit_fn == nullptr)
          return static_cast<Result_t>(wabt::interp::Result::TrapFailedJITCompilation);
      }
    }

    if (meta->jit_fn) {
      CHECK_TRAP_IN_HELPER(meta->jit_fn());
    } else {
      auto result = call_interp();
      if (result != wabt::interp::Result::Returned)
        return static_cast<Result_t>(result);
    }
  } else {
    auto result = call_interp();
    if (result != wabt::interp::Result::Returned)
      return static_cast<Result_t>(result);
  }
  th->PopCall();

  return static_cast<Result_t>(wabt::interp::Result::Ok);
}

FunctionBuilder::Result_t FunctionBuilder::CallIndirectHelper(wabt::interp::Thread* th, Index table_index, Index sig_index, Index entry_index, uint8_t* current_pc) {
  using namespace wabt::interp;
  auto* env = th->env_;
  Table* table = &env->tables_[table_index];
  TRAP_IF(entry_index >= table->func_indexes.size(), UndefinedTableIndex);
  Index func_index = table->func_indexes[entry_index];
  TRAP_IF(func_index == kInvalidIndex, UninitializedTableElement);
  Func* func = env->funcs_[func_index].get();
  TRAP_UNLESS(env->FuncSignaturesAreEqual(func->sig_index, sig_index),
              IndirectCallSignatureMismatch);
  if (func->is_host) {
    th->CallHost(cast<HostFunc>(func));
  } else {
    auto result = CallHelper(th, cast<DefinedFunc>(func)->offset, current_pc);
    if (result != static_cast<Result_t>(interp::Result::Ok))
      return result;
  }
  return static_cast<Result_t>(interp::Result::Ok);
}

void FunctionBuilder::CallHostHelper(wabt::interp::Thread* th, Index func_index) {
  th->CallHost(cast<wabt::interp::HostFunc>(th->env_->funcs_[func_index].get()));
}

void* FunctionBuilder::MemoryTranslationHelper(interp::Thread* th, uint32_t memory_id, uint64_t address, uint32_t size) {
  auto* memory = &th->env_->memories_[memory_id];

  if (address + size > memory->data.size()) {
    return nullptr;
  } else {
    return memory->data.data() + address;
  }
}

FunctionBuilder::FunctionBuilder(interp::Thread* thread, interp::DefinedFunc* fn, TypeDictionary* types)
    : TR::MethodBuilder(types),
      thread_(thread),
      fn_(fn),
      valueType_(types->LookupUnion("Value")),
      pValueType_(types->PointerTo(types->LookupUnion("Value"))) {
  DefineLine(__LINE__);
  DefineFile(__FILE__);
  DefineName(fn->dbg_name_.c_str());

  DefineReturnType(types->toIlType<Result_t>());

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
  DefineFunction("f64_sqrt", __FILE__, "0",
                 reinterpret_cast<void*>(static_cast<double (*)(double)>(std::sqrt)),
                 Double,
                 1,
                 Double);
  DefineFunction("f64_copysign", __FILE__, "0",
                 reinterpret_cast<void*>(static_cast<double (*)(double, double)>(std::copysign)),
                 Double,
                 2,
                 Double,
                 Double);
  DefineFunction("CallHelper", __FILE__, "0",
                 reinterpret_cast<void*>(CallHelper),
                 types->toIlType<Result_t>(),
                 3,
                 types->toIlType<void*>(),
                 types->toIlType<wabt::interp::IstreamOffset>(),
                 types->PointerTo(Int8));
  DefineFunction("CallIndirectHelper", __FILE__, "0",
                 reinterpret_cast<void*>(CallIndirectHelper),
                 types->toIlType<Result_t>(),
                 5,
                 types->toIlType<void*>(),
                 types->toIlType<Index>(),
                 types->toIlType<Index>(),
                 types->toIlType<Index>(),
                 types->PointerTo(Int8));
  DefineFunction("CallHostHelper", __FILE__, "0",
                 reinterpret_cast<void*>(CallHostHelper),
                 NoType,
                 2,
                 types->toIlType<void*>(),
                 types->toIlType<Index>());
  DefineFunction("MemoryTranslationHelper", __FILE__, "0",
                 reinterpret_cast<void*>(MemoryTranslationHelper),
                 types->toIlType<void*>(),
                 4,
                 types->toIlType<void*>(),
                 types->toIlType<uint32_t>(),
                 types->toIlType<uint64_t>(),
                 types->toIlType<uint32_t>());
}

bool FunctionBuilder::buildIL() {
  setVMState(new OMR::VirtualMachineState());

  const uint8_t* istream = thread_->GetIstream();

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(ReadOpcodeAt(&istream[fn_->offset]).GetName())),
                          &istream[fn_->offset]);
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
void FunctionBuilder::Push(TR::IlBuilder* b, const char* type, TR::IlValue* value, const uint8_t* pc) {
  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);

  EmitTrapIf(b,
  b->        UnsignedGreaterOrEqualTo(
                 stack_top,
  b->            Const(static_cast<int32_t>(thread_->value_stack_.size()))),
  b->        Const(static_cast<Result_t>(interp::Result::TrapValueStackExhausted)),
             pc);

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

const char* FunctionBuilder::TypeFieldName(Type t) const {
  switch (t) {
    case Type::I32:
      return TypeFieldName<int32_t>();
    case Type::I64:
      return TypeFieldName<int64_t>();
    case Type::F32:
      return TypeFieldName<float>();
    case Type::F64:
      return TypeFieldName<double>();
    default:
      TR_ASSERT_FATAL(false, "Invalid primitive type");
      return nullptr;
  }
}

TR::IlValue* FunctionBuilder::Const(TR::IlBuilder* b, const interp::TypedValue* v) const {
  switch (v->type) {
    case Type::I32:
      return b->ConstInt32(v->value.i32);
    case Type::I64:
      return b->ConstInt64(v->value.i64);
    case Type::F32:
      return b->ConstFloat(Bitcast<float>(v->value.f32_bits));
    case Type::F64:
      return b->ConstDouble(Bitcast<double>(v->value.f64_bits));
    default:
      TR_ASSERT_FATAL(false, "Invalid primitive type");
      return nullptr;
  }
}

template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h) {
  auto* rhs = Pop(b, TypeFieldName<T>());
  auto* lhs = Pop(b, TypeFieldName<T>());

  Push(b, TypeFieldName<TResult>(), h(lhs, rhs), pc);
}

template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitUnaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h) {
  Push(b, TypeFieldName<TResult>(), h(Pop(b, TypeFieldName<T>())), pc);
}

template <typename T>
void FunctionBuilder::EmitIntDivide(TR::IlBuilder* b, const uint8_t* pc) {
  static_assert(std::is_integral<T>::value,
                "EmitIntDivide only works on integral types");

  EmitBinaryOp<T>(b, pc, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
    EmitTrapIf(b,
    b->        EqualTo(divisor, b->Const(static_cast<T>(0))),
    b->        Const(static_cast<Result_t>(interp::Result::TrapIntegerDivideByZero)),
               pc);

    EmitTrapIf(b,
    b->        And(
    b->            EqualTo(dividend, b->Const(std::numeric_limits<T>::min())),
    b->            EqualTo(divisor, b->Const(static_cast<T>(-1)))),
    b->        Const(static_cast<Result_t>(interp::Result::TrapIntegerOverflow)),
               pc);

    return b->Div(dividend, divisor);
  });
}

template <typename T>
void FunctionBuilder::EmitIntRemainder(TR::IlBuilder* b, const uint8_t* pc) {
  static_assert(std::is_integral<T>::value,
                "EmitIntRemainder only works on integral types");

  EmitBinaryOp<T>(b, pc, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
    EmitTrapIf(b,
    b->        EqualTo(divisor, b->Const(static_cast<T>(0))),
    b->        Const(static_cast<Result_t>(interp::Result::TrapIntegerDivideByZero)),
               pc);

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
TR::IlValue* FunctionBuilder::EmitMemoryPreAccess(TR::IlBuilder* b, const uint8_t** pc) {
  auto th_addr = b->ConstAddress(thread_);
  auto mem_id = b->ConstInt32(ReadU32(pc));
  auto offset = b->ConstInt64(static_cast<uint64_t>(ReadU32(pc)));

  auto address = b->Call("MemoryTranslationHelper",
                         4,
                         th_addr,
                         mem_id,
                         b->Add(b->UnsignedConvertTo(Int64, Pop(b, "i32")), offset),
                         b->ConstInt32(sizeof(T)));

  EmitTrapIf(b,
  b->        EqualTo(address, b->ConstAddress(nullptr)),
  b->        Const(static_cast<Result_t>(interp::Result::TrapMemoryAccessOutOfBounds)),
             *pc);

  return address;
}

void FunctionBuilder::EmitTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc) {
  if (pc != nullptr) {
    b->StoreAt(b->ConstAddress(&thread_->pc_),
               b->ConstInt32(pc - thread_->GetIstream()));
  }

  b->Return(result);
}

void FunctionBuilder::EmitCheckTrap(TR::IlBuilder* b, TR::IlValue* result, const uint8_t* pc) {
  TR::IlBuilder* trap_handler = nullptr;

  b->IfThen(&trap_handler,
  b->       NotEqualTo(result, b->Const(static_cast<Result_t>(interp::Result::Ok))));

  EmitTrap(trap_handler, result, pc);
}

void FunctionBuilder::EmitTrapIf(TR::IlBuilder* b, TR::IlValue* condition, TR::IlValue* result, const uint8_t* pc) {
  TR::IlBuilder* trap_handler = nullptr;

  b->IfThen(&trap_handler, condition);
  EmitTrap(trap_handler, result, pc);
}

template <>
TR::IlValue* FunctionBuilder::EmitIsNan<float>(TR::IlBuilder* b, TR::IlValue* value) {
  return b->GreaterThan(
         b->           And(
         b->               CoerceTo(Int32, value),
         b->               ConstInt32(0x7fffffffU)),
         b->           ConstInt32(0x7f800000U));
}

template <>
TR::IlValue* FunctionBuilder::EmitIsNan<double>(TR::IlBuilder* b, TR::IlValue* value) {
  return b->GreaterThan(
         b->           And(
         b->               CoerceTo(Int64, value),
         b->               ConstInt64(0x7fffffffffffffffULL)),
         b->           ConstInt64(0x7ff0000000000000ULL));
}

template <typename ToType, typename FromType>
void FunctionBuilder::EmitTruncation(TR::IlBuilder* b, const uint8_t* pc) {
  static_assert(std::is_floating_point<FromType>::value, "FromType in EmitTruncation call must be a floating point type");

  auto* value = Pop(b, TypeFieldName<FromType>());

  // TRAP_IF is NaN
  EmitTrapIf(b,
             EmitIsNan<FromType>(b, value),
  b->        Const(static_cast<Result_t>(interp::Result::TrapInvalidConversionToInteger)),
             pc);

  // TRAP_UNLESS conversion is in range
  EmitTrapIf(b,
  b->        Or(
  b->           LessThan(value,
  b->                    Const(static_cast<FromType>(std::numeric_limits<ToType>::lowest()))),
  b->           GreaterThan(value,
  b->                       Const(static_cast<FromType>(std::numeric_limits<ToType>::max())))),
  b->        Const(static_cast<Result_t>(interp::Result::TrapIntegerOverflow)),
             pc);

  auto* target_type = b->typeDictionary()->toIlType<ToType>();

  // this could be optimized using templates or constant expressions,
  // but the compiler should be able to simplify this anyways
  auto* new_value = std::is_unsigned<ToType>::value ? b->UnsignedConvertTo(target_type, value)
                                                    : b->ConvertTo(target_type, value);

  Push(b, TypeFieldName<ToType>(), new_value, pc);
}

/**
 * @brief Special case of EmitTruncation for unsigned integers as target type
 *
 * This function is designed to handle the case of truncating to an unsigned integer type.
 * When the target type is an unsigned integer type smaller than 64-bits, the floating-point
 * value can be safely truncated to a *signed* 64-bit integer and then converted to
 * the target type.
 */
template <typename ToType, typename FromType>
void FunctionBuilder::EmitUnsignedTruncation(TR::IlBuilder* b, const uint8_t* pc) {
  static_assert(std::is_floating_point<FromType>::value, "FromType in EmitTruncation call must be a floating point type");
  static_assert(std::is_integral<ToType>::value, "ToType in EmitUnsignedTruncation call must be an integer type");
  static_assert(std::is_unsigned<ToType>::value, "ToType in EmitUnsignedTruncation call must be unsigned");

  auto* value = Pop(b, TypeFieldName<FromType>());

  // TRAP_IF is NaN
  EmitTrapIf(b,
             EmitIsNan<FromType>(b, value),
  b->        Const(static_cast<Result_t>(interp::Result::TrapInvalidConversionToInteger)),
             pc);

  // TRAP_UNLESS conversion is in range
  EmitTrapIf(b,
  b->        Or(
  b->           LessThan(value,
  b->                    Const(static_cast<FromType>(std::numeric_limits<ToType>::lowest()))),
  b->           GreaterThan(value,
  b->                       Const(static_cast<FromType>(std::numeric_limits<ToType>::max())))),
  b->        Const(static_cast<Result_t>(interp::Result::TrapIntegerOverflow)),
             pc);

  auto* target_type = b->typeDictionary()->toIlType<ToType>();
  auto* new_value = b->UnsignedConvertTo(target_type, b->ConvertTo(Int64, value));

  Push(b, TypeFieldName<ToType>(), new_value, pc);
}

template <typename T>
TR::IlValue* FunctionBuilder::CalculateShiftAmount(TR::IlBuilder* b, TR::IlValue* amount) {
  return b->UnsignedConvertTo(Int32,
         b->                  And(amount, b->Const(static_cast<T>(sizeof(T) * 8 - 1))));
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

    case Opcode::Br: {
      auto target = &istream[ReadU32(&pc)];
      auto it = std::find_if(workItems_.cbegin(), workItems_.cend(), [&](const BytecodeWorkItem& b) {
        return target == b.pc;
      });
      if (it != workItems_.cend()) {
        b->AddFallThroughBuilder(it->builder);
      } else {
        int32_t next_index = static_cast<int32_t>(workItems_.size());
        workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                      const_cast<char*>(ReadOpcodeAt(target).GetName())),
                                target);
        b->AddFallThroughBuilder(workItems_[next_index].builder);
      }
      return true;
    }

    // case Opcode::BrIf: This opcode is never generated as it's always
    // transformed into a BrUnless. So, there's no need to handle it.

    case Opcode::Return:
      b->Return(b->Const(static_cast<Result_t>(interp::Result::Ok)));
      return true;

    case Opcode::Unreachable:
      EmitTrap(b, b->Const(static_cast<Result_t>(interp::Result::TrapUnreachable)), pc);
      return true;

    case Opcode::I32Const: {
      auto* val = b->ConstInt32(ReadU32(&pc));
      Push(b, "i32", val, pc);
      break;
    }

    case Opcode::I64Const: {
      auto* val = b->ConstInt64(ReadU64(&pc));
      Push(b, "i64", val, pc);
      break;
    }

    case Opcode::F32Const: {
      auto* val = b->ConstFloat(ReadUx<float>(&pc));
      Push(b, "f32", val, pc);
      break;
    }

    case Opcode::F64Const: {
      auto* val = b->ConstDouble(ReadUx<double>(&pc));
      Push(b, "f64", val, pc);
      break;
    }

    case Opcode::GetGlobal: {
      interp::Global* g = thread_->env()->GetGlobal(ReadU32(&pc));

      // The type of value stored in a global will never change, so we're safe
      // to use the current type of the global.
      const char* type_field = TypeFieldName(g->typed_value.type);

      if (g->mutable_) {
        // TODO(thomasbc): Can the address of a Global change at runtime?
        auto* addr = b->Const(&g->typed_value.value);
        Push(b, type_field, b->LoadIndirect("Value", type_field, addr), pc);
      } else {
        // With immutable globals, we can just substitute their actual value as
        // a constant at compile-time.
        Push(b, type_field, Const(b, &g->typed_value), pc);
      }

      break;
    }

    case Opcode::SetGlobal: {
      interp::Global* g = thread_->env()->GetGlobal(ReadU32(&pc));
      assert(g->mutable_);

      // See note for get_global
      const char* type_field = TypeFieldName(g->typed_value.type);

      // TODO(thomasbc): Can the address of a Global change at runtime?
      auto* addr = b->Const(&g->typed_value.value);

      b->StoreIndirect("Value", type_field, addr, Pop(b, type_field));
      break;
    }

    case Opcode::GetLocal: {
      // note: to work around JitBuilder's lack of support unions as value types,
      // just copy a field that's the size of the entire union
      auto* local_addr = Pick(b, ReadU32(&pc));
      Push(b, "i64", b->LoadIndirect("Value", "i64", local_addr), pc);
      break;
    }

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

    case Opcode::Call: {
      auto th_addr = b->ConstAddress(thread_);
      auto offset = b->ConstInt32(ReadU32(&pc));
      auto current_pc = b->Const(pc);

      b->Store("result",
      b->      Call("CallHelper", 3, th_addr, offset, current_pc));

      // Don't pass the pc since a trap in a called function should not update the thread's pc
      EmitCheckTrap(b, b->Load("result"), nullptr);

      break;
    }

    case Opcode::CallIndirect: {
      auto th_addr = b->ConstAddress(thread_);
      auto table_index = b->ConstInt32(ReadU32(&pc));
      auto sig_index = b->ConstInt32(ReadU32(&pc));
      auto entry_index = Pop(b, "i32");
      auto current_pc = b->Const(pc);

      b->Store("result",
      b->      Call("CallIndirectHelper", 5, th_addr, table_index, sig_index, entry_index, current_pc));

      // Don't pass the pc since a trap in a called function should not update the thread's pc
      EmitCheckTrap(b, b->Load("result"), nullptr);

      break;
    }

    case Opcode::InterpCallHost: {
      Index func_index = ReadU32(&pc);
      b->Call("CallHostHelper", 2,
      b->     ConstAddress(thread_),
      b->     ConstInt32(func_index));
      break;
    }

    case Opcode::I32Load8S: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc);
      Push(b,
           "i32",
      b->  ConvertTo(Int32,
      b->            LoadAt(typeDictionary()->PointerTo(Int8), addr)),
           pc);
      break;
    }

    case Opcode::I32Load8U: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc);
      Push(b,
           "i32",
      b->  UnsignedConvertTo(Int32,
      b->                    LoadAt(typeDictionary()->PointerTo(Int8), addr)),
           pc);
      break;
    }

    case Opcode::I32Load16S: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc);
      Push(b,
           "i32",
      b->  ConvertTo(Int32,
      b->            LoadAt(typeDictionary()->PointerTo(Int16), addr)),
           pc);
      break;
    }

    case Opcode::I32Load16U: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc);
      Push(b,
           "i32",
      b->  UnsignedConvertTo(Int32,
      b->                    LoadAt(typeDictionary()->PointerTo(Int16), addr)),
           pc);
      break;
    }

    case Opcode::I64Load8S: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc);
      Push(b,
           "i64",
      b->  ConvertTo(Int64,
      b->            LoadAt(typeDictionary()->PointerTo(Int8), addr)),
           pc);
      break;
    }

    case Opcode::I64Load8U: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc);
      Push(b,
           "i64",
      b->  UnsignedConvertTo(Int64,
      b->                    LoadAt(typeDictionary()->PointerTo(Int8), addr)),
           pc);
      break;
    }

    case Opcode::I64Load16S: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc);
      Push(b,
           "i64",
      b->  ConvertTo(Int64,
      b->            LoadAt(typeDictionary()->PointerTo(Int16), addr)),
           pc);
      break;
    }

    case Opcode::I64Load16U: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc);
      Push(b,
           "i64",
      b->  UnsignedConvertTo(Int64,
      b->                    LoadAt(typeDictionary()->PointerTo(Int16), addr)),
           pc);
      break;
    }

    case Opcode::I64Load32S: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc);
      Push(b,
           "i64",
      b->  ConvertTo(Int64,
      b->            LoadAt(typeDictionary()->PointerTo(Int32), addr)),
           pc);
      break;
    }

    case Opcode::I64Load32U: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc);
      Push(b,
           "i64",
      b->  UnsignedConvertTo(Int64,
      b->                    LoadAt(typeDictionary()->PointerTo(Int32), addr)),
           pc);
      break;
    }

    case Opcode::I32Load: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc);
      Push(b,
           "i32",
      b->  LoadAt(typeDictionary()->PointerTo(Int32), addr),
           pc);
      break;
    }

    case Opcode::I64Load: {
      auto* addr = EmitMemoryPreAccess<int64_t>(b, &pc);
      Push(b,
           "i64",
      b->  LoadAt(typeDictionary()->PointerTo(Int64), addr),
           pc);
      break;
    }

    case Opcode::F32Load: {
      auto* addr = EmitMemoryPreAccess<float>(b, &pc);
      Push(b,
           "f32",
      b->  LoadAt(typeDictionary()->PointerTo(Float), addr),
           pc);
      break;
    }

    case Opcode::F64Load: {
      auto* addr = EmitMemoryPreAccess<double>(b, &pc);
      Push(b,
           "f64",
      b->  LoadAt(typeDictionary()->PointerTo(Double), addr),
           pc);
      break;
    }

    case Opcode::I32Store8: {
      auto value = b->ConvertTo(Int8, Pop(b, "i32"));
      b->StoreAt(EmitMemoryPreAccess<int8_t>(b, &pc), value);
      break;
    }

    case Opcode::I32Store16: {
      auto value = b->ConvertTo(Int16, Pop(b, "i32"));
      b->StoreAt(EmitMemoryPreAccess<int16_t>(b, &pc), value);
      break;
    }

    case Opcode::I64Store8: {
      auto value = b->ConvertTo(Int8, Pop(b, "i64"));
      b->StoreAt(EmitMemoryPreAccess<int8_t>(b, &pc), value);
      break;
    }

    case Opcode::I64Store16: {
      auto value = b->ConvertTo(Int16, Pop(b, "i64"));
      b->StoreAt(EmitMemoryPreAccess<int16_t>(b, &pc), value);
      break;
    }

    case Opcode::I64Store32: {
      auto value = b->ConvertTo(Int32, Pop(b, "i64"));
      b->StoreAt(EmitMemoryPreAccess<int32_t>(b, &pc), value);
      break;
    }

    case Opcode::I32Store: {
      auto value = Pop(b, "i32");
      b->StoreAt(EmitMemoryPreAccess<int32_t>(b, &pc), value);
      break;
    }

    case Opcode::I64Store: {
      auto value = Pop(b, "i64");
      b->StoreAt(EmitMemoryPreAccess<int64_t>(b, &pc), value);
      break;
    }

    case Opcode::F32Store: {
      auto value = Pop(b, "f32");
      b->StoreAt(EmitMemoryPreAccess<float>(b, &pc), value);
      break;
    }

    case Opcode::F64Store: {
      auto value = Pop(b, "f64");
      b->StoreAt(EmitMemoryPreAccess<double>(b, &pc), value);
      break;
    }

    case Opcode::I32Add:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::I32Sub:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I32Mul:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I32DivS:
      EmitIntDivide<int32_t>(b, pc);
      break;

    case Opcode::I32RemS:
      EmitIntRemainder<int32_t>(b, pc);
      break;

    case Opcode::I32And:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I32Or:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I32Xor:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I32Shl:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrS:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrU:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32Rotl:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I32Rotr:
      EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I32Eqz:
      EmitUnaryOp<int32_t, int>(b, pc, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt32(0));
      });
      break;

    case Opcode::I32Eq:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32Ne:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LtS:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I32LtU:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtS:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtU:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32LeS:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LeU:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeS:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeU:
      EmitBinaryOp<int32_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64Add:
        EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
          return b->Add(lhs, rhs);
        });
        break;

    case Opcode::I64Sub:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I64Mul:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I64DivS:
      EmitIntDivide<int64_t>(b, pc);
      break;

    case Opcode::I64RemS:
      EmitIntRemainder<int64_t>(b, pc);
      break;

    case Opcode::I64And:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I64Or:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I64Xor:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I64Shl:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrS:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrU:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64Rotl:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Rotr:
      EmitBinaryOp<int64_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Eqz:
      EmitUnaryOp<int64_t, int>(b, pc, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt64(0));
      });
      break;

    case Opcode::I64Eq:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64Ne:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LtS:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I64LtU:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtS:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtU:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64LeS:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LeU:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeS:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeU:
      EmitBinaryOp<int64_t, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Abs:
      EmitUnaryOp<float>(b, pc, [&](TR::IlValue* value) {
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
      EmitUnaryOp<float>(b, pc, [&](TR::IlValue* value) {
        return b->Mul(value, b->ConstFloat(-1));
      });
      break;

    case Opcode::F32Sqrt:
      EmitUnaryOp<float>(b, pc, [&](TR::IlValue* value) {
        return b->Call("f32_sqrt", 1, value);
      });
      break;

    case Opcode::F32Add:
      EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::F32Sub:
      EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::F32Mul:
      EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::F32Div:
      EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Div(lhs, rhs);
      });
      break;

    case Opcode::F32Copysign:
      EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Call("f32_copysign", 2, lhs, rhs);
      });
      break;

    case Opcode::F32Eq:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Ne:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Lt:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F32Le:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Gt:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F32Ge:
      EmitBinaryOp<float, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Abs:
      EmitUnaryOp<double>(b, pc, [&](TR::IlValue* value) {
        auto* return_value = b->Copy(value);

        TR::IlBuilder* zero_path = nullptr;
        TR::IlBuilder* nonzero_path = nullptr;
        TR::IlBuilder* neg_path = nullptr;

        // We have to check explicitly for 0.0, since abs(-0.0) is 0.0.
        b->IfThenElse(&zero_path, &nonzero_path, b->EqualTo(value, b->ConstDouble(0)));
        zero_path->StoreOver(return_value, zero_path->ConstDouble(0));

        nonzero_path->IfThen(&neg_path, nonzero_path->LessThan(value, nonzero_path->ConstDouble(0)));
        neg_path->StoreOver(return_value, neg_path->Mul(value, neg_path->ConstDouble(-1)));

        return return_value;
      });
      break;

    case Opcode::F64Neg:
      EmitUnaryOp<double>(b, pc, [&](TR::IlValue* value) {
        return b->Mul(value, b->ConstDouble(-1));
      });
      break;

    case Opcode::F64Sqrt:
      EmitUnaryOp<double>(b, pc, [&](TR::IlValue* value) {
        return b->Call("f64_sqrt", 1, value);
      });
      break;

    case Opcode::F64Add:
      EmitBinaryOp<double>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::F64Sub:
      EmitBinaryOp<double>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::F64Mul:
      EmitBinaryOp<double>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::F64Div:
      EmitBinaryOp<double>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Div(lhs, rhs);
      });
      break;

    case Opcode::F64Copysign:
      EmitBinaryOp<double>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Call("f64_copysign", 2, lhs, rhs);
      });
      break;

    case Opcode::F64Eq:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Ne:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Lt:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F64Le:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Gt:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F64Ge:
      EmitBinaryOp<double, int>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32WrapI64: {
      auto* value = Pop(b, "i64");
      Push(b, "i32",
      b->  ConvertTo(Int32, value),
           pc);
      break;
    }

    case Opcode::I64ExtendSI32: {
      auto* value = Pop(b, "i32");
      Push(b, "i64",
      b->  ConvertTo(Int64, value),
           pc);
      break;
    }

    case Opcode::I64ExtendUI32: {
      auto* value = Pop(b, "i32");
      Push(b, "i64",
      b->  UnsignedConvertTo(Int64, value),
           pc);
      break;
    }

    case Opcode::F32DemoteF64: {
      auto* value = Pop(b, "f64");
      Push(b, "f32",
      b->  ConvertTo(Float, value),
           pc);
      break;
    }

    case Opcode::F64PromoteF32: {
      auto* value = Pop(b, "f32");
      Push(b, "f64",
      b->  ConvertTo(Double, value),
           pc);
      break;
    }

    case Opcode::I32Extend8S: {
      auto* value = b->ConvertTo(Int32, b->ConvertTo(Int8, Pop(b, "i32")));
      Push(b, "i32", value, pc);
      break;
    }

    case Opcode::I32Extend16S: {
      auto* value = b->ConvertTo(Int32, b->ConvertTo(Int16, Pop(b, "i32")));
      Push(b, "i32", value, pc);
      break;
    }

    case Opcode::I64Extend8S: {
      auto* value = b->ConvertTo(Int32, b->ConvertTo(Int8, Pop(b, "i32")));
      Push(b, "i32", value, pc);
      break;
    }

    case Opcode::I64Extend16S: {
      auto* value = b->ConvertTo(Int32, b->ConvertTo(Int16, Pop(b, "i32")));
      Push(b, "i32", value, pc);
      break;
    }

    case Opcode::I64Extend32S: {
      auto* value = b->ConvertTo(Int64, b->ConvertTo(Int32, Pop(b, "i64")));
      Push(b, "i64", value, pc);
      break;
    }

    case Opcode::F32ConvertSI32: {
      auto* value = b->ConvertTo(Float, Pop(b, "i32"));
      Push(b, "f32", value, pc);
      break;
    }

    case Opcode::F32ConvertUI32: {
      auto* value = b->UnsignedConvertTo(Float, Pop(b, "i32"));
      Push(b, "f32", value, pc);
      break;
    }

    case Opcode::F32ConvertSI64: {
      auto* value = b->ConvertTo(Float, Pop(b, "i64"));
      Push(b, "f32", value, pc);
      break;
    }

    case Opcode::F32ConvertUI64: {
      auto* value = b->UnsignedConvertTo(Float, Pop(b, "i64"));
      Push(b, "f32", value, pc);
      break;
    }

    case Opcode::F64ConvertSI32: {
      auto* value = b->ConvertTo(Double, Pop(b, "i32"));
      Push(b, "f64", value, pc);
      break;
    }

    case Opcode::F64ConvertUI32: {
      auto* value = b->UnsignedConvertTo(Double, Pop(b, "i32"));
      Push(b, "f64", value, pc);
      break;
    }

    case Opcode::F64ConvertSI64: {
      auto* value = b->ConvertTo(Double, Pop(b, "i64"));
      Push(b, "f64", value, pc);
      break;
    }

    case Opcode::F64ConvertUI64: {
      auto* value = b->UnsignedConvertTo(Double, Pop(b, "i64"));
      Push(b, "f64", value, pc);
      break;
    }

    case Opcode::F32ReinterpretI32: {
      auto* value = b->CoerceTo(Float, Pop(b, "i32"));
      Push(b, "f32", value, pc);
      break;
    }

    case Opcode::I32ReinterpretF32: {
      auto* value = b->CoerceTo(Int32, Pop(b, "f32"));
      Push(b, "i32", value, pc);
      break;
    }

    case Opcode::F64ReinterpretI64: {
      auto* value = b->CoerceTo(Double, Pop(b, "i64"));
      Push(b, "f64", value, pc);
      break;
    }

    case Opcode::I64ReinterpretF64: {
      auto* value = b->CoerceTo(Int64, Pop(b, "f64"));
      Push(b, "i64", value, pc);
      break;
    }

    case Opcode::I32TruncSF32:
      EmitTruncation<int32_t, float>(b, pc);
      break;

    case Opcode::I32TruncUF32:
      EmitUnsignedTruncation<uint32_t, float>(b, pc);
      break;

    case Opcode::I32TruncSF64:
      EmitTruncation<int32_t, double>(b, pc);
      break;

    case Opcode::I32TruncUF64:
      EmitUnsignedTruncation<uint32_t, double>(b, pc);
      break;

    case Opcode::I64TruncSF32:
      EmitTruncation<int64_t, float>(b, pc);
      break;

//    UNSIGNED TYPE NOT HANDLED
//    case Opcode::I64TruncUF32:
//      EmitTruncation<uint64_t, float>(b, pc);
//      break;

    case Opcode::I64TruncSF64:
      EmitTruncation<int64_t, double>(b, pc);
      break;

//    UNSIGNED TYPE NOT HANDLED
//    case Opcode::I64TruncUF64:
//      EmitTruncation<uint64_t, double>(b, pc);
//      break;

    case Opcode::InterpAlloca: {
      auto pInt32 = typeDictionary()->PointerTo(Int32);
      auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
      auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

      auto* old_value_stack_top = b->LoadAt(pInt32, stack_top_addr);
      auto* count = b->ConstInt32(ReadU32(&pc));
      auto* stack_top =  b->Add(old_value_stack_top, count);
      b->StoreAt(stack_top_addr, stack_top);

      EmitTrapIf(b,
      b->        UnsignedGreaterOrEqualTo(
                     stack_top,
      b->            Const(static_cast<int32_t>(thread_->value_stack_.size()))),
      b->        Const(static_cast<Result_t>(interp::Result::TrapValueStackExhausted)),
                 pc);

      TR::IlBuilder* set_zero = nullptr;
      b->ForLoopUp("i", &set_zero, old_value_stack_top, stack_top, b->Const(1));
      set_zero->StoreIndirect("Value", "i64",
      set_zero->              IndexAt(pValueType_, stack_base_addr,
      set_zero->                      Load("i")),
      set_zero->              ConstInt64(0));

      break;
    }

    case Opcode::InterpBrUnless: {
      auto target = &istream[ReadU32(&pc)];
      auto condition = Pop(b, "i32");
      auto it = std::find_if(workItems_.begin(), workItems_.end(), [&](const BytecodeWorkItem& b) {
        return target == b.pc;
      });
      if (it != workItems_.end()) {
        b->IfCmpEqualZero(&it->builder, condition);
      } else {
        int32_t next_index = static_cast<int32_t>(workItems_.size());
        workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                      const_cast<char*>(ReadOpcodeAt(target).GetName())),
                                target);
        b->IfCmpEqualZero(&workItems_[next_index].builder, condition);
      }
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
