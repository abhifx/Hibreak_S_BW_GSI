package com.hibreak.einkcenter;

/** Fixed, root-only SystemUI commands for the reversible e-ink preset. */
public final class EinkUiPreset {
    private EinkUiPreset() { }

    /**
     * Complete static e-ink UI. Android gesture navigation is the default;
     * restore never changes it back.
     */
    public static String applyCommand() {
        return "settings put global window_animation_scale 0; "
                + "settings put global transition_animation_scale 0; "
                + "settings put global animator_duration_scale 0; "
                + "settings put global disable_window_blur 1; "
                + "settings put secure high_text_contrast_enabled 0; "
                + "settings put secure accessibility_high_text_contrast_enabled 0; "
                + "settings put global heads_up_notifications_enabled 0; "
                + "settings put system smooth_fling_factor 0; "
                + "cmd uimode night no; "
                + "settings put secure ui_night_mode 0; "
                + "setprop vendor.xrz.eink_animation_mode 0; "
                + "settings put secure navigation_mode 2; "
                + "cmd overlay dump com.android.internal.systemui.navbar.gestural "
                + "| grep -q 'mState.*STATE_ENABLED' "
                + "|| { setprop ctl.restart idmap2d; sleep 2; "
                + "cmd overlay enable-exclusive --user 0 "
                + "com.android.internal.systemui.navbar.gestural; }";
    }

    /**
     * Battery-first policy: keep normal Android doze, but do not hold the device
     * in always-on/light-doze for AOD. A two-minute screen timeout also avoids
     * accidental long frontlight/display sessions.
     */
    public static String batteryFirstCommand() {
        return "settings put secure doze_always_on 0; "
                + "settings put secure doze_enabled 1; "
                + "settings put secure wake_gesture_enabled 0; "
                + "settings put secure double_tap_to_wake 0; "
                + "N=$(getprop persist.sys.phh.focaltech_node); "
                + "[ -n \"$N\" ] && echo 0 > \"$N\"; "
                + "setprop debug.doze.aod false; "
                + "setprop persist.sys.overlay.aod 0; "
                + "cmd overlay disable --user 0 "
                + "me.phh.treble.overlay.misc.aod_systemui; "
                + "settings put system screen_off_timeout 120000";
    }

    /**
     * Android defaults for the settings changed by the preset. The current
     * navigation mode and an unsupported/removed smooth-fling setting are left
     * alone deliberately.
     */
    public static String restoreCommand() {
        return "settings put global window_animation_scale 1; "
                + "settings put global transition_animation_scale 1; "
                + "settings put global animator_duration_scale 1; "
                + "settings put global disable_window_blur 0; "
                + "settings put secure high_text_contrast_enabled 0; "
                + "settings put secure accessibility_high_text_contrast_enabled 0; "
                + "settings put secure accessibility_high_text_contrast_enabled 1; "
                + "settings put global heads_up_notifications_enabled 1; "
                + "settings delete system smooth_fling_factor; "
                + "setprop vendor.xrz.eink_animation_mode 1";
    }
}
