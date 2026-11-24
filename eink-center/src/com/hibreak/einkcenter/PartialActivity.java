package com.hibreak.einkcenter;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.View;
import android.view.WindowManager;

/**
 * Diagnostic activity: animates a small block and invalidates only the old and
 * new block rectangles, so the layer damage is small instead of full-screen.
 * Used to check whether the XRZ composer does partial panel updates on this GSI.
 */
public class PartialActivity extends Activity {

    private PartialView view;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        view = new PartialView();
        setContentView(view);
    }

    private class PartialView extends View implements Choreographer.FrameCallback {

        private final Paint black = new Paint();
        private final Paint white = new Paint();
        private final Rect drawnR = new Rect();
        private final Rect nextR = new Rect();
        private final int w = 320, h = 240;
        private float x = 60, y = 260, vx = 14, vy = 10;
        private boolean first = true;

        PartialView() {
            super(PartialActivity.this);
            black.setColor(Color.BLACK);
            white.setColor(Color.WHITE);
            setBackgroundColor(Color.WHITE);
        }

        @Override
        protected void onAttachedToWindow() {
            super.onAttachedToWindow();
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override
        protected void onDetachedFromWindow() {
            Choreographer.getInstance().removeFrameCallback(this);
            super.onDetachedFromWindow();
        }

        @Override
        public void doFrame(long frameTimeNanos) {
            int W = getWidth(), H = getHeight();
            if (W > 0 && H > 0) {
                if (!first) {
                    invalidate(drawnR);
                }
                x += vx;
                y += vy;
                if (x < 0 || x + w > W) { vx = -vx; x = Math.max(0, Math.min(x, W - w)); }
                if (y < 0 || y + h > H) { vy = -vy; y = Math.max(0, Math.min(y, H - h)); }
                nextR.set((int) x, (int) y, (int) x + w, (int) y + h);
                invalidate(nextR);
                first = false;
            }
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override
        protected void onDraw(Canvas c) {
            if (drawnR.isEmpty()) {
                c.drawColor(Color.WHITE);
            } else {
                c.drawRect(drawnR, white);
            }
            c.drawRect(nextR, black);
            drawnR.set(nextR);
        }
    }
}
