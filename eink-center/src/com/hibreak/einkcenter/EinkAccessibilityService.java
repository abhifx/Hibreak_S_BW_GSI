package com.hibreak.einkcenter;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.app.KeyguardManager;
import android.content.BroadcastReceiver;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.ContentObserver;
import android.graphics.Bitmap;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.PowerManager;
import android.os.SystemClock;
import android.provider.Settings;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewConfiguration;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;
import android.view.accessibility.AccessibilityWindowInfo;
import android.widget.Button;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Foreground profile switching, the static accessibility-overlay menu and the
 * extra physical button. The button can arrive as scan 647 or as Volume Up from
 * mtk-pmic-keys; the real Volume Up rocker on gpio_keys is never consumed.
 */
public class EinkAccessibilityService extends AccessibilityService {
    public static final int EXTRA_BUTTON_SCAN_CODE = 647; // KEY_SIDEVU_SONAR / 0x287
    private static final long APP_DEBOUNCE_MS = 300;
    private static final long DOUBLE_MS = 300;
    private static final long LONG_MS = 700;
    private static final long SLEEP_LEAD_MS = 1800;
    private static final long SLEEP_STALE_MS = 3500;
    private static final String LM = "/sys/devices/platform/*/i2c-*/2-0036/lm3630a_";

    private static volatile EinkAccessibilityService instance;
    private static volatile long lastActivityNotice;

    private final Handler main = new Handler(Looper.getMainLooper());
    private final ExecutorService io = Executors.newSingleThreadExecutor(r -> {
        Thread t = new Thread(r, "eink-profile-io");
        t.setDaemon(true);
        return t;
    });
    private final Map<Integer, Long> ownedKeys = new HashMap<>();
    private final Map<String, String> appLabels = new ConcurrentHashMap<>();
    private final HardwareGesture gesture = new HardwareGesture();
    private final Runnable userActivityPosted = this::onUserActivityPosted;
    private final Runnable sleepCountdown = this::onSleepCountdown;
    private final Runnable sleepStaleCheck = this::onSleepStaleCheck;
    private long lastUserActivity = SystemClock.elapsedRealtime();
    private long sleepGeneration;

    private WindowManager windowManager;
    private View bubble;
    private WindowManager.LayoutParams bubbleParams;
    private static View navBar;
    private static WindowManager navBarWindowManager;
    private static EinkAccessibilityService navBarOwner;
    private View sleepWallpaper;
    private Bitmap pendingSleepWallpaper;
    private Bitmap shownSleepWallpaper;
    private TextView navBarToggle;
    private TextView bubbleMode;
    private View panel;
    private TextView panelApp;
    private final Map<Button, String> panelModes = new LinkedHashMap<>();
    private Button frontlightButton;
    private boolean expanded;
    private boolean shadeOpen;
    private long navAssistUntil;
    private boolean destroyed;
    private String currentApp = "";
    private String displayMode = EinkController.DEFAULT_MODE;

    private BroadcastReceiver stateReceiver;
    private ContentObserver timeoutObserver;
    private SharedPreferences.OnSharedPreferenceChangeListener preferenceListener;
    private final Runnable appCheck = this::detectForegroundApp;

    public static boolean isRunning() {
        return instance != null;
    }

    /** Called by the root touch reader; coalesced onto the service main thread. */
    public static void noteUserActivity() {
        EinkAccessibilityService service = instance;
        if (service == null || service.destroyed) return;
        long now = SystemClock.elapsedRealtime();
        if (now - lastActivityNotice < 100) return;
        lastActivityNotice = now;
        service.main.removeCallbacks(service.userActivityPosted);
        service.main.post(service.userActivityPosted);
    }

    /** Raw power-key reader; unlike accessibility filtering this can precede sleep. */
    public static void notePowerKey() {
        EinkAccessibilityService service = instance;
        if (service == null || service.destroyed) return;
        service.main.post(() -> {
            if (service.destroyed || !ProfileStore.sleepWallpaperEnabled(service)) return;
            PowerManager pm = service.getSystemService(PowerManager.class);
            if (pm != null && !pm.isInteractive()) return;
            service.sleepGeneration++;
            service.main.removeCallbacks(service.sleepCountdown);
            service.showSleepWallpaper();
            service.main.postDelayed(service.sleepStaleCheck, SLEEP_STALE_MS);
        });
    }

    /**
     * Quick Settings/tile entry point. The work is posted to the service's main
     * handler; the panel itself is independent of the optional bubble.
     *
     * @return false when the accessibility service is not connected
     */
    public static boolean requestMenu(Context context) {
        EinkAccessibilityService service = instance;
        if (service == null || service.destroyed) return false;
        android.util.Log.i("EinkCenter", "QS menu request accepted");
        service.main.post(() -> {
            if (instance != service || service.destroyed) return;
            service.performGlobalAction(AccessibilityService.GLOBAL_ACTION_DISMISS_NOTIFICATION_SHADE);
            service.main.post(service::expandPanel);
        });
        return true;
    }

    public static boolean enabledInSettings(Context context) {
        ComponentName me = new ComponentName(context, EinkAccessibilityService.class);
        String enabled = Settings.Secure.getString(context.getContentResolver(),
                Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);
        if (enabled == null) return false;
        for (String value : enabled.split(":")) {
            if (value.isEmpty()) continue;
            ComponentName component = ComponentName.unflattenFromString(value);
            if (me.equals(component)) return true;
        }
        return false;
    }

    @Override
    public void onCreate() {
        super.onCreate();
        windowManager = getSystemService(WindowManager.class);
        currentApp = ProfileStore.lastTargetApp(this);
        displayMode = ProfileStore.effectiveMode(this, currentApp);
        stateReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String action = intent == null ? null : intent.getAction();
                if (Intent.ACTION_SCREEN_OFF.equals(action)) {
                    collapsePanel();
                    updateOverlayVisibility();
                    showSleepWallpaper();
                } else if (Intent.ACTION_SCREEN_ON.equals(action)
                        || Intent.ACTION_USER_PRESENT.equals(action)) {
                    removeSleepWallpaper();
                    lastUserActivity = SystemClock.elapsedRealtime();
                    scheduleSleepCountdown();
                    main.postDelayed(appCheck, APP_DEBOUNCE_MS);
                    updateOverlayVisibility();
                    prepareSleepWallpaper();
                }
            }
        };
        IntentFilter filter = new IntentFilter();
        filter.addAction(Intent.ACTION_SCREEN_ON);
        filter.addAction(Intent.ACTION_SCREEN_OFF);
        filter.addAction(Intent.ACTION_USER_PRESENT);
        registerReceiver(stateReceiver, filter);
        timeoutObserver = new ContentObserver(main) {
            @Override
            public void onChange(boolean selfChange) {
                scheduleSleepCountdown();
            }
        };
        getContentResolver().registerContentObserver(
                Settings.System.getUriFor(Settings.System.SCREEN_OFF_TIMEOUT),
                false, timeoutObserver);
    }

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        AccessibilityServiceInfo info = getServiceInfo();
        if (info == null) info = new AccessibilityServiceInfo();
        info.eventTypes = AccessibilityEvent.TYPE_WINDOW_STATE_CHANGED
                | AccessibilityEvent.TYPE_WINDOWS_CHANGED;
        info.feedbackType = AccessibilityServiceInfo.FEEDBACK_GENERIC;
        info.flags = AccessibilityServiceInfo.FLAG_RETRIEVE_INTERACTIVE_WINDOWS
                | AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS
                | AccessibilityServiceInfo.FLAG_INCLUDE_NOT_IMPORTANT_VIEWS;
        info.notificationTimeout = 100;
        android.util.Log.i("EinkCenter", "service info before: flags=" + info.flags
                + " caps=" + info.getCapabilities());
        setServiceInfo(info);
        AccessibilityServiceInfo active = getServiceInfo();
        android.util.Log.i("EinkCenter", "service info active: flags=" + active.flags
                + " caps=" + active.getCapabilities());

        instance = this;
        // A previous service instance may have left an accessibility overlay
        // behind. Remove that owned view before this instance publishes its own.
        removeNavBar(true);
        shadeOpen = isShadeOpen();
        // Per-app profiles must let the vendor/HWC choose the live waveform.
        io.execute(() -> RootShell.get().cmd("setprop vendor.debug.hwc.shim.mode -1"));
        preferenceListener = (prefs, key) -> {
            if (destroyed || key == null) return;
            if (ProfileStore.BUBBLE_ENABLED.equals(key)) {
                updateOverlayVisibility();
            } else if (ProfileStore.NAV_BAR_ENABLED.equals(key)) {
                removeNavBar();
                updateOverlayVisibility();
            } else if (ProfileStore.SLEEP_WALLPAPER_ENABLED.equals(key)) {
                sleepGeneration++;
                main.removeCallbacks(sleepCountdown);
                main.removeCallbacks(sleepStaleCheck);
                if (ProfileStore.sleepWallpaperEnabled(this)) {
                    lastUserActivity = SystemClock.elapsedRealtime();
                    scheduleSleepCountdown();
                    prepareSleepWallpaper();
                } else {
                    removeSleepWallpaper();
                    recyclePendingSleepWallpaper();
                }
            } else if (ProfileStore.PROFILES_ENABLED.equals(key)
                    || ProfileStore.GLOBAL_MODE.equals(key)
                    || key.startsWith(ProfileStore.PROFILE_PREFIX)) {
                applyEffectiveMode();
            } else if ("light_on".equals(key)) {
                updatePanelState();
            }
        };
        ProfileStore.prefs(this).registerOnSharedPreferenceChangeListener(preferenceListener);
        // A fresh service must not reuse an overlay left by a previous mode.
        removeNavBar();
        updateOverlayVisibility();
        lastUserActivity = SystemClock.elapsedRealtime();
        scheduleSleepCountdown();
        prepareSleepWallpaper();
        if (!currentApp.isEmpty()) loadAppLabel(currentApp);
        // Reapply after a service reconnect even when last_target_app is unchanged.
        applyEffectiveMode();
        detectForegroundApp();
        main.postDelayed(appCheck, APP_DEBOUNCE_MS);
        // Prime the duplicate-write cache without polling.
        EinkController.readModeAsync(this, mode -> main.post(() -> {
            if (!destroyed && currentApp.isEmpty()) {
                displayMode = mode;
                updateOverlayState();
            }
        }));
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {
        if (destroyed) return;
        updateOverlayVisibility();
        // Shade open/close changes the focused SystemUI window. Check those
        // immediately so quality-mode apps do not queue slow shade frames.
        main.removeCallbacks(appCheck);
        if (event != null && event.getPackageName() != null
                && "com.android.systemui".contentEquals(event.getPackageName())) {
            main.post(this::detectForegroundApp);
        } else {
            main.postDelayed(appCheck, APP_DEBOUNCE_MS);
        }
    }

    @Override
    public void onInterrupt() {
        cancelHardwareKeys();
        collapsePanel();
    }

    @Override
    public void onDestroy() {
        destroyed = true;
        cancelHardwareKeys();
        main.removeCallbacksAndMessages(null);
        if (preferenceListener != null) {
            ProfileStore.prefs(this).unregisterOnSharedPreferenceChangeListener(preferenceListener);
            preferenceListener = null;
        }
        if (stateReceiver != null) {
            unregisterReceiver(stateReceiver);
            stateReceiver = null;
        }
        if (timeoutObserver != null) {
            getContentResolver().unregisterContentObserver(timeoutObserver);
            timeoutObserver = null;
        }
        removeOverlay();
        removeSleepWallpaper();
        recyclePendingSleepWallpaper();
        io.shutdownNow();
        if (instance == this) instance = null;
        super.onDestroy();
    }

    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        removeOverlay();
        updateOverlayVisibility();
    }

    // ---------------------------------------------------------------- foreground profiles

    private void detectForegroundApp() {
        if (destroyed || !isInteractiveUnlocked()) return;
        boolean shade = isShadeOpen();
        if (shade != shadeOpen) {
            shadeOpen = shade;
            applyEffectiveMode();
            if (shade) return;
        }
        if (shade) return;
        String found = activeApplication();
        if (found == null || ignoredPackage(found) || found.equals(currentApp)) return;
        currentApp = found;
        ProfileStore.setLastTargetApp(this, found);
        loadAppLabel(found);
        applyEffectiveMode();
    }

    /** A focused, near-full-height SystemUI window is the notification/QS shade. */
    private boolean isShadeOpen() {
        try {
            int half = getResources().getDisplayMetrics().heightPixels / 2;
            for (AccessibilityWindowInfo window : getWindows()) {
                if (window == null || window.getType() != AccessibilityWindowInfo.TYPE_SYSTEM
                        || !window.isFocused()) continue;
                if (!"com.android.systemui".equals(rootPackage(window))) continue;
                Rect bounds = new Rect();
                window.getBoundsInScreen(bounds);
                CharSequence title = window.getTitle();
                String name = title == null ? "" : title.toString();
                if (bounds.height() > half || name.contains("Notification")
                        || name.contains("Quick Settings") || name.contains("Shade")) return true;
            }
        } catch (RuntimeException ignored) { }
        return false;
    }

    private String activeApplication() {
        try {
            // Prefer a focused application window; active is the fallback.
            String active = null;
            for (AccessibilityWindowInfo window : getWindows()) {
                if (window == null || window.getType() != AccessibilityWindowInfo.TYPE_APPLICATION) continue;
                if (!window.isActive() && !window.isFocused()) continue;
                String pkg = rootPackage(window);
                if (pkg == null || ignoredPackage(pkg)) continue;
                if (window.isFocused()) return pkg;
                if (active == null) active = pkg;
            }
            return active;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private String rootPackage(AccessibilityWindowInfo window) {
        AccessibilityNodeInfo root = window.getRoot();
        if (root == null) return null;
        try {
            CharSequence pkg = root.getPackageName();
            return pkg == null ? null : pkg.toString();
        } finally {
            root.recycle();
        }
    }

    private boolean ignoredPackage(String pkg) {
        return pkg == null || pkg.isEmpty()
                || pkg.equals(getPackageName())
                || pkg.equals("com.android.systemui")
                || pkg.equals("android")
                || pkg.equals("com.android.keyguard")
                || isKeyboard(pkg);
    }

    private Set<String> keyboardPackages;
    private long keyboardCacheAt;

    private boolean isKeyboard(String pkg) {
        if (keyboardPackages == null || android.os.SystemClock.elapsedRealtime() - keyboardCacheAt > 60_000) {
            Set<String> packages = new HashSet<>();
            try {
                android.view.inputmethod.InputMethodManager imm = getSystemService(android.view.inputmethod.InputMethodManager.class);
                if (imm != null) {
                    for (android.view.inputmethod.InputMethodInfo info : imm.getInputMethodList()) {
                        packages.add(info.getPackageName());
                    }
                }
            } catch (RuntimeException ignored) { }
            keyboardPackages = packages;
            keyboardCacheAt = android.os.SystemClock.elapsedRealtime();
        }
        return keyboardPackages.contains(pkg);
    }

    private void applyEffectiveMode() {
        if (destroyed) return;
        String mode = (shadeOpen || android.os.SystemClock.uptimeMillis() < navAssistUntil)
                ? EinkController.HYPER
                : ProfileStore.effectiveMode(this, currentApp);
        displayMode = mode;
        updateOverlayState();
        EinkController.applyAsync(this, mode);
    }

    /** Bottom-edge touch assist: make transient system navigation paint quickly. */
    public static void requestNavAssist() {
        EinkAccessibilityService service = instance;
        if (service == null || service.destroyed) return;
        service.main.post(service::beginNavAssist);
    }

    private void beginNavAssist() {
        if (destroyed) return;
        navAssistUntil = android.os.SystemClock.uptimeMillis() + 10_000;
        displayMode = EinkController.HYPER;
        updateOverlayState();
        EinkController.applyAsync(this, EinkController.HYPER);
        main.removeCallbacks(navAssistEnd);
        main.postDelayed(navAssistEnd, 10_000);
    }

    private final Runnable navAssistEnd = () -> {
        if (destroyed) return;
        navAssistUntil = 0;
        applyEffectiveMode();
    };

    private void loadAppLabel(String pkg) {
        if (pkg == null || pkg.isEmpty()) return;
        if (panelApp != null) panelApp.setText("Foreground: " + foregroundLabel(pkg));
        if (appLabels.containsKey(pkg)) return;
        io.execute(() -> {
            String label = pkg;
            try {
                PackageManager pm = getPackageManager();
                label = pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString();
            } catch (Exception ignored) { }
            appLabels.put(pkg, label);
            main.post(() -> {
                if (!destroyed && pkg.equals(currentApp)) updatePanelState();
            });
        });
    }

    private String foregroundLabel(String pkg) {
        if (pkg == null || pkg.isEmpty()) return "Detecting…";
        String label = appLabels.get(pkg);
        return label == null ? pkg : label;
    }

    // ---------------------------------------------------------------- sleep wallpaper

    private void onUserActivityPosted() {
        if (destroyed) return;
        lastUserActivity = SystemClock.elapsedRealtime();
        removeSleepWallpaper();
        main.removeCallbacks(sleepStaleCheck);
        scheduleSleepCountdown();
    }

    private void scheduleSleepCountdown() {
        sleepGeneration++;
        main.removeCallbacks(sleepCountdown);
        main.removeCallbacks(sleepStaleCheck);
        if (destroyed || !ProfileStore.sleepWallpaperEnabled(this)) return;
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null && !pm.isInteractive()) return;
        long timeout;
        try {
            timeout = Settings.System.getLong(getContentResolver(),
                    Settings.System.SCREEN_OFF_TIMEOUT, 60_000);
        } catch (Throwable ignored) {
            timeout = 60_000;
        }
        long elapsed = SystemClock.elapsedRealtime() - lastUserActivity;
        long delay = Math.max(250, timeout - SLEEP_LEAD_MS - elapsed);
        android.util.Log.i("EinkCenter", "sleep countdown delay=" + delay
                + " timeout=" + timeout + " elapsed=" + elapsed);
        main.postDelayed(sleepCountdown, delay);
    }

    private void onSleepCountdown() {
        if (destroyed || !ProfileStore.sleepWallpaperEnabled(this)) return;
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null && !pm.isInteractive()) return;
        final long generation = sleepGeneration;
        long timeout;
        try {
            timeout = Settings.System.getLong(getContentResolver(),
                    Settings.System.SCREEN_OFF_TIMEOUT, 60_000);
        } catch (Throwable ignored) {
            timeout = 60_000;
        }
        final long expectedTimeout = timeout;
        io.execute(() -> {
            String state = RootShell.get().query("dumpsys power | grep -E "
                    + "'SCREEN_BRIGHT_WAKE_LOCK|mUserActivityTimeoutOverrideFromWindowManager|mWakefulness=|mLastUserActivityTime'", 2_000);
            long override = powerTimeoutOverride(state);
            long lastActivity = powerLastUserActivity(state);
            long remaining = lastActivity >= 0
                    ? expectedTimeout - (SystemClock.elapsedRealtime() - lastActivity)
                    : Long.MAX_VALUE;
            boolean allowed = state.contains("mWakefulness=Awake")
                    && !state.contains("SCREEN_BRIGHT_WAKE_LOCK")
                    && (override < 0 || override <= expectedTimeout);
            boolean reschedule = allowed && remaining > SLEEP_LEAD_MS;
            android.util.Log.i("EinkCenter", "sleep countdown state allowed=" + allowed
                    + " override=" + override + " timeout=" + expectedTimeout
                    + " remaining=" + remaining + " reschedule=" + reschedule
                    + " state=" + state.replace('\n', '|'));
            main.post(() -> {
                if (generation != sleepGeneration || destroyed
                        || !ProfileStore.sleepWallpaperEnabled(this)) return;
                if (reschedule) {
                    main.postDelayed(sleepCountdown,
                            Math.max(100, remaining - SLEEP_LEAD_MS));
                    return;
                }
                if (!allowed || remaining < -500) return;
                PowerManager current = getSystemService(PowerManager.class);
                if (current != null && !current.isInteractive()) return;
                showSleepWallpaper();
                main.postDelayed(sleepStaleCheck, SLEEP_STALE_MS);
            });
        });
    }

    private static long powerTimeoutOverride(String state) {
        for (String line : state.split("\n")) {
            int eq = line.indexOf('=');
            if (eq < 0 || !line.contains("mUserActivityTimeoutOverrideFromWindowManager")) continue;
            try {
                return Long.parseLong(line.substring(eq + 1).trim());
            } catch (NumberFormatException ignored) {
                return -1;
            }
        }
        return -1;
    }

    private static long powerLastUserActivity(String state) {
        for (String line : state.split("\n")) {
            int eq = line.indexOf('=');
            if (eq < 0 || !line.contains("mLastUserActivityTime(")) continue;
            try {
                return Long.parseLong(line.substring(eq + 1).trim());
            } catch (NumberFormatException ignored) {
                return -1;
            }
        }
        return -1;
    }

    private void onSleepStaleCheck() {
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null && pm.isInteractive() && sleepWallpaper != null) {
            removeSleepWallpaper();
        }
    }

    private void prepareSleepWallpaper() {
        if (destroyed || !ProfileStore.sleepWallpaperEnabled(this)) return;
        PowerManager pm = getSystemService(PowerManager.class);
        if (pm != null && !pm.isInteractive()) return;
        io.execute(() -> {
            Bitmap bitmap = LockWallpapers.nextSleep(this);
            main.post(() -> {
                if (destroyed || !ProfileStore.sleepWallpaperEnabled(this)) {
                    if (bitmap != null) bitmap.recycle();
                    return;
                }
                recyclePendingSleepWallpaper();
                pendingSleepWallpaper = bitmap;
            });
        });
    }

    private void recyclePendingSleepWallpaper() {
        if (pendingSleepWallpaper != null) {
            pendingSleepWallpaper.recycle();
            pendingSleepWallpaper = null;
        }
    }

    private void showSleepWallpaper() {
        if (destroyed || !ProfileStore.sleepWallpaperEnabled(this)) return;
        if (sleepWallpaper != null && sleepWallpaper.isAttachedToWindow()) return;
        removeSleepWallpaper();

        View view;
        if (pendingSleepWallpaper != null) {
            shownSleepWallpaper = pendingSleepWallpaper;
            pendingSleepWallpaper = null;
            ImageView image = new ImageView(this);
            image.setScaleType(ImageView.ScaleType.CENTER_CROP);
            image.setBackgroundColor(android.graphics.Color.WHITE);
            image.setImageBitmap(shownSleepWallpaper);
            image.setContentDescription("Sleep wallpaper");
            view = image;
        } else {
            TextView fallback = new TextView(this);
            fallback.setGravity(Gravity.CENTER);
            fallback.setBackgroundColor(android.graphics.Color.WHITE);
            fallback.setTextColor(android.graphics.Color.BLACK);
            fallback.setTextSize(24);
            fallback.setText("Sleeping\n"
                    + android.text.format.DateFormat.getTimeFormat(this)
                    .format(new java.util.Date()));
            fallback.setContentDescription("Sleeping");
            view = fallback;
        }

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.MATCH_PARENT,
                WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.OPAQUE);
        params.gravity = Gravity.TOP | Gravity.LEFT;
        params.windowAnimations = 0;
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setCanPlayMoveAnimation(false);
        params.setTitle("EinkCenterSleepWallpaper");
        params.setFitInsetsTypes(0);
        try {
            windowManager.addView(view, params);
            sleepWallpaper = view;
            android.util.Log.i("EinkCenter", "sleep wallpaper shown bitmap="
                    + (shownSleepWallpaper != null));
        } catch (RuntimeException e) {
            if (shownSleepWallpaper != null) {
                shownSleepWallpaper.recycle();
                shownSleepWallpaper = null;
            }
            android.util.Log.w("EinkCenter", "could not add sleep wallpaper", e);
        }
    }

    private void removeSleepWallpaper() {
        View old = sleepWallpaper;
        WindowManager manager = windowManager;
        sleepWallpaper = null;
        if (old != null && manager != null) {
            try {
                if (old.isAttachedToWindow()) manager.removeViewImmediate(old);
            } catch (RuntimeException ignored) { }
        }
        if (shownSleepWallpaper != null) {
            shownSleepWallpaper.recycle();
            shownSleepWallpaper = null;
        }
    }

    // ---------------------------------------------------------------- floating overlay

    private void updateOverlayVisibility() {
        if (destroyed) return;
        boolean visible = isInteractiveUnlocked();
        if (ProfileStore.bubbleEnabled(this) && visible) showBubble();
        else {
            // Hiding the optional bubble must not close an explicitly opened
            // panel. Screen-off/keyguard still removes both.
            removeBubble();
            if (!visible) collapsePanel();
        }
        if (ProfileStore.navBarEnabled(this) && visible) showNavBar();
        else if (visible) showGestureStrip();
        else removeNavBar();
    }

    private boolean isInteractiveUnlocked() {
        PowerManager pm = getSystemService(PowerManager.class);
        KeyguardManager km = getSystemService(KeyguardManager.class);
        return pm != null && km != null && pm.isInteractive() && !km.isKeyguardLocked();
    }

    private void showBubble() {
        if (destroyed || (bubble != null && bubble.isAttachedToWindow())) return;
        Rect bounds = windowBounds();
        int size = EinkUi.dp(this, 56);
        int fallbackX = EinkUi.dp(this, 12);
        int fallbackY = Math.round(bounds.height() / 3f);
        int x = clamp(ProfileStore.bubbleX(this, fallbackX), 0, Math.max(0, bounds.width() - size));
        int y = clamp(ProfileStore.bubbleY(this, fallbackY), 0, Math.max(0, bounds.height() - size));

        LinearLayout view = new LinearLayout(this);
        view.setOrientation(LinearLayout.VERTICAL);
        view.setGravity(Gravity.CENTER);
        view.setPadding(EinkUi.dp(this, 2), EinkUi.dp(this, 2), EinkUi.dp(this, 2), EinkUi.dp(this, 2));
        view.setBackground(EinkUi.box(EinkUi.BLACK, EinkUi.WHITE,
                Math.max(1, EinkUi.dp(this, 1)), size / 2));
        view.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_YES);

        TextView mark = EinkUi.text(this, "E", 23, EinkUi.WHITE, true);
        mark.setGravity(Gravity.CENTER);
        bubbleMode = EinkUi.text(this, EinkController.label(displayMode), 9, EinkUi.WHITE, true);
        bubbleMode.setGravity(Gravity.CENTER);
        view.addView(mark, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
        view.addView(bubbleMode, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        view.setContentDescription("E-ink Center, " + EinkController.label(displayMode)
                + ". Tap to expand, hold to open settings.");
        view.setOnClickListener(v -> expandPanel());
        view.setOnTouchListener(new BubbleTouch(view));

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                size, size, WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.OPAQUE);
        params.gravity = Gravity.TOP | Gravity.LEFT;
        params.x = x;
        params.y = y;
        params.windowAnimations = 0;
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setCanPlayMoveAnimation(false);
        params.setTitle("EinkCenterBubble");
        params.setFitInsetsTypes(0);
        try {
            windowManager.addView(view, params);
            bubble = view;
            bubbleParams = params;
            updateBubbleText();
        } catch (RuntimeException e) {
            bubble = null;
            bubbleParams = null;
            android.util.Log.w("EinkCenter", "could not add bubble", e);
        }
    }

    private void showNavBar() {
        if (destroyed || (navBar != null && navBar.isAttachedToWindow())) return;
        // E-ink-first default: a compact gesture strip, not a permanent opaque
        // three-button bar. Users can still enable the legacy full bar.
        if (!ProfileStore.navBarEnabled(this)) {
            showGestureStrip();
            return;
        }
        int height = EinkUi.dp(this, 64);
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER);
        bar.setPadding(EinkUi.dp(this, 4), EinkUi.dp(this, 4),
                EinkUi.dp(this, 4), EinkUi.dp(this, 4));
        bar.setBackground(EinkUi.box(EinkUi.WHITE, EinkUi.BLACK,
                Math.max(1, EinkUi.dp(this, 2)), 0));
        addNavButton(bar, "BACK", GLOBAL_ACTION_BACK);
        addNavButton(bar, "HOME", GLOBAL_ACTION_HOME);
        addNavButton(bar, "RECENTS", GLOBAL_ACTION_RECENTS);

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT, height,
                WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.OPAQUE);
        params.gravity = Gravity.BOTTOM | Gravity.LEFT;
        params.windowAnimations = 0;
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setCanPlayMoveAnimation(false);
        params.setTitle("EinkCenterNav");
        params.setFitInsetsTypes(0);
        try {
            windowManager.addView(bar, params);
            navBar = bar;
            navBarWindowManager = windowManager;
            navBarOwner = this;
        } catch (RuntimeException e) {
            navBar = null;
            android.util.Log.w("EinkCenter", "could not add navigation bar", e);
        }
    }

    private void showGestureStrip() {
        if (destroyed || (navBar != null && navBar.isAttachedToWindow())) return;
        final int heightPx = EinkUi.dp(this, 28);
        // Visual-only handle. Android's own gesture monitors own the hit region,
        // so this overlay never steals touches from apps or SystemUI.
        LinearLayout strip = new LinearLayout(this);
        strip.setGravity(Gravity.CENTER);
        strip.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        TextView handle = new TextView(this);
        handle.setBackground(EinkUi.box(EinkUi.BLACK, EinkUi.BLACK, 0, EinkUi.dp(this, 4)));
        strip.addView(handle, new LinearLayout.LayoutParams(EinkUi.dp(this, 144), EinkUi.dp(this, 8)));
        strip.setContentDescription("Android gesture navigation handle");

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                WindowManager.LayoutParams.MATCH_PARENT, heightPx,
                WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
                        | WindowManager.LayoutParams.FLAG_LAYOUT_NO_LIMITS,
                PixelFormat.TRANSLUCENT);
        params.gravity = Gravity.BOTTOM | Gravity.LEFT;
        params.windowAnimations = 0;
        params.flags |= WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE;
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setCanPlayMoveAnimation(false);
        params.setTitle("EinkCenterGestureStrip");
        params.setFitInsetsTypes(0);

        try {
            windowManager.addView(strip, params);
            navBar = strip;
            navBarWindowManager = windowManager;
            navBarOwner = this;
        } catch (RuntimeException e) {
            navBar = null;
            android.util.Log.w("EinkCenter", "could not add gesture strip", e);
        }
    }

    private void addNavButton(LinearLayout bar, String label, int action) {
        TextView button = EinkUi.text(this, label, 15, EinkUi.BLACK, true);
        button.setGravity(Gravity.CENTER);
        button.setMinHeight(EinkUi.dp(this, 56));
        button.setClickable(true);
        button.setFocusable(true);
        button.setContentDescription(label);
        button.setBackground(EinkUi.box(EinkUi.WHITE, EinkUi.BLACK,
                Math.max(1, EinkUi.dp(this, 1)), EinkUi.dp(this, 6)));
        button.setOnClickListener(v -> performGlobalAction(action));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                0, ViewGroup.LayoutParams.MATCH_PARENT, 1f);
        lp.setMargins(EinkUi.dp(this, 3), 0, EinkUi.dp(this, 3), 0);
        bar.addView(button, lp);
    }

    private void removeNavBar() {
        removeNavBar(false);
    }

    private void removeNavBar(boolean anyOwner) {
        if (!anyOwner && navBarOwner != this) return;
        View old = navBar;
        WindowManager manager = navBarWindowManager;
        navBar = null;
        navBarWindowManager = null;
        navBarOwner = null;
        if (old == null || manager == null) return;
        try {
            manager.removeViewImmediate(old);
        } catch (RuntimeException ignored) { }
    }

    private final class BubbleTouch implements View.OnTouchListener {
        private final View view;
        private float downX, downY;
        private int startX, startY;
        private long downAt;
        private boolean dragging;
        private boolean longFired;
        private final Runnable longPress;

        BubbleTouch(View view) {
            this.view = view;
            this.longPress = () -> {
                if (!dragging && !longFired && view.isAttachedToWindow()) {
                    longFired = true;
                    openMainActivity();
                }
            };
        }

        @Override
        public boolean onTouch(View v, MotionEvent event) {
            switch (event.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = event.getRawX();
                    downY = event.getRawY();
                    startX = bubbleParams.x;
                    startY = bubbleParams.y;
                    downAt = android.os.SystemClock.uptimeMillis();
                    dragging = false;
                    longFired = false;
                    main.postDelayed(longPress, ViewConfiguration.getLongPressTimeout());
                    return true;
                case MotionEvent.ACTION_MOVE:
                    float dx = event.getRawX() - downX;
                    float dy = event.getRawY() - downY;
                    if (!dragging && Math.hypot(dx, dy) > ViewConfiguration.get(v.getContext()).getScaledTouchSlop()) {
                        dragging = true;
                        main.removeCallbacks(longPress);
                    }
                    if (dragging && bubbleParams != null) {
                        Rect bounds = windowBounds();
                        int size = EinkUi.dp(v.getContext(), 56);
                        bubbleParams.x = clamp(startX + Math.round(dx), 0, Math.max(0, bounds.width() - size));
                        bubbleParams.y = clamp(startY + Math.round(dy), 0, Math.max(0, bounds.height() - size));
                        try {
                            windowManager.updateViewLayout(view, bubbleParams);
                        } catch (RuntimeException ignored) { }
                    }
                    return true;
                case MotionEvent.ACTION_UP:
                    main.removeCallbacks(longPress);
                    if (dragging && bubbleParams != null) {
                        ProfileStore.setBubblePosition(EinkAccessibilityService.this, bubbleParams.x, bubbleParams.y);
                    } else if (!longFired
                            && android.os.SystemClock.uptimeMillis() - downAt < ViewConfiguration.getLongPressTimeout()) {
                        v.performClick();
                    }
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    main.removeCallbacks(longPress);
                    if (dragging && bubbleParams != null) {
                        ProfileStore.setBubblePosition(EinkAccessibilityService.this, bubbleParams.x, bubbleParams.y);
                    }
                    return true;
                default:
                    return true;
            }
        }
    }

    private void expandPanel() {
        android.util.Log.i("EinkCenter", "expandPanel requested destroyed=" + destroyed +
                " interactive=" + isInteractiveUnlocked() + " expanded=" + expanded);
        if (destroyed || !isInteractiveUnlocked()) return;
        if (expanded || panel != null) {
            collapsePanel();
            return;
        }
        Rect bounds = windowBounds();
        int width = Math.min(EinkUi.dp(this, 352), Math.max(EinkUi.dp(this, 280), bounds.width() - EinkUi.dp(this, 16)));
        int maxHeight = Math.max(EinkUi.dp(this, 200), bounds.height() - EinkUi.dp(this, 16));

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(EinkUi.dp(this, 12), EinkUi.dp(this, 12), EinkUi.dp(this, 12), EinkUi.dp(this, 12));
        root.setBackground(EinkUi.box(EinkUi.WHITE, EinkUi.BLACK,
                Math.max(1, EinkUi.dp(this, 2)), EinkUi.dp(this, 14)));
        root.setOnTouchListener((v, event) -> {
            if (event.getActionMasked() == MotionEvent.ACTION_OUTSIDE) {
                collapsePanel();
                return true;
            }
            return false;
        });

        LinearLayout header = new LinearLayout(this);
        header.setOrientation(LinearLayout.HORIZONTAL);
        header.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout titles = new LinearLayout(this);
        titles.setOrientation(LinearLayout.VERTICAL);
        TextView title = EinkUi.text(this, "E-ink Center", 20, EinkUi.BLACK, true);
        panelApp = EinkUi.text(this, "Foreground: " + foregroundLabel(currentApp), 12, EinkUi.GRAY, false);
        panelApp.setPadding(0, EinkUi.dp(this, 2), 0, 0);
        titles.addView(title);
        titles.addView(panelApp);
        header.addView(titles, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        Button settings = compactButton("Settings", this::openMainActivity);
        Button close = compactButton("×", this::collapsePanel);
        header.addView(settings);
        header.addView(close);
        root.addView(header);
        root.addView(EinkUi.divider(this), new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, Math.max(1, EinkUi.dp(this, 1))));

        BoundedScrollView scroll = new BoundedScrollView(this, maxHeight);
        scroll.setFillViewport(false);
        scroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        LinearLayout body = new LinearLayout(this);
        body.setOrientation(LinearLayout.VERTICAL);
        body.setPadding(0, EinkUi.dp(this, 10), 0, 0);
        scroll.addView(body);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        TextView modeHeading = EinkUi.text(this,
                ProfileStore.profilesEnabled(this) && !currentApp.isEmpty()
                        ? "Per-app refresh mode" : "Global refresh mode",
                13, EinkUi.BLACK, true);
        modeHeading.setPadding(0, 0, 0, EinkUi.dp(this, 6));
        body.addView(modeHeading);
        panelModes.clear();
        String[] modes = EinkController.modes();
        for (int i = 0; i < modes.length; i += 2) {
            LinearLayout modeRow = new LinearLayout(this);
            modeRow.setOrientation(LinearLayout.HORIZONTAL);
            for (int j = i; j < Math.min(i + 2, modes.length); j++) {
                String mode = modes[j];
                Button button = EinkUi.button(this, EinkController.label(mode), () -> choosePanelMode(mode));
                button.setTextSize(15);
                button.setMinHeight(EinkUi.dp(this, 56));
                panelModes.put(button, mode);
                EinkUi.addWeighted(modeRow, button, 1f);
            }
            body.addView(modeRow);
        }

        Button toggle = EinkUi.button(this, "Toggle Fast / Hyper",
                () -> choosePanelMode(EinkController.toggleFastHyper(displayMode)));
        body.addView(toggle, fullWidth());
        Button refresh = EinkUi.solidButton(this, "Full refresh", () -> EinkController.fullRefreshAsync(this));
        body.addView(refresh, fullWidth());
        Button profiles = EinkUi.button(this, "Per-app modes", () -> {
            collapsePanel();
            startActivity(new Intent(this, ProfileActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        });
        body.addView(profiles, fullWidth());
        Button extra = EinkUi.button(this, "Extra button", () -> {
            collapsePanel();
            startActivity(new Intent(this, ButtonSettingsActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        });
        body.addView(extra, fullWidth());
        frontlightButton = EinkUi.button(this, "", this::toggleFrontlight);
        body.addView(frontlightButton, fullWidth());
        navBarToggle = EinkUi.button(this, "", () -> {
            ProfileStore.setNavBarEnabled(this, !ProfileStore.navBarEnabled(this));
            updateOverlayVisibility();
            updatePanelState();
        });
        body.addView(navBarToggle, fullWidth());

        int x;
        int y;
        if (bubbleParams != null) {
            x = bubbleParams.x + EinkUi.dp(this, 28) - width / 2;
            y = bubbleParams.y;
            if (y + maxHeight > bounds.height()) y = bubbleParams.y + EinkUi.dp(this, 56) - maxHeight;
        } else {
            x = (bounds.width() - width) / 2;
            y = (bounds.height() - maxHeight) / 2;
        }
        x = clamp(x, EinkUi.dp(this, 8), Math.max(EinkUi.dp(this, 8), bounds.width() - width - EinkUi.dp(this, 8)));
        y = clamp(y, EinkUi.dp(this, 8), Math.max(EinkUi.dp(this, 8), bounds.height() - maxHeight - EinkUi.dp(this, 8)));

        WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                width, ViewGroup.LayoutParams.WRAP_CONTENT,
                WindowManager.LayoutParams.TYPE_ACCESSIBILITY_OVERLAY,
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                        | WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                        | WindowManager.LayoutParams.FLAG_WATCH_OUTSIDE_TOUCH
                        | WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
                PixelFormat.OPAQUE);
        params.gravity = Gravity.TOP | Gravity.LEFT;
        params.x = x;
        params.y = y;
        params.windowAnimations = 0;
        if (android.os.Build.VERSION.SDK_INT >= 31) params.setCanPlayMoveAnimation(false);
        params.setTitle("EinkCenterPanel");
        params.setFitInsetsTypes(0);
        try {
            windowManager.addView(root, params);
            panel = root;
            expanded = true;
            updatePanelState();
        } catch (RuntimeException e) {
            panel = null;
            expanded = false;
            android.util.Log.w("EinkCenter", "could not add panel", e);
        }
    }

    private Button compactButton(String label, Runnable action) {
        Button button = EinkUi.button(this, label, action);
        button.setMinWidth(EinkUi.dp(this, 48));
        button.setMinimumWidth(EinkUi.dp(this, 48));
        button.setPadding(EinkUi.dp(this, 10), EinkUi.dp(this, 6), EinkUi.dp(this, 10), EinkUi.dp(this, 6));
        if ("×".equals(label)) button.setTextSize(22);
        else button.setTextSize(12);
        return button;
    }

    private LinearLayout.LayoutParams fullWidth() {
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        params.topMargin = EinkUi.dp(this, 6);
        return params;
    }

    private void choosePanelMode(String mode) {
        String safe = EinkController.normalizeMode(mode);
        if (safe == null) return;
        if (ProfileStore.profilesEnabled(this) && !currentApp.isEmpty() && !ignoredPackage(currentApp)) {
            ProfileStore.setProfile(this, currentApp, safe);
        } else {
            ProfileStore.setGlobalMode(this, safe);
        }
        displayMode = safe;
        updateOverlayState();
        EinkController.applyAsync(this, safe);
    }

    private void updateOverlayState() {
        updateBubbleText();
        updatePanelState();
    }

    private void updateBubbleText() {
        if (bubbleMode == null) return;
        String label = EinkController.label(displayMode);
        bubbleMode.setText(label);
        if (bubble != null) bubble.setContentDescription("E-ink Center, " + label
                + ". Tap to expand, hold to open settings.");
    }

    private void updatePanelState() {
        if (panel == null) return;
        if (panelApp != null) panelApp.setText("Foreground: " + foregroundLabel(currentApp));
        for (Map.Entry<Button, String> entry : panelModes.entrySet()) {
            boolean selected = entry.getValue().equals(displayMode);
            EinkUi.styleButton(entry.getKey(), selected, false);
            entry.getKey().setContentDescription(EinkController.label(entry.getValue())
                    + (selected ? ", selected" : ""));
        }
        if (frontlightButton != null) {
            boolean on = ProfileStore.prefs(this).getBoolean("light_on", true);
            frontlightButton.setText("Frontlight: " + (on ? "On" : "Off"));
        }
        if (navBarToggle != null) {
            navBarToggle.setText("Navigation buttons: "
                    + (ProfileStore.navBarEnabled(this) ? "On" : "Off"));
        }
    }

    private void toggleFrontlight() {
        SharedPreferences prefs = ProfileStore.prefs(this);
        boolean next = !prefs.getBoolean("light_on", true);
        int cold = prefs.getInt("light_cold", 80);
        int warm = prefs.getInt("light_warm", 40);
        int code = next ? -1 : 0;
        String command = "P=$(ls " + LM + "cold_light 2>/dev/null | head -1); "
                + "[ -n \"$P\" ] && echo " + (code < 0 ? cold : code) + " > \"$P\"; "
                + "P=$(ls " + LM + "warm_light 2>/dev/null | head -1); "
                + "[ -n \"$P\" ] && echo " + (code < 0 ? warm : code) + " > \"$P\"";
        prefs.edit().putBoolean("light_on", next).apply();
        io.execute(() -> RootShell.get().cmd(command));
        updatePanelState();
    }

    private void openMainActivity() {
        collapsePanel();
        try {
            startActivity(new Intent(this, MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
        } catch (RuntimeException ignored) { }
    }

    private void collapsePanel() {
        if (panel != null) {
            try {
                if (panel.isAttachedToWindow()) windowManager.removeViewImmediate(panel);
            } catch (RuntimeException ignored) { }
        }
        panel = null;
        panelApp = null;
        frontlightButton = null;
        navBarToggle = null;
        panelModes.clear();
        expanded = false;
    }

    private void removeBubble() {
        if (bubble != null) {
            try {
                if (bubble.isAttachedToWindow()) windowManager.removeViewImmediate(bubble);
            } catch (RuntimeException ignored) { }
        }
        bubble = null;
        bubbleParams = null;
        bubbleMode = null;
    }

    private void removeOverlay() {
        collapsePanel();
        removeBubble();
        removeNavBar();
    }

    private Rect windowBounds() {
        try {
            return windowManager.getCurrentWindowMetrics().getBounds();
        } catch (RuntimeException e) {
            return new Rect(0, 0, getResources().getDisplayMetrics().widthPixels,
                    getResources().getDisplayMetrics().heightPixels);
        }
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private static final class BoundedScrollView extends ScrollView {
        private final int maxHeight;

        BoundedScrollView(Context context, int maxHeight) {
            super(context);
            this.maxHeight = maxHeight;
        }

        @Override
        protected void onMeasure(int widthSpec, int heightSpec) {
            int height = MeasureSpec.getSize(heightSpec);
            if (height <= 0 || height > maxHeight) height = maxHeight;
            super.onMeasure(widthSpec, MeasureSpec.makeMeasureSpec(height, MeasureSpec.AT_MOST));
        }
    }

    // ---------------------------------------------------------------- hardware button (accessibility filter)

    /** Accessibility key filtering is not reliable on this GSI; never consume here. */
    @Override
    public synchronized boolean onKeyEvent(KeyEvent event) {
        if (event == null) return false;
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (event.getKeyCode() == KeyEvent.KEYCODE_POWER) {
                lastUserActivity = SystemClock.elapsedRealtime();
                notePowerKey();
            } else {
                noteUserActivity();
            }
        }
        InputDevice device = InputDevice.getDevice(event.getDeviceId());
        String deviceName = device == null ? "unknown" : device.getName();
        if (event.getKeyCode() == KeyEvent.KEYCODE_VOLUME_UP ||
                event.getScanCode() == EXTRA_BUTTON_SCAN_CODE) {
            android.util.Log.i("EinkCenter", "key action=" + event.getAction()
                    + " keyCode=" + event.getKeyCode()
                    + " scan=" + event.getScanCode()
                    + " device=" + event.getDeviceId() + ":" + deviceName
                    + " extra=" + isExtraButtonEvent(event));
        }
        if (!isExtraButtonEvent(event)) return false;
        int deviceId = event.getDeviceId();
        Long ownedDown = ownedKeys.get(deviceId);
        boolean owned = ownedDown != null && ownedDown.longValue() == event.getDownTime();
        boolean physical = isPhysicalNonVirtual(event);
        if (!owned && !physical) return false;

        boolean canceled = (event.getFlags() & KeyEvent.FLAG_CANCELED) != 0;
        if (canceled) {
            if (!owned) ownedKeys.put(deviceId, event.getDownTime());
            enqueueKey(event);
            return true;
        }

        switch (event.getAction()) {
            case KeyEvent.ACTION_DOWN:
                if (physical && event.getRepeatCount() == 0 && !owned) {
                    if (ownedKeys.containsKey(deviceId)) main.post(gesture::cancel);
                    ownedKeys.put(deviceId, event.getDownTime());
                    enqueueKey(event);
                }
                return true;
            case KeyEvent.ACTION_UP:
                if (owned) {
                    ownedKeys.remove(deviceId);
                    enqueueKey(event);
                }
                return true;
            case KeyEvent.ACTION_MULTIPLE:
            default:
                return true;
        }
    }

    private boolean isExtraButtonEvent(KeyEvent event) {
        if (event.getScanCode() == EXTRA_BUTTON_SCAN_CODE) return true;
        InputDevice device = InputDevice.getDevice(event.getDeviceId());
        return event.getKeyCode() == KeyEvent.KEYCODE_VOLUME_UP
                && device != null && "mtk-pmic-keys".equals(device.getName());
    }

    private boolean isPhysicalNonVirtual(KeyEvent event) {
        InputDevice device = InputDevice.getDevice(event.getDeviceId());
        return device != null && !device.isVirtual() && !device.isExternal()
                && (device.supportsSource(InputDevice.SOURCE_KEYBOARD)
                    || "mtk-pmic-keys".equals(device.getName()));
    }

    private void enqueueKey(KeyEvent event) {
        KeyEvent copy = new KeyEvent(event);
        main.post(() -> {
            if (!destroyed) gesture.onEvent(copy);
        });
    }

    private synchronized void cancelHardwareKeys() {
        gesture.cancel();
        ownedKeys.clear();
    }

    private final class HardwareGesture {
        private boolean down;
        private boolean inLong;
        private boolean doublePress;
        private boolean pendingSingle;
        private long lastUp;

        void onEvent(KeyEvent event) {
            if ((event.getFlags() & KeyEvent.FLAG_CANCELED) != 0) {
                cancel();
                return;
            }
            switch (event.getAction()) {
                case KeyEvent.ACTION_DOWN:
                    if (event.getRepeatCount() != 0 || down) return;
                    boolean second = pendingSingle && lastUp != 0
                            && event.getEventTime() - lastUp >= 0
                            && event.getEventTime() - lastUp <= DOUBLE_MS;
                    if (pendingSingle) {
                        pendingSingle = false;
                        main.removeCallbacks(singleAction);
                        if (!second) dispatchButtonAction(ProfileStore.PRESS_SINGLE);
                    }
                    down = true;
                    inLong = false;
                    doublePress = second;
                    if (second) {
                        dispatchButtonAction(ProfileStore.PRESS_DOUBLE);
                    } else {
                        main.postAtTime(longAction, event.getEventTime() + LONG_MS);
                    }
                    return;
                case KeyEvent.ACTION_UP:
                    if (!down) return;
                    down = false;
                    main.removeCallbacks(longAction);
                    if (!inLong && !doublePress) {
                        pendingSingle = true;
                        lastUp = event.getEventTime();
                        main.postAtTime(singleAction, event.getEventTime() + DOUBLE_MS);
                    } else {
                        // A recognized double/long can never pair with the next tap.
                        pendingSingle = false;
                        lastUp = 0;
                    }
                    return;
                default:
                    break;
            }
        }

        void cancel() {
            main.removeCallbacks(longAction);
            main.removeCallbacks(singleAction);
            down = false;
            inLong = false;
            doublePress = false;
            pendingSingle = false;
            lastUp = 0;
        }
    }

    private final Runnable longAction = () -> {
        if (!destroyed && gesture.down && !gesture.inLong && !gesture.doublePress) {
            gesture.inLong = true;
            dispatchButtonAction(ProfileStore.PRESS_LONG);
        }
    };

    private final Runnable singleAction = () -> {
        if (!destroyed && gesture.pendingSingle) {
            gesture.pendingSingle = false;
            gesture.lastUp = 0;
            dispatchButtonAction(ProfileStore.PRESS_SINGLE);
        }
    };

    private void dispatchButtonAction(String press) {
        if (destroyed) return;
        PowerManager pm = getSystemService(PowerManager.class);
        boolean screenOff = pm == null || !pm.isInteractive();
        String action = ProfileStore.buttonAction(this, screenOff, press);
        switch (ProfileStore.normalizeAction(action)) {
            case ProfileStore.ACTION_OPEN_MENU:
                expandPanel();
                break;
            case ProfileStore.ACTION_FULL_REFRESH:
                EinkController.fullRefreshAsync(this);
                break;
            case ProfileStore.ACTION_TOGGLE_FAST_HYPER:
                io.execute(() -> {
                    String next = EinkController.toggleFastHyper(EinkController.readMode(this));
                    EinkController.applyGlobalAsync(this, next);
                });
                break;
            case ProfileStore.ACTION_CYCLE_MODE:
                io.execute(() -> {
                    String next = EinkController.nextInCycle(EinkController.readMode(this));
                    EinkController.applyGlobalAsync(this, next);
                });
                break;
            case ProfileStore.ACTION_HOME:
                performGlobalAction(GLOBAL_ACTION_HOME);
                break;
            case ProfileStore.ACTION_BACK:
                performGlobalAction(GLOBAL_ACTION_BACK);
                break;
            case ProfileStore.ACTION_RECENTS:
                performGlobalAction(GLOBAL_ACTION_RECENTS);
                break;
            case ProfileStore.ACTION_NOTHING:
            default:
                break;
        }
    }
}
