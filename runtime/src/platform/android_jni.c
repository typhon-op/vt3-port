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
    return JNI_VERSION_1_6;
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
