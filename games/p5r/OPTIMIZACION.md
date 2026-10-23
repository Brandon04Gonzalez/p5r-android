# P5R en Android — optimización (rama `p5r-opt`)

Dispositivo: Lenovo Tab M11 (Snapdragon 685, **Adreno 610**, Vulkan 1.1). Mediciones con `private/p5r/ab.sh`
(pantalla CARGAR, la más pesada que se alcanza sin jugar: título 3D de Shibuya detrás; `NX_GPU_PROFILE=3`, orden
ABAB para que el calor no sesgue). En el casino jugable: **~13 fps**; en CARGAR: **~7,5 fps**.

## Dónde se va el frame (CARGAR, `NX_GPU_PROFILE=1`, 105 ms de GPU por frame)

| Pase | ms/frame |
|---|---|
| 960x540 fs 20ad01e0 (escena 3D; el fs del primer draw es pequeño: el coste es geometría/relleno) | 27,2 |
| 960x540 fs 62e537d4 | 25,8 |
| solo profundidad (sombras, 2 pases) | 12,1 |
| 960x540 fs 40869c9c | 6,8 |
| compute f82cd07a (skinning por compute, ~180 dispatches/frame) | 5,4 |
| resto (30 etiquetas) | ~28 |

CPU: el hilo de replay tarda ~33 ms/frame; ~1.265 draws/frame; 16–22 MB/frame copiados de memoria del juego al ring.
Conclusión: la Adreno 610 está limitada por GPU (≈ 3× por encima de los 33 ms de 30 fps) y el replay por CPU también
pasa de 33 ms. Para 30 fps estables en esta gama haría falta trabajo grande (caché persistente de búferes de
vértices, reducir pases/sombras).

## Probado

| Cambio | Resultado (ABAB) | Decisión |
|---|---|---|
| `NX_IMAGE_STORAGE=0` (imágenes de color sin uso STORAGE: Adreno conserva UBWC) | GPU 81,6/83,0 → 78,5/79,4 ms; 7,27/7,07 → 7,53/7,40 fps (**−4 %**) | **Por defecto en P5R** (P5R nunca usa imágenes storage) |
| `NX_RENDER_RES=960x540` (escala 75 %) | Sesión 5: **no se aplica** (P5R no usa esa resolución; recorte 1280x720 igual). ABAB con `NX_LOD_BIAS=1`: 70,5/66,5 → 69,2/72,0 ms (igual). La mejora de la sesión 4 era ruido | Retirada |
| 3D al 75 % desde la configuración del motor (`main+fe7260`, sesión 5) | solo cambia parte de los objetivos; los pases siguen a 960x540 y la GPU igual (61,6 ms en CARGAR) | Retirado (PROGRESO, punto 37) |
| `NX_HANDHELD=0` + `NX_RENDER_RES=960x540` | ventana 1920x1080 igualmente; GPU 70/72 → 93/95 ms | No |
| `NX_LOD_BIAS=1` (texturas un mip menos) | sin cambio medible en CARGAR | Opción "Gráficos: Bajos" (por si ayuda en el campo) |
| `NX_REPLAY_THREAD=1` | 7,70/7,73 → 7,07/6,97 fps (−9 %) | No |
| `NX_AFFINITY=2` | 7,60/7,63 → 7,43/7,53 fps | No (neutro/peor) |
| D24S8 nativo | P5R no usa ese formato (no se llamó nunca) | Revertido |
| `spirv-opt -O` de los shaders calientes | los fs de los pases caros son pequeños: no es coste de shader | No aplicado |

Ya estaban en el runtime (Wonder): R11G11B10F nativo, MUTABLE_FORMAT con lista (UBWC), resets en hilo janitor,
envío por trozos, ADPF.

## Sesión 6

- Menú del título 3D (`ab3.sh`, `NX_GPU_PROFILE=3`): **6,95 fps, GPU 75,3 ms por frame**.
- Oclusión: ~413 objetos por frame en el título se dibujan dos veces (pase de consultas + pase condicionado). El
  condicionado se puede hacer en la GPU con `vkCmdDrawIndexedIndirectCountKHR` (diseño en PROGRESO, punto 45).
- Sombras: 2 mapas D32 (2048 y 1024), ~6 % de la GPU en CARGAR; `NX_SHADOWS=0` los vacía.
- **Oclusión en la GPU hecha y descartada** (PROGRESO, punto 49): salta el 81 % de los objetos condicionados, pero el
  título queda igual (75,7 ms sin ella, 78,7 con ella, 80,6 saltándolos todos): esos draws son baratos. Apagada por
  defecto (`NX_OCCL=1`).
- Perfil por pases del título 3D (52 ms): pases 960x540 de 11,8 y 11,4 ms, sombras 4,7, skinning por compute 3,1.

## Sesión 7

- **Menú 3D: de ~7 a ~13,6 fps.** El hilo de render pasaba 55 ms por frame en `apply_write_back`, que comparaba
  palabra a palabra en el anillo de subida sin caché; ahora copia en bloque primero (PROGRESO, punto 52). El cuello
  de botella era la CPU, no la GPU.
- Sombras: el filtro no las reconocía en el título (punto 53). Con el arreglo ahorran ~10 % de GPU; tras el punto 52,
  ~2 % de fps. Opción del launcher, desactivadas por defecto.
- Resolución 1080p en el título 3D: 12,6 → 10,15 fps (−19 %); quita las motas del HUD en 720p (PROGRESO 56).
- Caché de búferes v1: 12,6 → 10,6 fps (peor); v2 (solo datos estables, clases de tamaño) sin medir en la tablet.
- Perfil de CPU: `simpleperf` del NDK sobre la app depurable (`--app io.github.p5rrecomp`); los símbolos salen de
  `private/p5r/ba/libp5r.so` puesto en un `binary_cache` con la ruta del dispositivo.

## Siguiente (por impacto estimado)

0. ~~Culling por oclusión real~~: hecho en la sesión 6, sin ganancia (los draws condicionados son baratos).
1. **Caché persistente de búferes** (vértices/índices estáticos en memoria de GPU, invalidada por write-watch): quitaría
   la mayor parte de los 16–22 MB/frame de copias y parte del replay.
2. Sombras: resolución de los mapas (12 ms/frame).
3. Tirones de 100–350 ms al entrar en escenas (`stall:` en el log): compilación de pipelines; precalentar la caché.
