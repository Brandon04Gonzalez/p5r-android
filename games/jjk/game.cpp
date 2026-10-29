// Experimental JJK target scaffold. This is metadata-only and is NOT a playable port.
// Do not add game-specific hooks until the target executable and renderer have been analyzed.
#include <nx/game.h>

namespace nx::game {

const Info kInfo = {
    "jjk",
    "Jujutsu Kaisen: Cursed Clash",
    0x01002FC012548000ull,
    "io.github.jjkrecomp",
    "private/jjk",
    0,  // NPDM system-resource size has not yet been verified from the user's dump.
    "A2834C16F37633B4"  // First 8 bytes of the observed NSO Build ID field; not yet runtime-validated.
};

void after_init() {
    // Intentionally empty: no JJK-specific guest hooks have been validated.
}

void renderer_start() {
    // Intentionally empty: renderer and shader path are still under investigation.
}

void first_frame() {
    // Intentionally empty: no successful JJK frame has been observed.
}

}  // namespace nx::game
