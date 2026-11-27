package com.hibreak.einkcenter;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.ContentObserver;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;

import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.BufferedReader;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Reproduces the stock PowerManager behaviour that the vendor composer needs
 * for its "full refresh every N pages" counter: raise
 * `vendor.xrz.check_commit` for 800 ms after user input, then drop it again.
 *
 * Why it matters: `vendor.xrz.clean_frequency=N` is only counted for frames
 * whose dirty area is at least 40% of the screen *while check_commit != 0*.
 * Without the pulse the counter never advances, so the automatic full refresh
 * never happens. Stock does this from PowerManagerService on BUTTON/TOUCH
 * activity; on a GSI nothing does.
 *
 * Input is read by streaming the touchscreen's /dev/input node from a root
 * shell (`cat`, unbuffered) and counting struct input_event records.
 * `getevent` is not used: its stdio is block-buffered into a pipe, so events
 * only appear after 4 KiB, and `input tap` injections never reach evdev at all.
 * The pulse is debounced so a gesture produces one pulse, not one per event.
 */
public class EinkDaemonService extends Service {

    private static final String CHANNEL = "eink-daemon";
    private static final int NOTIFICATION_ID = 1001;
    private static final long PULSE_MS = 800;
    private static final long DEBOUNCE_MS = 900;
    private static final long REAP_INTERVAL_MS = 30_000;
    private static EinkDaemonService inputOwner;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private volatile long lastPulse;
    private volatile long lastReap;
    private volatile boolean running;
    private volatile Process touchProcess;
    private volatile Process powerProcess;
    private boolean bottomTouch;
    private int lastTouchX = 360;
    private long lastNavAssist;

    /** Stock Bigme 37-step LM3630A frontlight curve. */
    private static final int[] FL = {0, 2, 20, 30, 40, 50, 60, 70, 80, 90, 102, 108, 114,
            120, 126, 132, 138, 144, 150, 156, 162, 168, 174, 180, 188, 192, 198, 204, 206,
            208, 210, 212, 214, 216, 218, 220, 222};

    private ContentObserver brightnessObserver;
    private final Runnable brightnessApply = this::applySystemBrightness;

    private BroadcastReceiver screenOffReceiver;
    private volatile boolean screenOn;
    private final Runnable wallpaperNext = () -> LockWallpapers.next(this);

    @Override
    public void onCreate() {
        super.onCreate();
        NotificationManager nm = getSystemService(NotificationManager.class);
        NotificationChannel ch = new NotificationChannel(CHANNEL, "E-ink input sync",
                NotificationManager.IMPORTANCE_MIN);
        ch.setShowBadge(false);
        nm.createNotificationChannel(ch);

        Notification n = new Notification.Builder(this, CHANNEL)
                .setContentTitle("E-ink input sync")
                .setContentText("Keeps the auto full-refresh counter in step with touches")
                .setSmallIcon(android.R.drawable.ic_menu_view)
                .setOngoing(true)
                .build();
        startForeground(NOTIFICATION_ID, n);

        // Mirror the Android/Quick Settings brightness slider to both LM3630A
        // channels. Settings.System is changed by SystemUI when the user drags
        // the brightness bar; the vendor Lights HAL does not touch LM3630A.
        brightnessObserver = new ContentObserver(handler) {
            @Override
            public void onChange(boolean selfChange) {
                android.util.Log.i("EINKBL", "brightness changed");
                handler.removeCallbacks(brightnessApply);
                handler.postDelayed(brightnessApply, 120);
            }
        };
        getContentResolver().registerContentObserver(
                Settings.System.getUriFor(Settings.System.SCREEN_BRIGHTNESS),
                false, brightnessObserver);
        // Apply the saved brightness once the service is up (boot / restart).
        handler.postDelayed(brightnessApply, 500);

        LockWallpapers.seed(this);

        PowerManager pm = getSystemService(PowerManager.class);
        screenOn = pm != null && pm.isInteractive();

        // Screen off: frontlight off + next lock wallpaper + reap zombie cgroups.
        // Screen on: restore the saved brightness + reap zombie cgroups.
        screenOffReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent.getAction();
                if (Intent.ACTION_SCREEN_OFF.equals(action)) {
                    screenOn = false;
                    handler.removeCallbacks(brightnessApply);
                    writeBoth(0);
                    reapEmptyCgroups();
                    handler.postDelayed(wallpaperNext, 250);
                } else if (Intent.ACTION_SCREEN_ON.equals(action)) {
                    screenOn = true;
                    reapEmptyCgroups();
                    if (getSharedPreferences("eink", MODE_PRIVATE)
                            .getBoolean("light_on", true)) {
                        handler.postDelayed(brightnessApply, 300);
                    }
                }
            }
        };
        IntentFilter screenFilter = new IntentFilter();
        screenFilter.addAction(Intent.ACTION_SCREEN_OFF);
        screenFilter.addAction(Intent.ACTION_SCREEN_ON);
        registerReceiver(screenOffReceiver, screenFilter);

        running = true;
        startInputWatchers(this);
    }

    private static synchronized void startInputWatchers(EinkDaemonService service) {
        if (inputOwner != null && inputOwner.running) return;
        inputOwner = service;
        // Clean orphan readers from an earlier service instance/process. The
        // anchored pattern cannot match the su wrapper or this shell command.
        RootShell.get().query("pkill -f '^cat /dev/input/event[12]$' 2>/dev/null; echo cleaned", 2_000);
        new Thread(service::watchInput, "eink-getevent").start();
        new Thread(service::watchPowerKey, "eink-power-key").start();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        running = false;
        synchronized (EinkDaemonService.class) {
            if (inputOwner == this) inputOwner = null;
        }
        if (touchProcess != null) touchProcess.destroy();
        if (powerProcess != null) powerProcess.destroy();
        handler.removeCallbacksAndMessages(null);
        if (brightnessObserver != null) {
            getContentResolver().unregisterContentObserver(brightnessObserver);
            brightnessObserver = null;
        }
        if (screenOffReceiver != null) {
            unregisterReceiver(screenOffReceiver);
            screenOffReceiver = null;
        }
        RootShell.get().cmd("setprop vendor.xrz.check_commit 0");
        super.onDestroy();
    }

    /** Apply the Android brightness setting to both frontlight channels. */
    private void applySystemBrightness() {
        PowerManager pm = getSystemService(PowerManager.class);
        screenOn = pm == null || pm.isInteractive();
        if (!screenOn) {
            writeBoth(0);
            return;
        }
        int v;
        try {
            v = Settings.System.getInt(getContentResolver(),
                    Settings.System.SCREEN_BRIGHTNESS);
        } catch (Settings.SettingNotFoundException e) {
            return;
        }
        int step = Math.round(v / 255f * (FL.length - 1));
        step = Math.max(0, Math.min(step, FL.length - 1));
        int code = v <= 0 ? 0 : FL[step];
        android.util.Log.i("EINKBL", "settings brightness=" + v + " -> code=" + code);
        writeBoth(code);
    }

    private void writeBoth(int code) {
        android.util.Log.i("EINKBL", "writeBoth " + code);
        RootShell.get().cmd("for ch in cold warm; do "
                + "P=$(ls /sys/devices/platform/*/i2c-*/2-0036/lm3630a_${ch}_light "
                + "2>/dev/null | head -1); "
                + "[ -n \"$P\" ] && echo " + code + " > \"$P\"; done");
    }

    /** Reclaim empty per-PID cgroup2 directories that pin ~900 KB of SUnreclaim slab each. */
    private static void reapEmptyCgroups() {
        RootShell.get().cmd("find /sys/fs/cgroup -mindepth 3 -maxdepth 3 -type d -name 'pid_*' -exec rmdir {} + 2>/dev/null");
    }

    private void watchInput() {
        while (running) {
            Process p = null;
            try {
                String node = findTouchNode();
                android.util.Log.i("EINKD", "touch node=" + node);
                p = Runtime.getRuntime().exec(new String[]{"su", "-c",
                        "cat /dev/input/" + node});
                touchProcess = p;
                InputStream in = p.getInputStream();
                byte[] rec = new byte[24]; // 64-bit struct input_event
                while (running) {
                    int off = 0;
                    while (off < rec.length) {
                        int n = in.read(rec, off, rec.length - off);
                        if (n < 0) break;
                        off += n;
                    }
                    if (off < rec.length) break;
                    handleTouch(rec);
                    pulse();
                }
            } catch (Exception e) {
                android.util.Log.e("EINKD", "watch failed: " + e);
            } finally {
                if (p != null) p.destroy();
            }
            try {
                Thread.sleep(2000);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    /** Resolve the touchscreen evdev node by name (fts_ts on this device). */
    private String findTouchNode() {
        try {
            String out = RootShell.get().query(
                    "for d in /sys/class/input/event*; do "
                    + "echo \"${d##*/} $(cat $d/device/name 2>/dev/null)\"; done", 5_000);
            String fallback = null;
            for (String line : out.split("\n")) {
                String[] parts = line.trim().split("\\s+", 2);
                if (parts.length < 2) continue;
                String name = parts[1].toLowerCase();
                if (fallback == null) fallback = parts[0];
                if (name.contains("fts") || name.contains("touch")) return parts[0];
            }
            if (fallback != null) return fallback;
        } catch (Exception e) {
            android.util.Log.e("EINKD", "resolve failed: " + e);
        }
        return "event2";
    }

    private void watchPowerKey() {
        while (running) {
            Process p = null;
            try {
                String node = findPowerNode();
                android.util.Log.i("EINKD", "power key node=" + node);
                p = Runtime.getRuntime().exec(new String[]{"su", "-c",
                        "cat /dev/input/" + node});
                powerProcess = p;
                InputStream in = p.getInputStream();
                byte[] rec = new byte[24];
                while (running) {
                    int off = 0;
                    while (off < rec.length) {
                        int n = in.read(rec, off, rec.length - off);
                        if (n < 0) break;
                        off += n;
                    }
                    if (off < rec.length) break;
                    ByteBuffer b = ByteBuffer.wrap(rec).order(ByteOrder.LITTLE_ENDIAN);
                    b.position(16);
                    int type = b.getShort() & 0xffff;
                    int code = b.getShort() & 0xffff;
                    int value = b.getInt();
                    if (type == 1 && code == android.view.KeyEvent.KEYCODE_POWER && value == 1) {
                        EinkAccessibilityService.notePowerKey();
                    }
                }
            } catch (Exception e) {
                android.util.Log.e("EINKD", "power-key watch failed: " + e);
            } finally {
                if (p != null) p.destroy();
            }
            try {
                Thread.sleep(2000);
            } catch (InterruptedException e) {
                return;
            }
        }
    }

    private String findPowerNode() {
        try {
            String out = RootShell.get().query(
                    "for d in /sys/class/input/event*; do "
                            + "echo \"${d##*/} $(cat $d/device/name 2>/dev/null)\"; done", 5_000);
            String fallback = null;
            for (String line : out.split("\n")) {
                String[] parts = line.trim().split("\\s+", 2);
                if (parts.length < 2) continue;
                String name = parts[1].toLowerCase();
                if (fallback == null && name.contains("key")) fallback = parts[0];
                if (name.contains("pmic-key") || name.contains("gpio_key")) return parts[0];
            }
            if (fallback != null) return fallback;
        } catch (Exception e) {
            android.util.Log.e("EINKD", "power-node resolve failed: " + e);
        }
        return "event1";
    }

    private void handleTouch(byte[] rec) {
        ByteBuffer b = ByteBuffer.wrap(rec).order(ByteOrder.LITTLE_ENDIAN);
        b.position(16);
        int type = b.getShort() & 0xffff;
        int code = b.getShort() & 0xffff;
        int value = b.getInt();
        if (type == 1 && code == 330 && value == 0) { // BTN_TOUCH up
            bottomTouch = false;
            return;
        }
        if (type == 1 && code == 57 && value == -1) { // tracking id up
            bottomTouch = false;
            return;
        }
        if (type != 3) return;
        if (code == 54) { // ABS_MT_POSITION_X
            lastTouchX = value;
            return;
        }
        if (code != 53) return; // ABS_MT_POSITION_Y
        if (value > 1350 && !bottomTouch) {
            bottomTouch = true;
            long now = SystemClock.elapsedRealtime();
            if (now - lastNavAssist > 3000 && lastTouchX > 80 && lastTouchX < 640) {
                lastNavAssist = now;
                EinkAccessibilityService.requestNavAssist();
            }
        } else if (value < 1200) {
            bottomTouch = false;
        }
    }

    private void pulse() {
        long now = SystemClock.elapsedRealtime();
        EinkAccessibilityService.noteUserActivity();
        if (now - lastPulse < DEBOUNCE_MS) return;
        lastPulse = now;
        if (now - lastReap > REAP_INTERVAL_MS) {
            lastReap = now;
            reapEmptyCgroups();
        }
        RootShell.get().cmd("setprop vendor.xrz.check_commit 1");
        handler.postDelayed(() ->
                        RootShell.get().cmd("setprop vendor.xrz.check_commit 0"),
                PULSE_MS);
    }
}
