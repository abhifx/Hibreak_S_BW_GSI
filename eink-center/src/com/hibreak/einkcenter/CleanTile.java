package com.hibreak.einkcenter;

import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/** QS tile: full-screen refresh (clears ghosting). */
public class CleanTile extends TileService {

    @Override
    public void onStartListening() {
        super.onStartListening();
        Tile t = getQsTile();
        if (t != null) {
            t.setLabel("E-ink refresh");
            t.setState(Tile.STATE_INACTIVE);
            t.updateTile();
        }
    }

    @Override
    public void onClick() {
        super.onClick();
        new Thread(() -> {
            // e-ink panels do a full GC refresh on wake
            Root.run("input keyevent 26; sleep 1; input keyevent 26; "
                    + "service call SurfaceFlinger 1008 i32 1");
        }).start();
    }
}
