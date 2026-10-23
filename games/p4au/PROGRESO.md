# Port nativo de Persona 4 Arena Ultimax a Android — progreso

Última actualización: 2026-10-23 (sesión 1, noche)

## Estado en una línea

**P4AU funciona en PC y en Android (emulador)**: logos de ATLUS/Arc System Works, aviso de guardado (crea la
partida), película de apertura (en Android se decodifica de verdad con `AMediaCodec`; en PC sale negra), pantalla de
título, menú principal en 3D, submenú de historia y selección de capítulo ("MODO HISTORIA"). PC a 60 fps; emulador
x86_64 (`P4AU_Dev`, Vulkan 1.3 de gfxstream sobre la RTX 4060) a 46–60 fps. Sonido: `hwopus` implementado con
libopus (las voces/música Opus del juego). **Los combates funcionan** en PC (60 fps) y en el emulador (Yu contra
Yosuke en la lección 1: personajes, Persona, efectos, HUD y escenario se ven bien).

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

Menú principal de P4AU (3D): categorías HISTORIA, COMBATE, RED, PRÁCTICA, OPCIONES. A entra, B vuelve (B en un
submenú pregunta "Se va a volver al menú principal. ¿Estás de acuerdo?", con "Cancelar" por defecto). En la selección
de capítulo de historia, las A automáticas abrían ese diálogo y la siguiente lo cancelaba: no se llegó a entrar en un
capítulo (probar a mano).

## Falta ❌ (por orden)

1. Entrar en un capítulo de historia y en Arcade/Versus completos; arreglar lo que salga.
2. Verificar en pantalla los colores de la película en Android (punto 10) y que el sonido Opus suena (punto 9).
3. Probar en un móvil real (arm64: Adreno/Mali/PowerVR) copiando los datos a `Persona 4 Arena Ultimax/`.
4. Controles táctiles pensados para lucha (ahora son los de P5R sin la mitad de cámara).
5. Rendimiento en gama media.

## Notas

- La A salta la película de apertura.
- `user/save/bbsave.dat` (≈950 KB) y `replay_list.dat` se crean bien. Si una prueba aborta mientras guarda, borra
  `private\p4au\user\save\bbsave.dat` (si no, el juego dice "Los datos están dañados").
- El idioma sale del sistema (español → textos en español).
