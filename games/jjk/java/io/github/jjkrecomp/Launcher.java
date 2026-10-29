package io.github.jjkrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.provider.OpenableColumns;
import android.os.ParcelFileDescriptor;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public final class Launcher extends Activity {
    private static final int PICK_GAME = 1, PICK_KEYS = 2, PICK_ICON = 3;
    private static final int BG = Color.rgb(20, 4, 6), RED = Color.rgb(230, 0, 18);
    private SharedPreferences prefs;
    private static boolean inspectorLoaded;
    private static String inspectorLoadError = "not attempted";
    static { try { System.loadLibrary("jjkinspector"); inspectorLoaded = true; inspectorLoadError = ""; } catch (UnsatisfiedLinkError e) { inspectorLoaded = false; inspectorLoadError = e.getMessage() == null ? e.toString() : e.getMessage(); android.util.Log.e("JJKLauncher", "Native inspector load failed", e); } }
    private native String inspectNative(int gameFd, String gameName, int keysFd);
    private native String importNative(int gameFd, String gameName, int keysFd, String outDir);
    private TextView gameStatus, gameValue, keysValue, iconValue;

    private int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN, WindowManager.LayoutParams.FLAG_FULLSCREEN);
        prefs = getSharedPreferences("jjk_launcher", MODE_PRIVATE);
        buildUi();
        refresh();
    }

    private TextView text(String value, int size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value);
        t.setTextSize(size);
        t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        t.setGravity(Gravity.CENTER_VERTICAL);
        return t;
    }

    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(15);
        return b;
    }

    private LinearLayout row(String label, View control) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        row.setPadding(0, dp(7), 0, dp(7));
        TextView name = text(label, 16, Color.WHITE, false);
        row.addView(name, new LinearLayout.LayoutParams(0, dp(54), 1));
        row.addView(control, new LinearLayout.LayoutParams(dp(210), dp(54)));
        return row;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        scroll.setBackgroundColor(BG);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(28);
        root.setPadding(pad, dp(22), pad, dp(24));
        scroll.addView(root);

        TextView title = text("JUJUTSU KAISEN: CURSED CLASH", 26, RED, true);
        root.addView(title, new LinearLayout.LayoutParams(-1, dp(44)));
        gameStatus = text("JJK Runtime Test · Experimental build", 14, Color.LTGRAY, false);
        root.addView(gameStatus, new LinearLayout.LayoutParams(-1, dp(32)));

        gameValue = text("Not selected", 12, Color.LTGRAY, false);
        Button chooseGame = button("Choose");
        chooseGame.setOnClickListener(v -> pick(PICK_GAME));
        root.addView(row("Game file (.nsp / .xci)", chooseGame));
        root.addView(gameValue, new LinearLayout.LayoutParams(-1, dp(22)));

        keysValue = text("Not selected", 12, Color.LTGRAY, false);
        Button chooseKeys = button("Choose");
        chooseKeys.setOnClickListener(v -> pick(PICK_KEYS));
        root.addView(row("Keys (prod.keys)", chooseKeys));
        root.addView(keysValue, new LinearLayout.LayoutParams(-1, dp(22)));

        iconValue = text("Optional", 12, Color.LTGRAY, false);
        Button chooseIcon = button("Choose a picture");
        chooseIcon.setOnClickListener(v -> pick(PICK_ICON));
        root.addView(row("Icon", chooseIcon));
        root.addView(iconValue, new LinearLayout.LayoutParams(-1, dp(22)));

        addSetting(root, "Text and subtitle language", "language",
            new String[]{"Automatic (device language)", "English", "Japanese"},
            new String[]{"auto", "en-US", "ja"}, 0);
        addSetting(root, "Resolution", "resolution",
            new String[]{"360p", "480p", "540p", "720p", "1080p"},
            new String[]{"640x360", "854x480", "960x540", "1280x720", "1920x1080"}, 3);
        addSetting(root, "Fullscreen (stretch)", "NX_STRETCH",
            new String[]{"Off", "On"}, new String[]{"0", "1"}, 0);
        addSetting(root, "Interface", "NX_HUD_1080",
            new String[]{"720p (faster)", "1080p (sharper text)"}, new String[]{"0", "1"}, 0);
        addSetting(root, "Graphics", "graphics",
            new String[]{"Low", "Medium", "High"}, new String[]{"low", "medium", "high"}, 0);
        addSetting(root, "Frame rate", "NX_FPS_CAP",
            new String[]{"30 fps", "60 fps (experimental)"}, new String[]{"30", "60"}, 0);
        addSetting(root, "Shadows", "NX_SHADOWS",
            new String[]{"Off", "On"}, new String[]{"0", "1"}, 0);
        addSetting(root, "FPS counter", "NX_SHOW_FPS",
            new String[]{"Off", "On"}, new String[]{"0", "1"}, 0);
        addSetting(root, "On-screen controls", "NX_TOUCH_SHOW",
            new String[]{"On", "Off"}, new String[]{"1", "0"}, 0);
        addSetting(root, "Button size", "NX_TOUCH_SIZE",
            new String[]{"Small", "Normal", "Large"}, new String[]{"80", "100", "125"}, 1);
        addSetting(root, "Button opacity", "NX_TOUCH_OPACITY",
            new String[]{"Low", "Normal", "High"}, new String[]{"50", "85", "100"}, 1);

        TextView shaderNote = text("Shader preparation: JJK-specific shader extraction is not implemented yet. First launch may compile graphics pipelines and stutter; cached pipelines depend on the driver.", 13, Color.LTGRAY, false);
        root.addView(shaderNote, new LinearLayout.LayoutParams(-1, dp(70)));

        Button play = button("Validate selected files");
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(-1, dp(56));
        playParams.topMargin = dp(24);
        root.addView(play, playParams);
        play.setOnClickListener(v -> validateSelectedFiles());

        Button importButton = button("Import game data");
        LinearLayout.LayoutParams importParams = new LinearLayout.LayoutParams(-1, dp(56));
        importParams.topMargin = dp(12);
        root.addView(importButton, importParams);
        importButton.setOnClickListener(v -> importSelectedFiles());
        writeRuntimeConfig();
        setContentView(scroll);
    }

    private void addSetting(LinearLayout root, String label, String key, String[] labels, String[] values, int defaultIndex) {
        LinearLayout settingRow = new LinearLayout(this);
        settingRow.setOrientation(LinearLayout.HORIZONTAL);
        settingRow.setGravity(Gravity.CENTER_VERTICAL);
        settingRow.setPadding(0, dp(5), 0, dp(5));
        TextView name = text(label, 16, Color.WHITE, false);
        settingRow.addView(name, new LinearLayout.LayoutParams(0, dp(54), 1));
        Button value = button(labels[0]);
        String saved = prefs.getString("setting_" + key, values[defaultIndex]);
        int selected = defaultIndex;
        for (int i = 0; i < values.length; i++) if (values[i].equals(saved)) selected = i;
        final int[] index = {selected};
        value.setText(labels[selected]);
        settingRow.addView(value, new LinearLayout.LayoutParams(dp(210), dp(54)));
        value.setOnClickListener(v -> {
            index[0] = (index[0] + 1) % values.length;
            value.setText(labels[index[0]]);
            prefs.edit().putString("setting_" + key, values[index[0]]).apply();
            writeRuntimeConfig();
        });
        root.addView(settingRow);
        prefs.edit().putString("setting_" + key, values[selected]).apply();
    }

    private void writeRuntimeConfig() {
        try {
            java.io.File dir = getExternalFilesDir(null);
            if (dir == null) dir = getFilesDir();
            java.io.File file = new java.io.File(dir, "config.txt");
            java.io.FileWriter writer = new java.io.FileWriter(file, false);
            String resolution = prefs.getString("setting_resolution", "1280x720");
            writer.write("resolution=" + resolution + "\n");
            writer.write("NX_STRETCH=" + prefs.getString("setting_NX_STRETCH", "0") + "\n");
            writer.write("NX_HUD_1080=" + prefs.getString("setting_NX_HUD_1080", "0") + "\n");
            writer.write("NX_LOD_BIAS=" + ("low".equals(prefs.getString("setting_graphics", "low")) ? "1" : "medium".equals(prefs.getString("setting_graphics", "low")) ? "0.5" : "0") + "\n");
            writer.write("language=" + prefs.getString("setting_language", "auto") + "\n");
            writer.write("NX_FPS_CAP=" + prefs.getString("setting_NX_FPS_CAP", "30") + "\n");
            writer.write("NX_SHADOWS=" + prefs.getString("setting_NX_SHADOWS", "0") + "\n");
            writer.write("NX_SHOW_FPS=" + prefs.getString("setting_NX_SHOW_FPS", "0") + "\n");
            writer.write("NX_TOUCH_SHOW=" + prefs.getString("setting_NX_TOUCH_SHOW", "1") + "\n");
            writer.write("NX_TOUCH_SIZE=" + prefs.getString("setting_NX_TOUCH_SIZE", "100") + "\n");
            writer.write("NX_TOUCH_OPACITY=" + prefs.getString("setting_NX_TOUCH_OPACITY", "85") + "\n");
            writer.flush();
            writer.close();
        } catch (Exception e) {
            android.util.Log.e("JJKLauncher", "Could not save runtime settings", e);
        }
    }

    private void pick(int kind) {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType(kind == PICK_ICON ? "image/*" : "*/*");
        if (kind == PICK_GAME) i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/octet-stream", "application/x-nintendo-switch-rom"});
        startActivityForResult(i, kind);
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        try { getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION); }
        catch (SecurityException ignored) {}
        String key = request == PICK_GAME ? "game_uri" : request == PICK_KEYS ? "keys_uri" : "icon_uri";
        prefs.edit().putString(key, uri.toString()).apply();
        refresh();
    }

    private String name(String uriString) {
        if (uriString == null) return "Not selected";
        Uri uri = Uri.parse(uriString);
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (column >= 0) {
                    String display = cursor.getString(column);
                    if (display != null && !display.trim().isEmpty()) return display;
                }
            }
        } catch (Exception ignored) {}
        String segment = uri.getLastPathSegment();
        return segment == null ? "Selected file" : segment;
    }

    private long fileSize(Uri uri) throws Exception {
        try (Cursor cursor = getContentResolver().query(uri, new String[]{OpenableColumns.SIZE}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                int column = cursor.getColumnIndex(OpenableColumns.SIZE);
                if (column >= 0 && !cursor.isNull(column)) return cursor.getLong(column);
            }
        }
        try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
            return pfd == null ? -1 : pfd.getStatSize();
        }
    }

    private String validateGameFile(Uri uri) throws Exception {
        long size = fileSize(uri);
        if (size <= 0) return "Game file is empty or its size cannot be read.";
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) return "Android could not open the selected game file.";
            byte[] header = new byte[0x104];
            int total = 0;
            while (total < header.length) {
                int n = in.read(header, total, header.length - total);
                if (n < 0) break;
                total += n;
            }
            boolean nsp = total >= 4 && header[0] == 'P' && header[1] == 'F' && header[2] == 'S' && header[3] == '0';
            boolean xci = total >= 0x104 && header[0x100] == 'H' && header[0x101] == 'E' && header[0x102] == 'A' && header[0x103] == 'D';
            if (!nsp && !xci) return "File header is not a recognized NSP (PFS0) or XCI (HEAD) header. The file may be wrong, split, encrypted differently, or damaged.";
            return (nsp ? "NSP/PFS0" : "XCI/HEAD") + " header recognized; size " + android.text.format.Formatter.formatShortFileSize(this, size) + ". Title ID, keys, and integrity are not verified yet.";
        }
    }

    private String validateKeys(Uri uri) throws Exception {
        long size = fileSize(uri);
        if (size <= 0 || size > 4L * 1024 * 1024) return "prod.keys is empty or unexpectedly large.";
        int valid = 0;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(getContentResolver().openInputStream(uri), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                String s = line.trim();
                if (s.isEmpty() || s.startsWith("#")) continue;
                int eq = s.indexOf('=');
                if (eq <= 0 || eq == s.length() - 1) return "prod.keys format error: expected key=value lines.";
                String value = s.substring(eq + 1).trim();
                if (!value.matches("(?i)[0-9a-f]+")) return "prod.keys contains a value that is not hexadecimal.";
                valid++;
            }
        }
        return valid > 0 ? "Keys file syntax looks valid (" + valid + " entries). Key correctness is not verified." : "No key entries found in prod.keys.";
    }

    private void importSelectedFiles() {
        String gameString = prefs.getString("game_uri", null);
        String keysString = prefs.getString("keys_uri", null);
        if (gameString == null || keysString == null) {
            new AlertDialog.Builder(this).setTitle("Files missing")
                .setMessage("Choose your own game dump and prod.keys first.")
                .setPositiveButton("OK", null).show();
            return;
        }
        if (!inspectorLoaded) {
            new AlertDialog.Builder(this).setTitle("Importer unavailable")
                .setMessage("The native importer library did not load: " + inspectorLoadError)
                .setPositiveButton("OK", null).show();
            return;
        }
        java.io.File out = new java.io.File(getFilesDir(), "jjk-import");
        java.io.File parent = out.getParentFile();
        long usable = parent == null ? 0 : parent.getUsableSpace();
        long required = 8L * 1024L * 1024L * 1024L;
        if (usable < required) {
            new AlertDialog.Builder(this).setTitle("Not enough storage")
                .setMessage("JJK extraction needs at least 8 GB free in app storage. Available: " +
                    android.text.format.Formatter.formatFileSize(this, usable) +
                    ". Free space first; the importer has not started.")
                .setPositiveButton("OK", null).show();
            return;
        }
        gameStatus.setText("Importing and verifying game data… Keep the app open.");
        new Thread(() -> {
            String message;
            try {
                Uri gameUri = Uri.parse(gameString);
                Uri keysUri = Uri.parse(keysString);
                try (ParcelFileDescriptor gamePfd = getContentResolver().openFileDescriptor(gameUri, "r");
                     ParcelFileDescriptor keysPfd = getContentResolver().openFileDescriptor(keysUri, "r")) {
                    if (gamePfd == null || keysPfd == null) throw new Exception("Cannot open selected files.");
                    message = importNative(gamePfd.getFd(), name(gameString), keysPfd.getFd(), out.getAbsolutePath());
                }
            } catch (Exception e) {
                message = "IMPORT_ERROR: " + e.getClass().getSimpleName() + ": " +
                    (e.getMessage() == null ? "import failed" : e.getMessage());
            }
            final String result = message;
            runOnUiThread(() -> {
                gameStatus.setText(result.startsWith("IMPORT_OK:") ?
                    "Import completed. Game startup is still experimental." : "Import did not complete.");
                new AlertDialog.Builder(this).setTitle("JJK import result").setMessage(result)
                    .setPositiveButton("OK", null).show();
            });
        }).start();
    }

    private void validateSelectedFiles() {
        String gameString = prefs.getString("game_uri", null);
        String keysString = prefs.getString("keys_uri", null);
        if (gameString == null || keysString == null) {
            new AlertDialog.Builder(this).setTitle("Files missing")
                .setMessage("Choose both your own game dump and prod.keys first.")
                .setPositiveButton("OK", null).show();
            return;
        }
        gameStatus.setText("Validating selected files…");
        new Thread(() -> {
            String result;
            try {
                Uri gameUri = Uri.parse(gameString);
                Uri keysUri = Uri.parse(keysString);
                String gameResult = validateGameFile(gameUri);
                String keyResult = validateKeys(keysUri);
                if (inspectorLoaded && gameResult.startsWith("XCI/HEAD") || inspectorLoaded && gameResult.startsWith("NSP/PFS0")) {
                    try (ParcelFileDescriptor gamePfd = getContentResolver().openFileDescriptor(gameUri, "r");
                         ParcelFileDescriptor keysPfd = getContentResolver().openFileDescriptor(keysUri, "r")) {
                        if (gamePfd == null || keysPfd == null) throw new Exception("Could not open selected files for native inspection.");
                        String nativeResult = inspectNative(gamePfd.getFd(), name(gameString), keysPfd.getFd());
                        if (nativeResult.startsWith("INSPECT_ERROR:")) {
                            gameResult += "\nNative inspection failed: " + nativeResult.substring("INSPECT_ERROR:".length()).trim();
                        } else {
                            gameResult += "\nNative metadata:\n" + nativeResult;
                            if (nativeResult.contains("TITLE_ID=01002FC012548000")) gameResult += "\nTitle ID matches JJK target.";
                            else gameResult += "\nWARNING: Title ID does not match the configured JJK target.";
                        }
                    }
                } else if (!inspectorLoaded) {
                    gameResult += "\nNative inspector library failed to load: " + inspectorLoadError;
                }
                result = "Game: " + gameResult + "\n\nKeys: " + keyResult;
            } catch (Exception e) {
                result = "Validation failed: " + e.getClass().getSimpleName() + ": " + (e.getMessage() == null ? "cannot read selected file" : e.getMessage());
            }
            final String message = result;
            runOnUiThread(() -> {
                gameStatus.setText(message.startsWith("Validation failed") ? "Validation failed. See details." : "Basic file checks complete. Import and game startup are not implemented yet.");
                new AlertDialog.Builder(this).setTitle("File validation").setMessage(message)
                    .setPositiveButton("OK", null).show();
            });
        }).start();
    }

    private void refresh() {
        String game = prefs.getString("game_uri", null);
        String keys = prefs.getString("keys_uri", null);
        String icon = prefs.getString("icon_uri", null);
        gameValue.setText(name(game));
        keysValue.setText(name(keys));
        iconValue.setText(icon == null ? "Optional" : name(icon));
        if (game != null && keys != null) {
            gameStatus.setText("Files selected. Dump validation has not run yet.");
        } else {
            gameStatus.setText("Select your own game dump and prod.keys to continue.");
        }
    }
}
