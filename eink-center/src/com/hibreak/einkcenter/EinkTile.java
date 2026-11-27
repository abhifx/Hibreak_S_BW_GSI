package com.hibreak.einkcenter;

import android.app.PendingIntent;
import android.content.Intent;
import android.service.quicksettings.Tile;
import android.service.quicksettings.TileService;

/**
 * Quick Settings menu entry. Tap asks the accessibility service to toggle its
 * panel; SystemUI long-press opens MainActivity through QS_TILE_PREFERENCES.
 */
public class EinkTile extends TileService {

    @Override
    public void onStartListening() {
        super.onStartListening();
        refresh();
    }

    @Override
    public void onClick() {
        super.onClick();
        if (!EinkAccessibilityService.requestMenu(this)) {
            android.util.Log.i("EinkCenter", "QS menu clicked; accessibility service not running");
            PendingIntent open = PendingIntent.getActivity(this, 0,
                    new Intent(this, MainActivity.class),
                    PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);
            startActivityAndCollapse(open);
        }
        refresh();
    }

    private void refresh() {
        Tile tile = getQsTile();
        if (tile == null) return;
        boolean connected = EinkAccessibilityService.isRunning();
        tile.setLabel("E-ink menu");
        tile.setSubtitle(connected ? "Tap: panel · Hold: app" : "Enable service · Hold: app");
        tile.setState(connected ? Tile.STATE_ACTIVE : Tile.STATE_INACTIVE);
        tile.updateTile();
    }
}
