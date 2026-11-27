/*
 * The port's own in-game UI: the quick menu (pause panel) and the performance overlay.
 *
 * Both are drawn with the renderer's 2D path, after the game's frame and before it is presented,
 * so they sit on top of the game without touching its drawing. The quick menu blocks the guest
 * thread while it is open (the frame loop simply does not advance), which is what makes it a pause
 * rather than a HUD.
 */
#ifndef VT3_MENU_H
#define VT3_MENU_H

/* Reads the markers/environment once: VT3_PERF_OVERLAY, VT3_PERF_OVERLAY_FULL, VT3_QUICK_MENU,
 * VT3_SETTINGS_DIR. Safe to call more than once; later calls only refresh the overlay flags. */
void vt3_menu_init(void);

/* One presented frame: updates the frame-rate estimate the overlay shows. */
void vt3_menu_tick(void);

/* Draws the performance overlay over the current frame when it is enabled. */
void vt3_menu_overlay(int drawable_w, int drawable_h);

/*
 * If the quick menu is open (or was asked for this frame), draws it and blocks the calling thread
 * until the player resumes. Returns 1 when it ran, 0 when there was nothing to do.
 * The caller presents and swaps its own frame afterwards.
 */
int vt3_menu_handle(int drawable_w, int drawable_h);

/* True while the quick menu is on screen. */
int vt3_menu_open(void);

/* Opens or closes the quick menu from outside (used by the overlay and by tests). */
void vt3_menu_set_open(int open);

/* Opens the Java settings screen from the native thread; implemented in android_jni.c. */
void vt3_android_open_settings(void);
/* The Touchscreen Controls editor, from the quick menu's Touchscreen row. */
void vt3_android_open_touch_settings(void);
/* 1 while a Java settings/editor/preview screen is up, so the menu can hold the pause. */
int  vt3_android_screen_open(void);

#endif /* VT3_MENU_H */
