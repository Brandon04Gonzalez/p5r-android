// Netplay in the runtime, offline build: this build has no online play, so a session never starts and the game always
// plays on this device alone. The deterministic mode (determinism.h) stays: it is not network code.
#include "netplay.h"

namespace nx::netplay {

void start() {}

bool active() { return false; }

void on_frame() {}

}  // namespace nx::netplay
