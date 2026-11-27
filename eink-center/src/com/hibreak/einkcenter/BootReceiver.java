package com.hibreak.einkcenter;

import android.app.AlarmManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/**
 * Applies the e-ink panel fix after each boot.
 *
 * Until the composer is told to hand frames to the e-ink engine the panel keeps
 * the boot logo, so the app itself would be invisible and untappable - the fix
 * has to happen by itself.
 *
 * BOOT_COMPLETED only schedules the work: holding goAsync() across the retry
 * window exceeds the broadcast timeout and produces an ANR, so each attempt is
 * a separate alarm instead. The same retries launch the optional HWC/helper
 * boot persistence script when deploy.ps1 has installed it.
 */
public class BootReceiver extends BroadcastReceiver {

    private static final String ACTION_TICK = "com.hibreak.einkcenter.TICK";
    private static final int[] DELAYS_MS = {6000, 12000, 25000, 45000};
    private static final String HWC_PERSIST = "/data/local/tmp/hwc-shim-mt6765/persist.sh";

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();

        if (Intent.ACTION_BOOT_COMPLETED.equals(action)) {
            // A foreground service start is refused after boot on this GSI
            // (ForegroundServiceStartNotAllowedException); do not let that stop
            // the tick alarms, which are what apply the e-ink fix.
            try {
                context.startForegroundService(new Intent(context, EinkDaemonService.class));
            } catch (Exception e) {
                try {
                    context.startService(new Intent(context, EinkDaemonService.class));
                } catch (Exception ignored) {
                }
            }
            AlarmManager am = context.getSystemService(AlarmManager.class);
            if (am == null) return;
            long now = System.currentTimeMillis();
            for (int i = 0; i < DELAYS_MS.length; i++) {
                Intent tick = new Intent(context, BootReceiver.class)
                        .setAction(ACTION_TICK)
                        .putExtra("delay", DELAYS_MS[i]);
                PendingIntent pi = PendingIntent.getBroadcast(context, i, tick,
                        PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
                am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, now + DELAYS_MS[i], pi);
            }
            return;
        }

        if (ACTION_TICK.equals(action)) {
            final PendingResult pending = goAsync();
            new Thread(() -> {
                try {
                    Root.run(EinkUiPreset.applyCommand() + "; "
                            + EinkUiPreset.batteryFirstCommand() + "; "
                            + "setprop persist.vendor.xrz.fps 120; "
                            + "setprop persist.vendor.xrz.fps_high 38; "
                            + "setprop persist.vendor.xrz.fps_low 30; "
                            // Auto full-refresh every 10 page turns: the daemon
                            // pulses check_commit on touch so this counter runs.
                            + "setprop vendor.xrz.auto_clean_enabled 1; "
                            + "setprop vendor.xrz.clean_frequency 10; "
                            // The GSI has no init property handler for
                            // auto_clean_enabled (stock's init.eink.rc writes these
                            // on the property change), so write the kernel nodes
                            // directly. clean_a2 is the kernel's whole-panel fast
                            // path + ghost cleanup: with it off, fast modes ghost
                            // badly and updates are ~2x slower.
                            + "echo 1 > /sys/kernel/debug/eink_debug/clean_a2; "
                            + "echo 1 > /sys/kernel/debug/eink_debug/anti_flicker; "
                            + "setprop vendor.xrz.anti_flicker_enabled 1; "
                            + "setprop vendor.xrz.hwc_idle 1; "
                            // Panel-level anti-aliasing + 256-gray dithering.
                            // The debugfs node is writable by root and resets
                            // on reboot; the vendor prop is otherwise blocked
                            // by the GSI's SELinux policy.
                            + "echo 1 > /sys/kernel/debug/eink_debug/anti_alias; "
                            + "setprop vendor.xrz.anti_alias_enabled 1; "
                            + "setprop vendor.xrz.gray_256_dither 1; "
                            // Fewer full-screen refreshes and no rotation churn.
                            + "settings put system accelerometer_rotation 0; "
                            + "service call SurfaceFlinger 1008 i32 1; "
                            + "settings put global stay_on_while_plugged_in 0; "
                            + "find /sys/fs/cgroup -mindepth 3 -maxdepth 3 -type d -name 'pid_*' -exec rmdir {} + 2>/dev/null; "
                            // Start the HBPC E-ink app's native daemon, if the
                            // patched copy is installed.  It accepts our app's
                            // uid and serves the /sys frontlight nodes.
                            + "killall a9_eink_server 2>/dev/null; "
                            + "nohup /data/local/tmp/a9_eink_server "
                            + ">/data/local/tmp/a9_eink_server.log 2>&1 &");
                    // Reapply the runtime HWC/helper overlay after a DSU reboot.
                    // The script is idempotent, verifies the vendor ABI/SELinux,
                    // and honors persist.disabled left by deploy.ps1 -Revert.
                    Root.run("if [ -x " + HWC_PERSIST + " ]; then "
                            + "nohup sh " + HWC_PERSIST
                            + " >/data/local/tmp/hwc-shim-mt6765/persist-boot.log 2>&1 </dev/null & fi");
                } finally {
                    pending.finish();
                }
            }, "eink-tick").start();
        }
    }
}
