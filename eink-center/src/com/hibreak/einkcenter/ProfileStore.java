package com.hibreak.einkcenter;

import android.content.Context;
import android.content.SharedPreferences;

import java.util.Arrays;
import java.util.List;

/** SharedPreferences schema for profiles, the bubble and the extra key. */
public final class ProfileStore {
    public static final String PREFS = "eink";

    public static final String PROFILES_ENABLED = "profiles_enabled";
    public static final String GLOBAL_MODE = "global_mode";
    public static final String PROFILE_PREFIX = "profile:";
    public static final String LAST_TARGET_APP = "last_target_app";
    public static final String BUBBLE_ENABLED = "bubble_enabled";
    public static final String NAV_BAR_ENABLED = "nav_bar_enabled";
    public static final String BUBBLE_X = "bubble_x";
    public static final String BUBBLE_Y = "bubble_y";
    public static final String SLEEP_WALLPAPER_ENABLED = "sleep_wallpaper_enabled";

    public static final String ACTION_NOTHING = "nothing";
    public static final String ACTION_OPEN_MENU = "open_menu";
    public static final String ACTION_FULL_REFRESH = "full_refresh";
    public static final String ACTION_TOGGLE_FAST_HYPER = "toggle_fast_hyper";
    public static final String ACTION_CYCLE_MODE = "cycle_mode";
    public static final String ACTION_HOME = "home";
    public static final String ACTION_BACK = "back";
    public static final String ACTION_RECENTS = "recents";

    public static final String PRESS_SINGLE = "single";
    public static final String PRESS_DOUBLE = "double";
    public static final String PRESS_LONG = "long";

    private static final List<String> ACTIONS = Arrays.asList(
            ACTION_NOTHING, ACTION_OPEN_MENU, ACTION_FULL_REFRESH,
            ACTION_TOGGLE_FAST_HYPER, ACTION_CYCLE_MODE,
            ACTION_HOME, ACTION_BACK, ACTION_RECENTS);

    private ProfileStore() { }

    public static SharedPreferences prefs(Context context) {
        return context.getApplicationContext().getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    public static boolean profilesEnabled(Context context) {
        return prefs(context).getBoolean(PROFILES_ENABLED, true);
    }

    public static void setProfilesEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(PROFILES_ENABLED, enabled).apply();
    }

    public static String globalMode(Context context) {
        String mode = EinkController.normalizeMode(prefs(context).getString(GLOBAL_MODE, null));
        return mode == null ? EinkController.DEFAULT_MODE : mode;
    }

    public static boolean setGlobalMode(Context context, String mode) {
        String safe = EinkController.normalizeMode(mode);
        if (safe == null) return false;
        prefs(context).edit().putString(GLOBAL_MODE, safe).apply();
        return true;
    }

    public static String profileMode(Context context, String packageName) {
        if (packageName == null || packageName.isEmpty()) return null;
        return EinkController.normalizeMode(prefs(context).getString(PROFILE_PREFIX + packageName, null));
    }

    public static boolean setProfile(Context context, String packageName, String mode) {
        if (packageName == null || packageName.isEmpty()) return false;
        SharedPreferences.Editor edit = prefs(context).edit();
        if (mode == null) {
            edit.remove(PROFILE_PREFIX + packageName);
            edit.apply();
            return true;
        }
        String safe = EinkController.normalizeMode(mode);
        if (safe == null) return false;
        edit.putString(PROFILE_PREFIX + packageName, safe).apply();
        return true;
    }

    public static void removeProfile(Context context, String packageName) {
        if (packageName == null) return;
        prefs(context).edit().remove(PROFILE_PREFIX + packageName).apply();
    }

    /** Package profile when enabled, otherwise the saved global mode. */
    public static String effectiveMode(Context context, String packageName) {
        if (profilesEnabled(context)) {
            String mode = profileMode(context, packageName);
            if (mode != null) return mode;
        }
        return globalMode(context);
    }

    public static String profileSummary(Context context, String packageName) {
        String mode = profileMode(context, packageName);
        return mode == null
                ? "Inherits global · " + EinkController.label(globalMode(context))
                : EinkController.label(mode);
    }

    public static String lastTargetApp(Context context) {
        return prefs(context).getString(LAST_TARGET_APP, "");
    }

    public static void setLastTargetApp(Context context, String packageName) {
        if (packageName == null || packageName.isEmpty()) return;
        if (packageName.equals(lastTargetApp(context))) return;
        prefs(context).edit().putString(LAST_TARGET_APP, packageName).apply();
    }

    public static boolean bubbleEnabled(Context context) {
        return prefs(context).getBoolean(BUBBLE_ENABLED, false);
    }

    public static void setBubbleEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(BUBBLE_ENABLED, enabled).apply();
    }

    public static boolean navBarEnabled(Context context) {
        return prefs(context).getBoolean(NAV_BAR_ENABLED, false);
    }

    public static void setNavBarEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(NAV_BAR_ENABLED, enabled).apply();
    }

    public static boolean sleepWallpaperEnabled(Context context) {
        return prefs(context).getBoolean(SLEEP_WALLPAPER_ENABLED, false);
    }

    public static void setSleepWallpaperEnabled(Context context, boolean enabled) {
        prefs(context).edit().putBoolean(SLEEP_WALLPAPER_ENABLED, enabled).apply();
    }

    public static int bubbleX(Context context, int fallback) {
        return prefs(context).getInt(BUBBLE_X, fallback);
    }

    public static int bubbleY(Context context, int fallback) {
        return prefs(context).getInt(BUBBLE_Y, fallback);
    }

    public static void setBubblePosition(Context context, int x, int y) {
        prefs(context).edit().putInt(BUBBLE_X, x).putInt(BUBBLE_Y, y).apply();
    }

    public static String actionKey(boolean screenOff, String press) {
        return "button_" + (screenOff ? "screen_off_" : "screen_on_") + press;
    }

    public static String defaultAction(String press, boolean screenOff) {
        if (screenOff) return ACTION_NOTHING;
        if (PRESS_SINGLE.equals(press)) return ACTION_FULL_REFRESH;
        if (PRESS_DOUBLE.equals(press)) return ACTION_OPEN_MENU;
        return ACTION_NOTHING;
    }

    public static String buttonAction(Context context, boolean screenOff, String press) {
        String action = prefs(context).getString(
                actionKey(screenOff, press), defaultAction(press, screenOff));
        return normalizeAction(action);
    }

    public static boolean setButtonAction(Context context, boolean screenOff, String press, String action) {
        if (action == null || !ACTIONS.contains(action) || !validPress(press)) return false;
        prefs(context).edit().putString(actionKey(screenOff, press), action).apply();
        return true;
    }

    public static void resetButtonActions(Context context) {
        SharedPreferences.Editor edit = prefs(context).edit();
        for (boolean off : new boolean[]{false, true}) {
            for (String press : new String[]{PRESS_SINGLE, PRESS_DOUBLE, PRESS_LONG}) {
                edit.putString(actionKey(off, press), defaultAction(press, off));
            }
        }
        edit.apply();
    }

    public static String normalizeAction(String action) {
        if (action == null) return ACTION_NOTHING;
        for (String allowed : ACTIONS) if (allowed.equals(action)) return allowed;
        return ACTION_NOTHING;
    }

    public static List<String> actions() {
        return ACTIONS;
    }

    public static String actionLabel(String action) {
        switch (normalizeAction(action)) {
            case ACTION_OPEN_MENU: return "Open E-ink menu";
            case ACTION_FULL_REFRESH: return "Full refresh";
            case ACTION_TOGGLE_FAST_HYPER: return "Toggle Fast / Hyper";
            case ACTION_CYCLE_MODE: return "Cycle refresh mode";
            case ACTION_HOME: return "Home";
            case ACTION_BACK: return "Back";
            case ACTION_RECENTS: return "Recent apps";
            default: return "Nothing";
        }
    }

    private static boolean validPress(String press) {
        return PRESS_SINGLE.equals(press) || PRESS_DOUBLE.equals(press) || PRESS_LONG.equals(press);
    }
}
