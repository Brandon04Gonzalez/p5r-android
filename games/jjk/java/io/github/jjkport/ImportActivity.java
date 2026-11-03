package io.github.jjkport;

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
import java.io.FileOutputStream;
import java.io.IOException;

/**
 * Prepares the game once (its own process, ":import": the shader translator's runtime never shares the game's): checks
 * the NSP the player picked with their keys and translates its shaders into Internal storage/Jujutsu Kaisen: Cursed Clash/spirv
 * (Importer, games/jjk/import). Started by Launcher with the game's URI and name.
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
        note.setText(t("Los módulos del juego y RomFS se extraen una vez. Mantén la app abierta hasta terminar.",
                "The base game modules and RomFS are extracted once. Keep the app open until import finishes."));
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

    /**
     * Validate the import directory before expensive extraction. A stale journal can be left
     * unwritable on Android's shared-storage filesystem, even when its parent is accessible.
     */
    static void prepareImportDirectory(File data) throws IOException {
        if (!data.isDirectory() && !data.mkdirs() && !data.isDirectory()) {
            throw new IOException("cannot create data folder: " + data.getAbsolutePath());
        }
        File meta = new File(data, ".import");
        if (!meta.isDirectory() && !meta.mkdirs() && !meta.isDirectory()) {
            throw new IOException("cannot create import folder: " + meta.getAbsolutePath());
        }

        File journal = new File(meta, "journal.tsv");
        if (journal.exists()) {
            try (FileOutputStream ignored = new FileOutputStream(journal, true)) {
                // Keep a writable journal so interrupted imports can resume.
            } catch (IOException staleJournal) {
                if (!journal.delete()) {
                    throw new IOException("cannot replace stale import journal: " + journal.getAbsolutePath()
                            + " (" + staleJournal.getMessage() + ")");
                }
            }
        }

        File probe = new File(meta, ".write-test-" + android.os.Process.myPid());
        try (FileOutputStream out = new FileOutputStream(probe, false)) {
            out.write(1);
            out.flush();
            out.getFD().sync();
        } catch (IOException e) {
            throw new IOException("Android cannot write to import folder " + meta.getAbsolutePath()
                    + ": " + e.getMessage() + ". Grant All files access or choose a writable storage location.", e);
        } finally {
            probe.delete();
        }
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
                try {
                    prepareImportDirectory(data);
                    try (ParcelFileDescriptor pfd = getContentResolver().openFileDescriptor(uri, "r")) {
                        if (pfd == null) throw new IOException("Android could not open the selected game file");
                        int jobs = Math.max(2, Runtime.getRuntime().availableProcessors());
                        rc = Importer.run(pfd.getFd(), name, keys.getPath(), titleKeys.exists() ? titleKeys.getPath() : "",
                                data.getPath(), work.getPath(), jobs);
                        if (rc != 0) error = Importer.error();
                    }
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
                            status.setText(t("Listo. Ya puedes jugar.", "Import complete. You can play now."));
                            setResult(RESULT_OK);
                        } else {
                            status.setText(t("No se pudo preparar el juego: ", "The game could not be imported: ") + message);
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
                int phase = p[0], done = p[1], total = p[2];
                // Native progress is reported in MiB. A zero total means the native
                // importer cannot provide a trustworthy total, so never fake 100%.
                double share = total > 0 ? (double) Math.min(done, total) / total : 0.0;
                int value = phase <= 1 ? 20 : phase == 2 ? 50 + (int) (200 * share)
                        : phase == 3 ? (total > 0 ? 250 + (int) (730 * share) : 650) : 990;
                bar.setProgress(value);
                if (phase == 2) {
                    status.setText(total > 0
                            ? Lang.f("Extrayendo módulos del juego (%d de %d MB)…", "Extracting game modules (%d of %d MB)…", done, total)
                            : Lang.f("Extrayendo módulos del juego (%d MB)…", "Extracting game modules (%d MB)…", done));
                } else if (phase == 3) {
                    status.setText(total > 0
                            ? Lang.f("Extrayendo RomFS (%d de %d MB)…", "Extracting RomFS (%d of %d MB)…", done, total)
                            : Lang.f("Extrayendo RomFS (%d MB extraídos)…", "Extracting RomFS (%d MB extracted)…", done));
                }
                else if (phase == 4)
                    status.setText(t("Terminando…", "Finishing…"));
                ui.postDelayed(this, 500);
            }
        }, 500);
    }
}
