// Jujutsu Kaisen: Cursed Clash (JP base title 01002FC012548000).
// Keep this target deliberately free of P5R hooks: offsets, shader assumptions and HLE workarounds from Persona 5
// Royal are not valid for this game. Add JJK-specific behavior only after it is confirmed from this dump's logs.
#include <nx/game.h>

namespace nx {
const game::Info game::kInfo = {
    "jjk",
    "Jujutsu Kaisen: Cursed Clash",
    0x01002FC012548000ull,
    "io.github.jjkport",
    "private/jjk",
    0,
    nullptr // Fill only after confirming the base game's main NSO build ID from the user's dump.
};

void game::after_init() {}
void game::renderer_start() {}
void game::first_frame() {}
} // namespace nx
