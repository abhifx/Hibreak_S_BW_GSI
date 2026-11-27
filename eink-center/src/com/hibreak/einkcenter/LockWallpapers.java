package com.hibreak.einkcenter;

import android.app.WallpaperManager;
import android.content.Context;
import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Environment;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.Arrays;
import java.util.Comparator;
import java.util.Locale;

/** Cycles lock-screen wallpapers from Pictures/EinkWallpapers on every lock. */
public final class LockWallpapers {

    public static final String DIR = "EinkWallpapers";
    private static final int W = 720, H = 1440;

    private LockWallpapers() { }

    public static File folder(Context c) {
        return new File(Environment.getExternalStoragePublicDirectory(
                Environment.DIRECTORY_PICTURES), DIR);
    }

    public static File[] images(Context c) {
        File dir = folder(c);
        File[] files = dir.listFiles((d, n) -> {
            String l = n.toLowerCase(Locale.US);
            return l.endsWith(".jpg") || l.endsWith(".jpeg") || l.endsWith(".png")
                    || l.endsWith(".webp") || l.endsWith(".bmp");
        });
        if (files != null) Arrays.sort(files, Comparator.comparing(File::getName));
        return files;
    }

    public static int count(Context c) {
        File[] f = images(c);
        return f == null ? 0 : f.length;
    }

    /** Copy factory samples from /system if the user folder is still empty. */
    public static void seed(Context c) {
        File dir = folder(c);
        if (!dir.isDirectory() && !dir.mkdirs()) return;
        File[] have = images(c);
        if (have != null && have.length > 0) return;
        File[] seeds = new File("/system/etc/eink/wallpapers").listFiles();
        if (seeds == null) return;
        for (File src : seeds) {
            String l = src.getName().toLowerCase(Locale.US);
            if (!(l.endsWith(".jpg") || l.endsWith(".jpeg") || l.endsWith(".png")
                    || l.endsWith(".webp") || l.endsWith(".bmp"))) continue;
            File dst = new File(dir, src.getName());
            try (FileInputStream in = new FileInputStream(src);
                 FileOutputStream out = new FileOutputStream(dst)) {
                byte[] buf = new byte[64 * 1024];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            } catch (Throwable ignored) {
                // Storage may still be read-only during early boot.
            }
        }
    }

    /** Advance to the next image and set it as the lock-screen wallpaper. */
    public static void next(Context c) {
        Bitmap out = takeNext(c, "wall_index");
        if (out == null) return;
        try {
            WallpaperManager wm = WallpaperManager.getInstance(c);
            wm.setBitmap(out, null, true, WallpaperManager.FLAG_LOCK);
        } catch (Throwable ignored) {
            // Write may fail while the screen is off; try again on the next lock.
        } finally {
            out.recycle();
        }
    }

    /** Decode the next sleep image without changing the Android wallpaper. */
    public static Bitmap nextSleep(Context c) {
        return takeNext(c, "sleep_wall_index");
    }

    private static Bitmap takeNext(Context c, String indexKey) {
        File[] files = images(c);
        if (files == null || files.length == 0) return null;
        SharedPreferences p = c.getSharedPreferences("eink", Context.MODE_PRIVATE);
        int idx = (p.getInt(indexKey, -1) + 1) % files.length;
        Bitmap src = decode(files[idx]);
        if (src == null) return null;
        try {
            Bitmap out = render(src);
            p.edit().putInt(indexKey, idx).apply();
            return out;
        } finally {
            src.recycle();
        }
    }

    private static Bitmap decode(File f) {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeFile(f.getAbsolutePath(), bounds);
        int sample = 1;
        while (bounds.outWidth / (sample * 2) >= W && bounds.outHeight / (sample * 2) >= H) {
            sample *= 2;
        }
        BitmapFactory.Options opts = new BitmapFactory.Options();
        opts.inSampleSize = sample;
        opts.inPreferredConfig = Bitmap.Config.ARGB_8888;
        return BitmapFactory.decodeFile(f.getAbsolutePath(), opts);
    }

    /** Center-crop to 720x1440 and apply a mild grayscale + contrast curve. */
    private static Bitmap render(Bitmap src) {
        Bitmap out = Bitmap.createBitmap(W, H, Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(out);
        c.drawColor(Color.WHITE);
        float scale = Math.max(W / (float) src.getWidth(), H / (float) src.getHeight());
        float dw = src.getWidth() * scale;
        float dh = src.getHeight() * scale;
        RectF dst = new RectF((W - dw) / 2f, (H - dh) / 2f, (W + dw) / 2f, (H + dh) / 2f);
        Paint p = new Paint(Paint.FILTER_BITMAP_FLAG);
        ColorMatrix cm = new ColorMatrix();
        cm.setSaturation(0);
        cm.postConcat(new ColorMatrix(new float[]{
                1.15f, 0, 0, 0, -18,
                0, 1.15f, 0, 0, -18,
                0, 0, 1.15f, 0, -18,
                0, 0, 0, 1, 0}));
        p.setColorFilter(new ColorMatrixColorFilter(cm));
        c.drawBitmap(src, null, dst, p);
        return out;
    }
}
