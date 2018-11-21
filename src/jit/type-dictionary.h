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

#ifndef TYPEDICTIONARY_HPP
#define TYPEDICTIONARY_HPP

#include "ilgen/TypeDictionary.hpp"

namespace wabt {
namespace jit {

class TypeDictionary : public TR::TypeDictionary {
 public:
  TypeDictionary();
};

template <typename T>
struct is_supported {
  static const bool value =  std::is_arithmetic<T>::value // note: is_arithmetic = is_integral || is_floating_point
                          || std::is_void<T>::value;
};
template <typename T>
struct is_supported<T*> {
  // a pointer type is supported iff the type being pointed to is supported
  static const bool value =  is_supported<T>::value;
};

template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) == 1)>::type* = 0) { return td->Int8; }
template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) == 2)>::type* = 0) { return td->Int16; }
template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) == 4)>::type* = 0) { return td->Int32; }
template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_integral<T>::value && (sizeof(T) == 8)>::type* = 0) { return td->Int64; }

template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_floating_point<T>::value && (sizeof(T) == 4)>::type* = 0) { return td->Float; }
template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_floating_point<T>::value && (sizeof(T) == 8)>::type* = 0) { return td->Double; }

template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_void<T>::value>::type* = 0) { return td->NoType; }

template <typename T>
TR::IlType* toIlType(TR::TypeDictionary *td, typename std::enable_if<std::is_pointer<T>::value && is_supported<typename std::remove_pointer<T>::type>::value>::type* = 0) {
  return td->PointerTo(toIlType<typename std::remove_pointer<T>::type>(td));
}

}
}

#endif // TYPEDICTIONARY_HPP
