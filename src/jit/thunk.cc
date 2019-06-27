#include "thunk.h"
#include "function-builder.h"
#include "thread.h"
#include "src/cast.h"
#include "src/interp/interp.h"

namespace wabt {
namespace jit {

Result_t InterpThunk(ThreadInfo* th, Index ind) {
  auto* env = th->thread->env();
  auto* func = cast<interp::DefinedFunc>(env->GetFunc(ind));

  th->pc = func->offset;
  th->thread->set_pc(func->offset);
  th->thread->call_stack_top_ = th->call_stack - th->thread->call_stack_.data();
  CHECK_TRAP_HELPER(env->TryJit(th->thread, func, ind));

  if (func->jit_fn_) {
    return func->jit_fn_(th, ind);
  } else {
    th->thread->in_jit_ = false;

    auto last_jit_frame = th->thread->last_jit_frame_;
    th->thread->last_jit_frame_ = th->thread->call_stack_top_;

    interp::Result result = interp::Result::Ok;
    while (result == wabt::interp::Result::Ok) {
      result = th->thread->Run(1000);
    }
    th->thread->last_jit_frame_ = last_jit_frame;

    th->call_stack = th->thread->call_stack_.data() + th->thread->call_stack_top_;
    if (result != interp::Result::Returned) {
      th->pc = th->thread->pc_;
      th->in_jit = th->thread->in_jit_;

      return static_cast<Result_t>(result);
    }

    th->thread->in_jit_ = true;
    th->jit_fn_table = th->thread->env()->jit_funcs_.data();

    return static_cast<Result_t>(interp::Result::Ok);
  }
}

Result_t HostCallThunk(ThreadInfo* th, Index ind) {
  auto* func = cast<interp::HostFunc>(th->thread->env()->GetFunc(ind));
  auto result = th->thread->CallHost(func);

  th->jit_fn_table = th->thread->env()->jit_funcs_.data();

  return static_cast<Result_t>(result);
}

}
}
