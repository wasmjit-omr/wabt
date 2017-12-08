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
#include "ilgen/MethodBuilder.hpp"

#include "src/interp.h"

namespace wabt {
namespace jit {

class FunctionBuilder : public TR::MethodBuilder {
 public:
  FunctionBuilder(interp::Thread* thread, interp::IstreamOffset const offset, TypeDictionary* types);
  bool buildIL() override;

 private:
  interp::Thread* thread_;
  interp::IstreamOffset const offset_;
};

}
}

#endif // FUNCTIONBUILDER_HPP
