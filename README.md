# Persona 5 Royal for Android (native port)

An Android app that runs **your own copy** of Persona 5 Royal (Nintendo Switch, version 1.0.0) natively on an arm64
phone or tablet. The Switch's operating system services, its audio, video and input, and NVN (the Switch's graphics
API) are reimplemented on Android, with Vulkan for the graphics. The game's own code runs directly on the phone's CPU:
the Switch and Android phones share the same CPU architecture (AArch64).

> [!IMPORTANT]
> **No game files are included, here or in the APK.** This repository and its releases hold no game code, assets,
> shaders, keys or firmware. The APK is only the engine: at start it reads the game's code (`exefs`) from your own
> dump, prepared on your PC. You need your own legally dumped copy of the game (the base game's NSP) and your own
> `prod.keys`. How to dump them from your console is **not** covered here. Please do not ask for game files or keys.

Persona 5 Royal is © ATLUS / © SEGA. This is an unofficial fan project, not affiliated with or endorsed by ATLUS,
SEGA or Nintendo.

## How it works

- **Native execution (the loader).** The APK reads `exefs/main` and `exefs/sdk` from your data folder, maps and links
  them as the Switch's loader would, and runs them on the phone's CPU (`runtime/src/nce/`). The few instructions that
  cannot run on Android as they are get patched into small stubs: system calls (`svc`), the thread-pointer registers,
  and the system counter. Calls into Switch system libraries go to the runtime's implementations.
- **The runtime** is the Switch runtime of
  [Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (SwitchGamesRecomp, MIT,
  by BelmanteGu / Projeto Belmante). It provides the kernel, the services, NVN on Vulkan, audio (AAudio), hardware video
  decoding (MediaCodec) and touch controls. This port adds Persona 5 Royal's needs and the native loader.
- **Shaders** are translated once on your PC, from the game's Maxwell shaders to SPIR-V, with
  [Ryujinx](https://github.com/Ryubing/Ryujinx)'s shader translator (MIT). `games/p5r/tools/prepare_data.ps1` does
  everything: it also imports the NSP.

The project started as a static recompilation (the game's code translated to C++), the upstream project's approach.
That build is still there for debugging on Windows (`games/p5r/tools/build_pc.cmd`), but it needs your own
recompiled game code, so it is private by nature. The Android APK is the native build (`NX_NCE`), which holds no game
code and can be shared.

## Status

Playable from the start of a new game, tested on **one device** so far:

| Device | SoC / GPU | Results |
|---|---|---|
| Lenovo Tab M11 (Android 14, 8 GB) | Snapdragon 685 / Adreno 610, Vulkan 1.1 | menus 60 fps, 3D title ~14 fps, the casino ~20 fps |

The Adreno 610 is a low-end GPU: on faster phones expect more. Other GPUs (Mali, PowerVR, Xclipse, newer Adreno) are
untested. Reports are welcome (see [Reporting problems](#reporting-problems)).

What works: the menus, films (hardware VP9), sound, cutscenes in sync, battles, saving and loading, 30 or 60 fps cap,
touch controls (movable) and game controllers, the text language of your choice. The native build (the APK here) has
been played so far from boot through a new game's first casino scenes; battles and saves were tested on the
recompiled build, which runs the same runtime around the same game code.

Known limits: version 1.0.0 only (not the update); performance on low-end GPUs; the shadows option costs a lot on
weak GPUs (off by default).

## Requirements

- An arm64 Android phone or tablet with **Android 10 or newer** and **Vulkan 1.1**, 6 GB of RAM or more (8 GB
  recommended), and about **16 GB** of free storage.
- A Windows 10/11 PC for the one-time data preparation, with Visual Studio 2022 Build Tools ("Desktop development with
  C++"), CMake and Ninja, Python 3.11+, the .NET 10 SDK, and about 20 GB free besides the NSP.
- Your own dump of **Persona 5 Royal 1.0.0** (title ID `01005CA01580E000`, the base game's NSP, build ID
  `708BCACA873EA32B`) and your console's `prod.keys` (and `title.keys` if your dump needs it).

## Install

1. **Prepare the data** on the PC, from a clone of this repository, in a "Developer PowerShell for VS 2022":
   ```powershell
   games\p5r\tools\prepare_data.ps1 -Nsp "D:\dumps\Persona 5 Royal.nsp" -Keys "D:\keys\prod.keys"
   ```
   It builds the importer and the translator, imports the NSP into `private\p5r\data` and translates the shaders
   (10 to 30 minutes). Everything stays under `private\`, which git ignores.
2. **Copy** `exefs\`, `extract\` and `spirv\` from `private\p5r\data` to the phone, into a folder named
   **`Persona 5 Royal`** at the top of its internal storage (`Internal storage/Persona 5 Royal/exefs/main`, and so on).
3. **Install the APK** from the [Releases](../../releases) page, open **P5R**, and allow "All files access" when it
   asks (it needs it to read that folder).
4. The home screen says "Data ready". Pick the options and press **Play**.

Saves are kept in `Internal storage/Persona 5 Royal/user/save`; the home screen can import and export them.

### Options

Text language, resolution (720p, or 1080p for stronger phones), interface resolution, graphics (texture detail),
frame rate cap (30, or 60 experimental), shadows, FPS counter, touch controls (show, size, opacity, and moving them).

## Building the APK

On Windows, with the Android SDK (`ANDROID_HOME`, or Android Studio's default location; platform 35, build-tools
35.0.0, NDK 28.2.13676358), a JDK 17+ (`JAVA_HOME`, or Android Studio's own), and the Vulkan headers 1.4.357:
```powershell
python -m venv private\venv; private\venv\Scripts\pip install cmake ninja
git clone --depth 1 --branch v1.4.357 https://github.com/KhronosGroup/Vulkan-Headers private\third_party\Vulkan-Headers-1.4.357
games\p5r\tools\build_android.cmd -Nce            # -> private\p5r\apk_nce\p5r.apk; -Install installs it with adb
```
The APK is signed with your Android debug key (`%USERPROFILE%\.android\debug.keystore`; Android Studio makes it, or
`keytool -genkeypair -keystore %USERPROFILE%\.android\debug.keystore -storepass android -keypass android -alias
androiddebugkey -dname "CN=Android Debug,O=Android,C=US" -keyalg RSA -validity 10000`). Without `-Nce` the script builds the recompiled flavour, which needs
your own recompiled game code in `private\p5r\gen` (see `docs/RECOMP.md`) and must never be shared.

## Reporting problems

Open an issue with your phone's model, SoC/GPU, Android version and RAM, what you did and what happened, and the log:
```
adb logcat -s p5r:V > p5r-log.txt
```
(run it before starting the game). Never attach game files, keys, or screenshots of copyrighted content beyond what
the report needs.

## En español

Una app de Android que ejecuta **tu propia copia** de Persona 5 Royal (Switch, versión 1.0.0) de forma nativa en un
móvil o tablet arm64. El APK no lleva nada del juego: al arrancar lee el código del juego (`exefs`) del volcado que
preparas en tu PC.

1. En el PC (Windows), con este repositorio clonado: `games\p5r\tools\prepare_data.ps1 -Nsp "<juego base>.nsp" -Keys "<prod.keys>"`.
2. Copia `exefs\`, `extract\` y `spirv\` de `private\p5r\data` al móvil, a la carpeta `Persona 5 Royal` en la raíz del
   almacenamiento interno.
3. Instala el APK de [Releases](../../releases), ábrelo, permite el acceso a todos los archivos y pulsa **Jugar**.

Solo la versión 1.0.0 del juego base (sin la actualización). No se proporcionan juegos ni claves.

## Credits and license

- Runtime: [SwitchGamesRecomp / Super Mario Bros. Wonder Recomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp)
  by BelmanteGu (Projeto Belmante), MIT.
- Shader translation: Ryujinx's shader translator (Ryubing), MIT, downloaded and built by `prepare_data.ps1`.
- The native-execution idea is the one Switch emulators call NCE; this implementation is written for this runtime.
- Like the upstream project, this port was written with a lot of AI assistance (Anthropic's Claude), directed and
  tested by a human.

MIT License, see [LICENSE](LICENSE).
