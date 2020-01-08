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

#include "type-dictionary.h"
#include "src/interp/interp.h"

wabt::jit::TypeDictionary::TypeDictionary() : TR::TypeDictionary() {
    using namespace wabt::interp;

    // Currently, WABT allows v128 values on the physical value stack. JITted code does not yet
    // support accessing such values, but we still need to take into account that the size of the
    // Value union is larger than the largest field we can access. For now, this is done by adding
    // a struct field with a fixed size in the Value union; this field can't be accessed, but will
    // cause the calculated size to be correct.
    DefineStruct("ValueSizePad");
    CloseStruct("ValueSizePad", sizeof(Value));

    DefineUnion("Value");
    UnionField("Value", "i32", toIlType<decltype(Value::i32)>(this));
    UnionField("Value", "i64", toIlType<decltype(Value::i64)>(this));
    UnionField("Value", "f32", toIlType<float>(this));
    UnionField("Value", "f64", toIlType<double>(this));
    UnionField("Value", "__size_pad", LookupStruct("ValueSizePad"));
    CloseUnion("Value");

    DefineStruct("CallFrame");
    DefineField("CallFrame", "pc", toIlType<IstreamOffset>(this));
    DefineField("CallFrame", "is_jit", Int8);
    DefineField("CallFrame", "is_jit_compiling", Int8);
    CloseStruct("CallFrame");

    auto pCallFrame = PointerTo(LookupStruct("CallFrame"));

    DefineStruct("ThreadInfo");
    DefineField("ThreadInfo", "pc", Int32);
    DefineField("ThreadInfo", "in_jit", Int32);
    DefineField("ThreadInfo", "call_stack_max", pCallFrame);
    DefineField("ThreadInfo", "call_stack", pCallFrame);
    DefineField("ThreadInfo", "jit_fn_table", toIlType<void**>(this));
    DefineField("ThreadInfo", "thread", toIlType<void*>(this));
    CloseStruct("ThreadInfo");

    DefineStruct("MemoryData");
    DefineField("MemoryData", "capacity", toIlType<size_t>(this));
    DefineField("MemoryData", "size", toIlType<size_t>(this));
    DefineField("MemoryData", "data", toIlType<void*>(this));
    CloseStruct("MemoryData");
}
