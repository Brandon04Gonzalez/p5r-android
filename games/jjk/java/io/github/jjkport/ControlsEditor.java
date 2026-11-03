package io.github.jjkport;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.util.Locale;
import java.util.Map;

/**
 * Drag the on-screen controls where you want them; the game draws them there (NX_TOUCH_LAYOUT in config.txt, read by
 * runtime/src/touch_layout.cpp). The d-pad and the A B X Y diamond move as a whole; L, ZL, R, ZR, - and + one by one;
 * the stick's ring too (STICK: the stick takes the touches of the half of the screen it is on). Controls that touch
 * each other are drawn red. Full screen like the game (same theme and system bars hidden the same way), so the
 * fractions of the screen mean the same.
 */
public class ControlsEditor extends Activity {
    // The game's layout (a copy of kJJK and the stick's place in runtime/src/touch_layout.cpp): name, label, x, y
    // (0..1 across the landscape screen), radius (a fraction of its height), group (moved together).
    static final String[] NAMES = {"A", "B", "X", "Y", "R", "ZR", "L", "ZL", "MINUS", "PLUS", "UP", "DOWN", "LEFT",
            "RIGHT", "STICK"};
    static final String[] LABELS = {"A", "B", "X", "Y", "R", "ZR", "L", "ZL", "−", "+", "▲", "▼", "◀", "▶", ""};
    static final float[][] DEFAULTS = {
            {0.940f, 0.66f, 0.068f}, {0.875f, 0.80f, 0.068f}, {0.875f, 0.52f, 0.068f}, {0.810f, 0.66f, 0.068f},
            {0.945f, 0.08f, 0.052f}, {0.945f, 0.24f, 0.052f}, {0.055f, 0.08f, 0.052f}, {0.055f, 0.24f, 0.052f},
            {0.440f, 0.07f, 0.045f}, {0.560f, 0.07f, 0.045f},
            {0.135f, 0.31f, 0.048f}, {0.135f, 0.49f, 0.048f}, {0.085f, 0.40f, 0.048f}, {0.185f, 0.40f, 0.048f},
            {0.150f, 0.76f, 0.120f}};
    static final int[] GROUP = {0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 7, 7, 7, 8};
    static final int STICK = 14;

    float[][] pos = new float[NAMES.length][];
    float sizeScale = 1;
    EditorView view;

    String t(String spanish, String english) { return Lang.t(spanish, english); }

    File configFile() { return new File(Launcher.dataDir(), "config.txt"); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Map<String, String> config = Launcher.readConfig(configFile());
        try {
            sizeScale = Math.max(50, Math.min(160, Integer.parseInt(config.get("NX_TOUCH_SIZE")))) / 100f;
        } catch (Exception ignored) {
        }
        reset("1".equals(config.get("NX_TOUCH_MIRROR")));
        parse(config.get("NX_TOUCH_LAYOUT"));

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.rgb(20, 4, 6));
        view = new EditorView(this);
        root.addView(view, new FrameLayout.LayoutParams(-1, -1));

        LinearLayout middle = new LinearLayout(this);
        middle.setOrientation(LinearLayout.VERTICAL);
        middle.setGravity(Gravity.CENTER_HORIZONTAL);
        TextView help = new TextView(this);
        help.setText(t("Arrastra los controles a donde quieras.\nLa cruceta y A B X Y se mueven juntos. En rojo: se tocan.",
                "Drag the controls where you want them.\nThe d-pad and A B X Y move together. Red: they touch."));
        help.setTextColor(Color.rgb(230, 230, 230));
        help.setTextSize(15);
        help.setGravity(Gravity.CENTER);
        middle.addView(help);
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER);
        bar.setPadding(0, dp(12), 0, 0);
        Button restore = button(t("Restablecer", "Reset"), Color.rgb(70, 70, 70));
        Button cancel = button(t("Cancelar", "Cancel"), Color.rgb(70, 70, 70));
        Button save = button(t("Guardar", "Save"), Color.rgb(230, 0, 18));
        restore.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                reset("1".equals(Launcher.readConfig(configFile()).get("NX_TOUCH_MIRROR")));
                view.invalidate();
            }
        });
        cancel.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) { finish(); }
        });
        save.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                Map<String, String> config = Launcher.readConfig(configFile());
                config.put("NX_TOUCH_LAYOUT", serialize());
                if (Launcher.writeConfig(configFile(), config)) {
                    Toast.makeText(ControlsEditor.this, t("Controles guardados", "Controls saved"), Toast.LENGTH_SHORT).show();
                    finish();
                } else {
                    Toast.makeText(ControlsEditor.this, t("No se pudo guardar config.txt", "Could not write config.txt"),
                            Toast.LENGTH_LONG).show();
                }
            }
        });
        for (Button b : new Button[] {restore, cancel, save}) {
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.leftMargin = dp(6);
            lp.rightMargin = dp(6);
            bar.addView(b, lp);
        }
        middle.addView(bar);
        root.addView(middle, new FrameLayout.LayoutParams(-2, -2, Gravity.CENTER));
        setContentView(root);
        hideSystemBars();
    }

    @Override
    public void onWindowFocusChanged(boolean focus) {
        super.onWindowFocusChanged(focus);
        if (focus) hideSystemBars();
    }

    void hideSystemBars() {  // as the game does (runtime/src/platform_android.cpp: 0x1706)
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_FULLSCREEN);
    }

    Button button(String text, int color) {
        Button b = new Button(this);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setBackgroundColor(color);
        b.setMinWidth(dp(120));
        return b;
    }

    void reset(boolean mirrored) {
        for (int i = 0; i < NAMES.length; i++) pos[i] = DEFAULTS[i].clone();
        if (mirrored) pos[STICK][0] = 1 - pos[STICK][0];
    }

    void parse(String layout) {  // NAME:x,y,r;... (what serialize() writes; the runtime reads the same)
        if (layout == null) return;
        for (String item : layout.split(";")) {
            int colon = item.indexOf(':');
            if (colon < 0) continue;
            String[] v = item.substring(colon + 1).split(",");
            for (int i = 0; i < NAMES.length && v.length == 3; i++)
                if (NAMES[i].equals(item.substring(0, colon))) {
                    try {
                        pos[i] = new float[] {Float.parseFloat(v[0]), Float.parseFloat(v[1]), Float.parseFloat(v[2])};
                    } catch (NumberFormatException ignored) {
                    }
                }
        }
    }

    String serialize() {
        StringBuilder s = new StringBuilder();
        for (int i = 0; i < NAMES.length; i++) {
            if (s.length() > 0) s.append(';');
            s.append(String.format(Locale.US, "%s:%.4f,%.4f,%.4f", NAMES[i], pos[i][0], pos[i][1], pos[i][2]));
        }
        return s.toString();
    }

    int dp(int v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    final class EditorView extends View {
        final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        int group = -1;
        float lastX, lastY;

        EditorView(Context c) {
            super(c);
            paint.setTypeface(Typeface.DEFAULT_BOLD);
        }

        float radius(int i) {  // pixels: the stick's ring keeps its size, the buttons follow NX_TOUCH_SIZE
            return pos[i][2] * (i == STICK ? 1 : sizeScale) * getHeight();
        }

        boolean touches(int i) {
            float w = getWidth(), h = getHeight();
            for (int j = 0; j < NAMES.length; j++)
                if (GROUP[j] != GROUP[i]
                        && Math.hypot((pos[i][0] - pos[j][0]) * w, (pos[i][1] - pos[j][1]) * h) < radius(i) + radius(j))
                    return true;
            return false;
        }

        @Override
        protected void onDraw(Canvas c) {
            float w = getWidth(), h = getHeight();
            for (int i = 0; i < NAMES.length; i++) {
                float cx = pos[i][0] * w, cy = pos[i][1] * h, r = radius(i);
                boolean bad = touches(i), held = GROUP[i] == group;
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(bad ? Color.argb(150, 230, 0, 18) : Color.argb(held ? 140 : 80, 255, 255, 255));
                c.drawCircle(cx, cy, r, paint);
                paint.setStyle(Paint.Style.STROKE);
                paint.setStrokeWidth(dp(2));
                paint.setColor(bad ? Color.rgb(255, 60, 60) : Color.argb(220, 255, 255, 255));
                c.drawCircle(cx, cy, r, paint);
                paint.setStyle(Paint.Style.FILL);
                paint.setColor(Color.WHITE);
                paint.setTextAlign(Paint.Align.CENTER);
                String label = i == STICK ? t("Stick", "Stick") : LABELS[i];
                paint.setTextSize(i == STICK ? r * 0.35f : r * 0.8f);
                c.drawText(label, cx, cy - (paint.ascent() + paint.descent()) / 2, paint);
            }
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            float w = getWidth(), h = getHeight();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN: {
                    group = -1;
                    float best = 1e9f;
                    for (int i = 0; i < NAMES.length; i++) {
                        float d = (float) Math.hypot(e.getX() - pos[i][0] * w, e.getY() - pos[i][1] * h) / radius(i);
                        if (d < 1.3f && d < best) {
                            best = d;
                            group = GROUP[i];
                        }
                    }
                    lastX = e.getX();
                    lastY = e.getY();
                    invalidate();
                    return true;
                }
                case MotionEvent.ACTION_MOVE: {
                    if (group < 0) return true;
                    float dx = (e.getX() - lastX) / w, dy = (e.getY() - lastY) / h;
                    // The whole group stays on screen: the move is cut where its first member would leave it.
                    for (int i = 0; i < NAMES.length; i++) {
                        if (GROUP[i] != group) continue;
                        float rx = radius(i) / w, ry = radius(i) / h;
                        dx = Math.max(rx - pos[i][0], Math.min(1 - rx - pos[i][0], dx));
                        dy = Math.max(ry - pos[i][1], Math.min(1 - ry - pos[i][1], dy));
                    }
                    for (int i = 0; i < NAMES.length; i++)
                        if (GROUP[i] == group) {
                            pos[i][0] += dx;
                            pos[i][1] += dy;
                        }
                    lastX = e.getX();
                    lastY = e.getY();
                    invalidate();
                    return true;
                }
                default:
                    group = -1;
                    invalidate();
                    return true;
            }
        }
    }
}
