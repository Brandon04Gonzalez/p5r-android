package io.github.p5rrecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileReader;
import java.io.FileWriter;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;

/**
 * The home screen in front of the game (a NativeActivity, libp5r.so): checks for the player's data in Internal
 * storage/Persona 5 Royal (asking for all-files access), shows the options and writes them to that folder's
 * config.txt, whose NX_* lines the runtime turns into its switches:
 *   resolution  720p (NX_HANDHELD=1, default) or 1080p (NX_HANDHELD=0)
 *   render scale 100% (default) or 75% (NX_RENDER_RES: the game renders for 960x540 / 1440x810 and the picture is
 *               scaled up; ~20% less GPU time on an Adreno 610, for weak GPUs)
 *   frame rate  30 (NX_FPS_CAP=30, default) or 60 (NX_FPS_CAP=60: games/p5r/game.cpp patches the engine's interval)
 *   FPS counter off (default) or on (NX_SHOW_FPS)
 *   on-screen controls on (default) or off (NX_TOUCH_SHOW), their size (NX_TOUCH_SIZE) and opacity (NX_TOUCH_OPACITY)
 * Other lines of config.txt are kept as they were. A private build for the owner of the game: never published.
 */
public class Launcher extends Activity {
    static final String DATA = "Persona 5 Royal";

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

    final boolean es = Locale.getDefault().getLanguage().equals("es");
    final List<Option> options = new ArrayList<>();
    final Map<String, String> config = new LinkedHashMap<>();
    TextView status;
    Button play;

    String t(String spanish, String english) { return es ? spanish : english; }

    File dataDir() { return new File(Environment.getExternalStorageDirectory(), DATA); }

    boolean hasAccess() { return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager(); }

    boolean hasData() { return new File(dataDir(), "analysis/main.bin").canRead(); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN);
        options.add(new Option("NX_HANDHELD", "Resolución", "Resolution", new String[] {"1", "0"},
                new String[] {"720p", "1080p"}, new String[] {"720p", "1080p"}, 0));
        options.add(new Option("RENDER_SCALE", "Escala de render", "Render scale", new String[] {"100", "75"},
                new String[] {"100%", "75% (rendimiento)"}, new String[] {"100%", "75% (performance)"}, 0));
        options.add(new Option("NX_FPS_CAP", "Fotogramas por segundo", "Frame rate", new String[] {"30", "60"},
                new String[] {"30 fps", "60 fps (experimental)"}, new String[] {"30 fps", "60 fps (experimental)"}, 0));
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

    void build() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 4, 6));
        int pad = dp(24);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("PERSONA 5 ROYAL");
        title.setTextColor(Color.rgb(230, 0, 18));
        title.setTextSize(30);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        status = new TextView(this);
        status.setTextColor(Color.rgb(220, 220, 220));
        status.setTextSize(14);
        status.setPadding(0, dp(6), 0, dp(14));
        root.addView(status);

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

        play = new Button(this);
        play.setTextSize(20);
        play.setAllCaps(false);
        play.setBackgroundColor(Color.rgb(230, 0, 18));
        play.setTextColor(Color.WHITE);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(18);
        root.addView(play, lp);
        play.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (!hasAccess()) {
                    askAccess();
                    return;
                }
                if (!hasData()) {
                    refresh();
                    return;
                }
                save();
                startActivity(new Intent(Launcher.this, android.app.NativeActivity.class));
            }
        });

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root);
        setContentView(scroll);
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
        } else if (!hasData()) {
            status.setText(t("No se encontraron los datos en Almacenamiento interno/" + DATA
                            + " (analysis, extract, spirv) de tu propia copia del juego.",
                    "No game data found in Internal storage/" + DATA
                            + " (analysis, extract, spirv) from your own copy of the game."));
            play.setText(t("Volver a buscar", "Look again"));
        } else {
            status.setText(t("Datos listos: ", "Data ready: ") + dataDir().getPath());
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

    void askAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    void load() {
        config.clear();
        File f = new File(dataDir(), "config.txt");
        if (!f.canRead()) return;
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line; (line = r.readLine()) != null; ) {
                line = line.trim();
                int eq = line.indexOf('=');
                if (eq > 0) config.put(line.substring(0, eq), line.substring(eq + 1));
            }
        } catch (Exception ignored) {
        }
    }

    void save() {
        if (!hasAccess() || !dataDir().isDirectory()) return;
        load();
        for (Option o : options) config.put(o.key, o.values[o.pick]);
        // RENDER_SCALE is the launcher's own key; the runtime reads NX_RENDER_RES, which follows the resolution.
        if ("75".equals(config.get("RENDER_SCALE")))
            config.put("NX_RENDER_RES", "1".equals(config.get("NX_HANDHELD")) ? "960x540" : "1440x810");
        else
            config.remove("NX_RENDER_RES");
        try (FileWriter w = new FileWriter(new File(dataDir(), "config.txt"))) {
            for (Map.Entry<String, String> e : config.entrySet()) w.write(e.getKey() + "=" + e.getValue() + "\n");
        } catch (Exception ignored) {
        }
    }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }
}
