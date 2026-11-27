package com.hibreak.einkcenter;

import android.content.Context;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * The one serialized refresh-mode writer.
 *
 * <p>Every caller (activity, tile, profile service and floating panel) comes here.
 * Values are kept as strings because HD256 is deliberately outside a signed
 * 31-bit decimal range. Only the six stock modes are accepted before a value is
 * ever placed in a shell command.</p>
 */
public final class EinkController {
    public static final String HD256 = "2147483825";
    public static final String REGAL = "180";
    public static final String FAST = "178";
    public static final String EXTREME = "179";
    public static final String HYPER = "183";
    public static final String A2 = "16";
    public static final String DEFAULT_MODE = FAST;

    private static final String[] MODES = {HD256, REGAL, FAST, EXTREME, HYPER, A2};
    private static final String[] CYCLE = {FAST, EXTREME, HYPER, A2, REGAL, HD256};
    private static final String STATUS = "__EINK_CENTER_MODE_APPLIED__";
    private static final Object LOCK = new Object();
    private static final ExecutorService WORKER = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "eink-mode-controller");
        t.setDaemon(true);
        return t;
    });

    private static String lastAppliedMode;

    private EinkController() { }

    public static String[] modes() {
        return MODES.clone();
    }

    public static String normalizeMode(String value) {
        if (value == null) return null;
        String mode = value.trim();
        for (String allowed : MODES) if (allowed.equals(mode)) return mode;
        return null;
    }

    public static boolean isValidMode(String value) {
        return normalizeMode(value) != null;
    }

    public static String label(String mode) {
        String safe = normalizeMode(mode);
        if (safe == null) return "Unknown";
        if (HD256.equals(safe)) return "HD 256";
        if (REGAL.equals(safe)) return "Regal";
        if (FAST.equals(safe)) return "Fast";
        if (EXTREME.equals(safe)) return "Extreme";
        if (HYPER.equals(safe)) return "Hyper";
        return "A2";
    }

    public static String nextInCycle(String mode) {
        String safe = normalizeMode(mode);
        if (safe == null) return FAST;
        for (int i = 0; i < CYCLE.length; i++) {
            if (CYCLE[i].equals(safe)) return CYCLE[(i + 1) % CYCLE.length];
        }
        return FAST;
    }

    public static String toggleFastHyper(String mode) {
        return HYPER.equals(normalizeMode(mode)) ? FAST : HYPER;
    }

    /**
     * Applies the four stock mode operations, in order, through the long-lived
     * root shell. The status echo only reports completion; it does not alter the
     * mode. Identical consecutive writes are skipped.
     *
     * @return true when applied or already applied; false for an invalid mode or
     *         a failed/timed-out root command
     */
    public static boolean apply(Context context, String mode) {
        String safe = normalizeMode(mode);
        if (safe == null) return false;
        synchronized (LOCK) {
            if (safe.equals(lastAppliedMode)) return true;
            String command = "setprop vendor.xrz.force_global_refresh_mode 0; "
                    + "setprop vendor.xrz.global_refresh_mode " + safe + "; "
                    + "echo " + safe + " > /sys/kernel/debug/eink_debug/global_mode; "
                    + "service call SurfaceFlinger 1008 i32 1; echo " + STATUS;
            String out = RootShell.get().query(command, 8_000);
            if (!containsStatus(out)) return false;
            lastAppliedMode = safe;
            return true;
        }
    }

    /** Changes the user's global choice and applies it on the serialized worker. */
    public static void applyGlobalAsync(Context context, String mode) {
        Context app = context.getApplicationContext();
        WORKER.execute(() -> {
            if (apply(app, mode)) ProfileStore.setGlobalMode(app, mode);
        });
    }

    /** Applies a profile/temporary mode without changing the saved global choice. */
    public static void applyAsync(Context context, String mode) {
        Context app = context.getApplicationContext();
        WORKER.execute(() -> apply(app, mode));
    }

    /**
     * Requests one full-panel GC16 repaint without changing power or lock state.
     * SF transaction 1004 schedules a composite without a redraw side effect; the
     * short delay lets the vendor consume the one-shot mode before it is cleared.
     */
    public static boolean fullRefresh(Context context) {
        String current = lastAppliedMode;
        if (current == null || normalizeMode(current) == null) {
            current = readMode(context);
        }
        String command = "setprop vendor.xrz.force_global_refresh_mode 4; "
                + "service call SurfaceFlinger 1004; "
                + "sleep 0.25; "
                + "setprop vendor.xrz.force_global_refresh_mode -1; "
                + "setprop vendor.xrz.global_refresh_mode " + current + "; "
                + "echo " + current + " > /sys/kernel/debug/eink_debug/global_mode; "
                + "echo " + STATUS;
        String out = RootShell.get().query(command, 4_000);
        return containsStatus(out);
    }

    public static void fullRefreshAsync(Context context) {
        Context app = context.getApplicationContext();
        WORKER.execute(() -> fullRefresh(app));
    }

    /** Reads the live property; preferences are the safe fallback on root failure. */
    public static String readMode(Context context) {
        String out = RootShell.get().query(
                "getprop vendor.xrz.global_refresh_mode", 4_000).trim();
        String safe = normalizeMode(firstLine(out));
        if (safe != null) {
            synchronized (LOCK) {
                lastAppliedMode = safe;
            }
            return safe;
        }
        return ProfileStore.globalMode(context);
    }

    public interface ModeCallback { void accept(String mode); }

    public static void readModeAsync(Context context, ModeCallback callback) {
        Context app = context.getApplicationContext();
        WORKER.execute(() -> {
            String mode = readMode(app);
            if (callback != null) callback.accept(mode);
        });
    }

    private static String firstLine(String value) {
        if (value == null || value.isEmpty()) return "";
        int nl = value.indexOf('\n');
        return nl < 0 ? value : value.substring(0, nl);
    }

    private static boolean containsStatus(String out) {
        return out != null && out.contains(STATUS);
    }
}
