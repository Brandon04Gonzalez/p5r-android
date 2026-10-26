package io.github.p5rrecomp;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;

/**
 * Prepares the game once (its own process, ":import": the shader translator's runtime never shares the game's): checks
 * the NSP the player picked with their keys and translates its shaders into Internal storage/Persona 5 Royal/spirv
 * (Importer, games/p5r/import). Started by Launcher with the game's URI and name.
 */
public class ImportActivity extends Activity {
    static final String EXTRA_URI = "uri", EXTRA_NAME = "name";

    TextView status;
    ProgressBar bar;
    Button back;
    final Handler ui = new Handler(Looper.getMainLooper());
    volatile boolean running;

    String t(String spanish, String english) { return Lang.t(spanish, english); }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 4, 6));
        int pad = dp(32);
        root.setPadding(pad, pad, pad, pad);
        TextView title = new TextView(this);
        title.setText(t("Preparando el juego", "Preparing the game"));
        title.setTextColor(Color.rgb(230, 0, 18));
        title.setTextSize(28);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        TextView note = new TextView(this);
        note.setText(t("Solo hace falta una vez. Deja la app abierta hasta que termine (unos minutos).",
                "This is needed only once. Keep the app open until it ends (a few minutes)."));
        note.setTextColor(Color.rgb(200, 200, 200));
        note.setTextSize(15);
        note.setPadding(0, dp(8), 0, dp(20));
        root.addView(note);
        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        root.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(24)));
        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setTextSize(17);
        status.setPadding(0, dp(12), 0, dp(12));
        root.addView(status);
        back = new Button(this);
        back.setAllCaps(false);
        back.setTextSize(18);
        back.setText(t("Volver", "Back"));
        back.setVisibility(View.GONE);
        back.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                finish();
            }
        });
        root.addView(back);
        setContentView(root);
        start();
    }

    @Override
    public void onBackPressed() {
        if (!running) super.onBackPressed();
    }

    void start() {
        final Uri uri = Uri.parse(getIntent().getStringExtra(EXTRA_URI));
        final String name = getIntent().getStringExtra(EXTRA_NAME);
        final File keys = Launcher.keysFile(this, "prod.keys"), titleKeys = Launcher.keysFile(this, "title.keys");
        final File data = Launcher.dataDir(), work = new File(getCacheDir(), "import");
        running = true;
        status.setText(t("Abriendo el juego…", "Opening the game…"));
        new Thread(new Runnable() {
            @Override
            public void run() {
                int rc;
                String error = "";
                try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                    int jobs = Math.max(2, Runtime.getRuntime().availableProcessors());
                    rc = Importer.run(pfd.getFd(), name, keys.getPath(), titleKeys.exists() ? titleKeys.getPath() : "",
                            data.getPath(), work.getPath(), jobs);
                    if (rc != 0) error = Importer.error();
                } catch (Exception e) {
                    rc = 1;
                    error = String.valueOf(e.getMessage());
                }
                final int result = rc;
                final String message = error;
                ui.post(new Runnable() {
                    @Override
                    public void run() {
                        running = false;
                        back.setVisibility(View.VISIBLE);
                        if (result == 0) {
                            bar.setProgress(1000);
                            status.setText(t("Listo. Ya puedes jugar.", "Done. You can play now."));
                            setResult(RESULT_OK);
                        } else {
                            status.setText(t("No se pudo preparar el juego: ", "The game could not be prepared: ") + message);
                        }
                    }
                });
            }
        }).start();
        ui.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!running) return;
                int[] p = Importer.progress();
                int phase = p[0], done = p[1], total = Math.max(1, p[2]);
                // the bar: opening 0-5 %, extracting 5-25 %, translating 25-98 %, finishing to 100 %
                double share = (double) Math.min(done, total) / total;
                int value = phase <= 1 ? 20 : phase == 2 ? 50 + (int) (200 * share) : phase == 3 ? 250 + (int) (730 * share) : 990;
                bar.setProgress(value);
                if (phase == 2)
                    status.setText(Lang.f("Extrayendo los shaders del juego (%d de %d)…", "Extracting the game's shaders (%d of %d)…",
                            done, total));
                else if (phase == 3)
                    status.setText(Lang.f("Traduciendo los shaders (%d de %d)…", "Translating the shaders (%d of %d)…", done,
                            total));
                else if (phase == 4)
                    status.setText(t("Terminando…", "Finishing…"));
                ui.postDelayed(this, 500);
            }
        }, 500);
    }
}
