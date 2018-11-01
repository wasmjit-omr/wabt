/*
 * Copyright 2018 wasmjit-omr project participants
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

#ifndef JIT_STACK_HPP
#define JIT_STACK_HPP

#include <vector>

#include "ilgen/IlBuilder.hpp"
#include "ilgen/VirtualMachineState.hpp"

namespace wabt {
namespace jit {

class VirtualStack : TR::VirtualMachineState {
public:
  void Push(TR::IlValue* v);
  TR::IlValue* Pop();
  void Drop() { DropKeep(1, 0); }
  void DropKeep(size_t drop_count, size_t keep_count);

  size_t Depth() { return values_.size(); }
  TR::IlValue* Top() { return Pick(0); }
  TR::IlValue* Pick(size_t i);
  TR::IlValue* PickBottom(size_t i);

  void MergeInto(const VirtualStack* other, TR::IlBuilder* b);

  void MergeInto(TR::VirtualMachineState* other, TR::IlBuilder* b) override {
    MergeInto(&dynamic_cast<VirtualStack&>(*other), b);
  }

  TR::VirtualMachineState* MakeCopy() override {
    return new VirtualStack(*this);
  }

private:
  std::vector<TR::IlValue*> values_;
};

}
}

#endif // JIT_STACK_HPP
