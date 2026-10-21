# Port nativo de Persona 5 Royal a Android — progreso

Última actualización: 2026-10-21 (sesión 3)

## Estado en una línea

**En PC (Windows, RTX 4060) el juego ya se ve y se puede manejar**: icono de carga "Take Your Heart", aviso de
fotosensibilidad, diálogo de red THIEVES GUILD (responde a los botones), el **logo 3D de ATLUS renderizado en tiempo
real**, el logo de ATLUS y el de P-STUDIO, en 720p y en 1080p. **Se cuelga al empezar el primer vídeo** (la intro):
`nn::movie` no está implementado. Android aún no se ha compilado.

## Cómo funciona el port

No es un emulador: es **recompilación estática**. El código ARM64 de la versión de Switch (que es tu dump) se
convierte en C++ y se compila de forma nativa. Las llamadas al sistema y a la GPU se implementan en el host.

- Herramienta base: **SwitchGamesRecomp**, el runtime de
  [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) (MIT). Reimplementa NVN (la API
  gráfica de Switch) sobre Vulkan y ya tiene target Android.
- Lo específico de cada juego va en `games/<juego>/`. P5R va en `games/p5r/`.
- Se depura primero en **PC (Windows)** porque es mucho más rápido que en el teléfono; el mismo código luego se
  compila para Android.

## Dónde está todo

| Qué | Ruta |
|---|---|
| Repo de trabajo (sin espacios en la ruta: el NDK falla con espacios) | `C:\Users\brand\p5r-port\recomp` |
| Código propio del port de P5R | `C:\Users\brand\p5r-port\recomp\games\p5r\` |
| Datos derivados del juego (nunca se publican) | `C:\Users\brand\p5r-port\recomp\private\` |
| Tus dumps | `C:\Users\brand\Downloads\persona 5 royal port android\` |
| Ejecutable de PC | `private\p5r\build\p5r.exe` |
| Logs de cada prueba | `private\p5r\runs\<nombre>\nx.log` (+ `frames\*.ppm`) |
| SPIR-V traducido | `private\p5r\spirv\` (29.703 `.spv` + `results.tsv`) |

> Nada de `private\` ni ningún APK/EXE construido con tus datos debe subirse ni compartirse.

## Comandos (PC)

```powershell
# compilar (MSVC + clang-cl del NDK; la primera vez configura CMake). ~15 min la primera vez, segundos después
cmd /c C:\Users\brand\p5r-port\recomp\games\p5r\tools\build_pc.cmd -j14
# ejecutar una prueba: log en private\p5r\runs\<Name>\nx.log
C:\Users\brand\p5r-port\recomp\games\p5r\tools\run_pc.ps1 -Name prueba1 -Seconds 90 -Headless -DumpEvery 150
#   -Docked = 1080p (por defecto 720p), -Env "NX_X=1;NX_Y=2" = switches extra, sin -Headless abre ventana
#   -Keys "30:A:0.3;40:DOWN:0.3" = botones simulados (segundos:botón:duración). NO meter NX_INPUT_SCRIPT en -Env:
#   -Env corta por ';' y solo llegaría la primera pulsación (eso confundió media sesión 3)
```

Switches útiles: `NX_TRACE_DRAWS=<n>` + `NX_TRACE_COUNT=<n>` (traza draws), `NX_TRACE_PROGRAMS=1` (etapas de cada
programa), `NX_DUMP_SHADER_MISSES=1` (vuelca shaders sin SPIR-V a `private\p5r\shader_misses`),
`NX_DUMP_DRAW_TEXTURES=<fs hash>@<draw>` (vuelca las texturas que lee un draw), `NX_THREAD_DUMP_AT=<s>` (pila de
cada hilo + x0..x2 y handle: qué mutex/evento espera), `NX_WATCH=<dir hex>` (watchpoint de escritura).

## Hecho ✅

### Sesión 1
1. Investigación (NVN, motor GFD/"needle" de Atlus, SDK 13.3.2, CRIWARE dentro de `main`, `subsdk0` = `nn::movie`).
2. Toolchain: NDK 28.2, build-tools 35, .NET 10, venv Python, headers Vulkan 1.4.357, traductor Ryubing.
3. Importador en Windows (`games/p5r/tools/win_compat/`), extracción del dump 1.0.0, lector de CPK (`cpk.py`).
4. CPU recompilada: `main` (46.419 funciones) y `sdk` (39.279), 0 instrucciones sin soportar.

### Sesión 2 (2026-10-21)
5. **Shaders: 29.703 traducidos a SPIR-V, 0 fallos** (SPIR-V 1.3, vale para Vulkan 1.1).
   - El extractor viejo emparejaba mal un tercio de los bloques de los GSC. Ahora `gsc_shaders.py` lee los
     registros `HNSH V002` directamente (offsets fijos: control en +0x24+[+0x54], código en +0x24+[+0x58], tamaño
     en +0x5c). Los 30.418 registros cuadran → 29.387 bloques.
   - Grupos extra: `exefs` (164, embebidos en `main`), `runtime_misses` (146 volcados por el runtime) y
     `compute_noheader` (6 compute shaders sin cabecera NVN; el harness ahora los reconoce:
     `tools/maxwell/spirv_harness/Program.cs`).
6. **`games/p5r/game.cpp`**: title ID `01005CA01580E000`, paquete `io.github.p5rrecomp`, datos en `private/p5r`,
   stubs de `glslc*`, y `fputs` copiado al log (así salen los mensajes de aserción del juego).
7. **Build de Windows** con el `clang-cl` del NDK (VS no tiene "C++ Clang tools"): `games/p5r/tools/build_pc.cmd`.
   Script de pruebas: `games/p5r/tools/run_pc.ps1`.
8. **Fibers de `nn::os`** (nuevo `runtime/src/hle_fiber.cpp`): P5R usa fibers como corrutinas en el hilo principal.
   Cada fiber del juego corre sobre un fiber real del host (Windows: API de fibers; Android/Linux/macOS: cambio de
   contexto en ensamblador AArch64/x86-64), guardando/restaurando los registros del CPU invitado.
9. **NVN**: `nvnDeviceRegisterFastClear*` devuelve `true` (P5R lo trataba como error fatal).
10. **hid** (`runtime/src/hle_hid.cpp`): `InitializeTouchScreen`, `GetTouchScreenState`, `GetSixAxisSensorState`,
    `StopSixAxisSensor`, `ResetSixAxisSensorFusionParameters`, `SetNpadJoyAssignmentModeDual`.
11. **Carga lenta arreglada**: P5R crea ~30.000 programas al cargar. `nvnProgramSetShaders` ahora es perezoso
    (`runtime/src/nvn/nvn_program.cpp`): guarda el código y ~100 bytes del control, y lee el `.spv` la primera vez
    que se usa. Antes: minutos de pantalla negra y 4–10 fps; ahora 30 fps estables desde el principio.
12. Diagnóstico: el informe de "stall" muestra en qué función HLE está cada hilo (`kernel.h`, `loader.cpp`,
    `kernel_watch.cpp`).
13. **RE de `nn::movie`** (para el vídeo): es un port de stagefright, API calcada de `MediaCodec` de Android. Datos
    sacados de los símbolos de `subsdk0`:
    - `Decoder` (0x10 bytes, vtable): +0x0 D2, +0x8 D0, +0x10 Configure(MediaData*), +0x18 RegisterClientEvents,
      +0x20 GetOutputFormat, +0x28 Start, +0x30 Stop, +0x38 Reset, +0x40 Flush, +0x48 AcquireInputBufferIndex(int*,int*),
      +0x50 GetInputBuffer(int, Buffer*), +0x58 SendInputBufferForDecode(idx, off, size, int64 time, uint32 flags; 4 = fin),
      +0x60 AcquireOutputBufferIndex(int*, int64*, uint32*, int*), +0x68 GetOutputBuffer(int, Buffer*),
      +0x70 GetOutputBufferSize, +0x78 ReleaseOutputBufferIndex(int), +0x80 GetLastError,
      +0x88 GetOutputBuffer(int, Buffer*, MediaData*), +0x90 GetOutputBufferFormat(int, MediaData*).
    - `Buffer` (0x40): +0 offset, +4 size, +8 int32, +0xc uint32, +0x10 owns, +0x18 capacity, +0x20 int64,
      +0x28 base, +0x30 nvnDev, +0x38 nvnSync.
    - `MediaData` = {AMessage*} (8 bytes), claves usadas por el juego: `width`, `height`, `stride`, `crop`,
      `nv12-y-stride`, `nv12-y-offset`, `nv12-uv-stride`, `nv12-uv-offset`; la config VP9 pone `mime`
      (`video/x-vnd.on2.vp9`), `width`, `height`, `max-input-size`.
    - Lo usa el backend `CriVpdSwitch` de CRI Mana (`main+1276000..127a000`); salida NV12.

### Sesión 3 (2026-10-21) — primera imagen y primeros menús
14. **Programa por etapa** (`nvn_draw_state.*`, `nvn_draw.cpp`): `nvnCommandBufferBindProgram` respeta la máscara de
    etapas (P5R enlaza un programa solo-vertex y otro solo-fragment). Compilado y probado: los draws ya tienen
    fragment shader.
15. **`nvnCommandBufferCopyBufferToTexture`** (nuevo en `nvn_copy.cpp` + `upload_region` en `nvn_upload.cpp`): así
    sube P5R *todas* sus texturas (buffer de staging → textura). Wonder no lo usaba y era un stub vacío: todas las
    texturas salían a cero. Convierte igual que la subida normal (BC/ASTC decodificados si la GPU no los tiene,
    R11G11B10F ensanchado, R8 de glifos como RGBA8). Más de 2.000 copias en el arranque, 0 fallidas.
16. **`nvnCommandBufferClearTexture`** (nvn_copy.cpp): textura entera → `vkCmdClearColorImage`; los rectángulos
    parciales se registran en el log y se ignoran (no han aparecido).
17. **Tabla de formatos NVN** (`nvn_texture_layout.cpp`): estaba en un offset fijo del SDK 16.2 de Wonder
    (`sdk+0xd34198`), fuera del módulo de P5R → todo formato salía 1×1 de 4 bytes (tamaños de textura y bytes por
    bloque mal). Ahora se busca por firma; en P5R está en `sdk+0xcd0d90`.
18. **Modo de profundidad** (`nvn_device.cpp`): `nvnDeviceSetDepthMode(1)` = z de recorte 0..w (como Vulkan). El
    runtime asumía siempre el modo 0 de Wonder (−w..w) y lo remapeaba; ahora solo si el juego no pide el modo 1.
    `nvnDeviceSetWindowOriginMode` registrado (P5R usa el 1, arriba-izquierda, igual que Wonder).
19. Volcado de hilos (`NX_THREAD_DUMP_AT`) con x0..x2 y handle de cada hilo; `run_pc.ps1 -Keys`.
20. Probado con botones simulados: el input funciona en modo portátil y TV, sin cambios en `hle_hid.cpp`.

Notas de la sesión 3 (para no repetir trabajo):
- La pantalla de carga negra con un rombo blanco = texturas a cero (punto 15).
- El hilo de render de P5R (`main+1025e04`) toma el mutex del dispositivo GFD durante `nvnQueuePresentTexture`. Con
  `NX_REPLAY_THREAD` apagado (por defecto) el replay de GPU y el present de Vulkan ocurren *dentro* de esa llamada,
  así que el hilo cargador (`main+fb3d28`, espera en `main+1014da8`) solo entra una vez por frame. No impide
  avanzar, pero puede alargar las cargas: probar `NX_REPLAY_THREAD=1` si las cargas son lentas en el teléfono.
- Rutina de mando de P5R: `main+fb8310` (lee por `main+fc7090`/`fc6930`), botones en `main+0x2326f7a`, combinados
  en `+0x2326f52`, flancos en `+0x2326f4a`. Mapa Npad→PS4: A→círculo(0x2000), B→cruz(0x4000), X→triángulo, Y→cuadrado.
- `nvnCommandBufferSetPrimitiveRestart(cmd, 1, 0xffff)` sigue siendo un stub: si aparecen triángulos basura en
  strips, implementarlo (clave de pipeline + `primitiveRestartEnable`).

## Falta ❌ (por orden)

1. **`nn::movie` HLE — lo siguiente**. El juego ya llega aquí: tras el logo de P-STUDIO llama a
   `movie::BufferC1`, `SetCoreMask`, `Set/GetThreadPriority` y `movie::Decoder::Decoder(type, format, mode, int)`
   (`main+12785d8`); como el import devuelve 0, crashea con dirección 0x18 (vtable nula). Primero un decodificador
   falso que devuelva frames NV12 negros con las marcas de tiempo correctas (para que el juego avance), luego
   `AMediaCodec` VP9 en Android (y quizá libvpx en PC). API: punto 13.
2. Servicios IPC que pida P5R y Wonder no usaba (ver `nx.log`: `bsd:u`, `ectx:aw` y varios `cmd ... not implemented`).
3. **Opciones pedidas** (conectar en el launcher de P5R): 720p por defecto (`NX_HANDHELD=1`), opción 1080p;
   **30 fps por defecto** (`NX_FPS_CAP=30`), opción 60 (parche del cheat: cadena `main+0x2327B60 → +0x10 → +0x60 →
   +0x8`, escribir `1` en `+0xF14` para 60 o `2` para 30); contador de FPS desactivado por defecto.
4. **Launcher Android `android/p5r/`** (copiar `android/wonder/`; ajustar `build_apk.sh/.ps1`).
5. **GPUs**: el runtime exige Vulkan 1.3; los dos dispositivos de prueba tienen 1.1. Fallback con
   `NX_RENDER_PASS_OBJECTS=1` (no hay `VK_KHR_dynamic_rendering` ni `VK_EXT_extended_dynamic_state` en la Tab M11),
   y tabla de quirks por vendorID (Adreno 0x5143, Mali 0x13B5, Xclipse 0x144D, PowerVR 0x1010, Maleoon).
6. **Compilar la `.so` de Android** y copiar ~15 GB de datos.
7. Arrancar en el dispositivo, leer `nx.log` y depurar.

### Dispositivos de prueba

| Dispositivo | SoC / GPU | Vulkan | Libre | Notas |
|---|---|---|---|---|
| Moto G54 (`ZY22J6G9TK`) | Dimensity 7020 / PowerVR BXM | 1.1 | ~4,3 GB | sin espacio para los datos |
| **Lenovo Tab M11 (`HA269JK2`, TB331FC)** | Snapdragon 685 (SM6225) / **Adreno 610** | **1.1.128** (instancia 1.3) | **23 GB** | Android 14, 8 GB RAM, 1200×1920; BC sí, `depth_clip_control` no, `synchronization2` y `robustness2` sí |

## Riesgos conocidos

- Vídeos VP9 (`nn::movie`): trabajo nuevo, pero la API mapea casi 1:1 a `AMediaCodec`.
- Diferencias de SDK (13.3.2 vs el de Wonder): más servicios/funciones sin implementar irán apareciendo.
- Rendimiento en gama media (Adreno 610 / PowerVR BXM) y Vulkan 1.1.

## Comandos útiles (Python)

```bash
cd /c/Users/brand/p5r-port/recomp
P=private/venv/Scripts/python
$P games/p5r/tools/cpk.py private/p5r/extract/romfs/CPK/ALL_USEU.CPK --list --grep X   # buscar archivos
$P games/p5r/tools/gsc_shaders.py --cpk private/p5r/extract/romfs/CPK/ALL_USEU.CPK --main private/p5r/analysis/main.bin --out private/p5r/shaders_extracted
dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll --batch private/p5r/shaders_extracted --out private/p5r/spirv --jobs 8 --no-val
```
