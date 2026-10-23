# Port nativo de Persona 4 Arena Ultimax a Android — progreso

Última actualización: 2026-10-23 (sesión 2, mañana: prueba en la tablet)

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

## Falta ❌ (por orden)

1. **Rendimiento en la tablet** (punto 24): el combate a 30 fps va a media velocidad. Averiguar qué bloquea
   `gpu_present` (perfetto con frecuencia de GPU y etapas de render de Adreno; marcas `tl()` dentro de `gpu_present`
   para separar acquire, pase de presentación y `vkQueuePresentKHR`) y por qué la GPU mide 15 ms con esperas y 31 ms
   sin ellas. Si la GPU de verdad no llega, bajar la resolución interna (P4AU ignora `NX_RENDER_RES`: buscar dónde
   fija 1280×768) o probar la caché de pases. Y en la CPU: el coste de hashes de la caché de búferes en combates de
   Arcade (punto 28).
2. Comprobar Versus (y el Modo Historia en la tablet, copiando sus datos de `story/` si hay espacio).
3. Sonido: comprobar de oído en la tablet (ya se renderiza y se decodifica Opus, puntos 16 y 23).
4. Controles táctiles: el panel de lucha ya está (punto 27); falta probarlo con los dedos en la tablet y ajustar
   tamaños y opacidad (en 16:10 tapan más imagen que en el emulador).
5. Probar en otros móviles (Mali/PowerVR) y gama media.

## Notas

- La A salta la película de apertura.
- `user/save/bbsave.dat` (≈950 KB) y `replay_list.dat` se crean bien. Si una prueba aborta mientras guarda, borra
  `private\p4au\user\save\bbsave.dat` (si no, el juego dice "Los datos están dañados").
- El idioma sale del sistema (español → textos en español).
