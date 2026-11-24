package com.hibreak.einkcenter;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/**
 * Quick Settings tile: flips between the two refresh modes that matter
 * (Hyper 183 = Bigme's fast mode, Fast 178 = the default). Long-press opens
 * the app.
 */
public class EinkTile extends TileService {

    private static final int MODE_FAST = 178;   // stock "Fast"
    private static final int MODE_HYPER = 183;  // stock "Hyper"

    @Override
    public void onStartListening() {
        super.onStartListening();
        new Thread(this::refresh).start();
    }

    @Override
    public void onClick() {
        super.onClick();
        new Thread(() -> {
            int current = readMode();
            int next = (current == MODE_HYPER) ? MODE_FAST : MODE_HYPER;
            Root.run("setprop vendor.xrz.force_global_refresh_mode 0; "
                    + "setprop vendor.xrz.global_refresh_mode " + next + "; "
                    + "echo " + next + " > /sys/kernel/debug/eink_debug/global_mode; "
                    + "service call SurfaceFlinger 1008 i32 1");
            refresh();
        }).start();
    }

    private int readMode() {
        String v = Root.run("getprop vendor.xrz.global_refresh_mode").trim();
        try {
            return Integer.parseInt(v);
        } catch (NumberFormatException e) {
            return MODE_FAST;
        }
    }

    private void refresh() {
        int mode = readMode();
        Tile t = getQsTile();
        if (t == null) return;
        t.setLabel("E-ink " + (mode == MODE_HYPER ? "hyper" : "fast"));
        t.setState(mode == MODE_HYPER ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        t.updateTile();
    }
}
