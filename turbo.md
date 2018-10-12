# TURBO: Boost WABT Performance using JitBuilder, SPLASH 2018

## Dispatching the JIT

### Calling the OMR compiler from JitBuilder

#### Background

The main task envolved in creating a JIT compiler using JitBuilder is to
translate the VM/interpreter's representation of functions into the OMR
compiler's Intermediate Langauge (IL); a process known as IL generation. In
JitBuilder, IL generation is implemented by subclassing `MethodBuilder` and
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

#### The task

Complete the implementation of `wabt::jit::compile()`.

```c++
JITedFunction compile(interp::Thread* thread, interp::DefinedFunc* fn) {
  TypeDictionary types;
  FunctionBuilder builder(thread, fn, &types);
  
  // YOUR CODE HERE
  
  return nullptr;
}
```

Given and interpreter "thread" and a defined function object, `compile()` will
(unconditionally) invoke the JIT compiler and return the entry point to the
generated body. If JIT compilation fails, `nullptr` is returned instead.

#### What to do

Complete `compile()` by:

- defining a variable of type `uint8_t *` to store the entry point to the JIT
compiled body
- calling `compileMethodBuilder()` with `types`, `builder`, and the address
of the variable used to store the entry point as arguments
- returning the entry point cast to `JITedFunction` if `compileMethodBuilder()`
returns `0`, `nullptr` (or `NULL`) otherwise

#### Possible Solution

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

### Deciding when to compile

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

### The task

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

#### What to do

- check `meta->tried_jit` is false, indicating that we have never tried to
compile this function
- increment `meta->num_calls` (the number of times this function has been called
- check if `meta->num_calls` greater than `jit_threshold`
- if it is
    - call `jit::compile(t, meta->wasm_fn)`
    - assign the returned value to `meta->jit_fn`
    - set `meta->tried_jit` to true
- if not
    - set the variable pointed to be `fn` to `nullptr`
    - return false to indicate compilation failure

#### Possible Solution

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

### Where to compile

#### Background

The last piece of the puzzle is to actually call the JIT compiler (`TryJit()`)
and to call the JIT compiled function. In many VMs, it makes sense to simply do
these calls where the interpreter handles calls. When a program is about to
call a function, the interpreter will invoke the JIT and, if compilation
succeeds, call the entry point returned by the JIT.

#### The task

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

#### What to do

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

#### Possible Solution

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

## Implement buildIL()

`buildIL()` is the function called by the OMR compiler to generate IL. It should
return `true` when IL generation succeeds, `false` otherwise.

```c++
bool FunctionBuilder::buildIL() {
  setVMState(new TR::VirtualMachineState());

  const uint8_t* istream = thread_->GetIstream();

  workItems_.emplace_back(OrphanBytecodeBuilder(0, const_cast<char*>(ReadOpcodeAt(&istream[fn_->offset]).GetName())),
                          &istream[fn_->offset]);
  AppendBuilder(workItems_[0].builder);

  return false;
}
```

### What's already done

First, we set a new instance of `VirtualMachineState` on the current
`FunctionBuilder` instance. `VirtualMachineState` is used to simulates state
changes that would happen if the function being compiled were to run int the VM.
The rest of the JIT implementation does not use this feature (yet!), so
instantiating the default base class is good enough.

`workItems_` is an array of structs. Each struct holds, among other things, a
pointer to a `BytecodeBuilder` instance. `BytecodeBuilder`s are used to generate
the IL corresponding to a specific instruction/bytecode in a function. The call
to `workItems_.emplace_back()` inserts an instance corresponding to the
first bytecode in the function being compiled. Importantly, notice that the
first argument to `OrphanBytecodeBuilder()` is `0`, indicating that the builder
instance corresponds to the byte code with index 0; the first bytecode in the
function being compiled.

The call to `AppendBuilder()` adds the newly created `BytecodeBuilder` instance
to the internal work list. The work list keeps track of which opcodes from the
function still need to be handled. Internally, the JIT will call
`AppendBuilder()` every time a new bytecode is encountered that requires IL
generation.

### What you need to do

To complete `buildIL()`, you need generate IL for every `BytecodeBuilder` in the
JitBuilder work list. You should use the following helpers provided:

- `GetNextBytecodeFromWroklist()` will return the index (insdie `workItems_`)
of the next bytecode to be handled. If no more bytecodes need to be handled -1
is returned instead.
- `Emit(builder, istream, pc)` will generate IL for a particular opcode on the
specified builder instance. Both the builder and `pc` can be retrieved from the
`workItems_` array using `workItems_[index].builder` and `workItems_[index].pc`.
The `istream` instance that is already provided can be used for the second
argument. `true` is returned if IL generation for the bytecode succeeded,
`false` otherwise.

**Remember** to return `true` if `buildIL()` succeeds and `false` otherwise!

#### Possible Solution

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

## Implement a few opcodes

### `Return`

```c++
case Opcode::Return:
  return false;
```

The provided implementation for the `Return` opcode just returns `false`,
causing IL generation, and hence compilation, to fail.

A JIT compiled body is expected to return an `interp::Result` value. For a
normal return (no error or trap), the value `interp::Result::Ok` should be
returned.

Use the JitBuilder `Return()` service to generate IL for the function return.
To return a value, generate the IL representation of the value and pass it as
argument to `Return()`. The `Const()` service generate the IL representation of
a constant value. The service takes as argument the value of the constant,
*which must be of a primitive type*. Use these services to implement the return
opcode.

**Hint:** `interp::Result` is a C++11 enum class and `Result_t` is a typedef for
the underlying integer type of the enum.

**Remember:** JitBuilder services must be called on an *Builder instance, which
in this case is `b`, not `this`

**Remember:** Once IL is generate for `Return`, `Emit()` should return `true` to
signal success

#### Possible Solution

```c++
case Opcode::Return:
  b->Return(b->Const(static_cast<Result_t>(interp::Result::Ok)));
  return true;
```

### `i32.add`, `i32.sub`, `i32.mul`

```c++
case Opcode::I32Add:
  return false;

case Opcode::I32Sub:
  return false;

case Opcode::I32Mul:
  return false;
```

To implement these opcodes, JitBuilder provides the following services:
`IlBuilder::Add()`, `IlBuilder::Sub()`, and `IlBuilder::Mul()`. For convenience,
the templated helper function `EmitBinaryOp<T>(builder, pc, operation)` takes
care of pop the operands and pushing the result of the operation.

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

**Hint:** you can pass the current builder `b` to `EmitBinaryOp`

**Hint:** the pc is stored in a variable called `pc`

**Remember:** instead of returning `false` after generating IL for the opcodes,
we only need to `break` out of the `switch` to allow the function to complete.
(The final code just ensures the that the opcodes that follow the current one
are added to the work list for processing.)

#### Possible Solution

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

### `Call`

```c++
case Opcode::Call: {
  auto th_addr = b->ConstAddress(thread_);
  auto offset = b->ConstInt32(ReadU32(&pc));
  auto current_pc = b->Const(pc);

  return false;
}
```

Because of the complexity involved in handling calls (i.e. calling the JIT,
dispatching JITed code vs interpreted code, etc.), we instead call a
*runtime helper* that will handle all this for us. The helper is called,
straightforwardly, `CallHelper`. It takes three arguments: a pointer to the
current `interp::Thread` instance, the offset of the function to be called, and
the current pc. For convenience, these are already provided to you as `th_addr`,
`offset`, and `current_pc`, respectively. `CallHelper` also returns a
`interp::Result`, which must be checked and trap values propagated.

Use the JitBuilder `Call()` service to generate call handling. You can use the
`EmitCheckTrap()` helper to generate IL to handle checking the value returned
by `CallHelper`. As arguments, it takes a builder object, IlValue representing
the value to be checked (return value of `CallHelper`), and `nullptr`.
(Actually, the last argument is a pointer to the pc that must be updated.
However, because we have *already* returned from the called function, there is
no need to update the pc.)

#### Possible Solution

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

## Implement `EmitBinaryOp`

```c++
template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h) {
}
```

To recap, `EmitBinaryOp<T>(builder, pc, operation)` takes care of pop the
operands and pushing the result of the operation. It's arguments are:

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

Use the provided `Push()` and `Pop()` helpers to implement this function. Both
a builder object as first argument and type name as second argument. `Push()`
also takes the IlValue to be "pushed" as third argument. To get the name of type
use the templated helper function `TypeFieldName()`. For example,
`TypeFiledName<int32_t>()` will return the name corresponding to the 32-bit
integer type.

#### Possible Solution

```c++
template <typename T, typename TResult, typename TOpHandler>
void FunctionBuilder::EmitBinaryOp(TR::IlBuilder* b, const uint8_t* pc, TOpHandler h) {
  auto* rhs = Pop(b, TypeFieldName<T>());
  auto* lhs = Pop(b, TypeFieldName<T>());

  Push(b, TypeFieldName<TResult>(), h(lhs, rhs), pc);
}
```

## Bonus: Implement a virtual stack