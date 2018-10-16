# TURBO18: Boost WABT Performance using JitBuilder

## WABT Overview


## JitBuilder Overview

* * *

## Part 1: Dispatching the JIT

### Exercise 1: Calling the OMR Compiler from JitBuilder

#### Background

The main task involved in creating a JIT compiler using JitBuilder is to
translate the VM/interpreter's representation of functions into the OMR
compiler's Intermediate Language (known as TR IL), a process known as IL generation. In
JitBuilder, IL generation is implemented by subclassing `OMR::MethodBuilder` and
overriding the required functions. To actually compile a function/method,
`compileMethodBuilder()` is called with an instance of the `MethodBuilder`
subclass, an instance of a `TypeDictionary` (an object used to describe
non-primitive types to JitBuilder), and a pointer to a variable in which the
entry point to a function will be stored, if compilation is successful.
`compileMethodBuilder()` returns 0 if compilation succeeds and some non-0 value
otherwise.

In wasmjit-omr, the `MethodBuilder` subclass is `wabt::jit::FunctionBuilder`.
A subclass of `TypeDictionary` is also implemented as
`wabt::jit::TypeDictionary`. In later sections, you will complete parts of
`FunctionBuilder` to practice generating IL using JitBuilder.

#### Your Task

Complete the implementation of `wabt::jit::compile()`.

```c++
JITedFunction compile(interp::Thread* thread, interp::DefinedFunc* fn) {
  TypeDictionary types;
  FunctionBuilder builder(thread, fn, &types);

  // YOUR CODE HERE

  return nullptr;
}
```

Given an interpreter "thread" and a defined function object, `compile()` will
(unconditionally) invoke the JIT compiler and return the entry point to the
generated body. If JIT compilation fails, `nullptr` is returned instead.

#### What To Do

Complete `compile()` by:

- defining a variable of type `uint8_t *` to store the entry point to the JIT
compiled body
- calling `compileMethodBuilder()` with `types`, `builder`, and the address
of the variable used to store the entry point as arguments
- returning the entry point cast to `JITedFunction` if `compileMethodBuilder()`
returns `0`, `nullptr` (or `NULL`) otherwise


### Exercise 2: Deciding When To Compile

#### Background

A key aspect of maximizing performance using a JIT compiler is to control when
the JIT compiler is invoked. Compiling too often can mean a VM spends more time
compiling than executing application code.

A simple way to limit what/when functions get compiled is to only compile
function if they have been called a fixed number of times; a so-called "counted
compilation".

Another thing that VMs must handle is the case when JIT compilation fails. The
JIT may fail to compile a function if, for example, the function uses a
language feature not supported by the compiler. In such cases, the function
must always be interpreted.

`JitMeta` is a struct used to keep track of information about WebAssembly
functions that is useful for controlling JIT compilation:

```
struct JitMeta {
  DefinedFunc* wasm_fn;    // pointer to objection for the wasm function
  uint32_t num_calls = 0;  // the number of times the function has been called

  bool tried_jit = false;          // whether we've already tried JITing this function
  JITedFunction jit_fn = nullptr;  // pointer to the entry point of the compiled body

  JitMeta(DefinedFunc* wasm_fn) : wasm_fn(wasm_fn) {}
};
```

#### Your Task

Complete the implementation of `Environment::TryJit()`.

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

Given an interpreter thread `t`, an instruction offset `offset`, and a
pointer to a pointer to a function `fn` (used as an in-out parameter), `TryJit()`
will try to JIT compile the function at `offset` if:

1. the JIT compiler is enabled
2. there is an entry for it in the meta-data table
3. there has never been a previous attempt to compile the function (preventing
recompilation of functions for which a previous compilation failed)
4. the function has been called at least the number of times set in `jit_threshold`
(a member of the `Environment` class)

If all conditions for compilation are met, the JIT compiler is invoked by
calling `jit::compile()` and the `tried_jit` field of the function's metadata is
set to `true`.

If compilation succeeds, the `jit_fn` field of the function's metadata and the
variable pointed to by the `fn` parameter are set to the entry point returned by
`jit::compile()`.

#### What To Do

- check `meta->tried_jit` is false, indicating that we have never tried to
compile this function
- increment `meta->num_calls` (the number of times this function has been called)
- check if `meta->num_calls` is greater than `jit_threshold`
- if it is
    - call `jit::compile(t, meta->wasm_fn)`
    - assign the returned value to `meta->jit_fn`
    - set `meta->tried_jit` to true
- if not
    - set the variable pointed to be `fn` to `nullptr`
    - return false to indicate compilation failure


### Exercise 3: Where To Compile

#### Background

The last piece of the puzzle is to actually call the JIT compiler (`TryJit()`)
and to call the JIT compiled function. In many VMs, it makes sense to simply do
these calls where the interpreter handles calls. When a program is about to
call a function, the interpreter will invoke the JIT and, if compilation
succeeds, call the entry point returned by the JIT.

#### Your Task

Complete the WABT interpreter's handling of the `Call` opcode to call `TryJit()`
and to call the entry point to the compiled body when successful.

```c++
       case Opcode::Call: {
         IstreamOffset offset = ReadU32(&pc);

         if (false) {

           if (true) {
             // We don't want to overwrite the pc of the JITted function if it traps
             tpc.Reload();

             return result;
           }

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

Complete the implementation of `FunctionBuilder::buildIL()`.

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
corresponding the current `FunctionBuilder` object.

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
item at the given index in `workItems_` is retireved and IL is generated for
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

* * *

## Part 3: Implement a few WASM opcodes

### Exercise 5: Implement `Return`

#### Background

To generate IL, JitBuilder provides "services" that must be called on `IlBuilder`
instance. Sub-classes of `IlBuilder` include `BytecodeBuilder`, `MethodBuilder`,
and sub-classes of these that are implemented by JitBuilder users (e.g.
`FunctionBuilder`).

Each JitBuilder service generates IL that represents a particular action. For
example, the `Return()` service generates IL representing "returning from a
function". When no arguments are passed, the generated IL represents simple
return, without return value. An argument, of type `IlValue`, can be passed to
represent a returned value.

`IlValue` is a class used by JitBuilder to represent values that "computed" by
the generated IL or, more precisely, values computed by the code *generated*
from the IL by the compiler. JitBuilder provides various services for generating
`IlValue` instances. For instance, `ConstInt32(int32_t)` generates the
representation of a constant, 32-bit integer value. All the arithmetic
operation services take as arguments and return instances of `IlValue`. Memory
load operations produce `IlValue` instances while memory store operations take
an instance as argument.

#### Your Task

Implement IL generation for the `Return` opcode.

```c++
case Opcode::Return:
  return false;
```

The generated IL must represent "return the value `interp::Result::Ok`".

Because not other instructions from the function are expected to be executed,
`Emit()` can simply return true after successfully generating IL for `Return`.

#### What To Do

- static cast the enum value `interp::Result::Ok` to the underlying integer type
`Result_t`
- generate an IlValue for the constant by calling `Const()` on the current
`BytecodeBuilder` instance `b`, passing the constant itself as argument
- generate the IL for the return by calling `Return()` on the builder `b`,
passing the `IlValue` instance as argument
- change `return false` to `return true` :)


### Exercise 6: Implement `i32.add`, `i32.sub`, `i32.mul`

#### Background

Being a stack-based VM, the WABT interpreter performs pushes and pops to the
operand stack every time an operation is performed. For convenience,
`FunctionBuilder` provides some helpers to generate IL for operand stack pushes
and pops.

`Pop(IlBuilder*, const char*)` generates IL corresponding to a stack pop. The
first argument is a pointer to the `IlBuilder` object that should be used to
generate the IL. The second argument is the name of the expected type of the
value popped. It returns an `IlValue` instance representing the value popped
from the stack.

`Push(IlBuilder*, const char*, IlValue*, const uint8_t)` generates IL
representing a stack push. The first and second arguments are the `IlBuilder`
object to be used and name of the type of the value being pushed, respectively.
The third argument is the `IlValue` instance representing the value being pushed.
Finally the last argument is the `pc` pointing to the instruction performing the
push. It is used generate a trap if the stack overflows because of the push.

The `TypeFieldName<T>()` helper can be used to get the string name of a C++ type
`T`. Only types that can be used in WebAssembly are supported and passing any
other types to the template function will result in a build error.

#### The task

Implement the 32-bit integer `Add`, `Sub`, and `Mul` opcodes.

```c++
case Opcode::I32Add:
  return false;

case Opcode::I32Sub:
  return false;

case Opcode::I32Mul:
  return false;
```

The IL generated for every binary arithmetic operation must:

- pop the RHS from the operand stack
- pop the LHS from the operand stack
- compute the operation
- push the result onto the operand stack

Because the instructions that follow the arithmetic operations must also be
processed, once IL is successfully generate, each `case` must `break`
out of the `switch` statement instead of `return`.

For convenience, the `EmitBinaryOp` templated function may be used:

```c++
template <typename T, typename TResult = T, typename TOpHandler>
void EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h);
```

The arguments are:

- `T`: the type of operation being emited (e.g. `int32_t` for 32-bit integer
binary operations)
- `TResult`: the type of the result of the operation (same as `T` by default)
- `TOpHandler`: type of the callable object (object) that generates IL for the
operation
- `builder`: a pointer to the builder object on which pushes and pops should be
generator
- `pc`: the current ("virtual") pc pointing to the instruction for which IL is
being generated
- `operation`: a lambda (or other callable object) that generates the only the
IL for the operation. The lambda is given as arguments the IlValues
corresponding to the operation operands and is expected to return the IlValue
corresponding to the result: `IlValue* lambda(IlValue* lhs, IlValue* rhs)`.

#### What To Do

- pop the RHS by calling `Pop()` with `b` and `TypeFieldName<int32_t>()` (or
"i32") as arguments
- pop the LHS following the style used to pop the RHS
- generate the appropriate computation using `b->Add()`, `b->Sub()`, or
`b->Mul()` with the LHS and RHS as arguments
- push the resulting `IlValue` using `Push()` with `b`, `TypeFieldName<int32_t>()`
(or "i32"), the `IlValue` instance itself, and `pc` as arguments
- `break` out of the `switch`

Or, alternatively:

- call `EmitBinaryOp<>()` with `int32_t` as the template argument, `b` and `pc`
as second and third arguments, respectively, and a lambda function that:
    - captures by reference
    - takes two `IlValue *`s as arguments
    - calls `Add()`, `Sub()`, or `Mul()` on `b`, forwarding the lambda's arguments
    - returns the resulting `IlValue` instance
- `break` out of the `switch`


### Exercise 7: Implement `Call`

#### Background

Both the interpreter and JIT compiled code must be capable of calling the
following entities:

- interpreted (not yet JIT compiled) functions
- JIT compiled function
- the JIT compiler itself

To call an interpreted function from JIT compiled code, a common strategy is
for the JIT compiled code to simply the interpreter, pointing its pc to the
first instruction in the function.

To avoid having to generate IL that represents all this logic, we can instead
generate a call to a so-called *runtime helper* that will take care of all the
complexity.

In WABT, the following is already implemented for you:

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

Complete IL generation for the `Call` opcode.

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

The `Call()` service returns an `IlValue` instance representing the value
returned by the function call. It takes as arguments:

- the name of the function to be called (must match the name used in `DefineFunction`)
- the number of arguments to be passed
- `IlValue` instances representing the values of the arguments (as a vararg)

The value returned by the function must then be checked for a trap values. If
it is a trap, then the value must be propagated back by returning from the
wasm function that IL is being generated for. The `EmitCheckTrap()` can be used
to generated IL representing the required trap handling. As arguments, it takes
a builder object, IlValue representing the value to be checked (return value of
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

* * *

## Exercise Solutions

### Exercise 1: Calling the OMR Compiler from JitBuilder

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

### Exercise 2: Deciding When To Compile

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

      if (meta->num_calls >= jit_threshold) {
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

### Exercise 3: Where To Compile

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

### Exercise 4: Implement `buildIL()`

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

### Exercise 5: Implement `Return`

```c++
case Opcode::Return:
  b->Return(b->Const(static_cast<Result_t>(interp::Result::Ok)));
  return true;
```

### Exercise 6: Implement `i32.add`, `i32.sub`, `i32.mul`

```c++
case Opcode::I32Add:
  EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
    return b->Add(lhs, rhs);
  });
  break;

case Opcode::I32Sub:
  EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
    return b->Sub(lhs, rhs);
  });
  break;

case Opcode::I32Mul:
  EmitBinaryOp<int32_t>(b, pc, [&](TR::IlValue* lhs, TR::IlValue* rhs) {
    return b->Mul(lhs, rhs);
  });
  break;
```

### Exercise 7: Implement `Call`

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
