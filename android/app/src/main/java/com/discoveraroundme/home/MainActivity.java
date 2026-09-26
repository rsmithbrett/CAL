package com.discoveraroundme.home;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.os.Bundle;
import android.view.View;

/** First build scaffold. It deliberately does not connect to the cloud. */
public final class MainActivity extends Activity {
    static {
        System.loadLibrary("discovercards");
    }

    private static native String nativeCardDescriptor();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setContentView(new CardCanvas(this, nativeCardDescriptor()));
    }

    private static final class CardCanvas extends View {
        private static final float WIDTH = 320f;
        private static final float HEIGHT = 240f;
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final String descriptor;

        CardCanvas(Context context, String descriptor) {
            super(context);
            this.descriptor = descriptor;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            super.onDraw(canvas);
            canvas.drawColor(Color.rgb(18, 33, 50));
            final float scale = Math.min(getWidth() / WIDTH, getHeight() / HEIGHT);
            canvas.save();
            canvas.translate((getWidth() - WIDTH * scale) / 2f,
                    (getHeight() - HEIGHT * scale) / 2f);
            canvas.scale(scale, scale);

            paint.setColor(Color.rgb(242, 246, 248));
            canvas.drawRoundRect(0, 0, WIDTH, HEIGHT, 12, 12, paint);
            paint.setColor(Color.rgb(13, 58, 87));
            canvas.drawRoundRect(0, 0, WIDTH, 45, 12, 12, paint);
            canvas.drawRect(0, 30, WIDTH, 45, paint);
            paint.setColor(Color.WHITE);
            paint.setTextSize(17);
            paint.setFakeBoldText(true);
            canvas.drawText("Discover Home", 15, 29, paint);

            paint.setColor(Color.rgb(13, 58, 87));
            paint.setTextSize(16);
            canvas.drawText("Android portability scaffold", 15, 83, paint);
            paint.setFakeBoldText(false);
            paint.setTextSize(13);
            canvas.drawText("Native descriptor: " + descriptor, 15, 112, paint);
            canvas.drawText("320 x 240 logical card surface", 15, 139, paint);
            canvas.drawText("Sample only; cloud data is not connected.", 15, 166, paint);
            paint.setColor(Color.rgb(191, 91, 36));
            canvas.drawRoundRect(15, 190, 305, 221, 5, 5, paint);
            paint.setColor(Color.WHITE);
            paint.setTextSize(12);
            paint.setFakeBoldText(true);
            canvas.drawText("PORTABILITY BUILD - NOT FOR DEPLOYMENT", 24, 211, paint);
            canvas.restore();
        }
    }
}
