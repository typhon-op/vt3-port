/*
 * Runtime logging.
 *
 * One entry is a level, a component name, an optional error code and a formatted message. The
 * producer only formats into a bounded ring and returns: the I/O (logcat plus the app's diagnostic
 * file, through a JNI sink) happens on a single writer thread, so a render or audio thread never
 * waits on a file. When the ring is full the entry is dropped and counted, never blocked on.
 *
 * DEBUG entries are only kept in detailed diagnostic mode (VT3_LOG_DETAIL, set from the app's
 * Diagnostics screen); WARN, ERROR and FATAL are always kept.
 */
#ifndef VT3_LOG_H
#define VT3_LOG_H

typedef enum vt3_log_level {
    VT3_LOG_DEBUG = 0,
    VT3_LOG_INFO,
    VT3_LOG_WARN,
    VT3_LOG_ERROR,
    VT3_LOG_FATAL
} vt3_log_level;

/* The entry point every macro below uses. */
void vt3_log(vt3_log_level level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void vt3_logc(const char *component, vt3_log_level level, int code, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

/* Blocks (bounded) until the writer thread has drained the ring: called at session end and before
 * a fatal entry returns, so the last words of a failing session are not lost with the process. */
void vt3_log_flush(void);
/* Entries dropped because the ring was full. Reported in the diagnostics summary. */
int  vt3_log_dropped(void);
const char *vt3_log_level_name(vt3_log_level level);

/* Detailed mode: DEBUG is kept. Set from the environment at start-up. */
void vt3_log_set_detail(int on);
int  vt3_log_detail(void);

/* A sink replaces the default one (stderr on the host, logcat/JNI on Android). For host tests. */
typedef void (*vt3_log_sink)(vt3_log_level level, const char *component, int code,
                             const char *message, void *user);
void vt3_log_set_sink(vt3_log_sink sink, void *user);

#define VT3_DEBUG(...) vt3_log(VT3_LOG_DEBUG, __VA_ARGS__)
#define VT3_INFO(...)  vt3_log(VT3_LOG_INFO, __VA_ARGS__)
#define VT3_WARN(...)  vt3_log(VT3_LOG_WARN, __VA_ARGS__)
#define VT3_WARN_ONCE(...) do { static int vt3_warned_; if (vt3_warned_++ == 0) VT3_WARN(__VA_ARGS__); } while (0)
#define VT3_ERROR(...) vt3_log(VT3_LOG_ERROR, __VA_ARGS__)
#define VT3_FATAL(...) vt3_log(VT3_LOG_FATAL, __VA_ARGS__)

/* Component-tagged variants: the component names the subsystem in the diagnostic report. */
#define VT3_CDEBUG(comp, ...) vt3_logc((comp), VT3_LOG_DEBUG, 0, __VA_ARGS__)
#define VT3_CINFO(comp, ...)  vt3_logc((comp), VT3_LOG_INFO, 0, __VA_ARGS__)
#define VT3_CWARN(comp, ...)  vt3_logc((comp), VT3_LOG_WARN, 0, __VA_ARGS__)
#define VT3_CERROR(comp, code, ...) vt3_logc((comp), VT3_LOG_ERROR, (code), __VA_ARGS__)
#define VT3_CFATAL(comp, code, ...) vt3_logc((comp), VT3_LOG_FATAL, (code), __VA_ARGS__)

#endif /* VT3_LOG_H */
