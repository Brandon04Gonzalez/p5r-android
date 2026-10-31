# Guía de Arquitectura, Optimizaciones de GPU y Solución de Errores para Ports de Nintendo Switch / NCE a Android

Esta guía recopila la experiencia, arquitectura, soluciones de bajo nivel y trampas técnicas encontradas durante el desarrollo del port nativo de **Persona 5 Royal** a Android basado en **SwitchGamesRecomp / NCE (Native Code Execution)**. Está pensada como documento de referencia y manual de buenas prácticas para evitar repetir errores en futuros ports de videojuegos de Nintendo Switch a dispositivos móviles.

---

## 1. Fundamentos: ¿Cómo funciona un Port Nativo Recomp vs Emulación?

Los emuladores tradicionales de Nintendo Switch (como Yuzu, Ryujinx, Strato o EggNS) emulan la CPU ARM64 interpretando o recompilando dinámicamente (JIT) el código mientras ejecutan la llamada al sistema en un entorno virtualizado.

Un port **Recomp / NCE**:
1. **CPU Nativa (Cero JIT en Android):** El código ARM64 de los ejecutables de Switch (`main`, `sdk`, `subsdk*`) corre directamente en el procesador ARM64 del teléfono como librerías compartidas nativas cargadas en memoria con `mmap` y `load_modules`. Las instrucciones se ejecutan a velocidad nativa sin sobrecarga de emulador.
2. **NVN sobre Vulkan 1.1/1.3:** La API gráfica propietaria de Nintendo (`NVN`), diseñada originalmente para el chip Maxwell de Nvidia, se reimplementa limpia y directamente sobre la API Vulkan de Android (`runtime/src/nvn/`).
3. **Traducción de Shaders Maxwell:** Los shaders Maxwell empaquetados en el juego se extraen y traducen estáticamente a SPIR-V para Vulkan una sola vez durante la preparación (`ImportActivity`).
4. **HLE de Servicios de Switch (Horizon OS):** Las llamadas al sistema (`svc`) y servicios como `nn::hid` (entrada), `nn::fs` (archivos), `nn::audio` (sonido AAudio), `nn::codec` (vídeo por hardware VP9/NVDEC) se interceptan mediante ganchos HLE y se traducen a llamadas equivalentes de la plataforma Android.

---

## 2. Errores Críticos de GPU Móviles y Soluciones Técnicas

El ecosistema Android cuenta con docenas de combinaciones de controladores Vulkan con comportamientos atípicos o no conformes con la especificación estándar. A continuación se detallan los fallos más comunes y su solución arquitectónica:

### A. Samsung Xclipse (AMD RDNA en Exynos 2200 / 2400 / 1480): Texturas Negras y Fuentes Invisibles
- **Síntoma:** El juego arranca y reproduce audio, pero los modelos 3D son negros, las pantallas de carga no tienen fondos y los cuadros de texto aparecen vacíos sin letras.
- **Causa Raíz:** El driver Vulkan de Samsung Xclipse declara `textureCompressionBC = true` en `VkPhysicalDeviceFeatures`, o expone los formatos BC1 a BC3, pero falla silenciosamente o no soporta muestreo con optimal tiling en texturas BC4, BC5 o BC7. Cuando el juego intenta muestrear estas texturas directamente en GPU, el controlador devuelve valores RGBA `(0, 0, 0, 0)`.
- **Solución Implementada:**
  1. Identificar GPUs Samsung Xclipse mediante `g_device_props.vendorID == 0x144d` o la presencia de la cadena `"Xclipse"` en `deviceName`.
  2. Forzar la decodificación por software CPU (`bc_host_decode()`) para formatos comprimidos BC si se detecta una GPU Xclipse, o si `p_vkGetPhysicalDeviceFormatProperties` reporta que el formato carece de la bandera `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT`.
  3. Los bloques BC se descomprimen a RGBA8 en memoria host y se suben como texturas convencionales, garantizando visualización perfecta en cualquier chipset Exynos con gráficos Xclipse.

### B. ARM Mali (Dimensity, Helio, Exynos Mali, Tensor): Pantallas Verdes/Moradas y Bloom Ciego
- **Síntoma:** Parpadeo continuo de la pantalla con tonalidades verde chillón o magenta brillante, y sobreexposición extrema de efectos de brillo (bloom que ciega la pantalla).
- **Causa Raíz:** En hardware Maxwell (Nvidia), los cálculos que producen valores negativos o indefinidos en formato `R11G11B10F` (`VK_FORMAT_B10G11R11_UFLOAT_PACK32`) se saturan automáticamente a cero. En arquitecturas ARM Mali (Valhall, Bifrost, 5ta generación), las operaciones de rasterizado o mezcla sobre este formato de punto flotante sin signo generan `NaN` (Not a Number) o `+Inf`. Una vez que un píxel se vuelve NaN, el efecto de difusión de bloom y los filtros gaussianos extienden el NaN por todo el búfer de post-procesado, convirtiendo el fotograma entero en verde o violeta.
- **Solución Implementada:**
  1. Detectar hardware Mali (`strstr(deviceName, "Mali") != nullptr || vendorID == 0x13b5`).
  2. En `native_r11g11b10()`, desactivar el uso del formato nativo R11G11B10F y sustituirlo automáticamente por `VK_FORMAT_R16G16B16A16_SFLOAT` (RGBA16F).
  3. RGBA16F maneja el signo y precisión de coma flotante de 16 bits sin corromper el búfer de color ni generar NaNs descontrolados.

### C. Qualcomm Adreno 8xx (Snapdragon 8 Gen 4 / 8s Gen 4 / Adreno 825): Bloqueo en Batallas
- **Síntoma:** El juego funciona fluido en exploración, pero al entrar en combates o animaciones con múltiples pases de post-procesado, la aplicación se congela por completo y Android muestra el diálogo ANR (Application Not Responding).
- **Causa Raíz:** En controladores Vulkan de Adreno serie 8 (v700+), la fragmentación de comandos de dibujo en chunks múltiples por frame (`NX_SUBMIT_CHUNK_DRAWS=150`) dispara una condición de carrera interna en el gestor de colas del driver, colgando el procesador de comandos de la GPU.
- **Solución Implementada:**
  1. Detectar chips Adreno serie 8 buscando el dígito `'8'` tras el prefijo `"Adreno"`.
  2. Anular la división en fragmentos (`g_chunk_draws = 0`) para enviar el búfer de comandos en un solo lote atómico por submit, evitando el cuelgue por completo.

### D. Optimización Selectiva de Bloom y Cadenas de Desenfoque (`NX_DISABLE_BLOOM`)
- **Problema:** En móviles de gama baja y media, los efectos de bloom realizan entre 5 y 8 pases adicionales de renderizado reduciendo progresivamente la resolución (downsampling) y volviéndola a subir (upsampling), consumiendo un 30-40% del tiempo de GPU por fotograma.
- **Solución:** Detectar en `nvn_draw.cpp` los dibujos dirigidos a niveles de mipmap mayores a cero (`t.views.level[0] > 0`). Si `NX_DISABLE_BLOOM=1` está activo, se omiten esos comandos de dibujo de forma instantánea. El búfer de bloom se queda limpio y el render final no suma intensidad de destello, liberando el ancho de banda de la GPU.

### E. Reducción de Multitudes de NPCs (`NX_NO_CROWD`)
- **Problema:** Zonas urbanas densas (como el cruce de Shibuya o estaciones de tren) instancian decenas de NPCs con skinning de esqueleto, colisiones y tareas de animación que colapsan tanto la CPU como la GPU móvil incluso a resoluciones mínimas (360p).
- **Solución:** Persona 5 Royal define la densidad y modelos de las multitudes en `RESRCCROWDUNIT.BIN`. El runtime intercepta este archivo mediante `add_virtual_file()` y lo reemplaza por un archivo virtual vacío de 0 bytes. El motor del juego omite pacíficamente la instanciación de la multitud, duplicando la tasa de fotogramas en escenarios abiertos sin alterar la historia ni los personajes clave.

---

## 3. Compatibilidad de Plataforma Android y Almacenamiento

### A. Fallo de Verificación de Clases en Android 10 (API 29)
- **Error:** `java.lang.NoSuchMethodError: no static or non-static method "Landroid/os/Environment;.isExternalStorageManager()Z"`
- **Causa:** En Android 10 no existía la API `Environment.isExternalStorageManager()`, introducida en Android 11 (API 30). Si una clase Java menciona directamente este método en su cuerpo, el verificador de clases DEX de Android 10 rechaza la clase completa al cargarla, impidiendo que el Launcher arranque.
- **Solución:** Encapsular todas las llamadas a APIs exclusivas de versiones superiores en clases auxiliares estáticas independientes (`Api30Helper`):
  ```java
  static final class Api30Helper {
      static boolean isExternalStorageManager() {
          return Environment.isExternalStorageManager();
      }
  }
  boolean hasAccess() {
      if (Build.VERSION.SDK_INT < 30) return true;
      return Api30Helper.isExternalStorageManager();
  }
  ```

### B. Sensibilidad a Mayúsculas/Minúsculas en Sistemas de Archivos Android
- **Problema:** Windows (NTFS) y macOS por defecto son insensibles a mayúsculas y minúsculas (`CASE-INSENSITIVE`), por lo que un mod con carpeta `cpk/patch1.cpk` carga sin problemas. En Android (ext4 / f2fs / sdcardfs), el sistema de archivos es estrictamente `CASE-SENSITIVE`. Una petición a `CPK/PATCH1.CPK` fallará con `ENOENT` si el archivo del mod está en minúsculas.
- **Solución Implementada:** Implementar una función de resolución en `hle_fs.cpp` (`find_case_insensitive`) que primero intente la ruta directa (ruta rápida de 0 coste), y si no existe, recorra los directorios comparando nombres mediante `std::tolower()`.

---

## 4. Arquitectura de Entrada: Mandos, Pantalla Táctil y Sensores SixAxis

### A. Distribución de Botones de Mandos Físicos (Nintendo vs Xbox / Estándar Android)
- **Problema:** La disposición física de los mandos de Nintendo Switch sitúa el botón `A` a la derecha y `B` abajo. Los mandos estándar de Android, Xbox, PlayStation y mandos Bluetooth genéricos tienen el botón principal de confirmación abajo (`AKEYCODE_BUTTON_A`).
- **Solución:** Agregar la opción `NX_GAMEPAD_LAYOUT`:
  - Modo `0` (Nintendo): Conserva el orden original de Switch (`BUTTON_A -> B`, `BUTTON_B -> A`).
  - Modo `1` (Xbox/PC): Mapea directamente `BUTTON_A -> A`, `BUTTON_B -> B`, `BUTTON_X -> X`, `BUTTON_Y -> Y`.
- **Ejes de Palanca Alternativos:** Algunos gamepads Bluetooth transmiten el stick derecho en `AXIS_RX` / `AXIS_RY` en lugar de `AXIS_Z` / `AXIS_RZ`. El código implementa un fallback transparente si el primer par devuelve 0.
- **Zona Muerta con Curva Lineal:** En lugar de un corte brusco (`if (abs(v) < 0.15f) return 0; else return v;`), la zona muerta se normaliza matemáticamente:
  $$\text{scaled} = \frac{v - \text{sign}(v) \cdot d}{1.0 - d}$$
  proporcionando un control analógico suave desde el borde de la zona muerta.

### B. Botones Táctiles L3 y R3 y Ajuste de Sensibilidad
- **Problema:** Acciones como correr, abrir el historial de voces o centrar la cámara requieren presionar los joysticks (L3 / R3). Además, pantallas de diferente tamaño o densidad de píxeles hacían que la cámara táctil se moviera demasiado rápido o demasiado lento.
- **Solución:**
  1. Agregar `L3` (`kStickL`) y `R3` (`kStickR`) a la matriz de botones táctiles con glifos dedicados en el shader de presentación.
  2. Añadir `NX_STICK_SENSITIVITY` y `NX_CAMERA_SENSITIVITY` (de 50% a 250%) aplicados como multiplicadores a `dx` y `dy` en `read_pad()`.

### C. Emulación de SixAxis / Sensores de Movimiento (Dardos y Minijuegos)
- **Problema:** Minijuegos como los Dardos en Kichijoji requieren mover la consola para apuntar al tablero. Si la llamada `GetSixAxisSensorStates` devuelve 0 estados, el juego asume que no hay sensor conectado y bloquea el minijuego.
- **Solución:**
  1. `GetSixAxisSensorHandles` debe devolver handle válido (`count = 1`).
  2. `GetSixAxisSensorStates` debe retornar `count = 1` y rellenar la estructura de `0x5c` bytes con:
     - Vector de aceleración de gravedad `(0, -1.0f, 0)`.
     - Matriz de orientación de identidad.
     - Velocidad angular (`angularVelocity`) e integración en tiempo (`angle`) conectadas al stick derecho o arrastre táctil de la cámara. Esto permite apuntar con total precisión en el minijuego de dardos incluso en dispositivos sin giroscopio físico.

---

## 5. Arquitectura de Mods: RomFS, Atmosphere, Eden y ExeFS SubSDK

### A. Detección Inteligente de Carpetas de Mods
El sistema LayeredFS ahora soporta tres distribuciones comunes de la comunidad:
1. **Layout Directo Atmosphere:** `mods/romfs/...` o `atmosphere/contents/<title_id>/romfs/...`.
2. **Layout Eden / Yuzu:** `mods/<NombreMod>/romfs/...`.
3. **Layout de Archivos Sueltos:** `mods/<NombreMod>/` que contenga directamente carpetas de juego (`CPK`, `sound`, `movie`, `field`, etc.).

### B. Carga Dinámica de Módulos ExeFS (Skyline y Plugins CBT)
- **Problema:** Mods avanzados que inyectan código nativo (como Raytwo `p5rcbt` o plugins Skyline) necesitan que se cargue una biblioteca adicional en el espacio ejecutable (`subsdk0` a `subsdk9`). El runtime original solo cargaba `main` y `sdk`.
- **Solución:** En `boot()` de `main.cpp`, el cargador escanea `mods/<nombre>/exefs/` buscando `main`, `sdk` y `subsdk0`..`subsdk9`. Todos los módulos encontrados se cargan con `nce::load_nso()` en bases contiguas (`base = kModuleBase + i * 0x10000000`) y se registran en `load_modules()`, permitiendo que los plugins modifiquen la tabla de funciones y enlacen archivos sueltos.

### C. Importación de Paquetes de Traducción `.cpk`
- Para traducciones de la comunidad distribuidas en un único archivo CPK, el Launcher incluye un botón nativo **"Importar CPK"** que copia el archivo seleccionado directamente a `mods/translation/romfs/CPK/PATCH1.CPK`, activando el parche sin necesidad de conectar el teléfono a un PC.

---

## 6. Soporte Multi-Región y Multi-Versión

Para aceptar dumps legítimos de cualquier región sin errores de incompatibilidad:
1. **Title IDs Soportados:**
   - América (US): `01005CA01580E000` (Build base `708BCACA873EA32B`)
   - Europa (EU): `01004B10157F2000` (Build base `B78A03E489E5BD92`)
   - Japón (JP): `0100B880154FC000` (Build base `6E1F4CAE92A28C91`)
   - Asia / Chino Tradicional: `01006BB00C6F0000` (Build base `2F2253ACD6BE5D87`)
2. **Búsqueda Automática de Shaders Regionales:**
   En lugar de buscar únicamente `CPK/ALL_USEU.CPK`, el importador busca sucesivamente `ALL_USEU.CPK`, `ALL_JP.CPK`, `ALL_ASIA.CPK` y `ALL.CPK`.
3. **Switch de Anulación de Versión:**
   Si el jugador utiliza una actualización o mod que altere el hash del ejecutable, la variable de entorno `NX_ALLOW_ANY_BUILD=1` permite continuar la ejecución emitiendo una advertencia en el log en lugar de terminar el proceso con un error fatal.

---

## 7. Telemetría y Exportación de Registros Sin PC (Diagnóstico Rápido en Dispositivos de Usuarios)

Para facilitar el soporte a usuarios que no tienen un PC ni conocimientos de ADB, es imprescindible integrar un exportador de registros directamente en la interfaz del Launcher.

### A. La Trampa del Filtrado Estricto con `-s` (Por qué NO usarlo)
- **El error común:** Ejecutar `logcat -d -s <tag>:V DEBUG:V vulkan:V`.
- **Por qué falla en producción:** La bandera `-s` (*silent*) descarta de forma tajante **cualquier mensaje que no coincida exactamente con las etiquetas especificadas**. Si la aplicación crashea debido a:
  1. Una excepción Java no capturada (`AndroidRuntime`).
  2. Fallos del cargador dinámico / linker de Android (`dlopen failed: undefined symbol`, falta de símbolos de biblioteca o incompatibilidad de ABI).
  3. Abortos de bajo nivel de `libc` (`fatal signal 6 / SIGABRT`).
  4. Cierres por agotamiento de memoria del sistema (`lowmemorykiller` / OOM).
  ...el log resultante queda **completamente vacío o cortado justo antes del error**, impidiendo diagnosticar el fallo.

### B. La Solución Correcta: Cola Circular con `logcat -d -t 500`
En lugar de filtrar por etiquetas, se deben solicitar las últimas 500 líneas completas del buffer de logcat (`logcat -d -t 500`). Esto garantiza capturar el contexto exacto del fallo: excepciones Java, trazas del linker nativo, advertencias de Vulkan, mensajes del driver de GPU y salidas de depuración del motor sin silenciar nada.

### C. Persistencia entre Sesiones con `nx.log.prev` (o `zakuro.log`)
Cuando el juego sufre un cierre inesperado al arrancar, el comportamiento natural del usuario es **volver a abrir la aplicación** para pulsar el botón de exportar registros.
- Al arrancar de nuevo, el motor nativo suele sobreescribir o reiniciar `nx.log` (o `zakuro.log`), perdiéndose el rastro del cierre anterior.
- **Práctica recomendada:** El exportador debe comprobar y concatenar tanto `nx.log` (sesión actual) como `nx.log.prev` (sesión previa al cuelgue), permitiendo ver con precisión quirúrgica qué llamada o módulo provocó la caída.

### D. El Gran Error de `EXTRA_TEXT` vs Envío de Archivo Adjunto (`EXTRA_STREAM`)
- **El problema de compartir texto crudo (`EXTRA_TEXT`):**
  Si se pasa el contenido del log como texto plano con `share.putExtra(Intent.EXTRA_TEXT, sb.toString())`, aplicaciones como WhatsApp, Telegram o Discord lo insertan directamente en la caja de redacción del mensaje.
  1. En textos largos (más de 500 líneas o decenas de kilobytes), las apps de mensajería sufren lag o recortan el mensaje por límite de caracteres.
  2. Los usuarios novatos intentan seleccionar texto y terminan copiando solo las primeras 5 líneas visibles (perdiendo el diagnóstico real que está más abajo).
- **La solución profesional: Adjuntar el `.txt` como Documento (`EXTRA_STREAM` + `ContentProvider`):**
  Al enviar el URI del archivo mediante `Intent.EXTRA_STREAM` a través de un `ContentProvider` (`LogProvider`), WhatsApp, Discord y Telegram adjuntan el archivo directamente como documento (`p5r-log.txt`). El usuario solo presiona "Enviar" en el chat y el archivo llega 100% íntegro, sin recortes ni esfuerzo manual.

### E. Implementación de Referencia (Java)

#### 1. Proveedor de Archivo (`LogProvider.java`)
Sin depender de bibliotecas externas (funciona en builds sin Gradle usando la API nativa de Android):
```java
package io.github.p5rrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import java.io.File;
import java.io.FileNotFoundException;

public class LogProvider extends ContentProvider {
    @Override public boolean onCreate() { return true; }
    @Override public String getType(Uri uri) { return "text/plain"; }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File file = new File(uri.getPath());
        if (!file.exists()) throw new FileNotFoundException("File not found: " + uri.getPath());
        return ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] selectionArgs, String sortOrder) {
        MatrixCursor cursor = new MatrixCursor(new String[] {
            OpenableColumns.DISPLAY_NAME,
            OpenableColumns.SIZE
        });
        File file = new File(uri.getPath());
        if (file.exists()) cursor.addRow(new Object[] { file.getName(), file.length() });
        return cursor;
    }

    @Override public Uri insert(Uri uri, ContentValues values) { return null; }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection, String[] selectionArgs) { return 0; }
}
```

#### 2. Declaración en `AndroidManifest.xml`
```xml
<provider
    android:name=".LogProvider"
    android:authorities="io.github.p5rrecomp.logprovider"
    android:exported="false"
    android:grantUriPermissions="true" />
```

#### 3. Método en el Launcher (`Launcher.java`)
```java
void exportLogs() {
    try {
        File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        if (!dl.exists()) dl.mkdirs();
        File out = new File(dl, "game-log.txt");
        StringBuilder sb = new StringBuilder();
        
        // Cabecera con datos esenciales del hardware
        sb.append("=== Game Recomp Log Export ===\n");
        sb.append("Device: ").append(Build.MANUFACTURER).append(" ").append(Build.MODEL).append("\n");
        sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        sb.append("Date: ").append(new java.util.Date().toString()).append("\n\n");

        // 1. Captura de logcat amplia (últimas 500 líneas sin filtrar)
        try {
            Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-d", "-t", "500"});
            try (BufferedReader br = new BufferedReader(new InputStreamReader(p.getInputStream()))) {
                String line;
                while ((line = br.readLine()) != null) sb.append(line).append("\n");
            }
        } catch (Exception e) {
            sb.append("logcat capture error: ").append(e.getMessage()).append("\n");
        }

        // 2. Registro nativo de la sesión actual
        File nxLog = new File(dataDir(), "nx.log");
        if (nxLog.exists()) {
            sb.append("\n=== nx.log ===\n");
            try (BufferedReader br = new BufferedReader(new FileReader(nxLog))) {
                String line;
                while ((line = br.readLine()) != null) sb.append(line).append("\n");
            }
        }

        // 3. Registro nativo de la sesión previa al cuelgue
        File nxLogPrev = new File(dataDir(), "nx.log.prev");
        if (nxLogPrev.exists()) {
            sb.append("\n=== nx.log.prev ===\n");
            try (BufferedReader br = new BufferedReader(new FileReader(nxLogPrev))) {
                String line;
                while ((line = br.readLine()) != null) sb.append(line).append("\n");
            }
        }

        // 4. Guardar archivo en Downloads
        try (FileWriter fw = new FileWriter(out)) {
            fw.write(sb.toString());
        }

        Toast.makeText(this, "Registros exportados a Download/" + out.getName(), Toast.LENGTH_SHORT).show();

        // 5. Lanzar hoja de compartir adjuntando el ARCHIVO directamente
        Uri logUri = Uri.parse("content://" + getPackageName() + ".logprovider" + out.getAbsolutePath());
        Intent share = new Intent(Intent.ACTION_SEND);
        share.setType("text/plain");
        share.putExtra(Intent.EXTRA_STREAM, logUri);
        share.putExtra(Intent.EXTRA_SUBJECT, "Game Debug Log");
        share.setClipData(android.content.ClipData.newRawUri("Log", logUri));
        share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        startActivity(Intent.createChooser(share, "Compartir archivo de registros"));
    } catch (Exception e) {
        Toast.makeText(this, "Error al exportar registros: " + e.getMessage(), Toast.LENGTH_LONG).show();
    }
}
```

---

## 8. Checklist Rápido para Nuevos Ports Recomp

Al iniciar el port de cualquier otro juego de Nintendo Switch a Android:
- [ ] **Comprobar formatos de compresión:** ¿Usa el juego texturas BC o ASTC? Si usa BC, verificar soporte en hardware Mali/Xclipse y prever fallback por CPU.
- [ ] **Verificar formatos de color HDR:** Si el juego usa `B10G11R11_UFLOAT`, forzar fallback a `RGBA16F` en GPUs Mali para evitar pantallas verdes/púrpuras.
- [ ] **Configurar pases de sombras y bloom:** Añadir switches de entorno (`NX_SHADOWS=0`, `NX_DISABLE_BLOOM=1`) para dispositivos de gama media.
- [ ] **Manejar la API de almacenamiento:** Encapsular comprobaciones de permisos de Android 11+ en helpers estáticos para no quebrar Android 10.
- [ ] **Sensibilidad a mayúsculas:** Usar resolución de archivos insensible a mayúsculas para cualquier mod o recurso externo.
- [ ] **Controles táctiles completos:** Asegurarse de que todos los botones de Switch (incluyendo L3, R3, +, -) estén disponibles y sean reubicables.
- [ ] **Mapeo de mandos:** Proveer opción de inversión AB/XY para usuarios de mandos Xbox/Android.
- [ ] **Sensores SixAxis:** Garantizar que `GetSixAxisSensorStates` retorne al menos 1 muestra y mapear el stick analógico como opción de apuntado.
- [ ] **Módulos ExeFS múltiples:** Soportar carga de `main`, `sdk` y `subsdk0`..`subsdk9` para compatibilidad con plugins de la comunidad.
- [ ] **Herramienta de logs integrada sin PC:** Implementar `logcat -d -t 500` (sin `-s`) y adjuntar `nx.log` + `nx.log.prev` (o `zakuro.log`), con guardado en `Download/` y hoja de compartir de Android (`Intent.ACTION_SEND`) para diagnóstico inmediato de crasheos.
