package io.github.jjkrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
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
    private TextView gameStatus, gameValue, keysValue, iconValue, resolutionValue, languageValue;

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

        languageValue = text("Automatic (device language)", 14, Color.WHITE, false);
        Button language = button("Automatic (device language)");
        language.setOnClickListener(v -> new AlertDialog.Builder(this)
            .setTitle("Text and subtitle language")
            .setItems(new String[]{"Automatic (device language)", "English", "Japanese"}, (d, which) -> {
                String[] values = {"auto", "en", "ja"};
                String[] labels = {"Automatic (device language)", "English", "Japanese"};
                prefs.edit().putString("language", values[which]).apply();
                languageValue.setText(labels[which]);
                language.setText(labels[which]);
            }).show());
        root.addView(row("Text and subtitle language", language));

        resolutionValue = text("480p", 14, Color.WHITE, false);
        Button resolution = button("480p");
        resolution.setOnClickListener(v -> new AlertDialog.Builder(this)
            .setTitle("Resolution")
            .setItems(new String[]{"480p", "720p", "1080p"}, (d, which) -> {
                String[] values = {"480p", "720p", "1080p"};
                prefs.edit().putString("resolution", values[which]).apply();
                resolutionValue.setText(values[which]);
                resolution.setText(values[which]);
            }).show());
        root.addView(row("Resolution", resolution));

        Button play = button("Start runtime test");
        LinearLayout.LayoutParams playParams = new LinearLayout.LayoutParams(-1, dp(56));
        playParams.topMargin = dp(24);
        root.addView(play, playParams);
        play.setOnClickListener(v -> new AlertDialog.Builder(this)
            .setTitle("Experimental runtime")
            .setMessage("The launcher UI is ready, but JJK dump validation/import and game startup are not implemented yet. This build will not launch the game.")
            .setPositiveButton("OK", null).show());
        setContentView(scroll);
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
        Uri u = Uri.parse(uriString);
        String segment = u.getLastPathSegment();
        return segment == null ? u.toString() : segment;
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
