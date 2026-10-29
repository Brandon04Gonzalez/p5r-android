# Jujutsu Kaisen: Cursed Clash (NCE bring-up)

This target is a clean starting point for the Japanese base title:

- Program ID: `01002FC012548000`
- Target: `jjk`
- Android package: `io.github.jjkport`
- Data root: `private/jjk`

## Important status

This is **not yet a playable JJK port**. It deliberately does not copy Persona 5 Royal's game hooks, timing patches, shader assumptions, build ID, or shader extraction pipeline. Those are game/version-specific and copying them would just turn one crash into several less obvious crashes.

The generic NCE runtime can be compiled for this target, but the exact JJK base-game `main` build ID and runtime behavior still need to be confirmed from the owner's own dump and device log. The build ID is intentionally unset until verified. This target also does not yet include an on-device importer/launcher UI.

## Build

From the repository's Windows developer shell:

```powershell
android\build_apk.ps1 -Game jjk -Nce
```

The build produces `private\jjk\apk_nce\jjk.apk`. The runtime expects the game's imported data in the folder selected by its storage configuration; this minimal target does not import an NSP/XCI by itself.

## Next validation steps

1. Capture the JJK base dump's main NSO build ID and SDK/module list from the import output.
2. Confirm the app's actual data-root lookup and adapt the importer UI to JJK's title ID.
3. Boot the generic NCE target and collect the first full logcat/runtime log.
4. Add only evidence-backed JJK-specific hooks and shader handling after the first boot identifies the next blocker.
