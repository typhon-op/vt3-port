/*
 * The bridge from the native log writer to the app's diagnostic log.
 *
 * Library.loadLibrary runs JNI_OnLoad on a Java thread, which is the only place FindClass can see
 * the app's own classes, so the DiagLog class and its static nativeWrite method are cached here.
 * The writer thread later attaches once and calls nativeWrite; if the app has not initialised its
 * log yet (or the class is somehow missing) the call is a no-op and the logger keeps logcat.
 */
#include "vt3/vt3_diag.h"

#include <jni.h>
#include <pthread.h>
#include <string.h>

static JavaVM *g_vm;
static jclass g_class;
static jmethodID g_write;
static jclass g_bridge;
static jmethodID g_open_settings;
static jmethodID g_open_touch;
static jmethodID g_screen_open;
static jmethodID g_open_menu;

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    g_vm = vm;
    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) return JNI_VERSION_1_6;
    /* Locate the class through the native library's own class loader, which is the app's. */
    jclass local = (*env)->FindClass(env, "net/vt3port/game/DiagLog");
    if (!local) {
        (*env)->ExceptionClear(env);
        return JNI_VERSION_1_6;
    }
    g_class = (jclass)(*env)->NewGlobalRef(env, local);
    (*env)->DeleteLocalRef(env, local);
    if (g_class)
        g_write = (*env)->GetStaticMethodID(env, g_class, "nativeWrite",
                                            "(ILjava/lang/String;ILjava/lang/String;)V");
    if (!g_write && g_class) (*env)->ExceptionClear(env);

    /* The quick menu's Settings row opens the Java settings screen through this class: an
     * Application context can start an activity, so no Activity reference has to be carried
     * across the JNI boundary. */
    jclass bridge = (*env)->FindClass(env, "net/vt3port/game/Bridge");
    if (bridge) {
        g_bridge = (jclass)(*env)->NewGlobalRef(env, bridge);
        (*env)->DeleteLocalRef(env, bridge);
        if (g_bridge)
            g_open_settings = (*env)->GetStaticMethodID(env, g_bridge, "openSettings", "()V");
        if (!g_open_settings) (*env)->ExceptionClear(env);
        if (g_bridge)
            g_open_touch = (*env)->GetStaticMethodID(env, g_bridge, "openTouchSettings", "()V");
        if (!g_open_touch) (*env)->ExceptionClear(env);
        if (g_bridge)
            g_screen_open = (*env)->GetStaticMethodID(env, g_bridge, "screenOpen", "()Z");
        if (!g_screen_open) (*env)->ExceptionClear(env);
        if (g_bridge)
            g_open_menu = (*env)->GetStaticMethodID(env, g_bridge, "openMainMenu", "()V");
        if (!g_open_menu) (*env)->ExceptionClear(env);
    } else {
        (*env)->ExceptionClear(env);
    }
    return JNI_VERSION_1_6;
}

/* Shows the port's main menu. Used when the guest (or the pause menu) exits a match. */
void vt3_android_open_main_menu(void)
{
    if (!g_vm || !g_bridge || !g_open_menu) return;
    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK || !env) return;
    } else if (rc != JNI_OK || !env) {
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_bridge, g_open_menu);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

/* 1 while one of the app's own screens (settings, the touch editor, the preview) is started. The
 * quick menu keeps the guest blocked while this is true, so the match does not run behind them. */
int vt3_android_screen_open(void)
{
    if (!g_vm || !g_bridge || !g_screen_open) return 0;
    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK || !env) return 0;
    } else if (rc != JNI_OK || !env) {
        return 0;
    }
    jboolean open = (*env)->CallStaticBooleanMethod(env, g_bridge, g_screen_open);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); return 0; }
    return open == JNI_TRUE;
}

void vt3_android_open_touch_settings(void)
{
    if (!g_vm || !g_bridge || !g_open_touch) return;
    JNIEnv *env = NULL;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK || !env) return;
    (*env)->CallStaticVoidMethod(env, g_bridge, g_open_touch);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    (*g_vm)->DetachCurrentThread(g_vm);
}

void vt3_android_open_settings(void)
{
    if (!g_vm || !g_bridge || !g_open_settings) return;
    JNIEnv *env = NULL;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return;
    } else if (rc != JNI_OK || !env) {
        return;
    }
    (*env)->CallStaticVoidMethod(env, g_bridge, g_open_settings);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

int vt3_diag_jni_write(int level, const char *component, int code, const char *message)
{
    if (!g_vm || !g_class || !g_write) return 0;
    JNIEnv *env = NULL;
    int attached = 0;
    jint rc = (*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6);
    if (rc == JNI_EDETACHED) {
        if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return 0;
        attached = 1;
    } else if (rc != JNI_OK || !env) {
        return 0;
    }
    jstring comp = (*env)->NewStringUTF(env, component ? component : "native");
    jstring msg = (*env)->NewStringUTF(env, message ? message : "");
    if (comp && msg) (*env)->CallStaticVoidMethod(env, g_class, g_write, (jint)level, comp, (jint)code, msg);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    if (comp) (*env)->DeleteLocalRef(env, comp);
    if (msg) (*env)->DeleteLocalRef(env, msg);
    /* Stay attached: the writer thread logs again in a moment, and detaching would cost more. */
    (void)attached;
    return 1;
}
