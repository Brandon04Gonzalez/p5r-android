# Port nativo de Persona 4 Arena Ultimax a Android — progreso

Última actualización: 2026-11-04 (sesión 4: Versus local con dos jugadores en PC y modo determinista)

## Estado en una línea

**P4AU funciona en PC y en Android (emulador)**: logos de ATLUS/Arc System Works, aviso de guardado (crea la
partida), película de apertura (en Android se decodifica de verdad con `AMediaCodec`; en PC sale negra), pantalla de
título, menú principal en 3D, submenú de historia y selección de capítulo ("MODO HISTORIA"). PC a 60 fps; emulador
x86_64 (`P4AU_Dev`, Vulkan 1.3 de gfxstream sobre la RTX 4060) a 46–60 fps. **Sonido**: AudioRenderer del SDK en
proceso (arreglado para el SDK 12.3.3, punto 16) y `hwopus` con libopus (música/voces Opus). **Los combates funcionan** en PC (60 fps) y en el emulador (Yu contra
Yosuke en la lección 1: personajes, Persona, efectos, HUD y escenario se ven bien). **El modo historia funciona**
(punto 18): selección de capítulo y novela visual (Sala Terciopelo con Igor y Margaret).

**En la tablet (Lenovo Tab M11, Adreno 610) también funciona** (sesión 2): menús, Arcade completo hasta el combate,
controles táctiles y sonido. Pero el combate va a **~30 fps**, y como la lógica cuenta frames, **a media velocidad**
(punto 24).

## Cómo funciona el port

Igual que el de P5R: **recompilación estática** del dump de Switch (AArch64 → C++) con el runtime
**SwitchGamesRecomp** (de [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp), MIT), que
reimplementa NVN sobre Vulkan. Lo específico de P4AU va en `games/p4au/`. No es un emulador.

- Juego: Persona 4 Arena Ultimax (Switch, USA, `010075A016A3A000`, v0), SDK 12.3.3, motor de Arc System Works sobre
  `nn::gfx`/NVN. Módulos: `main` (6,8 MB, 16.232 funciones), `sdk` (38.707), `subsdk0` = `nn::movie` (no se
  recompila: sus funciones se implementan en `runtime/src/hle_movie.cpp`).
- Shaders: un BNSH por etapa en `romfs/data/shader` (379 etapas, 336 bloques distintos → 336 SPIR-V, 0 fallos).
- Películas: `romfs/data/movie*/*.mp4` (H.264 1280x720 + AAC 48 kHz estéreo).

## Dónde está todo

| Qué | Ruta |
|---|---|
| Repo de trabajo (worktree, rama local **`p4au`**, sale de `p5r-opt`) | `C:\Users\brand\p4au-port\recomp` |
| Código propio del port | `games\p4au\` (game.cpp, tools\, java\, AndroidManifest.xml) |
| Datos derivados del juego (nunca se publican) | `private\p4au\` (analysis, exefs, extract\romfs, gen, spirv, build, runs) |
| Tu dump | `C:\Users\brand\Downloads\Persona 4 Arena Ultimax\Persona 4 Arena Ultimax [010075A016A3A000][USA][v0].nsp` |
| `private\venv`, `third_party`, `keys`, importador y traductor de shaders | uniones (junctions) a los de `p5r-port\recomp\private` |
| APK | `private\p4au\apk\p4au.apk` |

> Nada de `private\` ni ningún APK/EXE construido con tus datos debe subirse ni compartirse. La rama `p4au` es local.

## Comandos

```bash
cd /c/Users/brand/p4au-port/recomp
P=private/venv/Scripts/python
# 1. importar el NSP (claves de Ryujinx): ~2,5 min, 9.061 archivos, 15,5 GB
private/build/importer/nximport.exe --exefs --title 010075A016A3A000 --out private/p4au \
  "C:/Users/brand/AppData/Roaming/Ryujinx/system/prod.keys" "C:/Users/brand/AppData/Roaming/Ryujinx/system/title.keys" \
  "C:/Users/brand/Downloads/Persona 4 Arena Ultimax/Persona 4 Arena Ultimax [010075A016A3A000][USA][v0].nsp"
# 2. recompilar la CPU (~25 s cada uno)
$P -m recomp private/p4au/exefs/main private/p4au/gen/main --name main --base 0x4000000000
$P -m recomp private/p4au/exefs/sdk  private/p4au/gen/sdk  --name sdk  --base 0x4010000000
# 3. shaders
$P games/p4au/tools/bnsh_shaders.py private/p4au/extract/romfs/data/shader private/p4au/shaders_extracted/bnsh
dotnet private/build/spirv_harness/bin/Release/net10.0/spirv_harness.dll --batch private/p4au/shaders_extracted --out private/p4au/spirv --jobs 8 --no-val
```

```powershell
# PC: compilar y probar (log en private\p4au\runs\<nombre>\nx.log, capturas en frames\)
cmd /c C:\Users\brand\p4au-port\recomp\games\p4au\tools\build_pc.cmd -j15
C:\Users\brand\p4au-port\recomp\games\p4au\tools\run_pc.ps1 -Name prueba -Seconds 90 -Headless -DumpEvery 120 -Keys "15:A:0.2;20:A:0.2"
# Android: APK con las dos ABIs (móvil + emulador) e instalación
cmd /c C:\Users\brand\p4au-port\recomp\games\p4au\tools\build_android.cmd -Abis x86_64,arm64-v8a -Install
C:\Users\brand\p4au-port\recomp\games\p4au\tools\run_android.ps1 -Name a1 -Seconds 60 -Serial emulator-5560
```

Para un móvil o tablet: `games\p4au\tools\push_device.ps1 -Serial <serie>` instala el APK, copia los datos (~15,5 GB)
y da el permiso de archivos. Datos en el teléfono/emulador: `Almacenamiento interno/Persona 4 Arena Ultimax/` con `analysis/`, `spirv/` y
`extract/romfs/` (copiados desde `private\p4au`). El launcher pide el permiso de "acceso a todos los archivos".

Emulador: AVD propia **`P4AU_Dev`** (Android 15 x86_64, 40 GB de datos, 8 núcleos, 8 GB RAM, GPU del host):
`emulator -avd P4AU_Dev -port 5560 -gpu host` → `emulator-5560`. No tocar `PQ_Test` ni `PQ2_Port_Dev`.

## Hecho ✅

### Sesión 1 (2026-10-23, noche)

1. Importación del NSP, recompilación de `main`/`sdk` (0 instrucciones sin soportar) y 336 shaders a SPIR-V.
2. **Fallo del recompilador arreglado** (`recomp/analysis.py`): el `.eh_frame` de P4AU tiene FDEs de tamaño 0
   (`main+4f6b4`, `main+51310`, `main+cc404`). Se usaban como límites y la función salía vacía y llamándose a sí misma
   (clang la quitaba): el constructor `main+51310` nunca escribía su vtable y el juego saltaba a basura
   (`b42db84b…`). Ahora el inicio cuenta como función y el final sale de la siguiente entrada.
3. **Origen de ventana inferior-izquierdo de NVN** (`nvn_device.cpp`, `nvn_draw.cpp`, `nvn_pipeline.cpp`,
   `nvn_present.cpp`, `nvn_vk.cpp`, `nvn_debug.cpp`): P4AU nunca llama a `nvnDeviceSetWindowOriginMode`, así que usa el
   modo por defecto de NVN (0). Con él, Vulkan dibuja igual que NVN con viewport normal: se quita el viewport invertido,
   se invierte la cara frontal y la presentación (y los volcados de frames) leen la imagen al revés. Se elige con
   `NX_ORIGIN_MODE` (P4AU pone 0 en `game.cpp`; P5R/Wonder siguen en 1). Antes la imagen salía boca abajo.
4. Servicios: `friend:u` (P4AU pide su clave de historial al arrancar; antes crasheaba), eventos de
   `IApplicationFunctions` 140/150 (P4AU los consulta cada frame), `nn::fs::CommitSaveData` (antes abortaba al crear la
   partida), `EnableDefaultGlobalFileDataCache`.
5. **Películas** (`hle_movie.cpp`, `mp4_demux.cpp`): P4AU usa `movie::Extractor` + dos `movie::Decoder` (audio tipo 4,
   vídeo tipo 1 con salida NV12). Nuevo `Extractor` sobre un demuxer MP4 propio (pistas, tablas de muestras, SPS/PPS,
   AudioSpecificConfig, H.264 con códigos de inicio), estados de `AndroidErrorToNnError` (0x2000 = fin), vtable de 16
   entradas sacada de `_ZTVN5movie9ExtractorE` de subsdk0. El `Decoder` acepta H.264 y AAC: en Android con
   `AMediaCodec` (vídeo NV12, audio PCM 16 bits), en PC imagen negra y silencio. `MediaData::FindString`,
   `Buffer::Size/Offset` y `GetH264NalUnitStartPositionAndNalSize` (portada de subsdk0).
6. Android: build x86_64 (emulador) además de arm64 (`android/build_apk.ps1 -Abis`), perfilador de muestras para
   x86_64, launcher propio (`games/p4au/java`, amarillo de P4: resolución 720p/1080p, texturas, contador de FPS,
   controles táctiles, editor de posición), siempre a 60 fps (juego de lucha: la lógica cuenta frames).
7. Herramientas: `C:\Users\brand\p4au-port\tools\` (`it.sh` compila+prueba, `sheet.py` hoja de capturas, `disas.py`
   desensamblador, `rep.py` reemplazos que respetan CRLF), statusline del proyecto que guarda el % de uso.

### Sesión 1, segunda parte

8. **Funciona en el emulador Android** (APK x86_64): llega al menú de historia a 56–60 fps y la película de apertura
   se decodifica por hardware (`c2.goldfish.h264.decoder` + `c2.android.aac.decoder`).
9. **`hwopus` con libopus** (`runtime/src/hle_opus.cpp`; libopus 1.5.2 BSD en `private/third_party/opus-1.5.2`, CMake
   lo añade si existe y define `NX_HAVE_OPUS`). P4AU abre decodificadores con `OpenHardwareOpusDecoderEx` (cmd 4)
   al entrar en el menú. El tamaño del búfer de trabajo es simbólico (4 KB por canal): con 80 KB el juego agotaba su
   heap de sonido y hacía `memset` sobre un puntero nulo (`sdk+53c9d0`).
10. **Colores de las películas en Android**: el decodificador entrega YUV 4:2:0 "flexible" (planar en los de
    software); ahora se usa el `image-data` (MediaImage2: offset, paso de columna y de fila de cada plano) del
    formato de salida para convertir a NV12. Antes salía verde/magenta. (Sin verificar todavía en pantalla.)
11. **Control de flujo de las películas**: el juego mete datos mientras haya un búfer de entrada libre, mucho más
    rápido que el tiempo real (2.000+ paquetes en cola). Ahora, con 8 en cola, `AcquireInputBufferIndex` dice "no hay"
    y el evento de entrada se señala cuando hay hueco (hilo de bombeo en Android, `ReleaseOutputBufferIndex` en PC).
    Solo para los flujos MP4 (P5R/CRI como antes).

12. **Combate verificado** (COMBATE → primera opción → lección "No.01 Caminar", Yu Narukami contra Yosuke Hanamura):
    en PC a 60 fps y en el emulador. Ruta con pulsaciones: `-Keys "10:A:0.2;14:A:0.2;19:RIGHT:0.3;21:A:0.3;24:A:0.3;27:A:0.3;..."`
    (A cada 3 s hasta los 60 s). En el emulador `run_android.ps1` acepta el mismo `-Keys` (va en su propia línea de
    `config.txt`).
13. **Rendimiento en combate**: P4AU dibuja ~7,3 MB de vértices por frame, **100 % iguales** al frame anterior
    (`NX_UPLOAD_STATS=1`). La caché de búferes entre envíos (`nvn_buffer_cache.cpp`, de P5R sesión 7) los deja en la
    GPU: de 7,3 a 0,03 MB copiados por frame, imagen idéntica, ~0,5 ms de hash por frame. **Activada por defecto en
    P4AU** (`game.cpp`; `NX_BUFFER_CACHE=0` en `config.txt` la apaga). En el emulador sin caché el combate iba a ~14 fps
    (replay 48 ms por frame: copiar a la memoria de gfxstream es lento); **con la caché, 42–47 fps** durante el combate.

14. **Selección de capítulo de historia: no responde a nada** (no era cierto: ver punto 18). Al entrar, el juego guarda (`bbsave.dat`) y la pantalla
    se dibuja bien, pero ni la cruceta, ni A (corta o de 1 s), ni + hacen nada (capturas bajo demanda con un archivo
    `dump_frames` en `private\p4au`: `printf "64 15" > private/p4au/dump_frames` mientras corre). No abre archivos al
    pulsar y ningún hilo está bloqueado (`NX_THREAD_DUMP_AT`): 5 hilos, el 52 espera un evento en `main+35a14` desde el
    arranque. Hipótesis: espera a que empiece la música/voz del menú (los 4 decodificadores Opus se abren, pero **en
    ninguna prueba se decodificó un solo paquete**: `hwopus: N packets decoded` nunca sale). Siguiente paso: ver qué
    señala ese evento y qué espera la pantalla (desensamblar `main+35a14` y el bucle de la selección).
15. `hwopus` registra `hwopus: N packets decoded, peak P` (paquete 1, 100 y cada 2000) para saber si suena algo.
16. **Sonido arreglado** (`hle_audio.cpp`): el runtime buscaba el AudioRenderer interno del SDK con offsets fijos del SDK
    16 de Wonder (vtable del objeto sf y `ExecuteAudioRendererRendering`). En el SDK 12.3.3 de P4AU no lo encontraba
    (`renderer opened ..., impl 0`): sin hilo de render nadie señalaba el evento de audio del juego, su hilo de sonido
    (`main+35a14`, bucle `RequestUpdateAudioRenderer` + `WaitSystemEvent`) se quedaba parado tras la primera
    actualización y no sonaba nada. Ahora se buscan por símbolo (con los offsets de Wonder de respaldo). Resultado en
    PC: `audio: N frames rendered` y `hwopus: 100 packets decoded, peak 13786` (sonido real). La selección de capítulo
    (punto 14) sigue sin responder: no era el audio.
17. Perfil del hilo principal en la selección de capítulo (emulador): ~80 % de las muestras esperando el vsync /
    presentación (`main+48616c`, `main+4862f4`), muy poca lógica (`main+2d51fc`, `main+2fc24`, `main+2ce57c`,
    `main+2314e0`...): la pantalla está en un estado de espera, no cargando. Cómo perfilar en el emulador o un móvil:
    con el juego en marcha, `adb push <archivo con "8"> "/storage/emulated/0/Persona 4 Arena Ultimax/profile"`, esperar
    ~10 s y traer `profile.txt` (líneas: hilo, lr del invitado, pila del host). En Git Bash usar rutas `C:/...` para los
    archivos locales cuando `MSYS_NO_PATHCONV=1`. En PC el perfilador no existe.

### Sesión 2 (2026-10-23, mañana)

18. **La selección de capítulo sí funciona: el punto 14 era un error de la prueba.** No es un menú de lista: hay un
    **cursor libre** (la flecha amarilla) que se mueve con la cruceta/stick (14 unidades por paso; ZL/ZR acercan y
    alejan) y A solo abre el capítulo que está **debajo del cursor**. Al entrar, el cursor queda 65 unidades arriba a
    la izquierda de la tarjeta del capítulo (`main+2a78c4`), justo fuera de su zona (cursor ±32 contra las esquinas
    de la tarjeta ±32, `main+2a9df0`): hay que moverlo un poco (en la Switch es igual: es el mismo código). Anoche se
    pulsaba DERECHA y luego IZQUIERDA (vuelve al mismo sitio) y A fuera de la tarjeta; B sí respondía (abre "¿Volver
    al menú principal?"). Ruta que funciona:
    `-Keys "10:A:0.2;14:A:0.2;20:A:0.2;24:A:0.2;32:RIGHT:0.15;34:DOWN:0.15;37:A:0.3;45:A:0.3;..."` (A cada 5 s
    avanza los diálogos). El capítulo 1 ("Un invitado misterioso") empieza con varias líneas sobre negro (es así en
    el juego) y luego sale la Sala Terciopelo con Igor y Margaret.
19. Herramientas de depuración nuevas (todas apagadas por defecto):
    - `NX_P4AU_HOOKS="29868,cc424,..."` (game.cpp): cuenta las llamadas a esas funciones de `main` que pasan por la
      tabla de funciones (virtuales e indirectas) por llamador, x0, x1 y resultado; lo escribe cada 3 s.
    - `NX_P4AU_FOCUS=1`: la pila de "foco" de los menús (main+1ade3c0; las ayudas de entrada de menú, `main+8b500`
      y hermanas, ignoran un botón si `strcmp(foco, pantalla) != 0`) y el bloqueo de entrada. `NX_P4AU_STORY=1`:
      estado, pestaña, cursor, zoom y tarjeta bajo el cursor de la selección de historia (objeto `main+23bf0b8`).
      `NX_P4AU_PEEK=<dir>,<n>,<paso>`: palabras de memoria del invitado, cuando cambian.
    - `NX_WATCH_RW=<dir>[,<dir>...]` (platform_win.cpp, solo PC): hasta 4 puntos de observación de lectura/escritura
      por hardware en todos los hilos; cada 3 s, quién los tocó (lr del invitado, lr del marco, x19, pc del host).
    - `hid` registra cada llamador distinto de `GetNpadState`/`GetNpadStyleSet`; `NX_TRACE_TEX_CALLER` y
      `NX_TRACE_UPLOADS` dan ahora la dirección de la textura y de su memoria, y los cambios de CPU en texturas
      lineales de 1 MB o más.
    Cadena de entrada de P4AU: npad (`main+31980`, estado por id en main+7cf1a0) → dispositivo (vtable main+6489c8:
    Update `main+29418`, IsPressed `main+29868`) → mapeadores de botones (vtables main+64cb88 y main+64cbf0,
    IsPressed `main+cc424`/`main+cc740`; "recién pulsado" en +0x40) → ayudas de menú (`main+8b500`...) o lectura
    directa (`main+8a5cc` IsTriggered, `main+8a5e4` IsRepeat).

### Sesión 2, segunda parte (2026-10-23, mañana) — prueba en la tablet

Dispositivo: Lenovo Tab M11 (`HA269JK2`, TB331FC): Snapdragon 685, **Adreno 610** (Vulkan 1.1), 8 GB, Android 14,
panel 1200×1920 **solo a 90 Hz** (un único modo de pantalla). Capturas y logs en `private\p4au\runs\tab1`…`tab11`.

20. **Datos reducidos para la tablet**: tenía 8,6 GB libres y los datos ocupan 15,5 GB. Con
    `C:\Users\brand\p4au-port\tools\subset.py` se copian 7,2 GB (4.317 archivos) a
    `Persona 4 Arena Ultimax/extract/romfs`: solo español/inglés, voces y películas en inglés (más los 62 archivos
    pequeños de `sound/voice` que el juego abre igualmente, como `ns00.pac`), sin los recursos de novela visual de
    `story/` (BG, CG, avatar, se, Voice…) ni los créditos. **El Modo Historia no tiene sus datos en la tablet.** Los
    archivos se eligieron con una prueba en PC con `NX_TRACE_FS=1` (menús + combate): ninguno falta. Quedan 1,3 GB
    libres. No se borró nada de la tablet.
21. **Funciona en la tablet**: arranque, aviso de guardado (crea la partida), logos, película de apertura, título,
    menú principal 3D, COMBATE → **Modo Arcade completo**: selección de mando, de personaje (Yu: tipo, color),
    dificultad, introducción, charla previa (se salta con +) y **combate contra Chie**: personajes, Izanagi, golpes,
    contador de hits, HUD y menú de pausa se ven bien. Sin errores en el log.
22. **Controles táctiles**: todos los botones responden (probados con `adb shell input swipe` sobre cada botón). El
    juego ocupa 1920×1080 de los 1920×1200 de la pantalla y los botones se dibujan encima de la imagen.
23. **Sonido** en la tablet: AAudio en marcha (`192 frames per burst`), `audio: N frames rendered` y
    `hwopus: 6000 packets decoded, peak 32768`: hay música y voces (sin comprobar de oído).
24. **Rendimiento (el problema)**: menús 44–51 fps, selección de capítulo 44 fps, **combate 27–32 fps**. El reloj del
    combate bajó de 76 a 65 en ~21 s reales: **el juego va a ~55 % de velocidad** (la lógica avanza un paso por
    frame). La línea de tiempo (`NX_TRACE_TIMELINE=3000`, `runs\tab9`) muestra todo en serie en el hilo del juego,
    ~34 ms por frame: lógica y grabación ~8–10 ms, replay ~9–10 ms y **`gpu_present` ~15 ms bloqueado**
    (`vkAcquireNextImageKHR`/`vkQueuePresentKHR`: no son esperas de fences, que suman 0,1 ms). El hilo está en CPU solo
    el ~30 % del tiempo: **no es la CPU**. Pruebas que **no cambian nada** (todas ~28–30 fps en combate):
    `NX_REPLAY_THREAD=1` (present pasa a 21 ms), `NX_FIFO=1`, `NX_WINDOW_HZ=0`, `NX_RENDER_RES=960x540` (P4AU lo
    ignora: siempre dibuja a 1280×768). Medidas de GPU contradictorias: `NX_GPU_PROFILE=1` (espera cada envío) da
    **15 ms de GPU por frame** (7 ms en un solo pase de 1280×768, `fs ed25b265a8db96b2`), y `NX_GPU_PROFILE=3`
    (sin esperas) da **31–34 ms**. Incluso en una pantalla casi vacía (aviso de guardado, replay 0,5 ms) el present
    bloquea 16,6 ms y no pasa de 51 fps. La Tab M11 no expone la frecuencia de la GPU (`/sys/class/kgsl`: permiso
    denegado).
25. En la tablet, la selección de capítulo de historia no avanzó con A pulsada a ciegas y **B sí funcionó en ella**
    (abre "¿Volver al menú principal?"): es el cursor libre del punto 18. Allí no se probó más: los datos de `story/`
    no están copiados (punto 20).

Herramientas nuevas (en `C:\Users\brand\p4au-port\tools`): `tab.sh` (`shot <png>`, `press <botón> [ms]` sobre los
botones táctiles, `log <destino>`, `fps`) y `tabfight.sh <nombre> "<env>"` (arranca el juego en la tablet y llega solo
a un combate de Arcade, Yu contra Chie, en ~85 s; necesita la partida ya creada).

### Sesión 2, tercera parte (2026-10-23, mañana) — historia, controles de lucha

26. **Modo historia largo en PC**: 5 min del capítulo 1 pulsando A cada 0,7 s (`s2-story3`): Sala Terciopelo, Junes
    (Yu, Yosuke, Chie, Teddie, Yukiko), narraciones, un flashback y cambios de luz; sin errores. R ("Saltar") y ZR
    ("Saltar rápido") no saltan nada la primera vez (seguramente solo saltan texto ya leído): llegar al primer combate
    del capítulo pide más tiempo.
27. **Controles táctiles de lucha** (`touch_layout.cpp` kP4AU y su copia en `ControlsEditor.java`): panel de seis
    botones de recreativa abajo a la derecha, **R Y X** sobre **ZR B A** (Y X = ataques A y B del juego, B A = los de
    Persona), inclinado como un stick arcade; el editor del launcher lo mueve entero. L/ZL arriba a la izquierda, − +
    arriba al centro, la cruceta a la izquierda (menús) y el stick en la mitad izquierda. Probado en el emulador solo
    con toques (`tab.sh press`, que ya usa las posiciones nuevas: `SERIAL=emulator-5560 SCREEN_W=2400 SCREEN_H=1080`):
    película → título → menú → BATALLA → Arcade → mando → personaje → dificultad → combate contra Chie, y Y/X atacan
    ("1 HIT", "COUNTER"). En el emulador (20:9) los botones quedan en gran parte sobre las bandas negras.
28. **Pista de rendimiento**: en ese combate de Arcade (Chie, escenario de la calle) el emulador va a **~17 fps** (la
    lección 1 iba a 42–47). La caché de búferes acierta ~5,8 MB por frame y **gasta ~11,6 ms por frame en hashes**
    (`buffer cache: ... hashing 3488 ms` cada 300 frames); en la lección eran ~0,5 ms. Además replay 33–35 ms. Para el
    punto 24: evitar rehashear cada frame los rangos estables (vigilancia de escritura por páginas en Android, como
    las texturas de `write_watch.cpp`, o comprobar los rangos estables cada N frames) puede ahorrar mucho en la CPU.
    En PC, la ruta de Arcade del punto 12 acabó en "Lección" (el menú recuerda la última categoría): para Arcade,
    desde el menú principal DERECHA → A (Arcade es la primera opción de BATALLA) → IZQUIERDA → A (mando del lado P1).

Menú principal de P4AU (3D): categorías HISTORIA, COMBATE, RED, PRÁCTICA, OPCIONES. A entra, B vuelve (en un submenú
pregunta "Se va a volver al menú principal. ¿Estás de acuerdo?", con "Cancelar" por defecto).

### Sesión 3 (2026-10-25, noche) — la película de apertura en la tablet

29. **La película de apertura se congelaba en su primera imagen en la tablet** (el usuario: "la cinemática no
    reproduce"; en `runs/tab2`, `tab4` y en las del emulador pasaba lo mismo: solo avanzaba un segundo). El sonido de
    las películas sale por un AudioOut que el juego abre con `OpenDefaultAudioOut(AudioOut*, const
    AudioOutParameter&)`, la variante **sin evento**, que no estaba interceptada: iba al sdk y al stub de `audout:u`,
    nuestro hilo de AudioOut no arrancaba, ningún búfer volvía al juego, el audio del vídeo se paraba y el vídeo (que
    se sincroniza con él) también. Arreglado en `hle_audio.cpp` (commit `fc92b5b`): esa variante y las dos de
    `OpenAudioOut` con nombre son nuestras. Además P4AU tiene a la vez el AudioRenderer (el sonido del juego) y el
    AudioOut (el de las películas) sobre el único flujo del dispositivo: ahora el hilo del renderer **mezcla** las
    muestras del AudioOut en cada trama de 5 ms y devuelve cada búfer gastado (antes se habrían turnado, cada uno a
    media velocidad). P5R (solo AudioOut) no cambia. En la tablet la película va entera hasta el título: 30 imágenes
    y 47 tramas AAC por segundo (`runs/look2`).
    - Queda: la pantalla muestra ~15 imágenes nuevas por segundo de las 30 que el juego toma (las toma a ráfagas,
      huecos de hasta ~200 ms).
30. **Ritmo de pantalla en la tablet**: el panel de la Tab M11 solo va a 90 Hz y el juego pide 60 Hz: Android lo
    deja en 45 Hz (todos los intervalos entre 19 y 25 ms, `runs/f1`). En la selección de capítulo de historia, con la
    configuración del usuario (`NX_FIFO=1`, `NX_HANDHELD=0`, `GRAPHICS=high`): 42 fps, GPU 23,5 ms por frame.
    - El "aviso que sale varias veces al inicio" que vio el usuario no apareció en estas pruebas (la partida ya
      existe): falta que diga cuál es.
    - `tabfight.sh` ya no llega a Arcade (acabó en el Modo Historia: el menú recuerda la categoría) y **sobrescribe
      `config.txt` sin copia**: la del usuario está en `private/p4au/runs/user_1024/config.txt`.

### Sesión 4 (2026-11-03) — dos jugadores en local

31. **Versus local con dos mandos funciona en PC** (`runs/n1-versus`). El juego consulta los ids de npad 0-7 cada frame
    (`main+31ab0` → `main+31980`, que elige el lector de estado según el estilo de `GetNpadStyleSet`); antes solo el id 0
    decía estar conectado. Ahora `hle_hid.cpp` da al **jugador 2 (npad id 1)** el estilo full-key y su propio estado
    cuando tiene una fuente: el **segundo mando XInput** (`read_pad2` en `platform_win.cpp`; un hueco vacío se mira cada
    2 s), las pistas de guion `NX_INPUT_P2` (por frame) o `NX_INPUT_SCRIPT_P2` (por reloj), entrada remota, o
    `NX_P2=1` (`NX_P2=0` lo apaga). Sin nada de eso todo sigue como antes (también en Android). También se interceptan
    las variantes JoyLeft/JoyRight de `GetNpadState` que P4AU importa.
    - **Ruta a Versus** (desde el menú principal en HISTORIA, la categoría que recuerda la partida guardada; frames con
      `-FrameKeys`/`-FrameKeys2` de `run_pc.ps1`): A 500 (película), A 600 (título), DERECHA 900 (COMBATE: Arcade,
      Lluviadepuntos, GoldenArena, **Versus**, la lista da la vuelta), ABAJO ×3 (1000/1060/1120), A 1200 → **selección
      de mando** con un icono por mando conectado ("1" y "2"): J1 IZQUIERDA y J2 DERECHA a 1400, A los dos a 1460 →
      personaje (J1 Yu, J2 Yosuke), tipo, color, escenario (auto) con A de los dos cada ~100 frames → VS → combate
      (ROUND 1 hacia el frame 3400).
    - Con J1 quieto, `NX_INPUT_P2="3600:Left:300,3960:SRight:240,..."` hace andar a Yosuke hacia Yu (cruceta) y volver
      (stick): se mueve justo cuando se le manda.
    - Pistas por frame ampliadas: `<frame>:<botón>[:<frames>]` (8 por defecto), botones ZL ZR y `SLeft/SRight/SUp/SDown`
      (stick izquierdo). Van por frame presentado: no se desplazan con los volcados de frames (cada volcado para el
      juego ~0,66 s: con `-DumpEvery 30` el arranque va a ~26 fps y las rutas por reloj fallan).
    - **API para el juego en red** (`runtime/src/hid_input.h`): `hid::set_remote_input(jugador, frame, PadState)`
      fija la entrada de un jugador para un frame (desde entonces solo cuenta eso; un frame sin dato es un mando
      neutro), `clear_remote_input`, `read_local` (lo que dan los mandos y guiones locales, lo que se enviará) y
      `connected`. `NX_INPUT_REMOTE=1` mete las pistas por frame por esa API un frame antes (`runs/n1-remote`): mismas
      posiciones que `n1-versus` en cada frame del combate.
    - Para N2 (determinismo): en esas dos pruebas con la misma entrada el combate empezó unos 60 frames antes en una (el reloj
      del asalto iba una unidad por delante y la intro/"ROUND 1" salía antes): las cargas duran un número variable de
      frames, así que el número de frame absoluto no sirve de ancla entre ejecuciones ni entre dispositivos.
    - La ruta de Arcade con J1 solo sigue llegando al combate contra Chie (`runs/n1-arcade2`, ROUND 1 en el frame 3840):
      A 500, A 600, DERECHA 900, A 1000, IZQUIERDA 1200, A 1260 y A cada 90 frames desde 1500. Ojo: entrar en un modo
      guarda la categoría en `bbsave.dat` y cambia la ruta de la siguiente prueba (en estas pruebas se restauró la copia).

### Sesión 4, segunda parte (2026-11-03) — determinismo para el juego en red (N2)

32. **Modo determinista** `NX_DETERMINISTIC=1` (apagado por defecto; lo encenderá la sesión de red; `run_pc.ps1
    -Deterministic`), en `runtime/src/netplay/determinism.{h,cpp}`. Con la misma entrada por frame, dos ejecuciones
    llegan al mismo estado en el mismo frame aunque vayan a velocidades muy distintas. Auditoría con `NX_DET_TRACE=1`
    (quién lee el reloj, la fecha y `GenerateRandomBytes`, y en qué frame e hilo se abre cada archivo):
    - **La divergencia de N1 eran las semillas**: el juego lee el reloj solo en el hilo principal (`main+3831c`, µs):
      el controlador de frames (`main+3849c`) cada frame y, al arrancar, la semilla de dos de sus cuatro Mersenne
      Twister (`main+e20b8`, `main+e20e0`: los ms desde el arranque). Otra semilla en cada ejecución → otra variante
      de la intro de Yosuke → el asalto empezaba en otro frame. En modo determinista el reloj del hilo principal es
      1/60 s por frame presentado (+1 tick por lectura); los demás hilos siguen con el reloj real.
    - Las cargas son asíncronas: el hilo principal encola y el hilo 0x55 abre y lee (una carga de combate se reparte
      en 6 frames en PC). En modo determinista cada present espera a que los hilos cargadores estén parados
      (`det::thread_idle` en `kernel.cpp`).
    - El hilo principal arranca un hilo por cada operación de guardado (0x57-0x5a, frames 6, 11, 16 y 21), y su
      primera apertura de `bbsave.dat` caía en frames distintos (11 o 13). En modo determinista el hilo principal
      espera a que el hilo que arranca quede parado (`det::thread_started`), y la apertura cae siempre en el 11 y el 21.
    - La fecha (`GetCurrentTime`) queda fija (+ frame/60) y `GenerateRandomBytes` (solo lo usa el montón de nlib) sale
      de la semilla de sesión `NX_DET_SEED`. El audio va por reloj pero no realimenta la lógica en esta ruta.
    - **Hash de estado** `netplay::state_hash()` (lector en `game.cpp`): reloj del asalto (`main+19d1ac0`), vida y
      posición de los dos personajes (punteros en `main+183ac40`; vida en +0xc74: Yu 9500, Yosuke 8500), cámara y los
      cuatro MT (`main+68b358`). `NX_DET_HASH_EVERY=10` lo escribe en el log (`NX_DET_HASH_WORDS=1` añade todas las
      palabras) y `run_pc.ps1 -CompareTo <prueba>` compara dos pruebas frame a frame. `NX_FS_DELAY_US=<µs>` simula un
      almacenamiento lento (para probar).
    - **Resultado** (ruta Versus de N1, P2 golpea a P1): `n2-a` (volcado cada 120 frames) y `n2-b` (volcado cada 30 y
      2 ms más por lectura de archivo) tienen el mismo hash en los 1483 frames comparados (10-14830); el asalto 1
      empieza en el frame 3372 en las dos y el reloj se agota en el 9320 en las dos. Sin el modo (`n2-red-a`/`-b`) los
      491 frames difieren y el asalto empieza en el 3527 y el 3396. Sin nada (`n2-default`) todo va como en N1.
    - Pendiente: la primera ejecución tras compilar (`n2-boot-a`, en frío) fue 3-4 frames por detrás entre los frames
      10 y 40 y luego convergió (igual desde el 50). Causa sin encontrar; el lockstep debe empezar después del arranque.
    - Para N3: los dos equipos deben arrancar en modo determinista con la misma `NX_DET_SEED` y la **misma partida
      guardada** (la categoría del menú, opciones y desbloqueos están en `bbsave.dat`), y empezar el lockstep en un
      frame posterior al arranque en el que los dos tengan el mismo hash.

## Notas

- La A salta la película de apertura.
- `user/save/bbsave.dat` (≈950 KB) y `replay_list.dat` se crean bien. Si una prueba aborta mientras guarda, borra
  `private\p4au\user\save\bbsave.dat` (si no, el juego dice "Los datos están dañados").
- El idioma sale del sistema (español → textos en español).

### 2026-10-31 (sesión del port de NieR): arreglos de P5R v0.5.0 en el runtime

Commit `c355cbb`: los cambios de runtime de P5R v0.5.0 (`254b1a0` de `p5r-main`, historia no relacionada: cherry-pick,
no merge) que aplicaban limpios: decodificación BC por CPU en Xclipse (`nvn_bc.cpp`), R11G11B10F → RGBA16F en Mali
(`nvn_caps.cpp`), sin trocear envíos en Adreno 8xx (`nvn_frames.cpp`, `nvn_vk.*`), `NX_DISABLE_BLOOM` (apagado),
SixAxis con 1 estado (`hle_hid.cpp`), sensibilidad táctil y mando Nintendo/Xbox (`platform_android_input.cpp`), L3/R3
en la tabla táctil de P5R. Más el `switch (k) {` que ese commit perdió en `platform_android_input.cpp` (el APK no
compilaba). **Fuera**: `hle_fs.cpp` (mods sin mayúsculas / formatos de carpeta) y `main.cpp` (regiones de P5R, NCE), con
conflictos. El lanzador de P4AU no tiene aún las opciones nuevas (P4G las tiene desde su v0.2.0). Verificado: PC y APK
compilan; en PC arranca a 60 fps (`runs/p5rfix1`). Sin probar en la tablet.

### 2026-11-02: Arreglo de aspecto de pantalla panorámica y opción NX_STRETCH

- `runtime/src/platform_android.cpp`: `window_size` adaptado para calcular el ancho proporcional al aspect ratio físico del teléfono (`sw * target_h / sh`); `vulkan_create_surface` añade `ANativeWindow_setBuffersGeometry` proporcional; `unsetenv("NX_STRETCH")` en arranque.
- `games/p4au/java/io/github/p4aurecomp/Launcher.java`: agregada opción en interfaz de usuario para **Pantalla completa (estirar)** (`NX_STRETCH=0/1`); `save()` escribe resoluciones adaptativas según modo Handheld/Docked.

### 2026-11-03: Port de soluciones y PRs de Persona 5 a P4AU

- **Carga de controladores GPU personalizados (`libadrenotools` / Turnip, PR #133)**: soporte completo en `CMakeLists.txt`, `build_apk.ps1`, `platform_android.cpp` e interfaz en `Launcher.java` con selector e importador de archivos `.zip`/`.so` y restauración del controlador de fábrica.
- **Servicios HLE de Horizon OS (PR #47)**: ganchos `nn::ngc::ProfanityFilter`, `ngc:u`, `ngct:u`, y servicio `fsp-srv` (cmds 200, 202) en `svc_misc.cpp` conservando `friend:u`; applet `swkbd` (0x11) y `CreateTransferMemoryStorage` en `svc_am.cpp` conservando canales de invitación y notificación de P4AU.
- **Prevención de interbloqueo recursivo**: sustituido `std::mutex g_fs` por `std::recursive_mutex g_fs` en `runtime/src/hle_fs.cpp`.
- **Compatibilidad Windows PC para lector de imágenes NSP**: añadido include de `win_compat` (`sys/statvfs.h`) y directiva de inclusión de `NX_IMPORTER_DIR` en `CMakeLists.txt`.
- **Exportación segura de depuración**: integración de `LogProvider` en manifest y lanzador.
