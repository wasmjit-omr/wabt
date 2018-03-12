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
  DefineName("WASM_Function");

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
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, TOpHandler h) {
  auto* rhs = Pop(b, TypeFieldName<T>());
  auto* lhs = Pop(b, TypeFieldName<T>());

  Push(b, TypeFieldName<TResult>(), h(lhs, rhs));
}

template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitUnaryOp(TR::IlBuilder* b, TOpHandler h) {
  Push(b, TypeFieldName<TResult>(), h(Pop(b, TypeFieldName<T>())));
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

  TR::IlBuilder* trap_handler = nullptr;

  b->IfThen(&trap_handler,
  b->       EqualTo(address, b->ConstAddress(nullptr)));

  trap_handler->Return(
  trap_handler->       Const(static_cast<Result_t>(interp::Result::TrapMemoryAccessOutOfBounds)));

  return address;
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

    case Opcode::GetGlobal: {
      interp::Global* g = thread_->env()->GetGlobal(ReadU32(&pc));

      // The type of value stored in a global will never change, so we're safe
      // to use the current type of the global.
      const char* type_field = TypeFieldName(g->typed_value.type);

      if (g->mutable_) {
        // TODO(thomasbc): Can the address of a Global change at runtime?
        auto* addr = b->Const(&g->typed_value.value);
        Push(b, type_field, b->LoadIndirect("Value", type_field, addr));
      } else {
        // With immutable globals, we can just substitute their actual value as
        // a constant at compile-time.
        Push(b, type_field, Const(b, &g->typed_value));
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

    case Opcode::Call: {
      auto th_addr = b->ConstAddress(thread_);
      auto offset = b->ConstInt32(ReadU32(&pc));
      auto current_pc = b->Const(pc);

      b->Store("result",
      b->      Call("CallHelper", 3, th_addr, offset, current_pc));

      TR::IlBuilder* trap_handler = nullptr;

      b->IfThen(&trap_handler,
      b->       NotEqualTo(
      b->                  Load("result"),
      b->                  Const(static_cast<Result_t>(wabt::interp::Result::Ok))));

      trap_handler->Return(
      trap_handler->       Load("result"));

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

      TR::IlBuilder* trap_handler = nullptr;

      b->IfThen(&trap_handler,
      b->       NotEqualTo(
      b->                  Load("result"),
      b->                  Const(static_cast<Result_t>(wabt::interp::Result::Ok))));

      trap_handler->Return(
      trap_handler->       Load("result"));

      break;
    }

    case Opcode::InterpCallHost: {
      Index func_index = ReadU32(&pc);
      b->Call("CallHostHelper", 2,
      b->     ConstAddress(thread_),
      b->     ConstInt32(func_index));
      break;
    }

    case Opcode::I32Load:
      Push(b,
           "i32",
      b->  LoadAt(typeDictionary()->PointerTo(Int32), EmitMemoryPreAccess<int32_t>(b, &pc)));
      break;

    case Opcode::I64Load:
      Push(b,
           "i64",
      b->  LoadAt(typeDictionary()->PointerTo(Int64), EmitMemoryPreAccess<int64_t>(b, &pc)));
      break;

    case Opcode::F32Load:
      Push(b,
           "f32",
      b->  LoadAt(typeDictionary()->PointerTo(Float), EmitMemoryPreAccess<float>(b, &pc)));
      break;

    case Opcode::F64Load:
      Push(b,
           "f64",
      b->  LoadAt(typeDictionary()->PointerTo(Double), EmitMemoryPreAccess<double>(b, &pc)));
      break;

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

    case Opcode::I32Eqz:
      EmitUnaryOp<int32_t, int>(b, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt32(0));
      });
      break;

    case Opcode::I32Eq:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32Ne:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LtS:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I32LtU:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtS:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtU:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32LeS:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LeU:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeS:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeU:
      EmitBinaryOp<int32_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
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

    case Opcode::I64And:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I64Or:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I64Xor:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I64Shl:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrS:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrU:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64Rotl:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Rotr:
      EmitBinaryOp<int64_t>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Eqz:
      EmitUnaryOp<int64_t, int>(b, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt64(0));
      });
      break;

    case Opcode::I64Eq:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64Ne:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LtS:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I64LtU:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtS:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtU:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64LeS:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LeU:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeS:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeU:
      EmitBinaryOp<int64_t, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
      });
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

    case Opcode::F32Eq:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Ne:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Lt:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F32Le:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Gt:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F32Ge:
      EmitBinaryOp<float, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Eq:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Ne:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Lt:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F64Le:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Gt:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F64Ge:
      EmitBinaryOp<double, int>(b, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
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
