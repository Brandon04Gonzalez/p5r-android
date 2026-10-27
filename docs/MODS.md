# Mods (RomFS overlay, LayeredFS style)

Put mods in `Internal storage/Persona 5 Royal/mods/<ModName>/romfs/...`, with the same paths as inside the game's RomFS
(the same layout as Switch LayeredFS: `atmosphere/contents/<titleid>/romfs/...`).

- A file there replaces the game's file with the same path; a new file is added.
- Folders starting with `.` or `_` are ignored: rename `MyMod` to `_MyMod` to turn a mod off.
- Several mods apply in alphabetical order; the last wins.
- Paths are case sensitive on Android: match the game's case exactly.
- Mods are scanned each time the game starts. The log (`adb logcat -s p5r:V`) shows `fs: mod active: <name>` and
  `fs: mod file <path>` for each file served from a mod.
- The game file is never modified.
