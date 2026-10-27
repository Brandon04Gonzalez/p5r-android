# Persona 5 Royal for Android (native port)

An Android app that runs **your own copy** of Persona 5 Royal (Nintendo Switch, version 1.0.0) natively on an arm64
phone or tablet. The Switch's operating system services, its audio, video and input, and NVN (the Switch's graphics
API) are reimplemented on Android, with Vulkan for the graphics. The game's own code runs directly on the phone's CPU:
the Switch and Android phones share the same CPU architecture (AArch64).

**No PC needed:** install the APK, pick your game file (`.nsp` or `.xci`) and your `prod.keys` on the phone, let it
prepare the game once (a few minutes), and play. The game is read from your file as it is; nothing is extracted.

> [!IMPORTANT]
> **No game files are included, here or in the APK.** This repository and its releases hold no game code, assets,
> shaders, keys or firmware. The APK is only the engine: it reads the game from your own dump. You need your own
> legally dumped copy of the game (the base game, version 1.0.0) and your own `prod.keys`. How to dump them from your
> console is **not** covered here. Please do not ask for game files or keys.

Persona 5 Royal is © ATLUS / © SEGA. This is an unofficial fan project, not affiliated with or endorsed by ATLUS,
SEGA or Nintendo.

## Install

1. Copy your **Persona 5 Royal 1.0.0** `.nsp` (or `.xci`) and your `prod.keys` to the phone (the Download folder is
   fine). The base game only: not the update.
2. Install **P5R** from the [Releases](../../releases) page (allow installing apps from your browser or file manager
   when Android asks).
3. Open it and allow "All files access" (for its folder `Internal storage/Persona 5 Royal`: settings, saves, shaders).
4. **Game file → Choose**: pick your `.nsp`/`.xci`. **Keys → Choose**: pick your `prod.keys` (and `title.keys` too, if
   your dump needs it; you can select both). The app keeps the keys in its private storage; they are not uploaded or
   included in the APK.
5. Press **Prepare the game**. Keep the app open: it checks the game and translates its ~30,000 shaders for your GPU
   (about two minutes on a mid-range tablet; once).
6. Press **Play**.

Keep the game file where it is: every start reads the game from it. Saves are kept in
`Internal storage/Persona 5 Royal/user/save`; the home screen can import and export them. Mods (LayeredFS style) can be
placed in `Internal storage/Persona 5 Royal/mods/<ModName>/romfs/...` (see [docs/MODS.md](docs/MODS.md)).

### Options

Text language, resolution (720p, or 1080p for stronger phones), interface resolution, graphics (texture detail),
frame rate cap (30, or 60 experimental), shadows, FPS counter, touch controls (show, size, opacity, and moving them),
and **Icon → Choose a picture**: a home screen shortcut with a picture of your own (Android does not let an app
change its own icon in the app drawer, but the shortcut looks and works like one).

## Requirements

- An arm64 Android phone or tablet with **Android 10 or newer** and **Vulkan 1.1**, 6 GB of RAM or more (8 GB
  recommended).
- Storage: the game file (about 15 GB) plus about 1 GB for the translated shaders and saves.
- Your own dump of **Persona 5 Royal 1.0.0** (title ID `01005CA01580E000`, build ID `708BCACA873EA32B`) and your
  console's `prod.keys`.

## Status

The no-PC flow (selecting the game and keys, preparing shaders, and launching from the NSP) has been verified on **one
device** so far:

| Device | SoC / GPU | Results |
|---|---|---|
| Lenovo Tab M11 (Android 14, 8 GB) | Snapdragon 685 / Adreno 610, Vulkan 1.1 | menus 60 fps, 3D title ~14 fps, the casino ~20 fps |

The Adreno 610 is a low-end GPU: on faster phones expect more. Other GPUs (Mali, PowerVR, Xclipse, newer Adreno) are
untested. Reports are welcome (see [Reporting problems](#reporting-problems)).

What works: the menus, films (hardware VP9), sound, cutscenes in sync, battles, saving and loading, 30 or 60 fps cap,
touch controls (movable) and game controllers, the text language of your choice.

Known limits: version 1.0.0 only (not the update); performance on low-end GPUs; the shadows option costs a lot on
weak GPUs (off by default).

## How it works

- **Native execution (the loader).** The APK reads `main` and `sdk` from your game file's ExeFS, maps and links them
  as the Switch's loader would, and runs them on the phone's CPU (`runtime/src/nce/`). The few instructions that cannot
  run on Android as they are get patched into small stubs: system calls (`svc`), the thread-pointer registers, and the
  system counter. Calls into Switch system libraries go to the runtime's implementations.
- **The game file, read in place.** The NSP/XCI is opened with your keys (`android/wonder/importer/nxgame.h`) and the
  game's files are decrypted on the fly as the game reads them (`runtime/src/game_image.cpp`).
- **Shaders** are translated once, on the phone, from the game's Maxwell shaders to SPIR-V with
  [Ryujinx](https://github.com/Ryubing/Ryujinx)'s shader translator (MIT), compiled to a native Android library with
  .NET NativeAOT (`libspirvharness.so`). The extraction is `games/p5r/import/`.
- **The runtime** is the Switch runtime of
  [Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (SwitchGamesRecomp, MIT,
  by BelmanteGu / Projeto Belmante). It provides the kernel, the services, NVN on Vulkan, audio (AAudio), hardware video
  decoding (MediaCodec) and touch controls. This port adds Persona 5 Royal's needs and the native loader.

The project started as a static recompilation (the game's code translated to C++), the upstream project's approach.
That build is still there for debugging on Windows (`games/p5r/tools/build_pc.cmd`), but it needs your own
recompiled game code, so it is private by nature. The Android APK is the native build (`NX_NCE`), which holds no game
code and can be shared. `games/p5r/tools/prepare_data.ps1` still prepares a data folder on a PC, for that build or
for testing.

## Building the APK

On Windows, with the Android SDK (`ANDROID_HOME`, or Android Studio's default location; platform 35, build-tools
35.0.0, NDK 28.2.13676358), a JDK 17+ (`JAVA_HOME`, or Android Studio's own), the Vulkan headers 1.4.357, and Docker
Desktop (once, for the shader translator library):
```powershell
python -m venv private\venv; private\venv\Scripts\pip install cmake ninja
git clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers private\third_party\Vulkan-Headers-1.4.357
# the translator: Ryubing's source, then the library (NativeAOT for Android needs Linux: a Docker container)
mkdir private\third_party\ryubing
curl.exe -L -o private\third_party\ryubing.tar.gz https://codeberg.org/Software_arch/Ryubing/archive/a82350bb77.tar.gz
tar -xzf private\third_party\ryubing.tar.gz -C private\third_party\ryubing --strip-components=1
docker run --rm -v "${PWD}:/src" -w /src mcr.microsoft.com/dotnet/sdk:10.0 sh games/p5r/tools/build_harness_android.sh
games\p5r\tools\build_android.cmd -Nce            # -> private\p5r\apk_nce\p5r.apk; -Install installs it with adb
```
The APK is signed with your Android debug key (`%USERPROFILE%\.android\debug.keystore`; Android Studio makes it, or
`keytool -genkeypair -keystore %USERPROFILE%\.android\debug.keystore -storepass android -keypass android -alias
androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -validity 10000`). Without `-Nce` the script
builds the recompiled flavour, which needs your own recompiled game code in `private\p5r\gen` (see `docs/RECOMP.md`)
and must never be shared.

## Reporting problems

Open an issue with your phone's model, SoC/GPU, Android version and RAM, what you did and what happened, and the log:
```
adb logcat -s p5r:V > p5r-log.txt
```
(run it before starting the game). Never attach game files, keys, or screenshots of copyrighted content beyond what
the report needs.

## En español

Una app de Android que ejecuta **tu propia copia** de Persona 5 Royal (Switch, versión 1.0.0) de forma nativa en un
móvil o tablet arm64. **No hace falta PC.** El APK no lleva nada del juego: lo lee de tu propio archivo.

1. Copia al móvil tu `.nsp` (o `.xci`) de Persona 5 Royal 1.0.0 (el juego base, sin la actualización) y tu `prod.keys`
   (por ejemplo a Descargas).
2. Instala **P5R** desde [Releases](../../releases).
3. Ábrelo y permite el acceso a todos los archivos.
4. **Archivo del juego → Elegir**: tu `.nsp`/`.xci`. **Claves → Elegir**: tu `prod.keys`. La app guarda las claves en
   su almacenamiento privado; no se suben ni se incluyen en el APK.
5. Pulsa **Preparar el juego** y deja la app abierta: comprueba el juego y traduce sus shaders (unos dos minutos, una
   sola vez).
6. Pulsa **Jugar**.

No borres ni muevas el archivo del juego: cada vez que juegas se lee de ahí. Puedes poner mods (estilo LayeredFS) en
`Almacenamiento interno/Persona 5 Royal/mods/<Nombre>/romfs/...` (ver [docs/MODS.md](docs/MODS.md)). En **Icono → Elegir
imagen** puedes crear un acceso directo con la imagen que quieras. No se proporcionan juegos ni claves.

## Credits and license

- Runtime: [SwitchGamesRecomp / Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp)
  by BelmanteGu (Projeto Belmante), MIT.
- Shader translation: Ryujinx's shader translator (Ryubing), MIT, built into the APK as a native library.
- The native-execution idea is the one Switch emulators call NCE; this implementation is written for this runtime.
- Like the upstream project, this port was written with a lot of AI assistance (Anthropic's Claude), directed and
  tested by a human.

MIT License, see [LICENSE](LICENSE).
