// Per-player input for the npads the game reads (hle_hid.cpp): a second local player now, netplay later.
// Player k is npad id k (No1, No2); a connected player reports the full-key style.
//
// Sources of player 1: the host pads (XInput pad 0 + keyboard on Windows; game controllers + touch on Android),
// the NX_INPUT frame lane and the NX_INPUT_SCRIPT wall-clock lane. Player 2: XInput pad 1 (Windows), the NX_INPUT_P2
// frame lane and the NX_INPUT_SCRIPT_P2 wall-clock lane. Player 2 is connected while one of those exists, or remote
// input is fed for it; NX_P2=1 / NX_P2=0 forces it on or off. NX_INPUT_REMOTE=1 feeds the frame lanes through
// set_remote_input instead (one frame ahead, host pads ignored): the netplay path without a network.
#pragma once

#include <cstdint>

#include "platform.h"

namespace nx::hid {

constexpr int kPlayers = 2;
constexpr uint64_t kRemoteFrames = 256;  // how far ahead (and back) remote input is kept, per player

// Feeds `player`'s pad (buttons as plat::k* bits, sticks -kStickMax..kStickMax) for presented frame `frame`, that is
// the value of nx::g_frame while the game's logic for that frame reads its npads. This is the netplay core's hook:
// from the first call on, that player's npad shows exactly the inputs fed, frame by frame, and none of the player's
// local sources; a frame with nothing fed reads as a neutral pad (counted in the log). The player counts as
// connected meanwhile. Up to kRemoteFrames ahead; callable from any thread.
void set_remote_input(int player, uint64_t frame, const plat::PadState& p);
void clear_remote_input(int player);  // the player's local sources drive it again
// What the player's local sources give now (host pads and that player's script lanes at the current frame), without
// any remote input: what netplay sends for the local side.
plat::PadState read_local(int player);
bool connected(int player);  // whether the game sees the player's controller (player 1: always)

}  // namespace nx::hid
