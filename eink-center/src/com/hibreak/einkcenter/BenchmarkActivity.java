package com.hibreak.einkcenter;

import android.app.Activity;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.Choreographer;
import android.view.View;
import android.view.WindowManager;

/**
 * Frame-rate benchmark for the e-ink panel.
 *
 * Invalidates on every vsync and moves a large band, so the composer actually
 * receives a new buffer each frame. The commit rate can then be read from
 * `logcat -s hwcomposer` (`normal commit`) instead of relying on synthetic
 * input injection, which is far slower than the panel.
 */
public class BenchmarkActivity extends Activity {

    private BenchView view;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        float frac = getIntent() != null ? getIntent().getFloatExtra("frac", 0.35f) : 0.35f;
        view = new BenchView(frac);
        setContentView(view);
    }

    private class BenchView extends View implements Choreographer.FrameCallback {

        private final Paint paint = new Paint();
        private final Paint text = new Paint();
        private final float frac;
        private float phase;
        private int frames;
        private long start = -1;

        BenchView(float frac) {
            super(BenchmarkActivity.this);
            this.frac = frac;
            text.setColor(Color.BLACK);
            text.setTextSize(48f);
            text.setAntiAlias(true);
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
            invalidate();
            Choreographer.getInstance().postFrameCallback(this);
        }

        @Override
        protected void onDraw(Canvas c) {
            long now = System.nanoTime();
            if (start < 0) start = now;
            frames++;

            c.drawColor(Color.WHITE);

            // A large moving band: big enough to be a real update, small enough
            // that the composer can keep up.
            int h = getHeight();
            int w = getWidth();
            float bandH = h * frac;
            float y = (phase % (h + bandH)) - bandH;
            paint.setColor(Color.BLACK);
            c.drawRect(0, y, w, y + bandH, paint);

            phase += bandH / 6f;

            double secs = (now - start) / 1e9;
            c.drawText(String.format(java.util.Locale.US, "%.0f fps", frames / secs), 40, 80, text);
        }
    }
}
