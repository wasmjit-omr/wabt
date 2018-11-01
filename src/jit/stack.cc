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

#include "stack.h"
#include "infra/Assert.hpp"

namespace wabt {
namespace jit {

void VirtualStack::Push(TR::IlValue* v) {
  values_.push_back(v);
}

static TR::IlValue* CheckResult(TR::IlValue* v) {
  TR_ASSERT(v, "Attempt to pick/pop an invalid value on the stack");
  return v;
}

TR::IlValue* VirtualStack::Pop() {
  TR_ASSERT(values_.size() > 0, "Attempt to pop off an empty stack");

  auto* v = values_.back();
  values_.pop_back();

  return CheckResult(v);
}

void VirtualStack::DropKeep(size_t drop_count, size_t keep_count) {
  TR_ASSERT(drop_count + keep_count >= drop_count && values_.size() > drop_count + keep_count,
            "Attempt to drop_keep beyond the end of the stack");

  values_.erase(values_.end() - drop_count - keep_count, values_.end() - keep_count);
}

TR::IlValue* VirtualStack::Pick(size_t i) {
  TR_ASSERT(i < values_.size(), "Attempt to pick beyond the end of the stack");

  return CheckResult(*(values_.end() - i - 1));
}

TR::IlValue* VirtualStack::PickBottom(size_t i) {
  TR_ASSERT(i < values_.size(), "Attempt to pick beyond the end of the stack");

  return CheckResult(*(values_.begin() + i));
}

void VirtualStack::MergeInto(const VirtualStack* other, TR::IlBuilder* b) {
  TR_ASSERT(values_.size() == other->values_.size(),
            "Attempt to merge divergent stacks: sizes don't match (%zu != %zu)",
            values_.size(),
            other->values_.size());

  auto this_it = values_.begin();
  auto other_it = other->values_.begin();

  while (this_it != values_.end()) {
    if (*this_it != *other_it) {
      TR_ASSERT(*this_it && *other_it,
                "Attempt to merge divergent stacks: element %zu is a placeholder",
                this_it - values_.begin());
      TR_ASSERT((*this_it)->getDataType() == (*other_it)->getDataType(),
                "Attempt to merge divergent stacks: types don't match at element %zu (%s != %s)",
                this_it - values_.begin(),
                (*this_it)->getDataType()->toString(),
                (*this_it)->getDataType()->toString());

      b->StoreOver(*other_it, *this_it);
    }

    this_it++;
    other_it++;
  }
}

}
}
