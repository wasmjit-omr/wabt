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
#include "src/interp/interp.h"
#include "src/interp/interp-internal.h"
#include "ilgen/VirtualMachineState.hpp"
#include "infra/Assert.hpp"

#include <cmath>
#include <limits>
#include <type_traits>

namespace wabt {

namespace jit {

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
    auto result = static_cast<Result_t>(th->CallHost(cast<HostFunc>(func)));
    if (result != static_cast<Result_t>(interp::Result::Ok))
      return result;
  } else {
    auto result = CallHelper(th, cast<DefinedFunc>(func)->offset, current_pc);
    if (result != static_cast<Result_t>(interp::Result::Ok))
      return result;
  }
  return static_cast<Result_t>(interp::Result::Ok);
}

FunctionBuilder::Result_t FunctionBuilder::CallHostHelper(wabt::interp::Thread* th, Index func_index) {
  return static_cast<Result_t>(th->CallHost(cast<wabt::interp::HostFunc>(th->env_->funcs_[func_index].get())));
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

  DefineReturnType(toIlType<Result_t>(types));

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
                 toIlType<Result_t>(types),
                 3,
                 toIlType<void*>(types),
                 toIlType<wabt::interp::IstreamOffset>(types),
                 types->PointerTo(Int8));
  DefineFunction("CallIndirectHelper", __FILE__, "0",
                 reinterpret_cast<void*>(CallIndirectHelper),
                 toIlType<Result_t>(types),
                 5,
                 toIlType<void*>(types),
                 toIlType<Index>(types),
                 toIlType<Index>(types),
                 toIlType<Index>(types),
                 types->PointerTo(Int8));
  DefineFunction("CallHostHelper", __FILE__, "0",
                 reinterpret_cast<void*>(CallHostHelper),
                 toIlType<Result_t>(types),
                 2,
                 toIlType<void*>(types),
                 toIlType<Index>(types));
  DefineFunction("MemoryTranslationHelper", __FILE__, "0",
                 reinterpret_cast<void*>(MemoryTranslationHelper),
                 toIlType<void*>(types),
                 4,
                 toIlType<void*>(types),
                 toIlType<uint32_t>(types),
                 toIlType<uint64_t>(types),
                 toIlType<uint32_t>(types));
}

bool FunctionBuilder::buildIL() {
  const uint8_t* istream = thread_->GetIstream();
  const uint8_t* pc = &istream[fn_->offset];
  auto* state = new WabtState();

  SetUpLocals(this, &pc, &state->stack);
  setVMState(state);

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(interp::ReadOpcodeAt(pc).GetName())),
                          pc);
  AppendBuilder(workItems_[0].builder);

  int32_t next_index;

  while ((next_index = GetNextBytecodeFromWorklist()) != -1) {
    auto& work_item = workItems_[next_index];
    state = static_cast<WabtState*>(work_item.builder->vmState());

    if (!Emit(work_item.builder, istream, work_item.pc, state->stack))
      return false;
  }

  return true;
}

void FunctionBuilder::SetUpLocals(TR::IlBuilder* b, const uint8_t** pc, VirtualStack* stack) {
  // Add placeholders onto the virtual stack. These values should never be actually read, but will
  // eventually be dropped using drop or drop_keep in the function epilogue.
  for (size_t i = 0; i < fn_->param_and_local_types.size(); i++) {
    stack->Push(nullptr);
  }

  if (fn_->local_count == 0) return;

  Opcode opcode = interp::ReadOpcode(pc);
  TR_ASSERT_FATAL(opcode == Opcode::InterpAlloca, "Function with locals is missing alloca");

  auto alloc_num = interp::ReadU32(pc);
  TR_ASSERT_FATAL(alloc_num == fn_->local_count,
                  "Function has wrong alloca size (%u != %u)",
                  alloc_num, fn_->local_count);

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* old_value_stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* count = b->ConstInt32(fn_->local_count);
  auto* stack_top = b->Add(old_value_stack_top, count);
  b->StoreAt(stack_top_addr, stack_top);

  TR::IlBuilder* overflow_handler = nullptr;

  b->IfThen(&overflow_handler,
  b->       UnsignedGreaterOrEqualTo(
                stack_top,
  b->           Const(static_cast<int32_t>(thread_->value_stack_.size()))));
  overflow_handler->Return(
  overflow_handler->    Const(static_cast<Result_t>(interp::Result::TrapValueStackExhausted)));

  TR::IlBuilder* set_zero = nullptr;
  b->ForLoopUp("i", &set_zero, old_value_stack_top, stack_top, b->Const(1));
  set_zero->StoreIndirect("Value", "i64",
  set_zero->              IndexAt(pValueType_, stack_base_addr,
  set_zero->                      Load("i")),
  set_zero->              ConstInt64(0));
}

void FunctionBuilder::TearDownLocals(TR::IlBuilder* b) {
  if (fn_->param_and_local_types.size() == 0) return;

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);

  auto* old_stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* count = b->ConstInt32(fn_->param_and_local_types.size());

  b->StoreAt(stack_top_addr, b->Sub(old_stack_top, count));
}

uint32_t FunctionBuilder::GetLocalOffset(VirtualStack* stack, Type* type, uint32_t depth) {
  uint32_t i = stack->Depth() - depth;
  TR_ASSERT_FATAL(i < fn_->param_and_local_types.size(), "Attempt to access invalid local 0x%x", i);

  if (type != nullptr) {
    *type = fn_->param_and_local_types[i];
  }

  return fn_->param_and_local_types.size() - i;
}

void FunctionBuilder::MoveToPhysStack(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, uint32_t depth) {
  if (depth == 0)
    return;

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* new_stack_top = b->Add(stack_top, b->ConstInt32(depth));

  EmitTrapIf(b,
  b->        Or(
  b->            UnsignedGreaterThan(
                     new_stack_top,
  b->                Const(static_cast<int32_t>(thread_->value_stack_.size()))),
  b->            UnsignedLessThan(
                     new_stack_top,
                     stack_top)),
  b->        Const(static_cast<Result_t>(interp::Result::TrapValueStackExhausted)),
             pc);

  for (size_t i = 0; i < depth; i++) {
    auto* val = stack->Pick(depth - i - 1);

    b->StoreIndirect("Value", TypeFieldName(val->getDataType()),
    b->              IndexAt(pValueType_,
                             stack_base_addr,
    b->                      Add(stack_top, b->ConstInt32(i))),
                     val);
  }

  stack->DropKeep(depth, 0);

  b->StoreAt(stack_top_addr, new_stack_top);
}

void FunctionBuilder::MoveFromPhysStack(TR::IlBuilder* b, VirtualStack* stack, const std::vector<Type>& types) {
  if (types.size() == 0)
    return;

  auto pInt32 = typeDictionary()->PointerTo(Int32);
  auto* stack_top_addr = b->ConstAddress(&thread_->value_stack_top_);
  auto* stack_base_addr = b->ConstAddress(thread_->value_stack_.data());

  auto* stack_top = b->LoadAt(pInt32, stack_top_addr);
  auto* new_stack_top = b->Sub(stack_top, b->ConstInt32(types.size()));

  for (size_t i = 0; i < types.size(); i++) {
    auto* val = b->LoadIndirect("Value", TypeFieldName(types[i]),
    b->                         IndexAt(pValueType_,
                                        stack_base_addr,
    b->                                 Add(new_stack_top, b->ConstInt32(i))));

    stack->Push(val);
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
TR::IlValue* FunctionBuilder::PickPhys(TR::IlBuilder* b, uint32_t depth) {
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

const char* FunctionBuilder::TypeFieldName(TR::DataType dt) const {
  switch (dt.getDataType()) {
    case TR::Int32:
      return TypeFieldName<int32_t>();
    case TR::Int64:
      return TypeFieldName<int64_t>();
    case TR::Float:
      return TypeFieldName<float>();
    case TR::Double:
      return TypeFieldName<double>();
    default:
      TR_ASSERT_FATAL(false, "Invalid primitive type %s", dt.toString());
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
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, TOpHandler h) {
  auto* rhs = stack->Pop();
  auto* lhs = stack->Pop();

  stack->Push(h(lhs, rhs));
}

template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitUnaryOp(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack, TOpHandler h) {
  stack->Push(h(stack->Pop()));
}

template <typename T>
void FunctionBuilder::EmitIntDivide(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack) {
  static_assert(std::is_integral<T>::value,
                "EmitIntDivide only works on integral types");

  EmitBinaryOp<T>(b, pc, stack, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
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
void FunctionBuilder::EmitIntRemainder(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack) {
  static_assert(std::is_integral<T>::value,
                "EmitIntRemainder only works on integral types");

  EmitBinaryOp<T>(b, pc, stack, [&](TR::IlValue* dividend, TR::IlValue* divisor) {
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
TR::IlValue* FunctionBuilder::EmitMemoryPreAccess(TR::IlBuilder* b, const uint8_t** pc, VirtualStack* stack) {
  auto th_addr = b->ConstAddress(thread_);
  auto mem_id = b->ConstInt32(interp::ReadU32(pc));
  auto offset = b->ConstInt64(static_cast<uint64_t>(interp::ReadU32(pc)));

  auto address = b->Call("MemoryTranslationHelper",
                         4,
                         th_addr,
                         mem_id,
                         b->Add(b->UnsignedConvertTo(Int64, stack->Pop()), offset),
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
         b->               ConvertBitsTo(Int32, value),
         b->               ConstInt32(0x7fffffffU)),
         b->           ConstInt32(0x7f800000U));
}

template <>
TR::IlValue* FunctionBuilder::EmitIsNan<double>(TR::IlBuilder* b, TR::IlValue* value) {
  return b->GreaterThan(
         b->           And(
         b->               ConvertBitsTo(Int64, value),
         b->               ConstInt64(0x7fffffffffffffffULL)),
         b->           ConstInt64(0x7ff0000000000000ULL));
}

template <typename ToType, typename FromType>
void FunctionBuilder::EmitTruncation(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack) {
  static_assert(std::is_floating_point<FromType>::value, "FromType in EmitTruncation call must be a floating point type");

  auto* value = stack->Pop();

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

  auto* target_type = toIlType<ToType>(b->typeDictionary());

  // this could be optimized using templates or constant expressions,
  // but the compiler should be able to simplify this anyways
  auto* new_value = std::is_unsigned<ToType>::value ? b->UnsignedConvertTo(target_type, value)
                                                    : b->ConvertTo(target_type, value);

  stack->Push(new_value);
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
void FunctionBuilder::EmitUnsignedTruncation(TR::IlBuilder* b, const uint8_t* pc, VirtualStack* stack) {
  static_assert(std::is_floating_point<FromType>::value, "FromType in EmitTruncation call must be a floating point type");
  static_assert(std::is_integral<ToType>::value, "ToType in EmitUnsignedTruncation call must be an integer type");
  static_assert(std::is_unsigned<ToType>::value, "ToType in EmitUnsignedTruncation call must be unsigned");

  auto* value = stack->Pop();

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

  auto* target_type = toIlType<ToType>(b->typeDictionary());
  auto* new_value = b->UnsignedConvertTo(target_type, b->ConvertTo(Int64, value));

  stack->Push(new_value);
}

template <typename T>
TR::IlValue* FunctionBuilder::CalculateShiftAmount(TR::IlBuilder* b, TR::IlValue* amount) {
  return b->UnsignedConvertTo(Int32,
         b->                  And(amount, b->Const(static_cast<T>(sizeof(T) * 8 - 1))));
}

bool FunctionBuilder::Emit(TR::BytecodeBuilder* b,
                           const uint8_t* istream,
                           const uint8_t* pc,
                           VirtualStack& stack) {
  Opcode opcode = interp::ReadOpcode(&pc);
  TR_ASSERT(!opcode.IsInvalid(), "Invalid opcode");

  switch (opcode) {
    case Opcode::Select: {
      auto* sel = stack.Pop();
      auto* false_value = stack.Pop();
      auto* true_value = stack.Pop();

      TR::IlBuilder* true_path = nullptr;

      b->IfThen(&true_path, sel);
      true_path->StoreOver(false_value, true_value);

      stack.Push(false_value);
      break;
    }

    case Opcode::Br: {
      auto target = &istream[interp::ReadU32(&pc)];
      auto it = std::find_if(workItems_.cbegin(), workItems_.cend(), [&](const BytecodeWorkItem& b) {
        return target == b.pc;
      });
      if (it != workItems_.cend()) {
        b->AddFallThroughBuilder(it->builder);
      } else {
        int32_t next_index = static_cast<int32_t>(workItems_.size());
        workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                      const_cast<char*>(interp::ReadOpcodeAt(target).GetName())),
                                target);
        b->AddFallThroughBuilder(workItems_[next_index].builder);
      }
      return true;
    }

    // case Opcode::BrIf: This opcode is never generated as it's always
    // transformed into a BrUnless. So, there's no need to handle it.

    case Opcode::Return:
      TearDownLocals(b);
      MoveToPhysStack(b, pc, &stack, stack.Depth());
      b->Return(b->Const(static_cast<Result_t>(interp::Result::Ok)));
      return true;

    case Opcode::Unreachable:
      EmitTrap(b, b->Const(static_cast<Result_t>(interp::Result::TrapUnreachable)), pc);
      return true;

    case Opcode::I32Const: {
      stack.Push(b->ConstInt32(interp::ReadU32(&pc)));
      break;
    }

    case Opcode::I64Const: {
      stack.Push(b->ConstInt64(interp::ReadU64(&pc)));
      break;
    }

    case Opcode::F32Const: {
      stack.Push(b->ConstFloat(interp::ReadUx<float>(&pc)));
      break;
    }

    case Opcode::F64Const: {
      stack.Push(b->ConstDouble(interp::ReadUx<double>(&pc)));
      break;
    }

    case Opcode::GlobalGet: {
      interp::Global* g = thread_->env()->GetGlobal(interp::ReadU32(&pc));

      // The type of value stored in a global will never change, so we're safe
      // to use the current type of the global.
      const char* type_field = TypeFieldName(g->typed_value.type);

      if (g->mutable_) {
        // TODO(thomasbc): Can the address of a Global change at runtime?
        auto* addr = b->Const(&g->typed_value.value);
        stack.Push(b->LoadIndirect("Value", type_field, addr));
      } else {
        // With immutable globals, we can just substitute their actual value as
        // a constant at compile-time.
        stack.Push(Const(b, &g->typed_value));
      }

      break;
    }

    case Opcode::GlobalSet: {
      interp::Global* g = thread_->env()->GetGlobal(interp::ReadU32(&pc));
      assert(g->mutable_);

      // See note for get_global
      const char* type_field = TypeFieldName(g->typed_value.type);

      // TODO(thomasbc): Can the address of a Global change at runtime?
      auto* addr = b->Const(&g->typed_value.value);

      b->StoreIndirect("Value", type_field, addr, stack.Pop());
      break;
    }

    case Opcode::LocalGet: {
      Type t;
      uint32_t off = GetLocalOffset(&stack, &t, interp::ReadU32(&pc));

      if (t == Type::V128)
        return false;

      stack.Push(b->LoadIndirect("Value", TypeFieldName(t), PickPhys(b, off)));
      break;
    }

    case Opcode::LocalSet: {
      auto* value = stack.Pop();
      uint32_t off = GetLocalOffset(&stack, nullptr, interp::ReadU32(&pc));

      b->StoreIndirect("Value", TypeFieldName(value->getDataType()), PickPhys(b, off), value);
      break;
    }

    case Opcode::LocalTee: {
      auto* value = stack.Top();
      uint32_t off = GetLocalOffset(&stack, nullptr, interp::ReadU32(&pc));

      b->StoreIndirect("Value", TypeFieldName(value->getDataType()), PickPhys(b, off), value);
      break;
    }

    case Opcode::Call: {
      auto th_addr = b->ConstAddress(thread_);
      auto offset = interp::ReadU32(&pc);
      auto current_pc = b->Const(pc);

      auto meta_it = thread_->env()->jit_meta_.find(offset);
      TR_ASSERT_FATAL(meta_it != thread_->env()->jit_meta_.end(),
                      "Found call to unknown function at offset %u", offset);

      auto* meta = &meta_it->second;
      auto* sig = thread_->env()->GetFuncSignature(meta->wasm_fn->sig_index);

      MoveToPhysStack(b, pc, &stack, sig->param_types.size());

      b->Store("result",
      b->      Call("CallHelper", 3, th_addr, b->ConstInt32(offset), current_pc));

      // Don't pass the pc since a trap in a called function should not update the thread's pc
      EmitCheckTrap(b, b->Load("result"), nullptr);

      for (Type t : sig->result_types) {
        if (t == Type::V128)
          return false;
      }

      MoveFromPhysStack(b, &stack, sig->result_types);

      break;
    }

    case Opcode::CallIndirect: {
      auto th_addr = b->ConstAddress(thread_);
      auto table_index = b->ConstInt32(interp::ReadU32(&pc));
      auto sig_index = interp::ReadU32(&pc);
      auto entry_index = stack.Pop();
      auto current_pc = b->Const(pc);

      auto* sig = thread_->env()->GetFuncSignature(sig_index);

      MoveToPhysStack(b, pc, &stack, sig->param_types.size());

      b->Store("result",
      b->      Call("CallIndirectHelper", 5, th_addr, table_index, b->ConstInt32(sig_index), entry_index, current_pc));

      // Don't pass the pc since a trap in a called function should not update the thread's pc
      EmitCheckTrap(b, b->Load("result"), nullptr);

      for (Type t : sig->result_types) {
        if (t == Type::V128)
          return false;
      }

      MoveFromPhysStack(b, &stack, sig->result_types);

      break;
    }

    case Opcode::InterpCallHost: {
      Index func_index = interp::ReadU32(&pc);
      auto* sig = thread_->env()->GetFuncSignature(thread_->env()->GetFunc(func_index)->sig_index);

      MoveToPhysStack(b, pc, &stack, sig->param_types.size());

      b->Store("result",
      b->      Call("CallHostHelper", 2,
      b->           ConstAddress(thread_),
      b->           ConstInt32(func_index)));

      EmitCheckTrap(b, b->Load("result"), pc);

      for (Type t : sig->result_types) {
        if (t == Type::V128)
          return false;
      }

      MoveFromPhysStack(b, &stack, sig->result_types);

      break;
    }

    case Opcode::I32Load8S: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc, &stack);
      stack.Push(
      b-> ConvertTo(Int32,
      b->           LoadAt(typeDictionary()->PointerTo(Int8), addr)));
      break;
    }

    case Opcode::I32Load8U: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc, &stack);
      stack.Push(
      b-> UnsignedConvertTo(Int32,
      b->                   LoadAt(typeDictionary()->PointerTo(Int8), addr)));
      break;
    }

    case Opcode::I32Load16S: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc, &stack);
      stack.Push(
      b-> ConvertTo(Int32,
      b->           LoadAt(typeDictionary()->PointerTo(Int16), addr)));
      break;
    }

    case Opcode::I32Load16U: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc, &stack);
      stack.Push(
      b-> UnsignedConvertTo(Int32,
      b->                   LoadAt(typeDictionary()->PointerTo(Int16), addr)));
      break;
    }

    case Opcode::I64Load8S: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc, &stack);
      stack.Push(
      b-> ConvertTo(Int64,
      b->           LoadAt(typeDictionary()->PointerTo(Int8), addr)));
      break;
    }

    case Opcode::I64Load8U: {
      auto* addr = EmitMemoryPreAccess<int8_t>(b, &pc, &stack);
      stack.Push(
      b-> UnsignedConvertTo(Int64,
      b->                   LoadAt(typeDictionary()->PointerTo(Int8), addr)));
      break;
    }

    case Opcode::I64Load16S: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc, &stack);
      stack.Push(
      b-> ConvertTo(Int64,
      b->           LoadAt(typeDictionary()->PointerTo(Int16), addr)));
      break;
    }

    case Opcode::I64Load16U: {
      auto* addr = EmitMemoryPreAccess<int16_t>(b, &pc, &stack);
      stack.Push(
      b-> UnsignedConvertTo(Int64,
      b->                   LoadAt(typeDictionary()->PointerTo(Int16), addr)));
      break;
    }

    case Opcode::I64Load32S: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc, &stack);
      stack.Push(
      b-> ConvertTo(Int64,
      b->           LoadAt(typeDictionary()->PointerTo(Int32), addr)));
      break;
    }

    case Opcode::I64Load32U: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc, &stack);
      stack.Push(
      b-> UnsignedConvertTo(Int64,
      b->                   LoadAt(typeDictionary()->PointerTo(Int32), addr)));
      break;
    }

    case Opcode::I32Load: {
      auto* addr = EmitMemoryPreAccess<int32_t>(b, &pc, &stack);
      stack.Push(b->LoadAt(typeDictionary()->PointerTo(Int32), addr));
      break;
    }

    case Opcode::I64Load: {
      auto* addr = EmitMemoryPreAccess<int64_t>(b, &pc, &stack);
      stack.Push(b->LoadAt(typeDictionary()->PointerTo(Int64), addr));
      break;
    }

    case Opcode::F32Load: {
      auto* addr = EmitMemoryPreAccess<float>(b, &pc, &stack);
      stack.Push(b->LoadAt(typeDictionary()->PointerTo(Float), addr));
      break;
    }

    case Opcode::F64Load: {
      auto* addr = EmitMemoryPreAccess<double>(b, &pc, &stack);
      stack.Push(b->LoadAt(typeDictionary()->PointerTo(Double), addr));
      break;
    }

    case Opcode::I32Store8: {
      auto value = b->ConvertTo(Int8, stack.Pop());
      b->StoreAt(EmitMemoryPreAccess<int8_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I32Store16: {
      auto value = b->ConvertTo(Int16, stack.Pop());
      b->StoreAt(EmitMemoryPreAccess<int16_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I64Store8: {
      auto value = b->ConvertTo(Int8, stack.Pop());
      b->StoreAt(EmitMemoryPreAccess<int8_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I64Store16: {
      auto value = b->ConvertTo(Int16, stack.Pop());
      b->StoreAt(EmitMemoryPreAccess<int16_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I64Store32: {
      auto value = b->ConvertTo(Int32, stack.Pop());
      b->StoreAt(EmitMemoryPreAccess<int32_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I32Store: {
      auto value = stack.Pop();
      b->StoreAt(EmitMemoryPreAccess<int32_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I64Store: {
      auto value = stack.Pop();
      b->StoreAt(EmitMemoryPreAccess<int64_t>(b, &pc, &stack), value);
      break;
    }

    case Opcode::F32Store: {
      auto value = stack.Pop();
      b->StoreAt(EmitMemoryPreAccess<float>(b, &pc, &stack), value);
      break;
    }

    case Opcode::F64Store: {
      auto value = stack.Pop();
      b->StoreAt(EmitMemoryPreAccess<double>(b, &pc, &stack), value);
      break;
    }

    case Opcode::I32Add:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::I32Sub:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I32Mul:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I32DivS:
      EmitIntDivide<int32_t>(b, pc, &stack);
      break;

    case Opcode::I32RemS:
      EmitIntRemainder<int32_t>(b, pc, &stack);
      break;

    case Opcode::I32And:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I32Or:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I32Xor:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I32Shl:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrS:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32ShrU:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int32_t>(b, rhs));
      });
      break;

    case Opcode::I32Rotl:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I32Rotr:
      EmitBinaryOp<int32_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int32_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(32), amount)));
      });
      break;

    case Opcode::I32Eqz:
      EmitUnaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt32(0));
      });
      break;

    case Opcode::I32Eq:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32Ne:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LtS:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I32LtU:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtS:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32GtU:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I32LeS:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32LeU:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeS:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32GeU:
      EmitBinaryOp<int32_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64Add:
        EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
          return b->Add(lhs, rhs);
        });
        break;

    case Opcode::I64Sub:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::I64Mul:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::I64DivS:
      EmitIntDivide<int64_t>(b, pc, &stack);
      break;

    case Opcode::I64RemS:
      EmitIntRemainder<int64_t>(b, pc, &stack);
      break;

    case Opcode::I64And:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->And(lhs, rhs);
      });
      break;

    case Opcode::I64Or:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Or(lhs, rhs);
      });
      break;

    case Opcode::I64Xor:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Xor(lhs, rhs);
      });
      break;

    case Opcode::I64Shl:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftL(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrS:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->ShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64ShrU:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedShiftR(lhs, CalculateShiftAmount<int64_t>(b, rhs));
      });
      break;

    case Opcode::I64Rotl:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          ShiftL(lhs, amount),
        b->          UnsignedShiftR(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Rotr:
      EmitBinaryOp<int64_t>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        auto* amount = CalculateShiftAmount<int64_t>(b, rhs);

        return b->Or(
        b->          UnsignedShiftR(lhs, amount),
        b->          ShiftL(lhs, b->Sub(b->ConstInt32(64), amount)));
      });
      break;

    case Opcode::I64Eqz:
      EmitUnaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* val) {
        return b->EqualTo(val, b->ConstInt64(0));
      });
      break;

    case Opcode::I64Eq:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64Ne:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LtS:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::I64LtU:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtS:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64GtU:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterThan(lhs, rhs);
      });
      break;

    case Opcode::I64LeS:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64LeU:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedLessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeS:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I64GeU:
      EmitBinaryOp<int64_t, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->UnsignedGreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Abs:
      EmitUnaryOp<float>(b, pc, &stack, [&](TR::IlValue* value) {
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
      EmitUnaryOp<float>(b, pc, &stack, [&](TR::IlValue* value) {
        return b->Mul(value, b->ConstFloat(-1));
      });
      break;

    case Opcode::F32Sqrt:
      EmitUnaryOp<float>(b, pc, &stack, [&](TR::IlValue* value) {
        return b->Call("f32_sqrt", 1, value);
      });
      break;

    case Opcode::F32Add:
      EmitBinaryOp<float>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::F32Sub:
      EmitBinaryOp<float>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::F32Mul:
      EmitBinaryOp<float>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::F32Div:
      EmitBinaryOp<float>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Div(lhs, rhs);
      });
      break;

    case Opcode::F32Copysign:
      EmitBinaryOp<float>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Call("f32_copysign", 2, lhs, rhs);
      });
      break;

    case Opcode::F32Eq:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Ne:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Lt:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F32Le:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F32Gt:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F32Ge:
      EmitBinaryOp<float, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Abs:
      EmitUnaryOp<double>(b, pc, &stack, [&](TR::IlValue* value) {
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
      EmitUnaryOp<double>(b, pc, &stack, [&](TR::IlValue* value) {
        return b->Mul(value, b->ConstDouble(-1));
      });
      break;

    case Opcode::F64Sqrt:
      EmitUnaryOp<double>(b, pc, &stack, [&](TR::IlValue* value) {
        return b->Call("f64_sqrt", 1, value);
      });
      break;

    case Opcode::F64Add:
      EmitBinaryOp<double>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Add(lhs, rhs);
      });
      break;

    case Opcode::F64Sub:
      EmitBinaryOp<double>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Sub(lhs, rhs);
      });
      break;

    case Opcode::F64Mul:
      EmitBinaryOp<double>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Mul(lhs, rhs);
      });
      break;

    case Opcode::F64Div:
      EmitBinaryOp<double>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Div(lhs, rhs);
      });
      break;

    case Opcode::F64Copysign:
      EmitBinaryOp<double>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->Call("f64_copysign", 2, lhs, rhs);
      });
      break;

    case Opcode::F64Eq:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->EqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Ne:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->NotEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Lt:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessThan(lhs, rhs);
      });
      break;

    case Opcode::F64Le:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->LessOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::F64Gt:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterThan(lhs, rhs);
      });
      break;

    case Opcode::F64Ge:
      EmitBinaryOp<double, int>(b, pc, &stack, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
        return b->GreaterOrEqualTo(lhs, rhs);
      });
      break;

    case Opcode::I32WrapI64: {
      stack.Push(b->ConvertTo(Int32, stack.Pop()));
      break;
    }

    case Opcode::I64ExtendI32S: {
      stack.Push(b->ConvertTo(Int64, stack.Pop()));
      break;
    }

    case Opcode::I64ExtendI32U: {
      stack.Push(b->UnsignedConvertTo(Int64, stack.Pop()));
      break;
    }

    case Opcode::F32DemoteF64: {
      stack.Push(b->ConvertTo(Float, stack.Pop()));
      break;
    }

    case Opcode::F64PromoteF32: {
      stack.Push(b->ConvertTo(Double, stack.Pop()));
      break;
    }

    case Opcode::I32Extend8S: {
      stack.Push(b->ConvertTo(Int32, b->ConvertTo(Int8, stack.Pop())));
      break;
    }

    case Opcode::I32Extend16S: {
      stack.Push(b->ConvertTo(Int32, b->ConvertTo(Int16, stack.Pop())));
      break;
    }

    case Opcode::I64Extend8S: {
      stack.Push(b->ConvertTo(Int64, b->ConvertTo(Int8, stack.Pop())));
      break;
    }

    case Opcode::I64Extend16S: {
      stack.Push(b->ConvertTo(Int64, b->ConvertTo(Int16, stack.Pop())));
      break;
    }

    case Opcode::I64Extend32S: {
      stack.Push(b->ConvertTo(Int64, b->ConvertTo(Int32, stack.Pop())));
      break;
    }

    case Opcode::F32ConvertI32S: {
      stack.Push(b->ConvertTo(Float, stack.Pop()));
      break;
    }

    case Opcode::F32ConvertI32U: {
      stack.Push(b->UnsignedConvertTo(Float, stack.Pop()));
      break;
    }

    case Opcode::F32ConvertI64S: {
      stack.Push(b->ConvertTo(Float, stack.Pop()));
      break;
    }

    case Opcode::F32ConvertI64U: {
      stack.Push(b->UnsignedConvertTo(Float, stack.Pop()));
      break;
    }

    case Opcode::F64ConvertI32S: {
      stack.Push(b->ConvertTo(Double, stack.Pop()));
      break;
    }

    case Opcode::F64ConvertI32U: {
      stack.Push(b->UnsignedConvertTo(Double, stack.Pop()));
      break;
    }

    case Opcode::F64ConvertI64S: {
      stack.Push(b->ConvertTo(Double, stack.Pop()));
      break;
    }

    case Opcode::F64ConvertI64U: {
      stack.Push(b->UnsignedConvertTo(Double, stack.Pop()));
      break;
    }

    case Opcode::F32ReinterpretI32: {
      stack.Push(b->ConvertBitsTo(Float, stack.Pop()));
      break;
    }

    case Opcode::I32ReinterpretF32: {
      stack.Push(b->ConvertBitsTo(Int32, stack.Pop()));
      break;
    }

    case Opcode::F64ReinterpretI64: {
      stack.Push(b->ConvertBitsTo(Double, stack.Pop()));
      break;
    }

    case Opcode::I64ReinterpretF64: {
      stack.Push(b->ConvertBitsTo(Int64, stack.Pop()));
      break;
    }

    case Opcode::I32TruncF32S:
      EmitTruncation<int32_t, float>(b, pc, &stack);
      break;

    case Opcode::I32TruncF32U:
      EmitUnsignedTruncation<uint32_t, float>(b, pc, &stack);
      break;

    case Opcode::I32TruncF64S:
      EmitTruncation<int32_t, double>(b, pc, &stack);
      break;

    case Opcode::I32TruncF64U:
      EmitUnsignedTruncation<uint32_t, double>(b, pc, &stack);
      break;

    case Opcode::I64TruncF32S:
      EmitTruncation<int64_t, float>(b, pc, &stack);
      break;

//    UNSIGNED TYPE NOT HANDLED
//    case Opcode::I64TruncF32U:
//      EmitTruncation<uint64_t, float>(b, pc, &stack);
//      break;

    case Opcode::I64TruncF64S:
      EmitTruncation<int64_t, double>(b, pc, &stack);
      break;

//    UNSIGNED TYPE NOT HANDLED
//    case Opcode::I64TruncF64U:
//      EmitTruncation<uint64_t, double>(b, pc, &stack);
//      break;

    case Opcode::InterpBrUnless: {
      auto target = &istream[interp::ReadU32(&pc)];
      auto condition = stack.Pop();
      auto it = std::find_if(workItems_.cbegin(), workItems_.cend(), [&](const BytecodeWorkItem& b) {
        return pc == b.pc;
      });
      if (it != workItems_.cend()) {
        b->IfCmpEqualZero(it->builder, condition);
      } else {
        int32_t next_index = static_cast<int32_t>(workItems_.size());
        workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                      const_cast<char*>(interp::ReadOpcodeAt(pc).GetName())),
                                target);
        b->IfCmpEqualZero(&workItems_[next_index].builder, condition);
      }
      break;
    }

    case Opcode::Drop:
      stack.DropKeep(1, 0);
      break;

    case Opcode::InterpDropKeep: {
      uint32_t drop_count = interp::ReadU32(&pc);
      uint32_t keep_count = interp::ReadU32(&pc);
      stack.DropKeep(drop_count, keep_count);
      break;
    }

    case Opcode::Nop:
      break;

    default:
      return false;
  }

  auto it = std::find_if(workItems_.cbegin(), workItems_.cend(), [&](const BytecodeWorkItem& b) {
    return pc == b.pc;
  });
  if (it != workItems_.cend()) {
    b->AddFallThroughBuilder(it->builder);
  } else {
    int32_t next_index = static_cast<int32_t>(workItems_.size());
    workItems_.emplace_back(OrphanBytecodeBuilder(next_index,
                                                  const_cast<char*>(interp::ReadOpcodeAt(pc).GetName())),
                            pc);
    b->AddFallThroughBuilder(workItems_[next_index].builder);
  }

  return true;
}

}
}
