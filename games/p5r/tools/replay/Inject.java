// Touch injector for scenario replays (replay.py), run as the shell user with app_process (like scrcpy's server):
// SELinux keeps the shell from writing /dev/input, but the shell may inject input events through InputManager.
// Lines on stdin:
//   B                              a gesture begins: the time base restarts now
//   F d id x y [id x y ...]        d microseconds after the previous frame (or the B), the fingers down and where
//                                  (display pixels); fingers missing from the last frame went up, new ones went down
//   S                              answers "ok" once everything before it is injected
// Build: javac -source 1.8 -target 1.8 -bootclasspath android.jar Inject.java; d8 --output . Inject*.class
// Run: CLASSPATH=/data/local/tmp/inject.dex app_process / Inject
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.Map;

public class Inject {
    static Object manager;
    static Method inject;

    static void setup() throws Exception {
        try {  // Android 14: InputManagerGlobal
            Class<?> c = Class.forName("android.hardware.input.InputManagerGlobal");
            manager = c.getMethod("getInstance").invoke(null);
            inject = c.getMethod("injectInputEvent", InputEvent.class, int.class);
        } catch (ClassNotFoundException e) {
            Class<?> c = Class.forName("android.hardware.input.InputManager");
            manager = c.getMethod("getInstance").invoke(null);
            inject = c.getMethod("injectInputEvent", InputEvent.class, int.class);
        }
    }

    static long downTime;
    static Map<Integer, float[]> fingers = new LinkedHashMap<>();  // id -> x, y, in the order they went down

    static void send(int action, Map<Integer, float[]> set) throws Exception {
        int n = set.size();
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[n];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[n];
        int k = 0;
        for (Map.Entry<Integer, float[]> e : set.entrySet()) {
            props[k] = new MotionEvent.PointerProperties();
            props[k].id = e.getKey();
            props[k].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[k] = new MotionEvent.PointerCoords();
            coords[k].x = e.getValue()[0];
            coords[k].y = e.getValue()[1];
            coords[k].pressure = 1f;
            coords[k].size = 0.05f;
            k++;
        }
        long now = SystemClock.uptimeMillis();
        MotionEvent ev = MotionEvent.obtain(downTime, now, action, n, props, coords, 0, 0, 1f, 1f, 0, 0,
                InputDevice.SOURCE_TOUCHSCREEN, 0);
        inject.invoke(manager, ev, 0);  // INJECT_INPUT_EVENT_MODE_ASYNC
        ev.recycle();
    }

    static int index(Map<Integer, float[]> set, int id) {
        int k = 0;
        for (Integer i : set.keySet()) {
            if (i == id) return k;
            k++;
        }
        return -1;
    }

    static void frame(Map<Integer, float[]> next) throws Exception {
        // Ups first (the finger still in the event), then moves, then downs.
        for (Integer id : new ArrayList<>(fingers.keySet())) {
            if (next.containsKey(id)) continue;
            if (fingers.size() == 1) {
                send(MotionEvent.ACTION_UP, fingers);
            } else {
                send(MotionEvent.ACTION_POINTER_UP | (index(fingers, id) << MotionEvent.ACTION_POINTER_INDEX_SHIFT), fingers);
            }
            fingers.remove(id);
        }
        boolean moved = false;
        for (Map.Entry<Integer, float[]> e : next.entrySet()) {
            float[] p = fingers.get(e.getKey());
            if (p != null && (p[0] != e.getValue()[0] || p[1] != e.getValue()[1])) {
                p[0] = e.getValue()[0];
                p[1] = e.getValue()[1];
                moved = true;
            }
        }
        if (moved) send(MotionEvent.ACTION_MOVE, fingers);
        for (Map.Entry<Integer, float[]> e : next.entrySet()) {
            if (fingers.containsKey(e.getKey())) continue;
            fingers.put(e.getKey(), new float[] {e.getValue()[0], e.getValue()[1]});
            if (fingers.size() == 1) {
                downTime = SystemClock.uptimeMillis();
                send(MotionEvent.ACTION_DOWN, fingers);
            } else {
                send(MotionEvent.ACTION_POINTER_DOWN | (index(fingers, e.getKey()) << MotionEvent.ACTION_POINTER_INDEX_SHIFT),
                        fingers);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        setup();
        BufferedReader in = new BufferedReader(new InputStreamReader(System.in));
        System.out.println("ready");
        System.out.flush();
        long next = System.nanoTime();
        String line;
        while ((line = in.readLine()) != null) {
            if (line.startsWith("B")) {
                next = System.nanoTime();
            } else if (line.startsWith("F")) {
                String[] w = line.trim().split("\\s+");
                next += Long.parseLong(w[1]) * 1000L;
                long wait = next - System.nanoTime();
                if (wait > 0) Thread.sleep(wait / 1000000L, (int) (wait % 1000000L));
                Map<Integer, float[]> set = new LinkedHashMap<>();
                for (int k = 2; k + 2 < w.length; k += 3)
                    set.put(Integer.parseInt(w[k]), new float[] {Float.parseFloat(w[k + 1]), Float.parseFloat(w[k + 2])});
                frame(set);
            } else if (line.startsWith("S")) {
                System.out.println("ok");
                System.out.flush();
            }
        }
        if (!fingers.isEmpty()) frame(new LinkedHashMap<Integer, float[]>());
    }
}
