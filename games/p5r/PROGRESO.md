# Port nativo de Persona 5 Royal a Android — progreso

Última actualización: 2026-10-21 (sesión 4, noche)

## Estado en una línea

**P5R se juega en Android** (Lenovo Tab M11, Adreno 610, Vulkan 1.1) a **30 fps** con **controles táctiles**, **sonido**
y **vídeos por hardware**: arranque, título 3D, Nueva partida, la película de apertura completa y el **casino jugable**
(Joker se mueve, salta y escala; diálogos en español). Hay **launcher** con las opciones pedidas. Pendiente: terminar el
casino (primera batalla), verificar el parche de 60 fps en juego, guardado, y rendimiento (rama de optimización).

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

### Sesión 3b (2026-10-21, tarde) — Android
21. `nn::movie` HLE (`runtime/src/hle_movie.cpp`): decodificador con la API de CRI (eventos de entrada/salida,
    `GetOutputBuffer` copia el frame en la memoria de CRI, NV12 compacto stride = ancho). En Android usa
    `AMediaCodec` (VP9 por hardware); en PC frames negros. Texturas pitch-linear ya no se "limpian" (vídeo verde).
22. Build Android: `games/p5r/AndroidManifest.xml`, `tools/build_android.cmd [-Install]`, `tools/run_android.ps1`
    (escribe config.txt, lanza, copia nx.log a `private/p5r/runs/<n>`). Datos ya copiados a la tablet en
    `/storage/emulated/0/Persona 5 Royal` (analysis, extract/romfs, spirv, config.txt).
23. Arreglos para la tablet: `-march=armv8-a+crypto` (SIGILL en A53/A73), Vulkan < 1.3 sin structs 1.3 y con render
    pass objects, `shaderDrawParameters`, quitar interlock del SPIR-V, no `vkCmdClearColorImage` en BC (fallo de GPU
    Adreno), write-watch serializado, `CancelSynchronization` no se pierde (cuelgue al terminar el vídeo).
24. **Idioma del dispositivo**: `language.h` usa `plat::system_language()` (Android: locale; Windows: idioma del
    usuario) si no hay `language=`/`language.txt`. Español → `es`. Probado en PC (`am: desired language = es`).

Sin verificar en la tablet (compilado solo en PC al cerrar la sesión): reintento del write-watch con rangos
retirados (`write_watch.cpp`; el juego crasheaba al escribir frames de vídeo en `main+1017210`), el arreglo de
`CancelSynchronization` (`kernel.cpp`) y el idioma. Para probar: `build_android.cmd -Install`, luego
`run_android.ps1 -Name x -Seconds 25 -Keep` y pulsar A con `adb shell input keyevent --duration 250 KEYCODE_BUTTON_B`.
Capa de validación Vulkan: `private/third_party/vvl` (se activa con `settings put global gpu_debug_*`; ya desactivada).

### Sesión 4 (2026-10-21, noche) — controles táctiles, sonido, vídeo, launcher
25. **Controles táctiles de P5R** (`runtime/src/touch_layout.cpp`, `platform_android_input.cpp`): rombo A/B/X/Y abajo a
    la derecha, L/ZL y R/ZR en las esquinas, −/+ arriba, cruceta a la izquierda; la mitad izquierda es el stick (aparece
    donde cae el pulgar) y el resto de la mitad derecha mueve la **cámara** (stick derecho). Etiquetas dibujadas por el
    shader de presentación (`tools/shaders/present/overlay.glsl`, compartido con FSR para que no desaparezcan con
    `NX_FSR=1`). Se atenúan tras 8 s sin tocar (vídeos). Un toque corto se mantiene 150 ms (si no, el juego lo perdía).
    Teclado: Enter/Espacio = A, Esc = B (la tablet Lenovo usa BUTTON_B para la captura del lápiz).
26. **Sonido**: P5R usa AudioOut (no AudioRenderer). Su mezclador CRI preguntaba `GetAudioOutSampleRate/ChannelCount/
    SampleFormat/BufferDataSize/Name` a las versiones del SDK, que leían campos que nuestro `OpenDefaultAudioOut` no
    rellena: todo salía en silencio. Implementados en `hle_audio.cpp`; ahora suenan BGM, vídeos y voces (el log da
    `audio: AudioOut: peak N, x of 1000 buffers silent`).
27. **Vídeo que se congelaba** (película del casino tras Nueva partida, a los 424 frames): CRI espera el evento de
    salida antes de alimentar más, y los frames que el códec soltaba después de la última llamada nunca se anunciaban.
    Un hilo invitado (`__movie_pump_thread`, `hle_movie.cpp`) vacía el códec cada 2 ms y señala el evento: decodifica a
    30 fps (antes ~15).
28. **Launcher** (`games/p5r/java/io/github/p5rrecomp/Launcher.java`): comprueba los datos y el permiso de archivos,
    opciones guardadas en `config.txt`: **Resolución 720p** (defecto) / 1080p, **30 fps** (defecto) / 60 fps,
    **contador de FPS desactivado** (defecto), controles táctiles sí/no, tamaño y opacidad de los botones.
29. **Contador de FPS** (`NX_SHOW_FPS=1`): dibujado por el mismo shader (dígitos en `overlay.glsl`), arriba a la izquierda.
30. **60 fps** (`NX_FPS_CAP=60`, `games/p5r/game.cpp`): un hilo reescribe el intervalo del motor según el cheat que viene
    con el dump (`[[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14` = 1). En los menús y la película esa cadena acaba en 0
    (la estructura aún no existe); falta confirmar que se aplica en el campo.
31. Herramientas de prueba: `games/p5r/tools/tab.py` (capturas, botones, toques, stick/cámara con `input motionevent`),
    `private/p5r/newgame.sh` (de arrancar al casino), `explore.sh`, `sheet.py` (hoja de capturas tras cada movimiento).

Notas de la sesión 4:
- Menú de título: [Nueva partida] [Cargar partida] [??]; el cursor empieza en Cargar: IZQUIERDA + A = Nueva partida
  (esperar ~6 s tras entrar al menú, si no se pierde la pulsación).
- Casino: tras la primera lámpara, "Saltar hacia abajo" mirando a cámara en la viga de la pantalla roja; luego plataforma
  rosa → "Escalar" → agentes ("¡Ahí está!") → saltar a los pilares hexagonales → pasarela curva de cristal. Ahí me quedé
  (no encontré el siguiente punto de acción; la cámara es fija).
- Tirones de 100–350 ms al cargar escenas (informe `stall:` en el log): para la rama de optimización.

## Falta ❌ (por orden)

1. Terminar el casino (primera batalla: comprobar render de combate) y seguir la historia.
2. Confirmar el parche de 60 fps en el campo; si no, buscar el campo con `NX_WATCH`.
3. Guardar/cargar partida (`user/` en la carpeta de datos).
4. Servicios IPC que pida P5R y Wonder no usaba (`bsd:u`, `ectx:aw`, varios `cmd ... not implemented`).
5. **GPUs**: tabla de quirks por vendorID (Adreno 0x5143, Mali 0x13B5, Xclipse 0x144D, PowerVR 0x1010, Maleoon) y
   probar en el Moto G54 (PowerVR, pero solo 4,3 GB libres).
6. Rendimiento (rama `p5r-opt`): tirones de carga, `NX_REPLAY_THREAD=1`, caché de pipelines.

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
