# Port nativo de Persona 5 Royal a Android — progreso

Última actualización: 2026-10-23 (sesión 8)

## Estado en una línea

**P5R se juega en Android** (Lenovo Tab M11, Adreno 610, Vulkan 1.1) a **30 fps** con **controles táctiles**, **sonido**
y **vídeos por hardware**: arranque, título 3D, Nueva partida, la película de apertura completa y el **casino jugable**
hasta la primera batalla (ganada). Hay **launcher** con las opciones pedidas (incluida **Gráficos**, Bajos por defecto).
Sesión 5: las películas ya no se congelan, el sonido se retrasa para ir con la imagen cuando hay pocos fps, y la opción
de **60 fps funciona** (en PC sube de 30 a 45–60). Sesión 6: **los tirones de las películas, arreglados** (la imagen de
la luminancia se quedaba sin subir; punto 44). Pendiente: **rendimiento** (Adreno 610: título 3D ~7 fps, casino ~12 fps,
limitado por la GPU; siguiente paso, el culling por oclusión real, punto 45), opción de sombras, objetos que desaparecen
un frame (punto 39) y seguir la historia. Rama de trabajo **`p5r-opt`** (el APK instalado en la tablet es el de esa rama).

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
    con el dump (`[[[[main+0x2327B60]+0x10]+0x60]+0x8]+0xF14` = 1). **Aún sin efecto**: en menús, película y casino la
    cadena acaba en 0 (`[X+0x60]` apunta a `X+0x68` y `[X+0x70]` = 0: un contenedor vacío que en la consola tendría un
    elemento). Los 11 accesos a `+0xF14` del código de `main` son campos de cámara/CRI, no sirven para encontrarlo.
31. Herramientas de prueba: `games/p5r/tools/tab.py` (capturas, botones, toques, stick/cámara con `input motionevent`),
    `private/p5r/newgame.sh` (de arrancar al casino), `explore.sh`, `sheet.py` (hoja de capturas tras cada movimiento).

Notas de la sesión 4:
- Menú de título: [Nueva partida] [Cargar partida] [??]; el cursor empieza en Cargar. `private/p5r/title_pick.py`
  lo resuelve solo (detecta por color la pantalla de inicio, el menú, el resaltado y la lista CARGAR). Con el vídeo
  arreglado la intro dura menos: una A de más abre CARGAR.
- Guardado: `user/save/SYSTEM/SYSTEM.DAT` se escribe bien (datos de sistema). Las partidas, en el primer punto de
  guardado de la historia (no alcanzado aún).
- Casino: tras la primera lámpara, "Saltar hacia abajo" mirando a cámara en la viga de la pantalla roja; luego plataforma
  rosa → "Escalar" → agentes ("¡Ahí está!") → saltar a los pilares hexagonales → pasarela curva de cristal. Ahí me quedé
  (no encontré el siguiente punto de acción; la cámara es fija). En otra partida, desde los pilares hexagonales
  ("0 1", "1 1" + A) Joker siguió por una pasarela rosa con diálogos del equipo por el auricular hasta una lámpara
  redonda con pantallas (cámara fija, Joker casi fuera de cuadro).
- **Explorador automático** (`private/p5r/auto_explore.py SEGUNDOS [cada]`, `STEP_MS=250..450`): camina en una
  dirección pulsando A tras cada paso, gira cuando no hay corte de cámara y guarda una hoja de miniaturas en
  `private/p5r/shots/auto.png`. Así pasó el casino: lámparas → pasarela de cristal → letrero CASINO → **primera
  batalla (ganada con A; combate y pantalla VICTORY se ven bien)** → salón de suelo a cuadros → pasillo ("¡Venga,
  corre!") → almacén de jaulas azul ("Área de personal"). Ahí el explorador se pierde: abrir el mapa con **R** (se ve
  la salida, una flecha al sur del pasillo de jaulas, y dos puertas amarillas); la puerta doble del cuarto oeste no da
  aviso de abrir. En la 2.ª partida automática volvió a ganar la batalla y a llegar aquí (el port aguanta ~40 min
  seguidos sin fallos).
- Combate en la tablet: se ve bien (efectos de fuego, VICTORY); los comandos con A (atacar) bastan para ganarlo.
- Tirones de 100–350 ms al cargar escenas (informe `stall:` en el log): para la rama de optimización.

### Sesión 5 (2026-10-22) — películas, A/V, 60 fps, opción de gráficos

32. **Películas que se congelaban ~4 s una y otra vez** (la de apertura tras Nueva partida: "tirones en cinemáticas").
    CriVpdSwitch (`main+1276000..127a000`) se queda con los fotogramas que toma hasta mostrarlos y deja de mandar datos
    cuando tiene casi todos los huecos de salida: con 8 iba ~4 s y se paraba ~4 s (7 de 8 retenidos, 0 entradas). Con
    **16 huecos** (`hle_movie.cpp`, `NX_MOVIE_OUTPUTS`) decodifica a 30 fps de principio a fin (película de 50 s medida).
    Nuevas medidas en el log: `movie: per s: sent/decoded/acquired/got/released ...` cada 2 s, `movie: no input for ...`
    con volcado de hilos (`NX_MOVIE_DUMP=<n>`) y `video: N of 60 presents showed a new picture ...` (lo que se ve).
    `screenrecord` no funciona en la tablet mientras el VP9 a 1080p está abierto (el códec no arranca); durante el juego
    sí, a `--size 640x400`.
33. Queda un tirón pequeño en películas: 1–3 fotogramas cada 2 s se ven 100–170 ms (dos presents sin imagen nueva). No es
    el audio (el dispositivo AAudio pide cada ~4 ms sin cortes) ni la decodificación (30/s). Siguiente: la línea `video:`
    ya cuenta presents > 40 ms (¿frames lentos del juego?) y CRI elige la imagen por su reloj.
34. **A/V a pocos fps** ("desfase de audio"): la imagen de un frame llega a pantalla ~3 periodos después de la lógica (el
    juego un frame por delante, dos envíos en vuelo en la GPU); el sonido ~100 ms después. A 30 fps cuadra; a 12 fps la
    imagen iba ~150 ms por detrás del sonido. Ahora el audio sale de un anillo propio (`platform_android_audio.cpp`) con
    un retardo de 3 × (periodo − 33,3 ms), suavizado y hasta 250 ms (`nvn_queue.cpp` `av_sync`, log `av sync:`;
    `NX_AV_SYNC=0` lo quita). Los búferes del juego vuelven a ritmo real (su reloj no cambia). Probado a ~6 fps: 150 ms y
    subiendo, sin cortes. **Sin confirmar a oído.**
35. **FPS objetivo del juego**: `[[main+0x2209f50]+0x84]` (30), leído por `main+f73730` en cientos de sitios y fijado por
    `main+f73740(fps)` (que reinicia el temporizador `[[main+0x220a9c0]]`). Cada frame `main+fd4140` pone el intervalo
    del motor a 60/fps con el setter virtual `main+1048ee0` (1..4, `obj+0x30`), que el present (`main+1025c9c`) pasa a
    `nvnWindowSetPresentInterval`. `games/p5r/game.cpp` engancha ese setter (tabla de funciones): **la opción de 60 fps
    ahora funciona** (PC: 45–60 fps en vez de 30; velocidad de juego sin verificar). `NX_P5R_FPS=15|20|30|60` para
    pruebas. Con 15 el arranque tarda más (el cargador avanza una vez por frame); que la lógica escale con el objetivo no
    está demostrado (logos de ATLUS: 11 s a 24 fps reales frente a 15 s a 12 fps).
36. **Opción "Gráficos"** en el launcher (Bajos por defecto / Medios / Altos): hoy solo baja el detalle de texturas
    (`NX_LOD_BIAS` 1 / 0,5 / 0, `nvn_sampler.cpp`). Medido en CARGAR: sin cambio de GPU (66–72 ms). **La "escala de render
    75 %" de la sesión 4 nunca funcionó**: P5R no usa la resolución que devolvemos (`NX_RENDER_RES`), elige 720p/1080p por
    el modo de la consola (recorte 1280x720 con y sin la opción; la mejora medida era ruido). Retirada del launcher.
37. Tamaños de render de P5R (para bajar resolución más adelante): la tarea de arranque del motor (`main+fe7260`, lanzada
    por puntero desde `main+fe71ac`) pasa su configuración a `main+f73770 -> fd1ad0 -> fe3c30` (ventana 1920x1080, 3D 0 =
    la ventana; los fps en +0x34). `fe3c30` llena la tabla `[[main+0x220b4a0]+0x4758]` (entrada 2 = escena 3D) y los
    objetivos se crean con códigos relativos (`0x80000202` = entrada 2 / 2 = 960x540 en portátil; `main+fe3ec0`). Cambiar
    el 3D en esa configuración solo redimensiona parte de los objetivos (otros salen de `main+fdca14`): imagen descolocada
    en PC. **Además en la Adreno 610 el coste es geometría**: con menos píxeles los pases 3D cuestan casi igual.
    `NX_TRACE_TEX_CALLER=<w>x<h>` registra quién crea cada textura de ese tamaño.
38. **Culling por oclusión desactivado**: P5R usa consultas de oclusión (`ReportCounter` tipo 1 = samples passed,
    `ResetCounter` desde `main+fc3f74`, en `main+fc3dd0`) y `SetRenderEnableConditional`; el port responde siempre
    "visible", así que **dibuja todos los objetos ocultos**. Hacerlo de verdad (consultas de oclusión de Vulkan con un
    frame de retraso) es probablemente la mayor ganancia de GPU pendiente.
39. **Objetos que desaparecen un frame** (casino, tramo de correr; grabado mientras jugabas): en `runs/run2`, la imagen 7
    de `seq_a.png` (desde 0:18) sale sin la lámpara dorada de la izquierda ni las luces de la derecha, que están en la
    anterior y la siguiente. `private/p5r/flicker.py` los busca en una grabación. Hipótesis: lecturas de los contadores
    de oclusión antes de que el replay los escriba (valores de otro frame). Sin confirmar.
40. Pantalla: el juego pide 60 Hz (`ANativeWindow_setFrameRateWithChangeStrategy`, `NX_WINDOW_HZ`), pero la Tab M11 se
    queda a 90 Hz.
41. Vídeo en memoria vigilada: `write_watch_will_write` desprotege el rango de una vez (antes un fallo por página, 760 por
    fotograma). Efecto pequeño (la copia ya costaba 1,5–2 ms).
42. Tablet sin adb: el servicio **SuperDisplay** del PC (MirrorService, driver WinUSB de KeloCube) la pone en modo
    accesorio USB (`18D1:2D01`). Se arregla parando el servicio (`Stop-Service SuperDisplay`, admin) y reiniciando el
    dispositivo USB (`pnputil /restart-device "USB\VID_18D1&PID_2D01\HA269JK2"`, admin). Vuelve a arrancar con Windows.

### Sesión 6 (2026-10-22, tarde) — tirones de las películas, oclusión, sombras

43. **No hay logs ni depuración frenando el juego**: capa de validación apagada, APK en Release `-O2`, el log escribe
    ~2 KB/s. En tu partida, el hilo de replay esperó a la GPU en 279 de 281 envíos: la Adreno 610 es el límite. Medido
    en el menú del título 3D: **6,95 fps, GPU 75,3 ms por frame** (`runs/tmenu_v1k1`). Tus logs: `runs/user_1022`.
44. **Tirones de las películas arreglados** (la intro anime; commit `a17cf38`). El decodificador iba bien (3–4 imágenes de
    reserva) y CRI toma una por frame. Lo que fallaba era la subida a la GPU: P5R crea un objeto de textura por imagen
    sobre un anillo de 6 búferes NV12, y la **luminancia (Y) no se subía en el 21 % de los frames**. Se veía la imagen de
    6 frames antes durante 1–2 frames. Tres causas, todas en el runtime:
    - un objeto nuevo que **compartía la imagen** de otro aún vivo sobre la misma memoria la daba por subida
      (`nvn_image.cpp`: ahora se marca sucia; `NX_SHARE_KEEP=1`, como antes);
    - la **primera lectura del vigilante de escritura** (Y, ≥ 1 MB) y el **primer hash** (UV, < 1 MB) se ignoraban aunque la
      imagen se hubiera subido en un frame anterior (`nvn_sampler.cpp`: `armed_late`);
    - los planos de tamaño de película se comprueban **cada frame**, no cada 4.
    Resultado: **2.499 de 2.500 frames** suben Y y UV (`runs/film_fix4`). Con `NX_TRACE_VIDEO=<presents>` se ven, por
    present, los eventos del decodificador (acquired/taken/released), las subidas de cada plano y las decisiones.
    Notas: CRI copia cada imagen a su propio pool (5 × 4 MB) y la libera al instante. El `pts` que manda el juego es
    siempre 0. La intro anime arranca sola tras los logos, y una A la salta.
45. **Oclusión de P5R, cómo la usa** (`main+fc3dd0`; modo `[[main+0x220b4a0]+0x433c]` == 1: pase de consultas, otro
    valor: pase condicionado). En el **pase de consultas**, cada objeto hace `ResetCounter(1)`, **un** draw y
    `ReportCounter(1, 0x1010c0000 + 0x20·slot)`. En el **pase condicionado**, cada objeto hace
    `SetRenderEnableConditional(1 = dibujar si distinto, misma dirección)`, su draw y `SetRenderEnable(1)`. El
    condicionado va a otro color con el mismo depth, y compara el u64 de la dirección con el de +16 (que vale 0). En el
    título 3D son **~413 objetos por frame** (de ~1.265 draws); en la escena de la frame 707, 34. `ResetCounter` ya se
    registra (antes caía en el driver real del SDK). `NX_TRACE_OCCL=<líneas>[@frame]`. `NX_OCCL_FORCE=0` salta todos los
    condicionados (techo de la ganancia; la imagen pierde objetos) y `=1` los dibuja todos.
    **Plan (sin hacer)**: la Adreno 610 no tiene `VK_EXT_conditional_rendering`, pero sí `VK_KHR_draw_indirect_count` y
    `drawIndirectFirstInstance`. Habría que hacer:
    - una consulta de oclusión de Vulkan por draw del pase de consultas, en un solo pool con `vkCmdResetQueryPool` al
      primer `ResetCounter` de cada envío (fuera del pase);
    - en el primer condicionado, `vkCmdCopyQueryPoolResults` (u32, WAIT) al anillo de subida y una barrera
      TRANSFER → DRAW_INDIRECT;
    - cada draw condicionado como `vkCmdDraw[Indexed]IndirectCountKHR`, con count = el resultado y máximo 1. Así no hay
      frame de retraso ni objetos que aparecen tarde.

    Hay que activar `VK_KHR_draw_indirect_count` en `nvn_vk.cpp` (lista `dexts`). La medición del techo en el título
    (`private/p5r/ab3.sh tmenu - "NX_OCCL_FORCE=0" "NX_SHADOWS=0"`) quedó a medias: solo dio la referencia.
46. **Sombras**: los mapas de sombras son texturas de profundidad cuadradas D32 de 2048 y 1024 (registro
    `nvn: depth-only pass into ...`). Las de 960x540 y 1280x720 D32S8 son el depth de la escena. `NX_SHADOWS=0` no dibuja
    en los mapas (sus clears quedan: todo iluminado). En el perfil de CARGAR son 3,75 de 61,6 ms (~6 %). **Falta**: la
    opción "Sombras" en el launcher (pedida: mínimo por defecto) y medirla.
47. Scripts nuevos (`private/p5r/`): `screen_lib.py` (captura + estado del menú). `to_title.py [límite] [film]` llega al
    menú del título por lo que se ve en pantalla; con `film`, para cuando empieza la intro anime.
    `load_screen.py` llega a CARGAR (pantalla roja dos veces seguidas; en el menú pone el cursor en Cargar).
    `ab3.sh nombre variantes...` alterna variantes en el menú del título (`-` = sin switches); `ab2.sh` hace lo mismo en
    CARGAR. **`to_load.sh` y `ab.sh` ya no sirven**: sus tiempos fijos fallan con la pregunta de la red ("Gremio de
    ladrones").
48. Vulkan de la tablet: `adb shell cmd gpu vkjson`. No hay `conditional_rendering`, `extended_dynamic_state` ni
    `descriptor_buffer`. Sí hay `draw_indirect_count`, `host_query_reset`, `occlusionQueryPrecise` y
    `inheritedQueries`.

49. **Culling por oclusión en la GPU** (`nvn_occlusion.cpp`): cada draw del pase de consultas va dentro de una
    consulta de oclusión de Vulkan; al primer condicionado se copian los resultados al anillo de subida y cada draw
    condicionado pasa a `vkCmdDraw[Indexed]IndirectCountKHR` con el resultado como cuenta (máximo 1). Funciona en el PC
    y en la Adreno 610: el **81 %** de los objetos del título están ocultos y se saltan, y la imagen no cambia.
    **Pero no gana nada**. ABAB en el título 3D (`ab4.sh`):

    | Variante | GPU por frame | fps |
    |---|---|---|
    | sin oclusión (`NX_OCCL=0`, 3 medidas) | 75,7 ms | 7,43 |
    | oclusión en la GPU (2 medidas) | 78,7 ms | 7,13 |
    | todos los condicionados saltados (`NX_OCCL_FORCE=0`, techo) | 80,6 ms | 7,24 |

    Ni siquiera saltar todos los condicionados ahorra: esos draws son baratos. Queda **apagado por defecto**
    (`NX_OCCL=1` lo enciende; `NX_OCCL_STATS=1` registra el % oculto esperando cada envío, solo para comprobar).
50. **Perfil por pases del título 3D** (`NX_GPU_PROFILE=1`, `runs/tprof4`, 52 ms por frame en total; este modo espera
    cada envío): dos pases 960x540 de **11,8 y 11,4 ms** (fs del primer draw `20ad01e0` y `62e537d4`), sombras (pases
    solo profundidad, 2 por frame) **4,7 ms**, skinning por compute 3,1 ms, y el resto menos de 3 ms cada uno.
51. Scripts: `ab4.sh` (como `ab3.sh`, pero solo cuenta las ventanas del log del título 3D, de 10 o más partes por
    frame, y repite la prueba si no llegó). `to_title.py` exige ver el menú dos veces seguidas y el texto blanco de
    "Configuración" (`screen_lib.config_white`): los grafitis cian de la película lo engañaban. Guarda la captura que
    tomó por menú en `shots/to_title_last.png`. Si el menú se deja quieto, el juego vuelve a la película: medir en los
    primeros ~25 s. Un arranque con `NX_OCCL=0` se cerró solo al empezar (`runs/oc_v2k1`; sin causa en el log).

52. **El menú 3D pasa de ~7 a ~13,6 fps** (`nvn_written.cpp`, commit `01ece5e`). simpleperf en la tablet
    (`NDK/simpleperf`, `--app io.github.p5rrecomp`; símbolos con `private/p5r/ba/libp5r.so` en un `binary_cache`)
    mostró el hilo de render (`g54`) con un núcleo grande al 100 %. El 46 % se iba en `apply_write_back`, que devuelve
    a la memoria del juego lo que escriben los shaders de cómputo (el skinning). Comparaba palabra a palabra
    directamente en el anillo de subida, que es memoria sin caché: unos 34 MB/s, 55 ms por frame (el "other" del
    registro). Ahora copia el bloque con un `memcpy` y compara en memoria normal: 4,5 ms. El anillo con caché
    (`NX_RING_CACHED=1`) solo no cambiaba nada. `NX_WB_BULK=0` vuelve a lo anterior. El registro trae
    `nvn: write-backs per frame` (cantidad, MB, % cambiado, ms).

    | Menú 3D (`ab4.sh`, ABBA) | GPU por frame | fps | "other" |
    |---|---|---|---|
    | antes (sin sombras) | 69,4 ms | 7,67 | 55 ms |
    | copia en bloque (sin sombras) | 58,3 ms | 13,94 | 6 ms |
    | por defecto ahora, con sombras / sin | 59,3 / 58,1 ms | 13,59 / 13,81 | 6 ms |

    Perfil después: el hilo de render sigue al ~97 %. El 20 % es `memmove` (las subidas: ~25 MB por frame de vértices
    y constantes), 6 % `nx::call`, 4 % `dedup_upload`. Siguiente: la caché persistente de búferes (punto 4 de Falta).
53. **Sombras**: `NX_SHADOWS=0` no reconocía los mapas de sombras en el título, que P5R enlaza con dos colores vacíos
    además de la profundidad (`depth_only()` en `nvn_draw.cpp`). Arreglado: antes del punto 52, la GPU bajaba
    de 78,1 a 70,5 ms por frame (7,34 → 7,65 fps). Después del 52 la diferencia es pequeña (13,6 → 13,8 fps). La
    opción **"Sombras"** del launcher (desactivadas por defecto) ya está.
54. **Controles táctiles**: la flecha abajo de la cruceta tocaba el círculo del stick. La cruceta sube (centro y
    0,40) y el stick baja (0,76). El launcher tiene **"Posición de los controles → Mover"** (`ControlsEditor.java`):
    se arrastran los controles (la cruceta y A B X Y en grupo, el círculo del stick también), los que se tocan se
    pintan en rojo, y se guardan en `NX_TOUCH_LAYOUT` (`STICK:x,y,r` es nuevo: el stick toma la mitad de la pantalla
    donde está su círculo). Comprobado en la tablet: el juego dibuja los controles donde quedaron.
55. **Icono oficial**: `games/p5r/tools/nsp_icon.py <nsp> <prod.keys> private/p5r/icon` saca `icon_AmericanEnglish.dat`
    del NCA de control de tu NSP (AES-XTS de la cabecera, la clave de título del ticket, AES-CTR de la RomFS;
    `pycryptodome` en el venv) y hace los tamaños del launcher y un icono adaptativo. `build_apk.ps1` lo añade si
    existe `private/<juego>/icon/res`. El icono vive solo en `private/`.

56. **Retrato del HUD "pixelado" en tu turno de batalla**: no es un fallo de texturas. El arte del HUD está hecho para
    1080p (el retrato son 255×248 texeles del atlas BC3 de 1536×1536 para 255×248 píxeles en 1920×1080). En modo
    720p el juego dibuja la interfaz en 1280×720, o sea encoge ese arte 1,5× sin mipmaps (el juego ya compensa con un
    sesgo LOD de -0,585), y el detalle fino del retrato de turno (líneas y tramas) se vuelve motas; luego la tablet lo
    amplía 1,5×. El retrato de ataque es de formas simples y no se nota. Descartado en vivo: z-fighting (profundidad
    ALWAYS o sin escritura en la interfaz no cambia nada) y alpha-to-coverage (ningún pipeline lo usa). Arreglo
    disponible: **Resolución 1080p** en el launcher, que cuesta ~19 % de fps (título 3D 12,6 → 10,15). Alternativa
    pendiente: escalar solo las capas 2D (interfaz y ventana) a 1080p con el 3D a 720p, como el escalado de Ryujinx
    (el soporte `render_scale` de los shaders ya existe): trabajo grande.
57. **Herramientas de diagnóstico en vivo** (sin reiniciar el juego): un archivo `trace_draws` en la carpeta de datos
    registra todos los dibujos del siguiente frame (destino, profundidad y su formato, mezcla, alpha test, texturas y
    samplers, primeros vértices con UV); `live_flags` con `ui_always` / `ui_nowrite` cambia la profundidad de los
    dibujos de interfaz mientras existe. Scripts: `private/p5r/td_capture.sh` (traza + captura cada N s) y
    `td_cycle.sh` (traza, y capturas con cada experimento). `NX_NVN_STATES=1` lista los estados distintos del juego
    (P5R: alpha test siempre ALWAYS, profundidad LEQUAL con escritura en la interfaz, polygon offset en algunos sitios).
58. **Caché de búferes entre envíos** (`nvn_buffer_cache.cpp`, `NX_BUFFER_CACHE=1`, **apagada por defecto**).
    `NX_UPLOAD_STATS=1` mostró que en el título el 72 % de los bytes de vértices, el 97 % de índices y el 88 % de
    bloques uniform/storage son iguales a su copia anterior. La primera versión (todo a la caché, reparto first-fit)
    bajó el título de 12,6 a 10,6 fps en la tablet: los ~900 bloques que cambian cada frame giraban el reparto.
    La segunda solo guarda lo que se repite dos veces seguidas, en clases de tamaño potencia de dos; en el PC está
    verificada byte a byte (`NX_BUFFER_CACHE_VERIFY=1`) y ahorra copiar 448 MB frente a 45 MB en una ventana.
    **Falta medirla en la tablet** (`K=1 bash private/p5r/ab4.sh bc - "NX_BUFFER_CACHE=1" "NX_BUFFER_CACHE=1" -`).
59. `NX_LOD_BIAS` (Gráficos: Bajos) ya solo afecta a los dibujos 3D (los que prueban profundidad), no a la interfaz.

### Sesión 8 (2026-10-23) — audio adelantado en las escenas 3D

60. **"Audio adelantado" en escenas 3D** (Joker cae y sale el menú del título; Joker acorralado en el casino): no era
    latencia, era **el juego a cámara lenta**. Dos relojes:
    - El bucle del motor (`main+fe75ac`) mide cada frame pero lo **recorta a 0,05 s** (`[[main+0x220a9c0]+0x34]`,
      puesto por `main+ce39ac`) y lo guarda en `[[main+0x2209f50]+0x50]` (ticks; `+0x48` = inicio del frame), que
      `main+f730c0` pasa al update. Por debajo de 20 fps, lo que usa ese reloj va lento.
    - **Casi todo el juego avanza 1/objetivo por frame** (objetivo = `+0x84`, 30). Medido en PC con
      `NX_P5R_SLOW_MS=80` (~10 fps; sin volcar imágenes, que cuestan ~385 ms cada una): los logos de ATLUS (de
      confirmar "No" a que empieza la película) tardan 19,0 s a 30 fps y **36,6 s a 10 fps**; subir solo el recorte no
      cambió nada (35 s con y sin él); con objetivo 10, 17 s.
    - Arreglo (`games/p5r/game.cpp`): el recorte sube a 150 ms (`NX_P5R_MAX_FRAME_MS`, 50 = el del juego) y **el
      objetivo de fps sigue a los fps reales** (`auto_target`: ventanas de 0,5 s, media sin tirones > 3× la mediana,
      banda muerta de ±0,75 fps, sube con una ventana y baja con dos, entre `NX_P5R_MIN_FPS`=10 y el tope 30/60). El
      intervalo de presentación se queda en el del tope (si bajara con el objetivo, no podría volver a subir).
      `NX_P5R_AUTO_FPS=0` = lo de antes. A 30 fps reales no cambia nada (el objetivo se queda en 30). A ~10 fps: logos
      21–25 s (lo que falta es la carga de la película, que avanza un paso por frame) y el log ya no da desfase.
    - Log: `p5r: game clock X s, steps Y s in Z s real, N fps (target T)` cada 10 s cuando el juego va < 97 % del
      tiempo real; `p5r: target frame rate A -> B` (las 40 primeras veces).
    - Con 60 fps (launcher) también corrige el juego a cámara lenta cuando no llega a 60.
    - Primera prueba en la tablet ("mejoró, pero sigue un poquito adelantado"): iba a 15-24 fps y el juego avanzaba
      el 94-97 % del tiempo real (`steps 9,4-9,7 s in 10 s`): la media por ventanas con banda muerta dejaba el
      objetivo 0,5-1 fps por encima. **Ahora se elige cada frame** (`paced_target`): una deuda (tiempo real menos
      pasos; cada frame cuenta como mucho 1/`NX_P5R_MIN_FPS`, así no se recuperan los tirones de carga) y el periodo
      esperado dan el siguiente paso, periodo + deuda/4, redondeado a fps enteros. En PC a ~15 fps: `steps 10,0 s in
      10,0 s real` en todas las ventanas, y capturando la ventana desde fuera cada 0,5 s (sin el coste de
      `NX_DUMP_*`), los logos de ATLUS coinciden con los de 30 fps hasta ~0,5 s en 17 s. Lo que queda hasta la
      película es su carga, que avanza un paso por frame. El log da ahora `(target A-B)` por ventana de 10 s.
    - Si aún se oye adelantado: medir en la tablet la latencia real de la imagen (lógica -> pantalla) frente al
      sonido; `av_sync` (punto 34) supone 3 periodos de imagen y ~100 ms de sonido. La tablet tiene
      `VK_GOOGLE_display_timing` (hora real de cada imagen en pantalla) y AAudio da la hora de cada muestra: con
      eso se puede medir el desfase y ajustar el retardo en lazo cerrado.
61. **Bloqueo en la batalla del casino en la que se une Violet** (con el objetivo automático): las sombras quietas,
    sin menú de comandos, los botones no hacen nada. Pasó en las dos sesiones con objetivos de 17-21 (en la primera,
    ~8 min a 20 fps fijos hasta que se instaló el APK siguiente). Volcado de hilos en caliente
    (`adb shell touch ".../Persona 5 Royal/dump_threads"`): ningún hilo bloqueado; el principal espera su frame
    (`main+fe7508 -> f732b0 -> fc5acc`) y el de render (hilo 54) trabaja: es la lógica de la batalla la que espera
    algo que no llega. El juego está hecho para pasos de 1/30 y 1/60 s; con pasos de 1/20 algún "hasta llegar"
    no se cumple. (El explorador automático de la sesión 4 nunca llegó a esta batalla, así que no está probado que
    con 30 fijos pase: **falta confirmarlo jugando**.)
    - Ahora **el objetivo no baja de 30 por defecto** (`NX_P5R_MIN_FPS`, 30): en modo 30 fps es el juego original
      (cámara lenta y sonido adelantado por debajo de 30 fps); con 60 fps en el launcher sigue a la tablet entre 30
      y 60 (pasos dentro de lo que el juego conoce). El recorte del reloj medido es 1,5/suelo (50 ms con 30, el del
      juego).
    - **Interruptor en caliente**: un archivo `p5r_fps` en la carpeta de datos con un número (p. ej. 10) baja el
      suelo mientras exista (se lee cada ~2 s; log `p5r: lowest target frame rate`). Probado en PC: 30 -> sigue a
      los fps (16-19, `steps 10,0 s in 10,0 s real`) -> 30 al borrarlo. Sirve para probar si un atasco se suelta al
      volver a 30.
    - La sincronización de verdad por debajo de 30 fps necesita **saltar frames** (lógica a 30 por segundo,
      dibujando menos) o más rendimiento; bajar el objetivo no es seguro.

## Falta ❌ (por orden)

1. **Medir en la tablet la caché de búferes v2** (punto 58) y, si gana, activarla por defecto. El `memmove` de las
   subidas es el 20 % del hilo de render (punto 52), que sigue al ~97 %.
1b. HUD nítido en 720p (punto 56): escalar solo las capas 2D a 1080p, o recomendar Resolución 1080p.
2. Los dos pases 960x540 del título (11,8 + 11,4 ms, punto 50): qué dibujan y si es geometría o relleno (la GPU va
   a 58 ms por frame, cerca del hilo de render).
3. Revisar las barreras de Vulkan con la capa de validación (`private/third_party/vvl`), por los objetos que
   desaparecen un frame (punto 39).
4. Caché de búferes de vértices (16–22 MB/frame copiados) y precalentado de pipelines (`OPTIMIZACION.md`).
5. Confirmar que la batalla de Violet pasa con el objetivo en 30 (punto 61). Sincronía A/V por debajo de 30 fps:
   saltar frames (punto 61) y medir la latencia real imagen/sonido (punto 60).
6. Bajar la resolución 3D de verdad (punto 37) si deja de ser la geometría lo que limita.
7. Terminar el casino (salir del almacén de jaulas) y seguir la historia; guardar/cargar partida.
8. Servicios IPC que pida P5R y Wonder no usaba (`bsd:u`, `ectx:aw`, varios `cmd ... not implemented`).
9. **GPUs**: tabla de quirks por vendorID y probar en el Moto G54 (PowerVR). Opcional: drivers Turnip (adrenotools)
   como experimento solo en Adreno.

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
