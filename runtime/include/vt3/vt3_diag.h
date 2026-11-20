/*
 * Diagnostics: the numbers and the last known state that the app's diagnostic report needs, kept
 * without touching a file from a frame or an audio callback.
 *
 * The renderer feeds frame intervals into a millisecond histogram (cheap atomics, one per frame),
 * the platform layer records lifecycle transitions, the audio layer samples its stream state, and
 * the Vulkan init reports the GPU. vt3_diag_tick(), called from the platform pump, writes a
 * key=value snapshot to <dir>/diag_metrics.txt at most once a second, atomically (write + rename).
 * The Java report reads that file; nothing here blocks gameplay.
 *
 * A native crash cannot be logged through the async path, because the process is about to go
 * away: a signal handler writes a fixed line straight to <dir>/native_crash.txt with write(2),
 * so the next launch can say what happened. That is the honest limit of what can be preserved.
 */
#ifndef VT3_DIAG_H
#define VT3_DIAG_H

#include <stdint.h>

typedef struct vt3_audio vt3_audio;

/* dir is the app's data root (the report reads the snapshot from there); NULL disables the file. */
void vt3_diag_start(const char *dir, const char *version);
void vt3_diag_stop(void);
int  vt3_diag_active(void);

/* Rate-limited snapshot writer, called from the platform pump. */
void vt3_diag_tick(void);

/* Frame pacing: dt_ns is the interval since the previous presented frame. */
void vt3_diag_frame(uint64_t dt_ns);
void vt3_diag_render_size(int w, int h);
void vt3_diag_window_size(int w, int h);
void vt3_diag_gpu(uint32_t vendor_id, const char *name, uint32_t api_version, const char *extensions);
void vt3_diag_vulkan_error(void);

/* Named event: increments count_<name> and becomes lifecycle_last. */
void vt3_diag_event(const char *name);
void vt3_diag_counter(const char *name, int delta);
/* The open output (if any) whose stream state is sampled into each snapshot. */
void vt3_diag_bind_audio(vt3_audio *a);
void vt3_diag_audio(const vt3_audio *a);

/* Called by the log writer (Android) to hand an entry to the app's DiagLog. Returns 0 when the
 * app class is not reachable (then the logger falls back to logcat). */
int vt3_diag_jni_write(int level, const char *component, int code, const char *message);

#endif /* VT3_DIAG_H */
