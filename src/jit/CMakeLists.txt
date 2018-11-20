#
# Copyright 2017 wasmjit-omr project participants
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

add_library(wabtjit STATIC
  environment.cc
  type-dictionary.cc
  function-builder.cc
  wabtjit.cc
  stack.cc
)

# Mark JitBuilder include directories as "system" include directories. For most compilers, this
# suppresses any warnings they would otherwise generate.
get_property(JITBUILDER_INCLUDE TARGET jitbuilder PROPERTY INTERFACE_INCLUDE_DIRECTORIES)
target_include_directories(wabtjit SYSTEM PUBLIC "${JITBUILDER_INCLUDE}")
target_link_libraries(wabtjit PUBLIC jitbuilder)
