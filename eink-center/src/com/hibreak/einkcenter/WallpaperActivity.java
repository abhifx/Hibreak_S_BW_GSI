package com.hibreak.einkcenter;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.os.Bundle;
import android.app.WallpaperManager;

/** One-shot: sets a flat paper-white wallpaper for home and lock screens. */
public class WallpaperActivity extends Activity {

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        new Thread(() -> {
            try {
                Bitmap bmp = Bitmap.createBitmap(720, 1440, Bitmap.Config.ARGB_8888);
                Canvas c = new Canvas(bmp);
                c.drawColor(Color.rgb(0xF7, 0xF7, 0xF4));
                WallpaperManager wm = WallpaperManager.getInstance(this);
                wm.setBitmap(bmp, null, true, WallpaperManager.FLAG_SYSTEM);
                wm.setBitmap(bmp, null, true, WallpaperManager.FLAG_LOCK);
            } catch (Throwable t) {
                android.util.Log.e("WALLPAPER", "set failed", t);
            }
            runOnUiThread(this::finish);
        }, "set-wallpaper").start();
    }
}
