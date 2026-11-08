package io.github.p4aurecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Typeface;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;

import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.system.Os;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The home screen in front of the game (a NativeActivity, libp4au.so): checks for the player's data in Internal
 * storage/Persona 4 Arena Ultimax, shows options, allows on-device shader compilation directly from the NSP (zero PC
 * required, like P5R), and writes config.txt:
 *   resolution  720p (NX_HANDHELD=1, default) or 1080p (NX_HANDHELD=0)
 *   textures    high (default: the game's own), medium or low (NX_LOD_BIAS 0.5 / 1)
 *   FPS counter off (default) or on (NX_SHOW_FPS)
 *   on-screen controls on (default) or off (NX_TOUCH_SHOW), their size (NX_TOUCH_SIZE) and opacity (NX_TOUCH_OPACITY),
 *               and where they are (NX_TOUCH_LAYOUT, written by ControlsEditor)
 * The frame rate is always the game's 60 fps (NX_FPS_CAP=60): a fighting game's logic counts frames.
 * Other lines of config.txt are kept as they were. A private build for the owner of the game: never published.
 */
public class Launcher extends Activity {
    static final String DATA = "Persona 4 Arena Ultimax";
    static final int PICK_GAME = 1, PICK_KEYS = 2, PREPARE = 3, PICK_DRIVER = 4, PICK_ICON = 5;

    static final class Option {
        final String key, es, en;
        final String[] values, labelsEs, labelsEn;
        int pick;

        Option(String key, String es, String en, String[] values, String[] labelsEs, String[] labelsEn, int pick) {
            this.key = key;
            this.es = es;
            this.en = en;
            this.values = values;
            this.labelsEs = labelsEs;
            this.labelsEn = labelsEn;
            this.pick = pick;
        }
    }

    public static boolean isSpanish() {
        return Locale.getDefault().getLanguage().equals("es");
    }

    final boolean es = isSpanish();
    final List<Option> options = new ArrayList<>();
    final Map<String, String> config = new LinkedHashMap<>();
    TextView status;
    Button play;

    String t(String spanish, String english) { return es ? spanish : english; }

    static File dataDir() { return new File(Environment.getExternalStorageDirectory(), DATA); }

    static final class Api30Helper {
        static boolean isExternalStorageManager() {
            return Environment.isExternalStorageManager();
        }
    }

    boolean hasAccess() {
        if (Build.VERSION.SDK_INT < 30) return true;
        return Api30Helper.isExternalStorageManager();
    }

    SharedPreferences prefs() { return getSharedPreferences("game", MODE_PRIVATE); }
    String gameUri() { return prefs().getString("uri", null); }
    String gameName() { return prefs().getString("name", ""); }

    static File keysFile(Context c, String name) { return new File(new File(c.getFilesDir(), "keys"), name); }
    boolean hasKeys() { return keysFile(this, "prod.keys").length() > 0; }

    boolean gameReadable() {
        if (gameUri() == null) return false;
        try (ParcelFileDescriptor f = getContentResolver().openFileDescriptor(Uri.parse(gameUri()), "r")) {
            return f != null;
        } catch (Exception e) {
            return false;
        }
    }

    boolean prepared() {
        File note = new File(dataDir(), "spirv/import.txt");
        if (!note.canRead()) return false;
        try (BufferedReader r = new BufferedReader(new FileReader(note))) {
            return gameName().equals(r.readLine());
        } catch (Exception e) {
            return false;
        }
    }

    boolean hasFolderData() {
        return new File(dataDir(), "exefs/main").canRead() && new File(dataDir(), "exefs/sdk").canRead();
    }

    boolean hasData() {
        return hasFolderData() || (gameUri() != null && gameReadable());
    }

    void pick(int request, String type, boolean multiple) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(type);
        if (multiple) i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
        startActivityForResult(i, request);
    }

    void prepare() {
        Intent i = new Intent(this, ImportActivity.class);
        i.putExtra(ImportActivity.EXTRA_URI, gameUri());
        i.putExtra(ImportActivity.EXTRA_NAME, gameName());
        startActivityForResult(i, PREPARE);
    }

    boolean openGameForRuntime() {
        try {
            ParcelFileDescriptor f = getContentResolver().openFileDescriptor(Uri.parse(gameUri()), "r");
            if (f == null) return false;
            Os.setenv("NX_GAME_FD", String.valueOf(f.detachFd()), true);
            Os.setenv("NX_GAME_NAME", gameName(), true);
            Os.setenv("NX_KEYS", keysFile(this, "prod.keys").getPath(), true);
            File tk = keysFile(this, "title.keys");
            if (tk.exists()) Os.setenv("NX_TITLE_KEYS", tk.getPath(), true);
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    void onGamePicked(Uri uri) {
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        String name = displayName(uri);
        prefs().edit().putString("uri", uri.toString()).putString("name", name).apply();
        refresh();
        if (hasKeys() && !prepared()) prepare();
    }

    // A home screen shortcut with the player's picture (cropped to a square; an adaptive icon, so the home screen gives
    // it its own shape).
    void onIconPicked(Uri uri) {
        try {
            Bitmap src;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                src = BitmapFactory.decodeStream(in);
            }
            if (src == null) throw new IllegalArgumentException("not a picture");
            int side = Math.min(src.getWidth(), src.getHeight());
            Bitmap square = Bitmap.createBitmap(src, (src.getWidth() - side) / 2, (src.getHeight() - side) / 2, side, side);
            Bitmap icon = Bitmap.createScaledBitmap(square, 432, 432, true);
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null || !sm.isRequestPinShortcutSupported()) {
                tell(t("Icono", "Icon"), t("Tu pantalla de inicio no admite accesos directos.",
                        "Your home screen does not support shortcuts."));
                return;
            }
            Intent launch = new Intent(this, Launcher.class).setAction(Intent.ACTION_MAIN);
            ShortcutInfo info = new ShortcutInfo.Builder(this, "picture_" + System.currentTimeMillis())
                    .setShortLabel("P4AU").setLongLabel("Persona 4 Arena Ultimax").setIcon(Icon.createWithAdaptiveBitmap(icon))
                    .setIntent(launch).build();
            sm.requestPinShortcut(info, null);
            tell(t("Icono", "Icon"), t("Acepta el aviso para poner el acceso directo con tu imagen en la pantalla de inicio. "
                            + "El icono del cajón de apps no cambia: Android no deja que una app cambie el suyo.",
                    "Accept the prompt to put the shortcut with your picture on the home screen. The icon in the app "
                            + "drawer stays: Android does not let an app change its own."));
        } catch (Exception e) {
            tell(t("Icono", "Icon"), t("No se pudo usar esa imagen.", "That picture could not be used."));
        }
    }

    void onKeysPicked(Intent data) {
        File dir = new File(getFilesDir(), "keys");
        if (!dir.exists()) dir.mkdirs();
        List<Uri> uris = new ArrayList<>();
        if (data.getClipData() != null) {
            for (int k = 0; k < data.getClipData().getItemCount(); k++)
                uris.add(data.getClipData().getItemAt(k).getUri());
        } else if (data.getData() != null) {
            uris.add(data.getData());
        }
        boolean prod = false;
        for (Uri u : uris) {
            String n = displayName(u).toLowerCase(Locale.ROOT);
            String target = n.contains("title") ? "title.keys" : "prod.keys";
            if (target.equals("prod.keys")) prod = true;
            try (InputStream in = getContentResolver().openInputStream(u);
                 OutputStream out = new FileOutputStream(new File(dir, target))) {
                if (in != null) {
                    byte[] buf = new byte[8192];
                    int r;
                    while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
                }
            } catch (Exception ignored) {
            }
        }
        if (!prod && !hasKeys()) {
            tell(t("Claves no válidas", "Not a keys file"),
                    t("Ese archivo no tiene las claves. Elige el prod.keys de tu consola.",
                            "That file has no keys. Choose your console's prod.keys."));
        } else {
            Toast.makeText(this, t("Claves guardadas", "Keys saved"), Toast.LENGTH_SHORT).show();
            refresh();
            if (gameUri() != null && !prepared()) prepare();
        }
    }

    // ---- custom GPU driver ------------------------------------------------------------------------------------
    TextView driverName;

    File driverDir() {
        File d = new File(getFilesDir(), "gpu_driver");
        if (!d.exists()) d.mkdirs();
        return d;
    }

    String activeDriver() {
        try (java.io.BufferedReader r = new java.io.BufferedReader(new FileReader(new File(driverDir(), "active.txt")))) {
            String line = r.readLine();
            return line == null ? "" : line.trim();
        } catch (Exception e) {
            return "";
        }
    }

    void showDriver() {
        if (driverName == null) return;
        String a = activeDriver();
        driverName.setText(a.isEmpty() ? t("Driver de GPU: del sistema", "GPU driver: system")
                : String.format(Locale.ROOT, t("Driver de GPU: %s", "GPU driver: %s"), a));
    }

    void removeDriver() {
        File[] files = driverDir().listFiles();
        if (files != null) for (File f : files) f.delete();
        showDriver();
        Toast.makeText(this, t("Driver del sistema activado", "System driver restored"), Toast.LENGTH_LONG).show();
    }

    static void copyDriverStream(InputStream in, File dest) throws java.io.IOException {
        try (OutputStream out = new FileOutputStream(dest)) {
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        }
    }

    void importDriver(Uri uri) {
        try {
            String picked = displayName(uri);
            File dir = driverDir();
            File[] old = dir.listFiles();
            if (old != null) for (File f : old) f.delete();
            String libName = null;
            if (picked.toLowerCase(Locale.ROOT).endsWith(".zip")) {
                String soEntry = null, wanted = null;
                try (java.util.zip.ZipInputStream zin = new java.util.zip.ZipInputStream(getContentResolver().openInputStream(uri))) {
                    java.util.zip.ZipEntry e;
                    while ((e = zin.getNextEntry()) != null) {
                        String base = new File(e.getName()).getName();
                        if (e.isDirectory()) continue;
                        if (base.equals("meta.json")) {
                            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                            byte[] buf = new byte[8192];
                            int n;
                            while ((n = zin.read(buf)) > 0) bo.write(buf, 0, n);
                            java.util.regex.Matcher m = java.util.regex.Pattern.compile("\"libraryName\"\\s*:\\s*\"([^\"]+)\"")
                                    .matcher(bo.toString("UTF-8"));
                            if (m.find()) wanted = m.group(1);
                        } else if (base.endsWith(".so")) {
                            copyDriverStream(zin, new File(dir, base));
                            if (soEntry == null) soEntry = base;
                        }
                    }
                }
                libName = wanted != null && new File(dir, wanted).exists() ? wanted : soEntry;
            } else {
                libName = picked.endsWith(".so") ? picked : picked + ".so";
                try (InputStream in = getContentResolver().openInputStream(uri)) {
                    if (in == null) throw new java.io.IOException("Cannot open selected file");
                    copyDriverStream(in, new File(dir, libName));
                }
            }
            if (libName == null) throw new java.io.IOException(t("No hay ningun .so dentro", "No .so file inside"));
            try (FileWriter w = new FileWriter(new File(dir, "active.txt"))) {
                w.write(libName + "\n");
            }
            showDriver();
            new AlertDialog.Builder(this)
                    .setTitle(t("Driver importado", "Driver imported"))
                    .setMessage(String.format(Locale.ROOT, t("%s\n\nCierra el juego por completo y abrelo otra vez. Para volver al driver del sistema pulsa Quitar.",
                            "%s\n\nClose the game completely and start it again. To go back to the system driver press Remove."), libName))
                    .setPositiveButton("OK", null)
                    .show();
        } catch (Exception e) {
            new AlertDialog.Builder(this)
                    .setTitle(t("No se pudo importar el driver", "Driver import failed"))
                    .setMessage(String.valueOf(e.getMessage()))
                    .setPositiveButton("OK", null)
                    .show();
        }
    }

    void exportLogs() {
        try {
            File dl = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
            if (!dl.exists()) dl.mkdirs();
            File out = new File(dl, "p4au-log.txt");
            StringBuilder sb = new StringBuilder();
            sb.append("=== Persona 4 Arena Ultimax Recomp Log Export ===\n");
            sb.append("Device: ").append(Build.MANUFACTURER).append(" ").append(Build.MODEL).append("\n");
            sb.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
            sb.append("Date: ").append(new java.util.Date().toString()).append("\n\n");

            try {
                Process p = Runtime.getRuntime().exec(new String[] {"logcat", "-d", "-t", "500"});
                try (BufferedReader br = new BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            } catch (Exception e) {
                sb.append("logcat capture error: ").append(e.getMessage()).append("\n");
            }

            File nxLog = new File(dataDir(), "nx.log");
            if (nxLog.exists()) {
                sb.append("\n=== nx.log ===\n");
                try (BufferedReader br = new BufferedReader(new FileReader(nxLog))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            }

            File nxLogPrev = new File(dataDir(), "nx.log.prev");
            if (nxLogPrev.exists()) {
                sb.append("\n=== nx.log.prev ===\n");
                try (BufferedReader br = new BufferedReader(new FileReader(nxLogPrev))) {
                    String line;
                    while ((line = br.readLine()) != null) sb.append(line).append("\n");
                }
            }

            try (FileWriter fw = new FileWriter(out)) {
                fw.write(sb.toString());
            }

            Toast.makeText(this, t("Registros exportados a Download/p4au-log.txt",
                    "Logs exported to Download/p4au-log.txt"), Toast.LENGTH_SHORT).show();

            Uri logUri = Uri.parse("content://" + getPackageName() + ".logprovider" + out.getAbsolutePath());
            Intent share = new Intent(Intent.ACTION_SEND);
            share.setType("text/plain");
            share.putExtra(Intent.EXTRA_STREAM, logUri);
            share.putExtra(Intent.EXTRA_SUBJECT, "Persona 4 Arena Ultimax Debug Log");
            share.setClipData(android.content.ClipData.newRawUri("Log", logUri));
            share.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            startActivity(Intent.createChooser(share, t("Compartir archivo de registros", "Share log file")));
        } catch (Exception e) {
            Toast.makeText(this, t("Error al exportar registros: " + e.getMessage(),
                    "Error exporting logs: " + e.getMessage()), Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        options.add(new Option("NX_HANDHELD", "Resolución", "Resolution", new String[] {"1", "0"},
                new String[] {"720p", "1080p"}, new String[] {"720p", "1080p"}, 0));
        // NX_STRETCH (nvn_present.cpp): bars on the sides of a wide phone, or the picture stretched over the whole
        // screen. (2, enlarged to cover it, cuts the health bars and the clock in P4AU: not offered.)
        options.add(new Option("NX_STRETCH", "Pantalla completa", "Fullscreen",
                new String[] {"0", "1"},
                new String[] {"No (bordes negros)", "Sí (estirar)"},
                new String[] {"No (black bars)", "Yes (stretch)"}, 0));
        options.add(new Option("GRAPHICS", "Texturas", "Textures", new String[] {"high", "medium", "low"},
                new String[] {"Altas", "Medias", "Bajas"}, new String[] {"High", "Medium", "Low"}, 0));
        // NX_FRAMESKIP (nvn_queue.cpp): on a phone too slow to draw every frame of a fight, every other picture is
        // skipped so the game itself keeps its speed; fast phones never skip.
        options.add(new Option("NX_FRAMESKIP", "Saltar cuadros si va lento", "Skip frames when slow",
                new String[] {"1", "0"}, new String[] {"Automático", "Nunca"}, new String[] {"Auto", "Never"}, 0));
        options.add(new Option("NX_SHOW_FPS", "Contador de FPS", "FPS counter", new String[] {"0", "1"},
                new String[] {"Desactivado", "Activado"}, new String[] {"Off", "On"}, 0));
        options.add(new Option("NX_TOUCH_SHOW", "Controles táctiles", "On-screen controls", new String[] {"1", "0"},
                new String[] {"Activados", "Desactivados"}, new String[] {"On", "Off"}, 0));
        options.add(new Option("NX_TOUCH_SIZE", "Tamaño de los botones", "Button size",
                new String[] {"80", "100", "125"}, new String[] {"Pequeño", "Normal", "Grande"},
                new String[] {"Small", "Normal", "Large"}, 1));
        options.add(new Option("NX_TOUCH_OPACITY", "Opacidad de los botones", "Button opacity",
                new String[] {"50", "85", "100"}, new String[] {"Baja", "Normal", "Alta"},
                new String[] {"Low", "Normal", "High"}, 1));
        build();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    // What the play button checks first (access, game file, keys, preparation); false when it started one of those.
    boolean readyToPlay() {
        if (!hasAccess()) {
            askAccess();
            return false;
        }
        if (gameUri() != null) {
            if (!hasKeys()) pick(PICK_KEYS, "*/*", true);
            else if (!prepared()) prepare();
            else return true;
            return false;
        }
        if (!hasData()) {
            pick(PICK_GAME, "*/*", false);
            return false;
        }
        return true;
    }

    // Starts the game.
    void launch() {
        if (gameUri() != null && !openGameForRuntime()) {
            tell(t("No se pudo abrir el juego", "The game could not be opened"), gameName());
            return;
        }
        save();
        startActivity(new Intent(Launcher.this, android.app.NativeActivity.class));
    }

    void addRow(LinearLayout root, String label, String button, View.OnClickListener click) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(4), 0, dp(4));
        TextView name = new TextView(this);
        name.setText(label);
        name.setTextColor(Color.WHITE);
        name.setTextSize(17);
        row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button b = new Button(this);
        b.setAllCaps(false);
        b.setMinWidth(dp(180));
        b.setText(button);
        b.setOnClickListener(click);
        row.addView(b);
        root.addView(row);
    }

    void build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(16, 14, 4));
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("PERSONA 4 ARENA ULTIMAX");
        title.setTextColor(Color.rgb(255, 205, 0));
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(220, 220, 220));
        status.setTextSize(14);
        status.setPadding(0, dp(6), 0, dp(14));
        root.addView(status);

        // Game dump selection (on-device preparation without PC)
        addRow(root, t("Archivo del juego (.nsp / .xci)", "Game file (.nsp / .xci)"), t("Elegir", "Choose"),
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        pick(PICK_GAME, "*/*", false);
                    }
                });
        addRow(root, t("Claves (prod.keys)", "Keys (prod.keys)"), t("Elegir", "Choose"), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_KEYS, "*/*", true);
            }
        });
        addRow(root, t("Icono", "Icon"), t("Elegir imagen", "Choose a picture"), new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_ICON, "image/*", false);
            }
        });

        for (final Option o : options) {
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);
            row.setPadding(0, dp(4), 0, dp(4));
            TextView name = new TextView(this);
            name.setText(t(o.es, o.en));
            name.setTextColor(Color.WHITE);
            name.setTextSize(17);
            row.addView(name, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            final Button value = new Button(this);
            value.setAllCaps(false);
            value.setMinWidth(dp(180));
            row.addView(value);
            value.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    o.pick = (o.pick + 1) % o.values.length;
                    value.setText((es ? o.labelsEs : o.labelsEn)[o.pick]);
                    save();
                }
            });
            value.setTag(o);
            root.addView(row);
        }

        LinearLayout edit = new LinearLayout(this);
        edit.setOrientation(LinearLayout.HORIZONTAL);
        edit.setGravity(Gravity.CENTER_VERTICAL);
        edit.setPadding(0, dp(4), 0, dp(4));
        TextView editName = new TextView(this);
        editName.setText(t("Posición de los controles", "Controls position"));
        editName.setTextColor(Color.WHITE);
        editName.setTextSize(17);
        edit.addView(editName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button editButton = new Button(this);
        editButton.setAllCaps(false);
        editButton.setMinWidth(dp(180));
        editButton.setText(t("Mover", "Move"));
        edit.addView(editButton);
        editButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                save();
                startActivity(new Intent(Launcher.this, ControlsEditor.class));
            }
        });
        root.addView(edit);

        // Custom GPU driver (e.g. Mesa Turnip for Adreno)
        LinearLayout drvRow = new LinearLayout(this);
        drvRow.setOrientation(LinearLayout.HORIZONTAL);
        drvRow.setGravity(Gravity.CENTER_VERTICAL);
        drvRow.setPadding(0, dp(4), 0, dp(4));
        driverName = new TextView(this);
        driverName.setTextColor(Color.WHITE);
        driverName.setTextSize(17);
        drvRow.addView(driverName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button drvImport = new Button(this);
        drvImport.setAllCaps(false);
        drvImport.setMinWidth(dp(88));
        drvImport.setText(t("Importar", "Import"));
        drvRow.addView(drvImport);
        Button drvRemove = new Button(this);
        drvRemove.setAllCaps(false);
        drvRemove.setMinWidth(dp(88));
        drvRemove.setText(t("Quitar", "Remove"));
        drvRow.addView(drvRemove);
        drvImport.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pick(PICK_DRIVER, "*/*", false);
            }
        });
        drvRemove.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                removeDriver();
            }
        });
        root.addView(drvRow);
        showDriver();

        // Debug logs export
        LinearLayout logsRow = new LinearLayout(this);
        logsRow.setOrientation(LinearLayout.HORIZONTAL);
        logsRow.setGravity(Gravity.CENTER_VERTICAL);
        logsRow.setPadding(0, dp(4), 0, dp(4));
        TextView logsName = new TextView(this);
        logsName.setText(t("Registros de errores", "Debug logs"));
        logsName.setTextColor(Color.WHITE);
        logsName.setTextSize(17);
        logsRow.addView(logsName, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        Button logsButton = new Button(this);
        logsButton.setAllCaps(false);
        logsButton.setMinWidth(dp(180));
        logsButton.setText(t("Exportar registros", "Export logs"));
        logsButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                exportLogs();
            }
        });
        logsRow.addView(logsButton);
        root.addView(logsRow);

        play = new Button(this);
        play.setTextSize(20);
        play.setAllCaps(false);
        play.setBackgroundColor(Color.rgb(255, 205, 0));
        play.setTextColor(Color.BLACK);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(18);
        root.addView(play, lp);
        play.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (readyToPlay()) launch();
            }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
    }

    String displayName(Uri uri) {
        try (android.database.Cursor c = getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int nameIndex = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (nameIndex >= 0) return c.getString(nameIndex);
            }
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last == null ? "game.nsp" : last.substring(last.lastIndexOf('/') + 1);
    }

    void refresh() {
        load();
        for (Option o : options) {
            String v = config.get(o.key);
            for (int k = 0; v != null && k < o.values.length; k++)
                if (o.values[k].equals(v)) o.pick = k;
        }
        updateButtons(getWindow().getDecorView());
        if (!hasAccess()) {
            status.setText(t("Hace falta el permiso de acceso a todos los archivos para leer los datos del juego.",
                    "All-files access is needed to read the game data."));
            play.setText(t("Dar permiso", "Grant access"));
        } else if (gameUri() != null && !gameReadable()) {
            status.setText(t("No se puede leer el archivo del juego seleccionado: ", "Selected game file cannot be read: ") + gameName());
            play.setText(t("Elegir juego", "Choose game"));
        } else if (gameUri() != null && !hasKeys()) {
            status.setText(t("Falta prod.keys para preparar el juego.", "prod.keys is needed to prepare the game."));
            play.setText(t("Elegir prod.keys", "Choose prod.keys"));
        } else if (gameUri() != null && !prepared()) {
            status.setText(t("Juego listo para preparar: ", "Game ready to prepare: ") + gameName());
            play.setText(t("Preparar y Jugar", "Prepare & Play"));
        } else if (!hasData()) {
            if (gameUri() == null && dataDir().exists() && !hasFolderData() && (new File(dataDir(), "analysis").exists() || new File(dataDir(), "extract").exists() || new File(dataDir(), "spirv").exists())) {
                status.setText(t("Falta la carpeta exefs (main y sdk) en Almacenamiento interno/" + DATA + " para la versión NCE.",
                        "Missing exefs folder (main and sdk) in Internal storage/" + DATA + " for the NCE version."));
            } else {
                status.setText(t("Elige el archivo .nsp / .xci de tu juego para prepararlo directamente en el teléfono.",
                        "Choose your game's .nsp / .xci to prepare it directly on device."));
            }
            play.setText(t("Elegir juego", "Choose game"));
        } else {
            status.setText(t("Datos listos: ", "Data ready: ") + (gameUri() != null ? gameName() : dataDir().getPath()));
            play.setText(t("Jugar", "Play"));
        }
    }

    void updateButtons(View v) {
        if (v instanceof Button && v.getTag() instanceof Option) {
            Option o = (Option) v.getTag();
            ((Button) v).setText((es ? o.labelsEs : o.labelsEn)[o.pick]);
        } else if (v instanceof android.view.ViewGroup) {
            android.view.ViewGroup g = (android.view.ViewGroup) v;
            for (int k = 0; k < g.getChildCount(); k++) updateButtons(g.getChildAt(k));
        }
    }

    void tell(String title, String message) {
        new AlertDialog.Builder(this).setTitle(title).setMessage(message).setPositiveButton("OK", null).show();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null) return;
        if (request == PICK_GAME && data.getData() != null) {
            onGamePicked(data.getData());
            return;
        }
        if (request == PICK_KEYS) {
            onKeysPicked(data);
            return;
        }
        if (request == PICK_DRIVER && data.getData() != null) {
            importDriver(data.getData());
            return;
        }
        if (request == PICK_ICON && data.getData() != null) {
            onIconPicked(data.getData());
            return;
        }
        if (request == PREPARE) {
            refresh();
            return;
        }
    }

    void askAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    static Map<String, String> readConfig(File f) {
        Map<String, String> out = new LinkedHashMap<>();
        if (!f.canRead()) return out;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null; ) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq > 0) out.put(line.substring(0, eq), line.substring(eq + 1));
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    static boolean writeConfig(File f, Map<String, String> config) {
        try (FileWriter w = new FileWriter(f)) {
            for (Map.Entry<String, String> e : config.entrySet()) w.write(e.getKey() + "=" + e.getValue() + "\n");
            return true;
        } catch (Exception e) {
            return false;
        }
    }

    void load() {
        config.clear();
        config.putAll(readConfig(new File(dataDir(), "config.txt")));
    }

    // config.txt for the next start.
    void save() {
        if (!hasAccess() || !dataDir().isDirectory()) return;
        load();
        // lines an earlier build with online play may have left
        for (String k : new String[] {"net_host", "net_join", "net_wait_s", "net_port", "net_route", "net_delay"})
            config.remove(k);
        for (Option o : options) config.put(o.key, o.values[o.pick]);
        final String hh = config.get("NX_HANDHELD");
        if ("0".equals(hh)) {
            config.put("resolution", "1080p");
            config.put("NX_RENDER_RES", "1080p");
            config.put("NX_RES", "1080p");
        } else {
            config.put("resolution", "720p");
            config.put("NX_RENDER_RES", "720p");
            config.put("NX_RES", "720p");
        }
        for (String k : new String[] {"RENDER_SCALE", "NX_ANISO_MAX", "NX_LOD_BIAS", "NX_SHADOWS"}) config.remove(k);
        // P4AU's logic counts frames (a fighting game): always its own 60 fps, never capped lower.
        config.put("NX_FPS_CAP", "60");
        final String preset = config.get("GRAPHICS");
        if ("low".equals(preset)) config.put("NX_LOD_BIAS", "1");
        else if ("medium".equals(preset)) config.put("NX_LOD_BIAS", "0.5");
        writeConfig(new File(dataDir(), "config.txt"), config);
    }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
