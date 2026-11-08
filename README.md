# Persona 4 Arena Ultimax for Android (native port)

An Android app that runs **your own copy** of Persona 4 Arena Ultimax (Nintendo Switch) natively on an arm64 phone or
tablet. The Switch's operating system services, its audio, video and input, and NVN (the Switch's graphics API) are
reimplemented on Android, with Vulkan for the graphics. The game's own code runs directly on the phone's CPU: the
Switch and Android phones share the same CPU architecture (AArch64).

**No PC needed:** install the APK, pick your game file (`.nsp` or `.xci`) and your `prod.keys` on the phone, let it
prepare the game once, and play. The game is read from your file as it is; nothing is extracted.

> [!NOTE]
> **Online play is coming soon** as an option of its own. This release plays offline: Arcade, Story and Versus (also two
> players on one device with two controllers). Meanwhile, the best thing to do is train.

> [!IMPORTANT]
> **No game files are included, here or in the APK.** This repository and its releases hold no game code, assets,
> shaders, keys or firmware. The APK is only the engine: it reads the game from your own dump. You need your own
> legally dumped copy of the game and your own `prod.keys`. How to dump them from your console is **not** covered
> here. Please do not ask for game files or keys.

Persona 4 Arena Ultimax is © ATLUS / © SEGA, developed with Arc System Works. This is an unofficial fan project, not
affiliated with or endorsed by ATLUS, SEGA, Arc System Works or Nintendo.

## Community and support

- **Discord** (English and Spanish channels), for news, help and testing: https://discord.gg/rMgRneBTCe
- **Support the developer (optional):** [GitHub Sponsors](https://github.com/sponsors/Brandon04Gonzalez). These ports
  are free and made for preservation. Donating is entirely voluntary and helps with the costs of making them (test
  devices, hardware, servers). Nothing is locked behind a donation.

## Install

1. Copy your **Persona 4 Arena Ultimax** `.nsp` (or `.xci`) and your `prod.keys` to the phone (the Download folder is
   fine). The port is made for version 1.0.0 (v0), tested with the US release (`010075A016A3A000`).
2. Install **P4AU** from the [Releases](../../releases) page (allow installing apps from your browser or file manager
   when Android asks).
3. Open it and allow "All files access".
4. **Game file (.nsp / .xci) → Choose**: pick your game file. **Keys (prod.keys) → Choose**: pick your `prod.keys`
   (you can pick `prod.keys` and `title.keys` together). The app keeps the keys in its private storage; they are not
   uploaded or included in the APK.
5. Press **Prepare & Play**: the first time it translates the game's shaders on the phone (a few minutes, once). After
   that the button says **Play**.

Keep the game file where it is: every start reads the game from it (about 16 GB free for the file, a little more for
the shaders).

### Options

- **Resolution**: 720p (recommended) or 1080p.
- **Fullscreen**: "Yes (stretch)" fills a tall phone's screen with no black bars at the sides.
- **Skip frames when slow**: leave it on **Automatic**. On phones that cannot draw 60 pictures a second in fights, the
  game draws one in two when it falls behind, so fights keep their normal speed (no slow motion). Fast phones never
  skip.
- **On-screen controls**, their size, opacity and position (a layout editor). Bluetooth controllers work too.
- Custom GPU drivers (Turnip), save import/export, log export, and a home screen shortcut with a picture of your own.

## Requirements

- An arm64 Android phone or tablet with **Android 11 or newer** and **Vulkan 1.1**.
- Tested on a moto g54 5G (MediaTek Dimensity 7020, 8 GB) and a Lenovo Tab M11 (Snapdragon 680 / Adreno 610).

## Status

The menus, Arcade, Story (with its visual novel scenes), Versus, sound and films work. P4AU's logic counts frames, so a
phone that cannot draw 60 fps in fights uses frame skipping to keep the speed. Reports are very welcome, on Discord or
in [Issues](../../issues): your phone model, Android version, what happened and the log (**Export logs**).

## How it works

- **Native execution (the loader).** The APK reads `main` and `sdk` from your game file's ExeFS, maps and links them
  as the Switch's loader would, and runs them on the phone's CPU (`runtime/src/nce/`). The few instructions that cannot
  run on Android as they are get patched into small stubs: system calls (`svc`), the thread-pointer registers, and the
  system counter. Calls into Switch system libraries go to the runtime's implementations.
- **The game file, read in place.** The NSP/XCI is opened with your keys and the game's files are decrypted on the fly
  as the game reads them (`runtime/src/game_image.cpp`).
- **Shaders** are translated once, on the phone, from the game's Maxwell shaders to SPIR-V with
  [Ryujinx](https://github.com/Ryubing/Ryujinx)'s shader translator (MIT), compiled to a native Android library with
  .NET NativeAOT (`libspirvharness.so`).
- **The runtime** is the Switch runtime of
  [Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (SwitchGamesRecomp, MIT,
  by BelmanteGu / Projeto Belmante), shared with the [Persona 5 Royal port](https://github.com/Brandon04Gonzalez/p5r-android).
  This port adds P4AU's needs (`games/p4au/`): hwopus (libopus), the SDK 12 audio renderer, fighting-game controls,
  a second local player, frame skipping, and the native loader.

The project started as a static recompilation (the game's code translated to C++). That build is still there for
debugging on Windows (`games/p4au/tools/build_pc.cmd`), but it needs your own recompiled game code, so it is private by
nature. The Android APK is the native build (`NX_NCE`), which holds no game code and can be shared.

## Building the APK

The same toolchain as the [P5R port](https://github.com/Brandon04Gonzalez/p5r-android#building-the-apk), plus the
libopus 1.5.2 source in `private\third_party\opus-1.5.2`, then:
```powershell
curl.exe -L -o private\third_party\opus-1.5.2.tar.gz https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
tar -xzf private\third_party\opus-1.5.2.tar.gz -C private\third_party
games\p4au\tools\build_android.cmd -Nce           # -> private\p4au\apk_nce\p4au.apk; -Install installs it with adb
```

## En español

Una app de Android que ejecuta **tu propia copia** de Persona 4 Arena Ultimax (Switch) de forma nativa en un móvil o
tablet arm64. **No hace falta PC.** El APK no lleva nada del juego: lo lee de tu propio archivo.

> [!NOTE]
> **Muy pronto saldrá una opción para jugar en línea.** Esta versión es sin conexión: Arcade, Historia y Versus (también
> dos jugadores en un mismo dispositivo con dos mandos). Mientras tanto, recomendamos que
> vayan entrenando.

- **Discord** (canales en español e inglés), para novedades, ayuda y pruebas: https://discord.gg/rMgRneBTCe
- **Apoyar al desarrollador (opcional):** [GitHub Sponsors](https://github.com/sponsors/Brandon04Gonzalez). Los ports
  son gratis y se hacen para preservación. Donar es totalmente voluntario y ayuda con los gastos que conlleva hacerlos
  (dispositivos de prueba, equipo, servidores). No hay nada de pago.

1. Copia al móvil tu `.nsp` (o `.xci`) de Persona 4 Arena Ultimax (versión 1.0.0, probado con la de EE. UU.,
   `010075A016A3A000`) y tu `prod.keys` (por ejemplo a Descargas).
2. Instala **P4AU** desde [Releases](../../releases).
3. Ábrelo y permite el acceso a todos los archivos.
4. **Archivo del juego (.nsp / .xci) → Elegir**: tu archivo. **Claves (prod.keys) → Elegir**: tu `prod.keys` (puedes
   elegir `prod.keys` y `title.keys` juntos). La app guarda las claves en su almacenamiento privado; no se suben ni se
   incluyen en el APK.
5. Pulsa **Preparar y Jugar**: la primera vez traduce los shaders del juego en el teléfono (unos minutos, una sola
   vez). Después el botón dice **Jugar**.

No borres ni muevas el archivo del juego: cada vez que juegas se lee de ahí. En **Saltar cuadros si va lento** deja
**Automático**: en móviles que no llegan a 60 imágenes por segundo en las peleas, el juego dibuja una de cada dos para
que la pelea vaya a su velocidad normal. Si algo falla, cuéntalo en Discord o en [Issues](../../issues) con tu modelo
de móvil y el registro (**Exportar registros**). No se proporcionan juegos ni claves.

## Credits and license

- Runtime: [SwitchGamesRecomp / Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp)
  by BelmanteGu (Projeto Belmante), MIT.
- Shader translation: Ryujinx's shader translator (Ryubing), MIT, built into the APK as a native library.
- Opus decoding: [libopus](https://opus-codec.org/) (BSD-3-Clause). Custom GPU drivers:
  [libadrenotools](https://github.com/bylaws/libadrenotools) (BSD-2-Clause).
- The native-execution idea is the one Switch emulators call NCE; this implementation is written for this runtime.
- Like the upstream project, this port was written with a lot of AI assistance, directed and tested by a human.

MIT License, see [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
