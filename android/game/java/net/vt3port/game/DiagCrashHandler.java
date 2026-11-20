package net.vt3port.game;

/** Records an uncaught Java exception, then lets the previous handler (the platform's) finish the job. */
final class DiagCrashHandler implements Thread.UncaughtExceptionHandler {
    private final Thread.UncaughtExceptionHandler previous;

    DiagCrashHandler(Thread.UncaughtExceptionHandler previous) {
        this.previous = previous;
    }

    @Override
    public void uncaughtException(Thread thread, Throwable error) {
        try {
            DiagLog.recordCrash(thread, error);
            DiagLog.f("crash", 0, "uncaught exception in " + thread.getName(), error);
            DiagLog.flush(2000);
        } catch (Throwable ignored) {
            /* never mask the original failure with a logging one */
        }
        if (previous != null) previous.uncaughtException(thread, error);
    }
}
