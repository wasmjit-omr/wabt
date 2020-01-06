#ifndef JIT_THUNK_HPP
#define JIT_THUNK_HPP

#include "src/jit/environment.h"

namespace wabt {
namespace jit {

Result_t InterpThunk(ThreadInfo* th, Index ind);
Result_t HostCallThunk(ThreadInfo* th, Index ind);

}
}

#endif
