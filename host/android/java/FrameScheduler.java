package local.ios.host;

import android.app.Activity;
import android.hardware.display.DisplayManager;
import android.os.Handler;
import android.os.Looper;
import android.view.Choreographer;
import java.util.function.Consumer;

final class FrameScheduler implements Choreographer.FrameCallback, DisplayManager.DisplayListener {
    private final Activity activity;
    private final Consumer<String> failure;
    private final DisplayManager displays;
    private final Handler mainHandler = new Handler(Looper.getMainLooper());
    private boolean enabled, foreground = true, pending,closed;
    private double refreshRate;
    private static native String frame(long timestamp, double refreshRate);

    FrameScheduler(Activity activity, Consumer<String> failure) {
        this.activity = activity;
        this.failure = failure;
        displays = activity.getSystemService(DisplayManager.class);
        refreshDisplay();
        if (displays != null) displays.registerDisplayListener(this, mainHandler);
    }

    void active(boolean value) { mainHandler.post(() -> { enabled = value; update(); }); }
    void foreground(boolean value) {
        if (Looper.myLooper() == Looper.getMainLooper()) { foreground = value; if(value)refreshDisplay(); update(); }
        else mainHandler.post(() -> { foreground = value; if(value)refreshDisplay(); update(); });
    }

    void refreshDisplay() { if(!closed)refreshRate=activity.getWindowManager().getDefaultDisplay().getRefreshRate(); }
    @Override public void onDisplayAdded(int id) { refreshDisplay(); }
    @Override public void onDisplayRemoved(int id) { refreshDisplay(); }
    @Override public void onDisplayChanged(int id) { refreshDisplay(); }

    private void update() {
        if (!closed && enabled && foreground && !pending) {
            pending = true;
            Choreographer.getInstance().postFrameCallback(this);
        } else if ((closed || !enabled || !foreground) && pending) {
            Choreographer.getInstance().removeFrameCallback(this);
            pending = false;
        }
    }

    @Override public void doFrame(long timestamp) {
        pending = false;
        if (closed || !enabled || !foreground) return;
        String error = frame(timestamp, refreshRate);
        if (error != null) { enabled = false; failure.accept(error); }
        update();
    }
    void close(){closed=true;enabled=false;update();if(displays!=null)displays.unregisterDisplayListener(this);mainHandler.removeCallbacksAndMessages(null);}
}
