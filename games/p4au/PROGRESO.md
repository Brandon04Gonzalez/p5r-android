# Port nativo de Persona 4 Arena Ultimax a Android — progreso

Última actualización: 2026-10-23 (sesión 1, noche)

## Estado en una línea

**P4AU arranca en PC hasta los menús**: logos de ATLUS/Arc System Works, aviso de guardado (crea la partida),
película de apertura (en PC sale negra: no hay decodificador), pantalla de título, menú principal en 3D, selección
de capítulo y "MODO HISTORIA", a 60 fps. El APK de Android (arm64-v8a para móviles + x86_64 para el emulador) se
construye con launcher y controles táctiles; ver "Sesión 1 — Android" para lo probado en el emulador.

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

Datos en el teléfono/emulador: `Almacenamiento interno/Persona 4 Arena Ultimax/` con `analysis/`, `spirv/` y
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

## Falta ❌ (por orden)

1. Probar el APK en el emulador y en un móvil real (Adreno/Mali/PowerVR) y arreglar lo que salga.
2. Sonido: P4AU usa `hwopus` (decodificador Opus por hardware, servicio sin implementar: `cmd 5` sin respuesta) para
   voces/efectos y el AudioRenderer. Hace falta un decodificador Opus (libopus) detrás de `hwopus`.
3. Entrar en un combate y en el modo historia (probado solo hasta el menú de "MODO HISTORIA").
4. Controles táctiles pensados para lucha (ahora son los de P5R sin cámara).
5. Rendimiento en gama media.

## Notas

- La A salta la película de apertura.
- `user/save/bbsave.dat` (≈950 KB) y `replay_list.dat` se crean bien. Si una prueba aborta mientras guarda, borra
  `private\p4au\user\save\bbsave.dat` (si no, el juego dice "Los datos están dañados").
- El idioma sale del sistema (español → textos en español).
