package io.github.p4aurecomp;

import android.app.Activity;
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
 * Prepares Persona 4 Arena Ultimax once on device: checks the NSP with prod.keys and translates
 * its shaders into Internal storage/Persona 4 Arena Ultimax/spirv (Importer, games/p4au/import).
 */
public class ImportActivity extends Activity {
    static final String EXTRA_URI = "uri", EXTRA_NAME = "name";

    TextView status;
    ProgressBar bar;
    Button back;
    final Handler ui = new Handler(Looper.getMainLooper());
    volatile boolean running;

    String t(String es, String en) { return Launcher.isSpanish() ? es : en; }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.rgb(20, 18, 6));
        int pad = dp(32);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText(t("Preparando Persona 4 Arena Ultimax", "Preparing Persona 4 Arena Ultimax"));
        title.setTextColor(Color.rgb(255, 204, 0));
        title.setTextSize(26);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);

        TextView note = new TextView(this);
        note.setText(t("Solo hace falta una vez. Compilando shaders en el teléfono...",
                "Only needed once. Compiling shaders on device..."));
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
                double share = (double) Math.min(done, total) / total;
                int value = phase <= 1 ? 20 : phase == 2 ? 50 + (int) (200 * share) : phase == 3 ? 250 + (int) (730 * share) : 990;
                bar.setProgress(value);
                if (phase == 2)
                    status.setText(String.format(t("Extrayendo los shaders del juego (%d de %d)…",
                            "Extracting the game's shaders (%d of %d)…"), done, total));
                else if (phase == 3)
                    status.setText(String.format(t("Traduciendo los shaders a Vulkan (%d de %d)…",
                            "Translating shaders to Vulkan (%d of %d)…"), done, total));
                else if (phase == 4)
                    status.setText(t("Terminando…", "Finishing…"));
                ui.postDelayed(this, 500);
            }
        }, 500);
    }
}
