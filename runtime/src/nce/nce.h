// Native code execution (NX_NCE builds, AArch64 hosts): the game's own code, read from the player's dump at run time
// (exefs/main, exefs/sdk), runs as is on the host CPU, like the NCE mode of Switch emulators. The build holds no
// recompiled game code: only the runtime (kernel, services, NVN on Vulkan, audio, video, input).
//
// What cannot run as is goes through the runtime:
//   svc #imm                 a stub next to the module saves the guest registers into the thread's CPU, runs the
//                            runtime's svc() on the host stack and resumes after the svc with every register intact
//   mrs/msr TPIDR_EL0,       the guest's TLS registers live in the thread's block (TPIDR_EL0 holds the host's thread
//   mrs TPIDRRO_EL0          pointer while the runtime runs, the block while the guest runs)
//   mrs CNTPCT_EL0           the virtual counter scaled to the Switch's 19.2 MHz (system_tick() reads the same)
//   calls to HLE functions   every thunk address (kThunkBase + 16 * index) is a stub into the same path
//   game hooks               hook(): the function's first instruction becomes a branch to such a stub
// and call() (guest_call) runs guest code from the runtime (nce_enter, back through the return stub).
//
// The block TPIDR_EL0 points to while guest code runs starts with a copy of the host thread's TLS slots (bionic's
// tp-16..tp+72: errno and pthread data, the stack guard, ART's thread), so a signal handler that runs in guest code
// (write watches, crashes) finds what it would find on the host side.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nx/runtime.h>

namespace nx::nce {

// The counter scale and offset and the HLE thunk stubs. Before any guest code runs (memory_init first).
void init();

// After a module is copied and relocated (loader.cpp): its svc/mrs/msr sites patched, its text made executable.
void prepare_module(const LoadedModule& m);

// Runs guest code at `target` on context c until it returns (call(), for every address outside the thunks).
void enter(CPU& c, uint64_t target);

// Replaces the guest function at `addr` (a module's text) with fn, called as an HLE function. Returns an address
// where the original function can still be called with call() (its first instruction moved, then a branch back),
// 0 if the function's first instruction cannot be moved.
uint64_t hook(uint64_t addr, HleFn fn);

// The Switch's 19.2 MHz system tick, from the host's virtual counter (what the patched mrs CNTPCT_EL0 returns).
uint64_t tick();

// Crash handler (platform_android.cpp): the guest registers when the fault is in guest code (true), from the signal's
// context, into c.
bool guest_fault_context(void* ucontext, CPU& c, uint64_t& pc);

}  // namespace nx::nce
