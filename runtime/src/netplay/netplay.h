// Netplay in the runtime: in this build there is no online play (netplay.cpp), so nothing here starts a session and
// the game plays as before.
#pragma once

namespace nx::netplay {

// boot(), before the guest starts: sets the session up from the settings and blocks for the handshake. On failure
// the game runs offline.
void start();
bool active();
// After each present, with g_frame at its new value (nvnQueuePresentTexture, after det::on_present): the state
// check, this device's input for frame + delay, then the stall until both inputs of this frame are known.
void on_frame();

}  // namespace nx::netplay
