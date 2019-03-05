#include <cstddef>
#include "JitBuilder.hpp"
#include "ilgen/VirtualMachineState.hpp"

// This file contains hacky nonsense that makes VirtualMachineState work correctly because
// *somebody* decided not to have a fallback for if no client callback was registered for
// VirtualMachineState::MergeInto. So we have to manually register a passthrough callback that
// simply calls VirtualMachineState::MergeInto on the implementation object. If we don't do this,
// then WabtState::MergeInto will simply never be called, breaking a bunch of stuff.
//
// This hack should be removed as soon as either JitBuilder fixes this nonsense or we update to
// using the client API.

namespace wabt {
namespace jit {

static void vmsHack_MergeInto(OMR::JitBuilder::VirtualMachineState* self, OMR::JitBuilder::VirtualMachineState* other, OMR::JitBuilder::IlBuilder* b) {
  static_cast<TR::VirtualMachineState*>(self->_impl)->MergeInto(static_cast<TR::VirtualMachineState*>(other->_impl), static_cast<TR::IlBuilder*>(b->_impl));
}

void initializeVmsHack(TR::VirtualMachineState* vms) {
  vms->setClientCallback_MergeInto(reinterpret_cast<void*>(vmsHack_MergeInto));
}

}
}
