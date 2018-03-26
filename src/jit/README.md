# wasmjit-omr JIT compiler

This directory contains the implementation of the WebAssembly Just-in-Time compiler.

## Introduction

This JIT compiler is implements using the Eclipse OMR JitBuilder framework. The
framework is included in the project as a submodule in the `third_party` directory.

## Buildering

The CMake file produce a static library called `wabtjit`. This file is linked into
any files that also link `libwabt`.

## High-level Structure

All definitions of the JIT compiler are located in the `wabt::jit` namespace.

`type-dictionary.h` and `type-dictionary.cc` contain the declaration of the
`wabt::jit::TypeDictionary` class, which extends the `TR::TypeDictionary` class.
The type dictionary allows the JIT to interface with types used in the interpreter.

`function-builder.h` contains the declaration of the `wabt::jit::FunctionBuilder`
class, which extends the `TR::MethodBuilder` class. `function-builder.cc` contains
the implementation and comprises the majority of the JIT implementation.

The `environment.h` and `environment.cc` files define a class that acts as an RAII
wrapper for JIT initialization and shutdown. This class is used to in the
`wabt::Evironment` class.

Finally, `wabtjit.h` and `wabtjit.cc` contain definition of the function
`wabt::jit::compile()`, which dispatches the JIT compiler to compile a WebAssembly
function.

