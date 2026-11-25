package local.ios.host;

import android.content.Context;
import android.os.Build;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;

/** One exception log per process launch; activity recreation never truncates it. */
public final class CrashLog {
    private static File file;
    private static boolean initialized, attached;

    public static synchronized void initialize(Context context) {
        if (initialized) return;
        initialized = true;
        File directory = context.getExternalFilesDir(null);
        if (directory == null) directory = context.getFilesDir();
        file = new File(directory, "crash.log");
        try (FileOutputStream output = new FileOutputStream(file, false)) {
            String header = "I2A crash log v1\npackage=" + context.getPackageName()
                + "\npackage_revision=" + BuildProfile.PACKAGE_REVISION
                + "\nguest_binary_sha256=" + BuildProfile.GUEST_BINARY_SHA256
                + "\nvariant=" + (BuildProfile.DEVELOPMENT ? "dev" : "release")
                + "\nstarted_epoch_ms=" + System.currentTimeMillis()
                + "\npid=" + android.os.Process.myPid() + "\nuid=" + android.os.Process.myUid()
                + "\nandroid=" + Build.VERSION.RELEASE + "\nsdk=" + Build.VERSION.SDK_INT
                + "\nfingerprint=" + Build.FINGERPRINT + "\nabis=" + String.join(",", Build.SUPPORTED_ABIS)
                + "\nlog_path=" + file.getAbsolutePath() + "\nnormal_logs=disabled\n\n";
            output.write(header.getBytes(StandardCharsets.UTF_8));
            output.getFD().sync();
        } catch (IOException unavailable) {
            file = null;
        }
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, failure) -> {
            try { record("Uncaught Java exception in " + thread.getName(), failure); }
            finally {
                if (previous != null) previous.uncaughtException(thread, failure);
                else {
                    android.os.Process.killProcess(android.os.Process.myPid());
                    System.exit(1);
                }
            }
        });
    }

    public static synchronized void attach(java.util.function.ToIntFunction<String> nativeAttach) {
        if (!attached && file != null) {
            // Native libraries belong to the payload's ClassLoader in protected
            // builds. The caller supplies a method owned by that same loader.
            int error = nativeAttach.applyAsInt(file.getAbsolutePath());
            if (error == 0) attached = true;
            else record("Native crash recorder unavailable: errno=" + error, null);
        }
    }

    public static void record(String message, Throwable failure) {
        File target = file;
        if (target == null) return;
        try {
            StringBuilder text = new StringBuilder("\nJAVA_EXCEPTION epoch_ms=");
            text.append(System.currentTimeMillis()).append(" tid=").append(android.os.Process.myTid())
                .append(" thread=").append(Thread.currentThread().getName()).append('\n').append(message).append('\n');
            if (failure != null) {
                Set<Throwable> visited = Collections.newSetFromMap(new IdentityHashMap<Throwable, Boolean>());
                appendThrowable(text, failure, "", visited);
            } else {
                for (StackTraceElement frame : Thread.currentThread().getStackTrace()) text.append("    at ").append(frame).append('\n');
            }
            // Each event is a single append, independent of native writes and
            // without a Java monitor that a crashing thread could hold.
            try (FileOutputStream output = new FileOutputStream(target, true)) {
                output.write(text.toString().getBytes(StandardCharsets.UTF_8));
                output.getFD().sync();
            }
        } catch (Throwable unavailable) {
            // Recording must not replace the original failure.
        }
    }

    private static void appendThrowable(StringBuilder text, Throwable failure, String prefix, Set<Throwable> visited) {
        if (!visited.add(failure)) { text.append(prefix).append("[circular cause: ").append(failure.getClass().getName()).append("]\n"); return; }
        text.append(prefix).append(failure.getClass().getName()).append(": ").append(failure.getMessage()).append('\n');
        for (StackTraceElement frame : failure.getStackTrace()) text.append(prefix).append("    at ").append(frame).append('\n');
        for (Throwable suppressed : failure.getSuppressed()) {
            text.append(prefix).append("Suppressed:\n"); appendThrowable(text, suppressed, prefix + "    ", visited);
        }
        if (failure.getCause() != null) { text.append(prefix).append("Caused by:\n"); appendThrowable(text, failure.getCause(), prefix, visited); }
    }
}
