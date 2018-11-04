# TURBO18: Boost WABT Performance using JitBuilder

In this tutorial you will learn how to integrate a just-in-time (JIT) compiler into
a language runtime.  The WebAssembly Binary Toolkit (WABT) provides a number of tools
for working with WebAssembly (Wasm) files, and the objective of this tutorial is to
provide a JIT compiler for the WABT interpreter.  The JIT compiler is based on
Eclipse OMR, which is a toolkit of runtime technology components that can be extended
and customized for the needs of different runtimes.  This tutorial uses a simplified
interface to the compiler component called JitBuilder, which makes it easy for a runtime
to describe methods that can be input into the JIT for compilation.

The concepts you will learn here are the same general steps you need to consider when
building a dynamic compiler for any language runtime.

You will not be building a complete Wasm JIT from scratch.  Rather, you will be building
upon (and in some cases re-implementing) the functionality already provided by an existing Wasm
JIT for the WABT interpreter.  This is mainly so that you can continuously build as you follow
the exercises, watch the evolution of your changes, and run a real workload that demonstrates
the effect of JITing code.

The workload is an implementation of a Mandelbrot viewer that will plot the result.
The speed at which the set is computed and the plot completed is a function of the speed
of execution of the underlying Wasm functions.  The faster you can make your Wasm functions
(i.e., by compiling them) the faster the plot will complete.  You will use this Mandelbrot
viewer as a measure of your progress.

## How to Complete the Tutorial

The means by which you complete this tutorial is up to you!  You may set your own pace
and complete the exercises as you choose.  You should be able to build after each exercise
is complete to validate your work.

The best way to make the most of this tutorial is to go through each exercise and
complete the code yourself by following the hints provided and diving into and exploring
the JitBuilder and wasm-jit codebases.

The source code has been annotated with comments `// YOUR CODE HERE` to guide you
to the right place in the source code to implement your solution.

Alternatively, solutions for each exercise are provided at the end of this tutorial booklet that
you can either cut-and-paste or type in directly.  Links to the GitHub repository are also provided.
Typing the solutions in yourself
may give you more pause to think about the details of the solution.  A cut-and-paste
approach will allow you to complete this tutorial very quickly and allow you to get
a high-level overview of the steps in involved in integrating a JIT compiler in a
runtime.

## Getting Started

Before providing you with an overview of the technologies you will be working with in this
tutorial, you may wish to begin installing and building the components you will need
to complete the tutorial.  Because building may take several minutes, you can read the
background material while the installation is proceeding.

But feel free to to skip ahead and then come back to this step when you're ready.

### Prerequisites

To complete this tutorial you will need:

* A laptop running Linux or macOS (Windows is not yet supported in this tutorial)
* a C++11 toolchain (gcc, Clang)
* CMake 2.6+, and a supported backend build system (make, Ninja)
* Python 2.7+ (WABT requirement)
* git

### Cloning, Building, and Running the Project

Start by cloning the repository.
The `turbo` branch will be checked out by default.
Depending on your network connection, this step should take approximately 25 seconds.

```sh
git clone --recursive https://github.com/omr-turbo/wasmjit-omr.git
cd wasmjit-omr
```

Next, build the project.  This step typically takes around 7 minutes depending
on your build machine and development environment.  If you are building in a
virtual machine then building may take significantly longer.

```sh
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..     # optionally with -GNinja
make libc-interp                        # or ninja libc-interp
env time ./libc-interp mandelbrot.wasm
```

Run and time the execution of the `mandelbrot.wasm` example using the
`libc-interp` tool. You should see output similar to:

```
$ env time ./libc-interp mandelbrot.wasm


                                                       *
                                                     ****
                                                     ****
                                             *        **
                                             **  ************
                                             ********************
                                             *******************
                                            *********************
                                          *************************
                                  *       ************************
                               ********  *************************
                              ********** *************************
                             *********** ************************
            ***************************************************
                             *********** ************************
                              ********** *************************
                               ********  *************************
                                  *       ************************
                                          *************************
                                            *********************
                                             *******************
                                             ********************
                                             **  ************
                                             *        **
                                                     ****
                                                     ****
                                                       *

exit_group(0) called
_start() => error: host function trapped
  at $_Exit [@7383]
  at $exit [@7947]
  at $__libc_start_main [@7202]
  at $_start_c [@436]
  at $_start [@127]
21.35user 0.01system 0:21.37elapsed 99%CPU (0avgtext+0avgdata 3172maxresident)k
0inputs+0outputs (0major+916minor)pagefaults 0swaps
```

Note that the error status reported is expected!

* * *

## WABT Overview

The [WebAssembly Binary Toolkit](https://github.com/WebAssembly/wabt) (WABT) is a suite
of tools for working with WebAssembly.  The tools are written in C/C++ and are designed
for easy integration into other projects.

This tutorial builds upon the [wasmjit-omr](https://github.com/wasmjit-omr/wasmjit-omr)
project, which provides a rudimentary JIT for the Wasm interpreter library in WABT.

The tool we will be focused on for this tutorial is **libc-interp**.  This is a
stack-based interpreter that can decode and interpret the Wasm instructions in a
Wasm binary file.  It also provides a library of utility functions that can be called at
runtime from Wasm functions, similar to the C runtime for C applications.

The bulk of the source for the interpreter can be found in `src/` with JIT specializations
in `src/jit/`.  The main interpreter module, Wasm runtime, and Mandelbrot demo file for
this tutorial can be found in `libc-interp/`.

## JitBuilder Overview

JitBuilder provides a simplified interface to the compiler technology that allows you to
integrate a compiler into your runtime more quickly.  It abstracts away many of the low-level
compilation details that you would normally need to consider when building an Eclipse OMR
based compiler behind a much simpler API.

JitBuilder packages the guts of the compiler into a static library that you can then link
against in your language runtime using APIs, which are described in just a few header files.

There are a few structures that are particularly important when using JitBuilder:

`TR::MethodBuilder` is an object that represents the ABI (parameters, return value) and
the operations that should be performed when a compiled method is called. To compile your
own methods, you subclass `OMR::MethodBuilder` and implement its constructor to specify
the ABI and then specify the operations the compiled method should perform by implementing
a `MethodBuilder` member function called `buildIL()`.  After passing a `TR::MethodBuilder`
object to `compileMethodBuilder()`, you get back a pointer to a function that you can call
by casting that pointer to a C function prototype. Each `TR::MethodBuilder` has a hash map
of names (C strings) representing the method’s local variables, and you can define them all
at once or make them up as you go.

`TR::TypeDictionary` is an object that you can use to describe the shape of structures, unions,
and other types so that JitBuilder knows how to access their fields and values.

`TR::IlBuilder` is an object that represents the operations to perform when a particular
control flow path is reached. The method entry point is one particular control flow path,
and so a `TR::MethodBuilder` object is also a `TR::IlBuilder` object. But you can create
arbitrary `TR::IlBuilder` objects and connect them together (even nest them) in arbitrary
ways using an ever-growing list of services like `IfThen` or `ForLoopUp` or `WhileDoLoop`.
Each service you call on a `TR::IlBuilder` appends operations to the control flow path and
so describes the intended order of operations to the compiler. There is no restriction on
how you add operations to different `TR::IlBuilder` objects. You can create all the
`TR::IlBuilder` objects up front and then add operations to them individually, or you can
start from the `TR::MethodBuilder` object (function entry) and create `TR::IlBuilder`
objects as you need them to represent the different paths of execution as you require them.

`TR::BytecodeBuilder` is a special kind of IlBuilder that’s designed to simplify writing
JIT compilers for bytecode based languages. You allocate a `TR::BytecodeBuilder` object for
each bytecode in a method and translate the operations needed for that bytecode using that
object. You have to specify every control flow edge (even fall through edges) between bytecode
builders. `TR::BytecodeBuilder` also taps into a handy and mostly automatic built-in
worklist algorithm provided by `TR::MethodBuilder` that can traverse all flow edges in a
control flow graph, visiting each bytecode at most once. Any bytecodes in the method that
aren’t reachable (many parsers generate unreachable code to simplify the process of initial
code generation from a parse tree) will not be visited.

`TR::IlValue` is an object that represents the values that are created and consumed by
expressions. If you load a local variable by name, the value that’s loaded is a
`TR::IlValue`. If you create a constant integer, that’s a `TR::IlValue`. If you pass both
of those `TR::IlValue`s into the Add operation, you’ll get back another `TR::IlValue` that
represents their sum. There is an ever growing list of operations you can apply to create
arbitrarily complex `TR::IlValue` expressions.

These key structures can be found in `third_party/omr/compiler/ilgen` and can be extended
by your language runtime.

## Further resources

After the tutorial, you may wish to review the following resources for a deeper understanding
of the wasmjt-omr and JitBuilder content.

* VIDEO (30 minutes) [Performant WebAssembly outside the browser using Eclipse OMR JitBuilder](https://www.youtube.com/watch?v=wWhGlNVg2O8)

* [JitBuilder Library and Eclipse OMR: Just-in-time compilers made easy](https://developer.ibm.com/code/2016/07/19/jitbuilder-library-and-eclipse-omr-just-in-time-compilers-made-easy/)

* [Build more complicated methods using the JitBuilder library](https://developer.ibm.com/code/2017/03/08/build-more-complicated-methods-using-the-jitbuilder-library/)

* [Building the JitBuilder library from source code](https://developer.ibm.com/code/2017/04/14/building-jitbuilder-library-source-code/)

* * *

## Part 1: Dispatching the JIT

### Exercise 1: Calling the OMR Compiler from JitBuilder

#### Background

The main task involved in creating a JIT compiler using JitBuilder is to
translate the VM/interpreter's representation of functions into the OMR
compiler's Intermediate Language (known as TR IL), a process known as IL generation. In
JitBuilder, IL generation is implemented by subclassing `OMR::MethodBuilder` ([`third_party/omr/compiler/ilgen/OMRMethodBuilder.hpp`](https://github.com/eclipse/omr/blob/3123e6d913318d74e0f0ecacd9de96533874e1fc/compiler/ilgen/OMRMethodBuilder.hpp#L47)) and
overriding the required functions. To actually compile a function/method,
`compileMethodBuilder()` is called with an instance of the `OMR::MethodBuilder`
subclass, an instance of a `TypeDictionary` (an object used to describe
non-primitive types to JitBuilder), and a pointer to a variable in which the
entry point to a function will be stored, if compilation is successful.
`compileMethodBuilder()` returns 0 if compilation succeeds and some non-0 value
otherwise.

In wasmjit-omr, the `OMR::MethodBuilder` subclass is `wabt::jit::FunctionBuilder` ([`src/jit/function-builder.h`](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.h#L32)).
A subclass of `TypeDictionary` is also implemented as `wabt::jit::TypeDictionary` ([`src/jit/type-dictionary.h`](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/type-dictionary.h#L25)).
In later sections, you will complete parts of
`FunctionBuilder` to practice generating IL using JitBuilder.

#### Your Task

In [`src/jit/wabtjit.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/wabtjit.cc#L26), complete the implementation
of `wabt::jit::compile(interp::Thread* thread, interp::DefinedFunc* fn)`.

```c++
JITedFunction compile(interp::Thread* thread, interp::DefinedFunc* fn) {
  TypeDictionary types;
  FunctionBuilder builder(thread, fn, &types);

  // YOUR CODE HERE

  return nullptr;
}
```

Given an interpreter `thread` and a defined function object `fn`, `compile()` should
(unconditionally) invoke the JIT compiler and return the entry point to the
generated body. If JIT compilation fails, `nullptr` should be returned instead.

#### What To Do

Complete `compile()` by:

- defining a variable of type `uint8_t *` to store the entry point to the JIT
compiled body
- calling `compileMethodBuilder()` with `types`, `builder`, and the address
of the variable used to store the entry point as arguments
- returning the entry point cast to `JITedFunction` if `compileMethodBuilder()`
returns `0`, `nullptr` (or `NULL`) otherwise

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-1-calling-the-omr-compiler-from-jitbuilder)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/wabtjit.cc#L26).


### Exercise 2: Deciding When To Compile

#### Background

A key aspect of maximizing performance using a JIT compiler is to control when
the JIT compiler is invoked. Compiling too often can mean a VM spends more time
compiling than executing application code.

A simple way to limit what/when functions get compiled is to only compile
functions if they have been called a fixed number of times--a so-called "counted
compilation".

Another thing that VMs must handle is the case when JIT compilation fails. The
JIT may fail to compile a function if, for example, the function uses a
language feature not supported by the compiler. In such cases the function
must always be interpreted.

`wabt::interp::JitMeta` ([`src/interp.h`](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/interp.h#L476))
is a WABT struct used to keep track of information about WebAssembly
functions that is useful for controlling JIT compilation:

```c++
struct JitMeta {
  DefinedFunc* wasm_fn;            // pointer to the description object for the Wasm function
  uint32_t num_calls = 0;          // the number of times the function has been called

  bool tried_jit = false;          // whether we've already tried JITing this function
  JITedFunction jit_fn = nullptr;  // pointer to the entry point of the compiled body

  JitMeta(DefinedFunc* wasm_fn) : wasm_fn(wasm_fn) {}
};
```

#### Your Task

In [`src/interp.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/interp.cc#L1189), complete the implementation
of `Environment::TryJit()`.

```c++
bool Environment::TryJit(Thread* t, IstreamOffset offset, Environment::JITedFunction* fn) {
  if (!enable_jit) {
    *fn = nullptr;
    return false;
  }

  auto meta_it = jit_meta_.find(offset);

  if (meta_it != jit_meta_.end()) {
    JitMeta* meta = &meta_it->second;

    // YOUR CODE HERE

    *fn = meta->jit_fn;
    return trap_on_failed_comp || *fn;
  } else {
    *fn = nullptr;
    return trap_on_failed_comp;
  }
}
```

Given an interpreter thread `t` ([`src/interp.h`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/interp.h#L502)), an instruction offset `offset`, and a
pointer to a pointer to a function `fn` (used as an in-out parameter) ([`src/jit/wabjit.h`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/wabtjit.h#L26)),
`TryJit()` should try to JIT compile the function at `offset` if:

1. the JIT compiler is enabled
2. there is an entry for it in the meta-data table
3. there has never been a previous attempt to compile the function (preventing
recompilation of functions for which a previous compilation failed)
4. the function has been called at least the number of times set in `jit_threshold`
(a member of the `Environment` class)

If all conditions for compilation are met, the JIT compiler is invoked by
calling `jit::compile()` and the `tried_jit` field of the function's metadata should be
set to `true`.

If compilation succeeds, the `jit_fn` field of the function's metadata and the
variable pointed to by the `fn` parameter are set to the entry point returned by
`jit::compile()`.

#### What To Do

- check if `meta->tried_jit` is false, indicating that we have never tried to
compile this function before
- increment `meta->num_calls` (the number of times this function has been called)
- check if `meta->num_calls` is greater than `jit_threshold`
- if it is
    - call `jit::compile(t, meta->wasm_fn)`
    - assign the returned value to `meta->jit_fn`
    - set `meta->tried_jit` to true
- if not
    - set the variable pointed to be `fn` to `nullptr`
    - return false to indicate compilation failure

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-2-deciding-when-to-compile)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/interp.cc#L1189).


### Exercise 3: Where To Compile

#### Background

The last piece of the puzzle is to actually call the JIT compiler (`TryJit()`)
and to call the JIT compiled function. In many VMs, it makes sense to simply do
these calls where the interpreter handles calls. When a program is about to
call a function, the interpreter will invoke the JIT and, if compilation
succeeds, call the entry point returned by the JIT.

To correctly manage calls, the VM must keep track of the call stack at all times.
For convenience, the `PushCall(const uint8_t* pc)` and `PopCall()` function are
provided. `PushCall()` takes as an argument the `pc` of the call instruction and
returns a status to indicate possible trap conditions due to a call stack
overflow. `PopCall()` simply resets the interpreter's pc and takes no arguments
and produces no return value.

When the interpreter calls a JIT compiled function body, it is important to call
both of the above functions before and after the JIT compiled function call.
More precisely, the sequence of events must be:

- push a new stack by calling `PushCall(pc)`, checking for a trap condition in the
  returned value (`CHECK_TRAP()` macro can be used for this purpose)
- call the JIT compiled function
- check that the JIT compiled function returns `interp::Result::Ok`
- pop the stack frame by calling `PopCall()`

#### Your Task

In [`src/interp.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/interp.cc#L1371),
complete the WABT interpreter's handling of the
`Call` opcode to call `TryJit()` and to call the entry point to the compiled
body when successful.

```c++
      case Opcode::Call: {
        IstreamOffset offset = ReadU32(&pc);

        // YOUR CODE HERE

        // REPLACE `false` WITH CHECK OF `TryJit()` RETURN VALUE
        if (false) {

          // REPLACE `true WITH CHECK THAT VALUE RETURNED BY COMPILED BODY IS NOT `Result::Ok`
          if (true) {
            // **DO NOT CHANGE ANYTHING HERE**

            // We don't want to overwrite the pc of the JITted function if it traps
            tpc.Reload();

            return result;
          }

          // POP CALL STACK

        } else {
          CHECK_TRAP(PushCall(pc));
          GOTO(offset);
        }
        break;
      }
```

When the interpreter encounters a `Call` instruction, it proceeds as follows:

- get the offset of the function's first instruction
- try to JIT compile the function by calling `TryJit()`
- if compilation succeeds
    - emit `TrapFailedJITCompilation` if the entry point is null
    - push the current `pc` on the call stack, check for any trap conditions
    - call the JIT compiled body
    - if the compiled function *does not* return `interp::Result::Ok`
        - call `tpc.Reload()`
        - return the returned value
    - pop the pc off the call stack
- otherwise
    - push the current `pc` on the call stack, check for any trap conditions
    - jump to the offset
- break out of the case

#### What To Do

- Add a local variable of type `Environment::JITedFunction` to store the entry
point to the JIT compiled function body
- call `TryJit()` on the current environment (stored in the `env_` member
variable) passing `this` as the thread argument, the offset of the function, and
the address of the variable to store the entry point to the JIT compiled body
- replace the `if (false)` with a check that `TryJit()` returned `true`
- in the `if` body
    - use `TRAP_IF(<check entry point is null>, FailedJITCompilation);` to emit a
    trap if the entry point is null
    - push the current pc onto the call stack with `CHECK_TRAP(PushCall(pc));`
    - call the entry point (a JIT compiled function body takes no arguments)
    - replace the `if (true)` to check that JIT compiled function *did not*
    return `interp::Result::Ok` (`interp::` is optional)
        - *(no modifications to the body of the `if` needed)*
    - after the `if`, call `PopCall()` to pop the pc from the call stack

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-3-where-to-compile)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/interp.cc#L1380).

* * *

## Part 2: Generate TR IL

### Exercise 4: Implement `buildIL()`

#### Background

`buildIL()` is the function called by the OMR compiler to generate IL. It should
return `true` when IL generation succeeds, `false` otherwise. A JitBuilder JIT
overrides this method and implements their own IL generation.

`VirtualMachineState` is used to simulates state changes that would happen if
the function being compiled were to run int the VM.

`BytecodeBuilder`s are used to generate the IL corresponding to a specific
instruction/bytecode in a function. The call to `workItems_.emplace_back()`
inserts an instance corresponding to the first bytecode in the function being
compiled.

The work list keeps track of which opcodes from the function still need to be
handled. Internally, the JIT will call `AppendBuilder()` every time a new
instruction is encountered that requires IL generation.

#### Your Task

In [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L245),
complete the implementation of `FunctionBuilder::buildIL()`.

```c++
bool FunctionBuilder::buildIL() {
  setVMState(new TR::VirtualMachineState());

  const uint8_t* istream = thread_->GetIstream();

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(ReadOpcodeAt(&istream[fn_->offset]).GetName())),
                          &istream[fn_->offset]);
  AppendBuilder(workItems_[0].builder);

  // YOUR CODE HERE

  return false;
}
```

When invoked, `buildIL()` will generate IL for the WebAssembly function
corresponding to the current `FunctionBuilder` object.

It first sets an instance of `VirtualMachineState` on the current object. Since
the current JIT implementation does not use this particular JitBuilder feature,
using an instance of the base class is sufficient.

It then inserts a single element into the `workItems_` list. Each element of the
list contains an instance of `BytecodeBuilder` and the address pointing to a
corresponding instruction in the function. The first element inserted in the
list corresponds to the first instruction of the function and is therefore given
the index 0.

Next, the newly created `BytecodeBuilder` instance is added to the internal
JitBuilder.

In a loop, `GetNextBytecodeFromWorklist()` is called and the index is verified
to be different than -1 (which indicates the end of the work list). The work
item at the given index in `workItems_` is retrieved and IL is generated for
the corresponding instruction, using the contained `builder` object. If IL
generation for an instruction fails, the function returns false.

Once IL is successfully generated for all instructions, `buildIL()` returns
true.

#### What To Do

- in a loop, call `GetNextBytecodeFromWorklist()`
- break out of the loop if the returned index is -1
- get the work item at the returned index in `workItems_`
- call `Emit()`, passing as arguments:
    - the `BytecodeBuilder` instance for the instruction (`.builder` member of
    the work item object)
    - the instruction stream `istream`
    - a pointer to the instruction (`.pc` member of the the work item object)
- if `Emit()` returns false, `buildIL()` should also return false
- change the `return false` at the end of `buildIL()` to `return true`

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-4-implement-buildil)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.cc#L245).

* * *

## Part 3: Implement a few Wasm opcodes

### Exercise 5: Implement `Return`

#### Background

To generate IL, JitBuilder provides "services" that must be called on an `OMR::IlBuilder`
instance. Sub-classes of `OMR::IlBuilder` include `OMR::BytecodeBuilder`, `OMR::MethodBuilder`,
and sub-classes of these that are implemented by JitBuilder users (e.g.
`wabt::jit::FunctionBuilder`).

Each JitBuilder service generates IL that represents a particular action. For
example, the `Return()` service generates IL representing "returning from a
function". When no arguments are passed, the generated IL represents a simple
return, without a return value. An argument, of type `TR::IlValue`, can be passed to
represent a returned value.

`TR::IlValue` ([`third_party/omr/compiler/ilgen/OMRIlValue.hpp`](https://github.com/eclipse/omr/blob/a3d48e4713fa0078d0f34a5e901b9c2b84ad3c6d/compiler/ilgen/OMRIlValue.hpp#L41))
is a class used by JitBuilder to represent values that are "computed" by
the generated IL or, more precisely, values computed by the code *generated*
from the IL by the compiler. JitBuilder provides various services for generating
`TR::IlValue` instances. For instance, `ConstInt32(int32_t)` generates the
representation of a constant, 32-bit integer value. All the arithmetic
operation services take as arguments and return instances of `TR::IlValue`. Memory
load operations produce `TR::IlValue` instances while memory store operations take
an instance as argument.

#### Your Task

In [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L678),
implement IL generation for the `Return` opcode.

```c++
case Opcode::Return:
  return false;
```

The generated IL must represent "return the value `interp::Result::Ok`".

Because no other instructions from the function are expected to be executed,
`Emit()` can simply return true after successfully generating IL for `Return`.

#### What To Do

- static cast the enum value `interp::Result::Ok` to the underlying integer type
`Result_t`
- generate a `TR::IlValue` for the constant by calling `Const()` on the current
`BytecodeBuilder` instance `b`, passing the constant itself as an argument
- generate the IL for the return by calling `Return()` on the builder `b`,
passing the `TR::IlValue` instance as an argument
- change `return false` to `return true` :)

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-5-implement-return)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.cc#L683).

### Exercise 6: Implement `i32.sub` and `i32.mul`

#### Background

Being a stack-based VM, the WABT interpreter performs pushes and pops to the
operand stack every time an operation is performed. For convenience,
`FunctionBuilder` provides some helpers to generate IL for operand stack pushes
and pops.

`PopI32(TR::IlBuilder*)` generates IL corresponding to a stack pop. The first
argument is a pointer to the `TR::IlBuilder` object that should be used to
generate the IL. It returns a `TR::IlValue` instance representing the value
popped from the stack.

`PushI32(TR::IlBuilder*, TR::IlValue*, const uint8_t*)` generates IL representing
a stack push. The first argument is the `TR::IlBuilder` object to be used to
generate IL. The second argument is the `TR::IlValue` instance representing the
value being pushed. The final argument is the `pc` pointing to the instruction
performing the push. It is used to generate a trap if the stack overflows
because of the push.

#### Your Task

In [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L993),
implement the 32-bit integer `Sub` and `Mul` opcodes.

```c++
case Opcode::I32Sub:
  return false;
```

The IL generated for every binary arithmetic operation (`Add`, `Sub`, `Mul`, etc.)
must:

- pop the RHS from the operand stack
- pop the LHS from the operand stack
- compute the operation
- push the result onto the operand stack

Because the instructions that follow the arithmetic operations must also be
processed, once IL is successfully generated, each `case` must `break`
out of the `switch` statement instead of `return`.

Use the implementation of `i32.add` as an example to guide you:

```c++
case Opcode::I32Add: {
  auto rhs = PopI32(b);
  auto lhs = PopI32(b);
  PushI32(b, b->Add(lhs, rhs), pc);
  break;
}
```

#### What To Do

- pop the RHS by calling `PopI32()` with `b` as argument
- pop the LHS following the style used to pop the RHS
- generate the computation by calling the appropriate service
  (`b->Sub()` or `b->Mul()`) with the LHS and RHS as arguments
- push the resulting `TR::IlValue` using `PushI32()` with `b`, the `TR::IlValue`
  instance, and `pc` as arguments
- `break` out of the `switch`

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-6-implement-i32-sub-and-i32-mul)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.cc#L1007).

### Exercise 7: Implement `f32.sub` and `f32.mul`

Because most arithmetic opcode implementations follow a similar pattern, it is
useful to abstract the commonalities into a helper function. In wasmjit-omr,
the `EmitBinaryOp` templated function can be used for this purpose:

```c++
/**
 * \tparam    `T` : the type of operation being emitted (e.g., `int32_t` for 32-bit
 *           integer binary operations)
 * \tparam    `TResult` : the type of the result of the operation (same as `T` by
 *           default)
 * \tparam    `TOpHandler` : the type of the callable object that generates IL for
 *           the operation
 * \param[in] `builder` : a pointer to the builder object on which pushes and pops
 *           should be generated
 * \param[in] `pc` : the current ("virtual") pc pointing to the instruction for which
 *           IL is being generated
 * \param[in] `operation` : a lambda (or other callable object) that generates only
 *           the IL for the operation.  The lambda's arguments are the TR::IlValues
 *           corresponding to the operation operands and is expected to return the
 *           IlValue corresponding to the result:
 *              `TR::IlValue * lambda(TR::IlValue* lhs, TR::IlValue* rhs)`
 */
template <typename T, typename TResult = T, typename TOpHandler>
void EmitBinaryOp(TR::IlBuilder* builder, const uint8_t* pc, TOpHandler operation);
```

#### Your Task

In [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L1328),
implement the 32-bit floating-point `Sub` and `Mul` opcodes using the
`EmitBinaryOp` template function.

```c++
case Opcode::F32Sub:
  return false;

case Opcode::F32Mul:
  return false;
```

Use the implementation of `f32.add` as an example to guide you:

```c++
case Opcode::F32Add:
  EmitBinaryOp<float>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
    return b->Add(lhs, rhs);
  });
  break;
```

#### What To Do

- call `EmitBinaryOp<>()` with `float` as the template argument, `b` and `pc`
as second and third arguments, respectively, and a lambda function that:
    - captures by reference
    - calls `Sub()` or `Mul()` on `b`, forwarding the lambda's arguments
    - returns the resulting `TR::IlValue` instance
- `break` out of the `switch`

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-7-implement-f32-sub-and-f32-mul)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.cc#L1330).

### Exercise 8: Implement `Call`

#### Background

Both the interpreter and JIT compiled code must be capable of calling the
following entities:

- interpreted (not yet JIT compiled) functions
- JIT compiled function
- the JIT compiler itself

To call an interpreted function from JIT compiled code, a common strategy is
for the JIT compiled code to simply call the interpreter, pointing its pc to the
first instruction in the function.

To avoid having to generate IL that represents all this logic, we can instead
generate a call to a so-called *runtime helper* that will take care of all the
complexity.

In WABT, the following is already implemented for you [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L95):

```c++
Result_t CallHelper(interp::Thread* th, interp::IstreamOffset offset, uint8_t* current_pc)
```

Runtime helpers are "registered" with JitBuilder by calling the `DefineFunction`
service, normally in the constructor of the `MethodBuilder` subclass.

```c++
DefineFunction("CallHelper", __FILE__, "0",          // the name of the helper (with dummy file and line number)
               reinterpret_cast<void*>(CallHelper),  // pointer to the function implementation
               types->toIlType<Result_t>(),          // JitBuilder representation of the returnt type
               3,                                    // number of parameters
               types->toIlType<void*>(),             // JitBuilder representation of the parameter types
               types->toIlType<wabt::interp::IstreamOffset>(),
               types->PointerTo(Int8));
```

Registered functions can then be called using the `Call()` services.

#### Your Task

In [`src/jit/function-builder.cc`](https://github.com/wasmjit-omr/wasmjit-omr/blob/42b8ae72308581eaff882626496ec1cf8dadff8f/src/jit/function-builder.cc#L764),
complete IL generation for the `Call` opcode.

```c++
case Opcode::Call: {
  auto th_addr = b->ConstAddress(thread_);
  auto offset = b->ConstInt32(ReadU32(&pc));
  auto current_pc = b->Const(pc);

  // YOUR CODE HERE

  return false;
}
```


The IL generated for `Call` must generate IL for a `Call` to the `CallHelper`
runtime helper. The arguments to the call must be:

- a pointer to the current thread instance (`thread_` member of `FunctionBuilder`)
- the offset of the function to be called (offset of the function's first instruction
- the pc of the of the current `Call` instruction

The `Call()` service returns a `TR::IlValue` instance representing the value
returned by the function call. It takes as arguments:

- the name of the function to be called (must match the name used in `DefineFunction`)
- the number of arguments to be passed
- `TR::IlValue` instances representing the values of the arguments (as a vararg)

The value returned by the function must then be checked for a trap value. If
it is a trap, then the value must be propagated back by returning from the
Wasm function that IL is being generated for. The `EmitCheckTrap()` can be used
to generate IL representing the required trap handling. As arguments, it takes
a builder object, `TR::IlValue` representing the value to be checked (return value of
`CallHelper` in this case), and a pointer to the pc that must be updated if a
trap condition is detected.

Note: in the generated code, because the called function has *already returned*
by this point, there is no need to update the pc, which can be indicated to
`EmitCheckTrap()` by passing a null pointer in place of a pointer to the pc.

Finally, the code must break out of the `switch` statement to handle execution
of the next instruction.

#### What To Do

- call the `Call()` JitBuilder service, saving the returned value, and passing
as arguments:
    - `"CallHelper"` to call the runtime helper that has been registered for you
    - `3` for the 3 arguments that must be passed
    - `th_addr`, which represents a pointer to the current `interp::Thread`
    instance
    - `offset`, which represents the instruction offset of the function to be
    called
    - `current_pc`, which represents an instruction pointer to the current
    `Call` instruction
- call `EmitCheckTrap`, passing as arguments:
    - the current builder object `b`
    - an IlValue representing the value returned by the call to `CallHelper`
    - `nullptr` since the pc does not need to be updated
- change the `return false` to `break`

#### Solution

The solution to this exercise is available in this document [here](#solution-exercise-8-implement-call)
or from the GitHub repository [here](https://github.com/wasmjit-omr/wasmjit-omr/blob/2f7c7ba59fa36f7b5beed916d9b0e444c9dc2da8/src/jit/function-builder.cc#L770).

* * *

## BONUS Part 4: Set a different JIT compilation threshold

As was previously discussed, the JIT compiler will only compile a function after
it has been called a fixed number of times. By default, the current
implementation uses a threshold of 1, so functions will only be compiled after
being called once. In many applications, it's useful to be able to configure
the threshold. For example, the call-count threshold for JIT compilation can
be configured for `libc-interp` using the `--jit-threshold` option. For example:

```sh
env time ./libc-interp mandelbrot.wasm --jit-threshold 2
```

will compile functions after they have been called twice.

Try varying the threshold and see if you can find the "optimal" value for the
Mandelbrot example we've been using.

You will find that the "optimal" value for one application may not be the "optimal"
value for another application.  Choosing a general compilation threshold that behaves
reasonably well for different applications is a challenge for JIT compilers.  More
complex heuristics (such as method sampling) may be needed.

## BONUS Part 5: Generate a verbose log

A useful feature of the OMR compiler is its ability to log what it's compiling.
The functionality can be triggered in JitBuilder-based compilers by setting
(or exporting) the `TR_Options` environment variable to `verbose`. For example:

```sh
TR_Options=verbose env time ./libc-interp mandelbrot.wasm
```

Try experimenting with different JIT thresholds and see how the output changes.
What different functions get compiled?

Note: `TR_Options` is a general way of passing options to the OMR compiler.

## BONUS Part 6: Generate a trace log

In addition to tracking what is being compiled, the OMR compiler also logs what
the compiler is doing *while compiling* each function. As an example, run
the Mandelbrot example with `TR_Options` set as follows:

```sh
TR_Options=traceIlGen,traceFull,log=trtrace.log ./libc-interp mandelbrot.wasm
```

The `traceIlGen` option will cause the OMR compiler (and JitBuilder) to trace
IL generation. `traceFull` (which, un-intuitively, doesn't actually trace
everything) causes a few important steps of compilation to be traced. Among
other things, it provides information such as what optimizations are executed
and how the IL is transformed by each pass. Finally, `log=trtrace.log` will
cause the OMR compiler to dump the trace log to a file called `trtrace.log`.
Try opening the file in a text editor. **(Warning: it is possible for this file
to contain a few *million* lines!)**

For information about what the content of the log file means, take a look at the
[`Compilation Log`](https://github.com/eclipse/omr/blob/master/doc/compiler/ProblemDetermination.md#compilation-log)
section of `third_party/omr/doc/compiler/ProblemDetermination.md`.

* * *

## Exercise Solutions

### Solution Exercise 1: Calling the OMR Compiler from JitBuilder

```c++
JITedFunction compile(interp::Thread* thread, interp::DefinedFunc* fn) {
  TypeDictionary types;
  FunctionBuilder builder(thread, fn, &types);
  uint8_t* function = nullptr;

  if (compileMethodBuilder(&builder, &function) == 0) {
    return reinterpret_cast<JITedFunction>(function);
  } else {
    return nullptr;
  }
}
```

### Solution Exercise 2: Deciding When To Compile

```c++
bool Environment::TryJit(Thread* t, IstreamOffset offset, Environment::JITedFunction* fn) {
  if (!enable_jit) {
    *fn = nullptr;
    return false;
  }

  auto meta_it = jit_meta_.find(offset);

  if (meta_it != jit_meta_.end()) {
    JitMeta* meta = &meta_it->second;
    if (!meta->tried_jit) {
      meta->num_calls++;

      if (meta->num_calls > jit_threshold) {
        meta->jit_fn = jit::compile(t, meta->wasm_fn);
        meta->tried_jit = true;
      } else {
        *fn = nullptr;
        return false;
      }
    }

    *fn = meta->jit_fn;
    return trap_on_failed_comp || *fn;
  } else {
    *fn = nullptr;
    return trap_on_failed_comp;
  }
}
```

### Solution Exercise 3: Where To Compile

```c++
       case Opcode::Call: {
         IstreamOffset offset = ReadU32(&pc);
         Environment::JITedFunction jit_fn;

         if (env_->TryJit(this, offset, &jit_fn)) {
           TRAP_IF(!jit_fn, FailedJITCompilation);
           CHECK_TRAP(PushCall(pc));

           auto result = jit_fn();
           if (result != Result::Ok) {
             // We don't want to overwrite the pc of the JITted function if it traps
             tpc.Reload();

             return result;
           }

           PopCall();
         } else {
           CHECK_TRAP(PushCall(pc));
           GOTO(offset);
         }
         break;
       }

```

### Solution Exercise 4: Implement `buildIL`

```c++
bool FunctionBuilder::buildIL() {
  setVMState(new TR::VirtualMachineState());

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
```

### Solution Exercise 5: Implement `Return`

```c++
case Opcode::Return:
  b->Return(b->Const(static_cast<Result_t>(interp::Result::Ok)));
  return true;
```

### Solution Exercise 6: Implement `i32.sub` and `i32.mul`

```c++
case Opcode::I32Sub: {
  auto rhs = PopI32(b);
  auto lhs = PopI32(b);
  PushI32(b, b->Sub(lhs, rhs), pc);
  break;
}

case Opcode::I32Mul: {
  auto rhs = PopI32(b);
  auto lhs = PopI32(b);
  PushI32(b, b->Mul(lhs, rhs), pc);
  break;
}
```

### Solution Exercise 7: Implement `f32.sub` and `f32.mul`

```c++

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
```

### Solution Exercise 8: Implement `Call`

```c++
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
```
