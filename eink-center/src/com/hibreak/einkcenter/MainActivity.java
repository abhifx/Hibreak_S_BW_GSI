package com.hibreak.einkcenter;

import android.app.Activity;
import android.app.AlertDialog;
import android.graphics.drawable.GradientDrawable;
import android.os.Bundle;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Switch;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * E-ink Center for the Bigme HiBreak on a Treble/GSI ROM.
 *
 * Bigme's stock E-ink Center runs inside their patched framework. Everything it
 * drove is reachable underneath (see README): composer props, the
 * /sys/kernel/debug/eink_debug nodes and the frontlight LED. All of it needs
 * root, which phh's su grants through the companion su-stub package.
 */
public class MainActivity extends Activity {

    private static final String EINK = "/sys/kernel/debug/eink_debug";
    private static final String LIGHT = "/sys/class/leds/lcd-backlight/brightness";
    /** LM3630A dual frontlight; glob keeps it model independent. */
    private static final String LM = "/sys/devices/platform/*/i2c-*/2-0036/lm3630a_";

    private static final String[] PROPS = {
            "persist.vendor.xrz.fps",
            "persist.vendor.xrz.fps_high",
            "persist.vendor.xrz.fps_low",
            "vendor.xrz.global_refresh_mode",
            "vendor.xrz.force_global_refresh_mode",
            "vendor.xrz.gray_256_dither",
            "vendor.xrz.color_restoration_enabled",
            "vendor.xrz.auto_clean_enabled",
            "vendor.xrz.enable_home_clean",
            "vendor.xrz.anti_flicker_enabled",
            "vendor.xrz.clean_frequency",
            "vendor.xrz.eink_animation_mode",
    };

    private static final String[] NODES = {
            "global_mode", "contrast", "brightness", "saturation",
            "anti_flicker", "global_dither",
    };

    private final ExecutorService exec = Executors.newSingleThreadExecutor();
    private final Map<String, String> cache = new LinkedHashMap<>();
    private final List<Object[]> switches = new ArrayList<>();
    private final List<Object[]> sliders = new ArrayList<>();
    private final List<Object[]> frontlightBars = new ArrayList<>();

    /** Stock Bigme 37-step frontlight table (raw lm3630a codes). */
    private static final int[] FL = {0, 2, 20, 30, 40, 50, 60, 70, 80, 90, 102, 108, 114, 120,
            126, 132, 138, 144, 150, 156, 162, 168, 174, 180, 188, 192, 198, 204, 206, 208, 210,
            212, 214, 216, 218, 220, 222};
    private final Map<String, Button> modeButtons = new LinkedHashMap<>();

    private TextView status;
    private TextView log;
    private EditText fpsBase;
    private EditText fpsHigh;
    private EditText fpsLow;
    private boolean editingFps;
    private boolean daemonStarted;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);

        ScrollView sv = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(12), dp(12), dp(12), dp(24));
        sv.addView(root);
        setContentView(sv);

        // ---------------------------------------------------------- status
        LinearLayout st = card(root, null);
        status = text("");
        status.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        status.setLineSpacing(dp(3), 1f);
        st.addView(status);
        row(st, button("Fix panel", this::fixPanel), button("Full refresh", this::fullRefresh));

        // --------------------------------------------------------- refresh
        LinearLayout rf = card(root, "Refresh");
        row(rf, modeButton("2147483825", "HD"), modeButton("180", "Regal"),
                modeButton("178", "Fast"));
        row(rf, modeButton("179", "Extreme"), modeButton("183", "Hyper"),
                modeButton("16", "A2"));
        EditText custom = number("other mode value");
        row(rf, custom, button("Apply", () -> {
            String v = custom.getText().toString().trim();
            if (!v.matches("[0-9]{1,10}")) {
                note("bad mode: " + v);
                return;
            }
            setMode(v);
        }));

        TextView fpsLabel = caption(rf, "Composer fps (base / high / low)");
        LinearLayout fpsRow = new LinearLayout(this);
        fpsRow.setOrientation(LinearLayout.HORIZONTAL);
        fpsBase = number("120");
        fpsHigh = number("38");
        fpsLow = number("30");
        fpsRow.addView(fpsBase, weight());
        fpsRow.addView(fpsHigh, weight());
        fpsRow.addView(fpsLow, weight());
        rf.addView(fpsRow);
        row(rf, button("Apply fps", this::applyFps), button("Stock 120/38/30", () -> {
            fpsBase.setText("120");
            fpsHigh.setText("38");
            fpsLow.setText("30");
            applyFps();
        }));

        // ------------------------------------------------------ frontlight
        LinearLayout lt = card(root, "Frontlight");
        // Master level: writes the same code to both LM3630A channels and keeps
        // the Android brightness setting in sync (so Quick Settings works).
        frontlight(lt, "Level (both)", "cold", "master", true);
        frontlight(lt, "Cold", "cold", "cold", false);
        frontlight(lt, "Warm", "warm", "warm", false);
        caption(lt, "Level drives cold + warm together; Cold/Warm trim each channel.");

        // ------------------------------------------------- lock wallpapers
        LinearLayout lw = card(root, "Lock wallpapers");
        final int[] wcount = {LockWallpapers.count(this)};
        TextView lwInfo = caption(lw, wcount[0] == 0
                ? "Add JPG/PNG/WebP to Pictures/EinkWallpapers"
                : wcount[0] + " images - a new one on every lock");
        row(lw, button("Next wallpaper", () -> bg(() -> {
                    LockWallpapers.next(this);
                    note("lock wallpaper advanced");
                })),
                button("Refresh", () -> {
                    wcount[0] = LockWallpapers.count(this);
                    lwInfo.setText(wcount[0] == 0
                            ? "Add JPG/PNG/WebP to Pictures/EinkWallpapers"
                            : wcount[0] + " images - a new one on every lock");
                }));

        // --------------------------------------------------------- picture
        LinearLayout pn = card(root, "Picture");
        slider(pn, "Contrast", 10, "sys.contrast", v -> sysfs("contrast", String.valueOf(v)));
        slider(pn, "Brightness", 10, "sys.brightness", v -> sysfs("brightness", String.valueOf(v)));
        slider(pn, "Saturation", 10, "sys.saturation", v -> sysfs("saturation", String.valueOf(v)));
        toggle(pn, "256-level dither", "vendor.xrz.gray_256_dither", true);
        toggle(pn, "Color restoration", "vendor.xrz.color_restoration_enabled", false);

        // -------------------------------------------------------- ghosting
        LinearLayout gh = card(root, "Ghosting");
        toggle(gh, "Auto clean", "vendor.xrz.auto_clean_enabled", false);
        toggle(gh, "Clean on home screen", "vendor.xrz.enable_home_clean", false);
        toggle(gh, "Anti-flicker", "vendor.xrz.anti_flicker_enabled", true);
        EditText freq = number("e.g. 10");
        row(gh, freq, button("Set frequency", () -> {
            String v = freq.getText().toString().trim();
            if (v.isEmpty()) return;
            run("setprop vendor.xrz.clean_frequency " + v, "clean_frequency=" + v);
        }));

        // ------------------------------------------------------- animation
        LinearLayout an = card(root, "Animation");
        row(an, button("Off", () -> setAnimation(0)),
                button("1", () -> setAnimation(1)),
                button("2", () -> setAnimation(2)),
                button("3", () -> setAnimation(3)));

        // --------------------------------------------------------- systemui
        LinearLayout ui = card(root, "SystemUI (e-ink)");
        settingToggle(ui, "Opaque shade (no blurs)", "s.blurs", true);
        settingToggle(ui, "High text contrast", "s.contrast", true);
        settingToggle(ui, "No animations", "s.anim", false);
        caption(ui, "Applied through Settings; SystemUI picks them up immediately.");

        // ---------------------------------------------------------- system
        LinearLayout sy = card(root, "System");
        Switch sync = new Switch(this);
        sync.setText("Input sync (auto-clean cadence)");
        sync.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        sync.setPadding(dp(2), dp(8), 0, dp(8));
        boolean on = getPreferences(MODE_PRIVATE).getBoolean("input_sync", true);
        sync.setChecked(on);
        sync.setOnCheckedChangeListener((btn, checked) -> {
            getPreferences(MODE_PRIVATE).edit().putBoolean("input_sync", checked).apply();
            if (checked) startForegroundService(new android.content.Intent(this, EinkDaemonService.class));
            else stopService(new android.content.Intent(this, EinkDaemonService.class));
            note("input sync " + (checked ? "on" : "off"));
        });
        sy.addView(sync);

        row(sy, button("FPS test", () -> startActivity(
                        new android.content.Intent(this, BenchmarkActivity.class))),
                button("Switch to stock ROM", this::switchToStock));
        TextView sysNote = caption(sy,
                "Stock boots after the reboot. Returning to this GSI needs one "
                + "adb command on the PC: adb shell gsi_tool enable && adb reboot");
        sysNote.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);

        // ------------------------------------------------------------- log
        LinearLayout lg = card(root, "Log");
        log = text("");
        log.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        log.setTextIsSelectable(true);
        lg.addView(log);

        reload();
        applyPanelFix();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (getPreferences(MODE_PRIVATE).getBoolean("input_sync", true) && !daemonStarted) {
            daemonStarted = true;
            try {
                startForegroundService(new android.content.Intent(this, EinkDaemonService.class));
            } catch (Exception e) {
                note("input sync: " + e.getMessage());
            }
        }
    }

    // ------------------------------------------------------------------ actions

    private void applyPanelFix() {
        bg(() -> {
            StringBuilder cmd = new StringBuilder();
            if (val("persist.vendor.xrz.fps").isEmpty())
                cmd.append("setprop persist.vendor.xrz.fps 120; ");
            if (val("persist.vendor.xrz.fps_high").isEmpty())
                cmd.append("setprop persist.vendor.xrz.fps_high 38; ");
            if (val("persist.vendor.xrz.fps_low").isEmpty())
                cmd.append("setprop persist.vendor.xrz.fps_low 30; ");
            // HWC overlays off is what makes the composer feed the e-ink engine.
            cmd.append("service call SurfaceFlinger 1008 i32 1");
            note("panel fix: " + orOk(su(cmd.toString())));
            reloadBlocking();
        });
    }

    private void fixPanel() {
        run("service call SurfaceFlinger 1008 i32 1", "panel fix");
    }

    private void fullRefresh() {
        run("input keyevent 26; sleep 1; input keyevent 26; "
                + "service call SurfaceFlinger 1008 i32 1", "full refresh");
    }

    private void setMode(String mode) {
        run("setprop vendor.xrz.force_global_refresh_mode 0; "
                + "setprop vendor.xrz.global_refresh_mode " + mode + "; "
                + "echo " + mode + " > " + EINK + "/global_mode",
                "refresh_mode=" + mode);
    }

    private void setAnimation(int mode) {
        run("setprop vendor.xrz.eink_animation_mode " + mode, "animation=" + mode);
    }

    private void applyFps() {
        editingFps = false;
        String base = fpsBase.getText().toString().trim();
        String high = fpsHigh.getText().toString().trim();
        String low = fpsLow.getText().toString().trim();
        if (base.isEmpty() || high.isEmpty() || low.isEmpty()) return;
        run("setprop persist.vendor.xrz.fps " + base + "; "
                + "setprop persist.vendor.xrz.fps_high " + high + "; "
                + "setprop persist.vendor.xrz.fps_low " + low + "; "
                + "service call SurfaceFlinger 1008 i32 1",
                "fps=" + base + "/" + high + "/" + low);
    }

    private void switchToStock() {
        new AlertDialog.Builder(this)
                .setTitle("Switch to stock ROM?")
                .setMessage("Disables this GSI and reboots into stock. To come back, "
                        + "run on the PC:\n\nadb shell gsi_tool enable && adb reboot")
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Reboot to stock", (d, w) -> bg(() -> {
                    String out = su("gsi_tool disable 2>&1; echo ---; gsi_tool status 2>&1");
                    note("gsi_tool: " + out.replace('\n', ' '));
                    if (out.contains("disabled")) {
                        note("rebooting to stock");
                        su("sync; sleep 1; reboot");
                    } else {
                        // Keep it enabled and failing is worse than forcing stock.
                        note("disable did not stick, removing booted marker");
                        su("rm -f /metadata/gsi/dsu/booted /metadata/gsi/dsu/one_shot_boot; "
                                + "sync; sleep 1; reboot");
                    }
                }))
                .show();
    }

    // ------------------------------------------------------------------ reading

    private void reload() {
        bg(this::reloadBlocking);
    }

    private void reloadBlocking() {
        StringBuilder cmd = new StringBuilder("for k in");
        for (String p : PROPS) cmd.append(' ').append(p);
        cmd.append("; do echo \"$k=$(getprop $k)\"; done;");
        cmd.append("for n in");
        for (String n : NODES) cmd.append(' ').append(n);
        cmd.append("; do echo \"sys.$n=$(cat ").append(EINK).append("/$n)\"; done;");
        cmd.append("echo \"light=$(cat ").append(LIGHT).append(")\";");
        cmd.append("C=$(ls ").append(LM).append("cold_light 2>/dev/null | head -1); ");
        cmd.append("W=$(ls ").append(LM).append("warm_light 2>/dev/null | head -1); ");
        cmd.append("echo \"cold=$(cat $C 2>/dev/null)\";");
        cmd.append("echo \"warm=$(cat $W 2>/dev/null)\";");
        cmd.append("echo \"master=$(cat $C 2>/dev/null)\";");
        cmd.append("echo \"s.blurs=$(settings get global disable_window_blurs)\";");
        cmd.append("echo \"s.contrast=$(settings get secure accessibility_high_text_contrast_enabled)\";");
        cmd.append("echo \"s.anim=$(settings get global window_animation_scale)\";");
        cmd.append("echo \"last=$(logcat -d -s hwcomposer | grep 'normal commit' "
                + "| tail -1 | sed 's/.*normal commit/normal commit/')\"");

        String out = su(cmd.toString());
        Map<String, String> fresh = new LinkedHashMap<>();
        for (String line : out.split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) fresh.put(line.substring(0, eq).trim(), line.substring(eq + 1).trim());
        }
        ui(() -> {
            cache.clear();
            cache.putAll(fresh);
            render();
        });
    }

    private void render() {
        status.setText("fps " + val("persist.vendor.xrz.fps")
                + " / " + val("persist.vendor.xrz.fps_high")
                + " / " + val("persist.vendor.xrz.fps_low")
                + "    mode " + val("vendor.xrz.global_refresh_mode")
                + "    rate " + rate() + "/s"
                + "\n" + val("last"));

        String mode = val("vendor.xrz.global_refresh_mode").trim();
        for (Map.Entry<String, Button> e : modeButtons.entrySet()) {
            e.getValue().setText(e.getKey().equals(mode)
                    ? e.getValue().getTag() + " •" : String.valueOf(e.getValue().getTag()));
        }

        for (Object[] e : switches) {
            Switch sw = (Switch) e[0];
            boolean[] syncing = (boolean[]) e[2];
            syncing[0] = true;
            String key = (String) e[1];
            boolean checked = isOn(val(key));
            if ("s.anim".equals(key)) {
                String v = val(key).trim();
                // unset ("null") or 1.0 means animations are on -> toggle off
                checked = !(v.isEmpty() || v.equalsIgnoreCase("null")
                        || v.equals("1") || v.equals("1.0"));
            }
            sw.setChecked(checked);
            syncing[0] = false;
        }

        if (!editingFps) {
            setIfChanged(fpsBase, val("persist.vendor.xrz.fps"));
            setIfChanged(fpsHigh, val("persist.vendor.xrz.fps_high"));
            setIfChanged(fpsLow, val("persist.vendor.xrz.fps_low"));
        }

        for (Object[] e : frontlightBars) {
            SeekBar sb = (SeekBar) e[0];
            TextView lbl = (TextView) e[2];
            int step = nearestStep(readInt(val((String) e[1])));
            sb.setProgress(step);
            lbl.setText(sb.getTag() + ": " + step + "/" + (FL.length - 1)
                    + "  (" + FL[step] + ")");
        }

        for (Object[] e : sliders) {
            SeekBar sb = (SeekBar) e[0];
            TextView lbl = (TextView) e[2];
            int max = sb.getMax();
            int v = Math.max(0, Math.min(max, readInt(val((String) e[1]))));
            sb.setProgress(v);
            lbl.setText(sb.getTag() + ": " + v);
        }
    }

    private static void setIfChanged(EditText e, String v) {
        if (!e.isFocused() && !e.getText().toString().equals(v)) e.setText(v);
    }

    private String val(String k) {
        String v = cache.get(k);
        return v == null ? "" : v;
    }

    /** Commits per second over the sampled window reported by the shell. */
    private String rate() {
        String[] ts = val("times").trim().split("\\s+");
        if (ts.length < 3) return "-";
        try {
            double a = seconds(ts[0]);
            double b = a;
            int n = 0;
            for (String t : ts) {
                if (t.isEmpty()) continue;
                b = seconds(t);
                n++;
            }
            double d = b - a;
            if (d <= 0) return "-";
            return String.format(java.util.Locale.US, "%.0f", (n - 1) / d);
        } catch (Exception e) {
            return "-";
        }
    }

    private static double seconds(String hhmmss) {
        String[] p = hhmmss.split(":");
        double s = Double.parseDouble(p[2]);
        return Integer.parseInt(p[0]) * 3600.0 + Integer.parseInt(p[1]) * 60.0 + s;
    }

    private static String strip(String v) {
        int eq = v.lastIndexOf('=');
        return eq >= 0 ? v.substring(eq + 1) : v;
    }

    private static boolean isOn(String v) {
        if (v == null) return false;
        v = v.trim();
        if (v.equals("1") || v.equalsIgnoreCase("true")) return true;
        if (v.startsWith("0x")) {
            try {
                return Integer.parseInt(v.substring(2), 16) != 0;
            } catch (NumberFormatException e) {
                return false;
            }
        }
        int eq = v.lastIndexOf('=');
        if (eq >= 0) return isOn(v.substring(eq + 1));
        return false;
    }

    private static int readInt(String v) {
        String s = strip(v).trim();
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // ------------------------------------------------------------------ widgets

    private LinearLayout card(LinearLayout root, String title) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(dp(14), dp(12), dp(14), dp(12));

        GradientDrawable bg = new GradientDrawable();
        bg.setCornerRadius(dp(18));
        bg.setStroke(Math.max(1, dp(1)), 0x33808080);
        l.setBackground(bg);

        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.bottomMargin = dp(12);
        root.addView(l, lp);

        if (title != null) {
            TextView t = text(title);
            t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
            t.setAllCaps(true);
            t.setLetterSpacing(0.08f);
            t.setTextColor(0xFF7A7A7A);
            t.setPadding(dp(2), 0, 0, dp(6));
            l.addView(t);
        }
        return l;
    }

    private TextView caption(LinearLayout parent, String s) {
        TextView t = text(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12);
        t.setTextColor(0xFF8A8A8A);
        t.setPadding(dp(2), dp(6), dp(2), dp(2));
        parent.addView(t);
        return t;
    }

    private TextView text(String s) {
        TextView t = new TextView(this);
        t.setText(s);
        return t;
    }

    private LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
    }

    private void row(LinearLayout parent, View... views) {
        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        l.setPadding(0, dp(4), 0, dp(4));
        for (View v : views) {
            LinearLayout.LayoutParams lp = weight();
            lp.setMargins(dp(2), 0, dp(2), 0);
            l.addView(v, lp);
        }
        parent.addView(l);
    }

    private Button modeButton(String mode, String label) {
        Button b = button(label, () -> setMode(mode));
        b.setTag(label);
        modeButtons.put(mode, b);
        return b;
    }

    private Button button(String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        b.setOnClickListener(v -> action.run());
        return b;
    }

    private EditText number(String hint) {
        EditText e = new EditText(this);
        e.setHint(hint);
        e.setInputType(InputType.TYPE_CLASS_NUMBER);
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        e.setOnFocusChangeListener((v, has) -> {
            if (has) editingFps = true;
        });
        return e;
    }

    /** Toggle backed by Settings; the cache key tells which one. */
    private void settingToggle(LinearLayout parent, String label, String cacheKey, boolean blurs) {
        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        sw.setPadding(dp(2), dp(8), 0, dp(8));
        final boolean[] syncing = {false};
        switches.add(new Object[]{sw, cacheKey, syncing});
        sw.setOnCheckedChangeListener((btn, on) -> {
            if (syncing[0]) return;
            String cmd;
            if (cacheKey.equals("s.blurs")) {
                cmd = "settings put global disable_window_blurs " + (on ? 1 : 0);
            } else if (cacheKey.equals("s.contrast")) {
                cmd = "settings put secure accessibility_high_text_contrast_enabled " + (on ? 1 : 0);
            } else {
                String v = on ? "0" : "1";
                cmd = "settings put global window_animation_scale " + v
                        + "; settings put global transition_animation_scale " + v
                        + "; settings put global animator_duration_scale " + v;
            }
            run(cmd, label + "=" + on);
        });
        parent.addView(sw);
    }

    private void toggle(LinearLayout parent, String label, String key, boolean alsoSysfs) {
        Switch sw = new Switch(this);
        sw.setText(label);
        sw.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        sw.setPadding(dp(2), dp(8), 0, dp(8));
        final boolean[] syncing = {false};
        switches.add(new Object[]{sw, key, syncing});
        sw.setOnCheckedChangeListener((b, on) -> {
            if (syncing[0]) return;
            String v = on ? "1" : "0";
            String cmd = "setprop " + key + " " + v;
            if (alsoSysfs) cmd += "; echo " + v + " > " + EINK + "/global_dither";
            if (key.endsWith("anti_flicker_enabled"))
                cmd += "; echo " + v + " > " + EINK + "/anti_flicker";
            run(cmd, key + "=" + v);
        });
        parent.addView(sw);
    }

    private void frontlight(LinearLayout parent, String label, String channel, String key,
                            boolean both) {
        TextView caption = text(label + ": 0/" + (FL.length - 1));
        caption.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        caption.setTag(label);
        caption.setPadding(dp(2), dp(10), dp(2), 0);

        SeekBar sb = new SeekBar(this);
        sb.setMax(FL.length - 1);
        sb.setTag(label);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                if (fromUser) caption.setText(label + ": " + p + "/" + (FL.length - 1));
            }
            public void onStartTrackingTouch(SeekBar s) { }
            public void onStopTrackingTouch(SeekBar s) {
                final int step = s.getProgress();
                final int code = FL[step];
                final String other = "cold".equals(channel) ? "warm" : "cold";
                final int sys = Math.round(code * 255f / FL[FL.length - 1]);
                String cmd = lmWrite(channel, code);
                if (both) {
                    cmd += "; " + lmWrite(other, code)
                            + "; settings put system screen_brightness " + sys;
                }
                final String finalCmd = cmd;
                bg(() -> {
                    String out = su(finalCmd);
                    note(label + " step " + step + " (" + code + ")"
                            + (out.isEmpty() ? "" : " -> " + out));
                });
            }
        });
        frontlightBars.add(new Object[]{sb, key, caption});
        parent.addView(caption);
        parent.addView(sb);
    }

    private int nearestStep(int code) {
        int best = 0;
        for (int i = 0; i < FL.length; i++) {
            if (Math.abs(FL[i] - code) < Math.abs(FL[best] - code)) best = i;
        }
        return best;
    }

    private interface IntAction { String apply(int v); }

    private void slider(LinearLayout parent, String label, int max, String key, IntAction setter) {
        TextView caption = text(label + ": 0");
        caption.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        caption.setTag(label);
        caption.setPadding(dp(2), dp(10), dp(2), 0);

        SeekBar sb = new SeekBar(this);
        sb.setMax(max);
        sb.setTag(label);
        sb.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                if (fromUser) caption.setText(label + ": " + p);
            }
            public void onStartTrackingTouch(SeekBar s) { }
            public void onStopTrackingTouch(SeekBar s) {
                final int v = s.getProgress();
                bg(() -> {
                    String out = su(setter.apply(v));
                    note(label + "=" + v + (out.isEmpty() ? "" : " -> " + out));
                });
            }
        });
        sliders.add(new Object[]{sb, key, caption});
        parent.addView(caption);
        parent.addView(sb);
    }

    // ------------------------------------------------------------------ plumbing

    private String su(String cmd) {
        return Root.run(cmd);
    }

    /** The shell does not expand a glob in a redirection target, so resolve
     *  the LM3630A node first and write to the concrete path. */
    /** Same rule: build only. The glob has to be resolved first because this
     *  shell does not expand one in a redirection target. */
    private String lmWrite(String channel, int value) {
        return "P=$(ls " + LM + channel + "_light 2>/dev/null | head -1); "
                + "if [ -n \"$P\" ]; then echo " + value + " > \"$P\"; fi";
    }

    /** Returns a shell command; never executes (setters run on the UI thread). */
    private String sysfs(String node, String value) {
        return "echo " + value + " > " + EINK + "/" + node;
    }

    private static String orOk(String out) {
        return out.isEmpty() ? "ok" : out;
    }

    /** Same as run() but does not re-read every value afterwards. */
    private void runQuiet(String cmd, String label) {
        bg(() -> {
            String out = su(cmd);
            note(label + (out.isEmpty() ? " (ok)" : " -> " + out));
        });
    }

    private void run(String cmd, String label) {
        bg(() -> {
            String out = su(cmd);
            note(label + (out.isEmpty() ? " (ok)" : " -> " + out));
            reloadBlocking();
        });
    }

    private void note(String s) {
        ui(() -> {
            log.append(s + "\n");
            CharSequence t = log.getText();
            if (t.length() > 4000) log.setText(t.subSequence(t.length() - 4000, t.length()));
        });
    }

    private void bg(Runnable r) {
        exec.execute(r);
    }

    private void ui(Runnable r) {
        runOnUiThread(r);
    }

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }
}
