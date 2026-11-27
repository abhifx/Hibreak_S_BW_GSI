package com.hibreak.einkcenter;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/**
 * QS tile: frontlight on/off, remembering the last cold/warm levels.
 *
 * Both LM3630A channels are written through a resolved path (the shell does not
 * expand a glob inside a redirection target).
 */
public class LightTile extends TileService {

    private static final String PREF_ON = "light_on";
    private static final String PREF_COLD = "light_cold";
    private static final String PREF_WARM = "light_warm";
    private static final int DEF_COLD = 80;
    private static final int DEF_WARM = 40;

    private static final String LM = "/sys/devices/platform/*/i2c-*/2-0036/lm3630a_";

    @Override
    public void onStartListening() {
        super.onStartListening();
        refresh();
    }

    @Override
    public void onClick() {
        super.onClick();
        boolean on = getSharedPreferences("eink", MODE_PRIVATE).getBoolean(PREF_ON, true);
        new Thread(() -> {
            setLight(!on);
            refresh();
        }).start();
    }

    private void setLight(boolean on) {
        int cold = getSharedPreferences("eink", MODE_PRIVATE).getInt(PREF_COLD, DEF_COLD);
        int warm = getSharedPreferences("eink", MODE_PRIVATE).getInt(PREF_WARM, DEF_WARM);
        if (on) {
            RootShell.get().cmd("P=$(ls " + LM + "cold_light 2>/dev/null | head -1); "
                    + "[ -n \"$P\" ] && echo " + cold + " > \"$P\"; "
                    + "P=$(ls " + LM + "warm_light 2>/dev/null | head -1); "
                    + "[ -n \"$P\" ] && echo " + warm + " > \"$P\"");
        } else {
            RootShell.get().cmd("P=$(ls " + LM + "cold_light 2>/dev/null | head -1); "
                    + "[ -n \"$P\" ] && echo 0 > \"$P\"; "
                    + "P=$(ls " + LM + "warm_light 2>/dev/null | head -1); "
                    + "[ -n \"$P\" ] && echo 0 > \"$P\"");
        }
        getSharedPreferences("eink", MODE_PRIVATE).edit().putBoolean(PREF_ON, on).apply();
    }

    private void refresh() {
        boolean on = getSharedPreferences("eink", MODE_PRIVATE).getBoolean(PREF_ON, true);
        Tile t = getQsTile();
        if (t == null) return;
        t.setLabel(on ? "Frontlight on" : "Frontlight off");
        t.setState(on ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        t.updateTile();
    }
}
