#ifndef JIT_THREAD_HPP
#define JIT_THREAD_HPP

#include "src/common.h"
#include "environment.h"

namespace wabt {
namespace interp {
struct CallFrame;
class Thread;
}

namespace jit {

struct ThreadInfo {
  uint32_t pc;
  uint32_t in_jit;

  interp::CallFrame* call_stack_max;
  interp::CallFrame* call_stack;

  JITedFunction* jit_fn_table;

  interp::Thread* thread;
};

}
}

#endif // JIT_THREAD_HPP
