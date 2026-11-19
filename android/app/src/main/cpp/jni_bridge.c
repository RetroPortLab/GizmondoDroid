/* GizmondoDroid JNI bridge: runs the HLE emulator on its own thread at 60 Hz and copies each
 * 320x240 frame to the Surface, optionally upscaled with a scanline or LCD-grid filter.
 * Input from the UI thread is latched here and applied by the emulator thread at the start
 * of each frame, so the core is only ever touched from one thread. */
#include <dlfcn.h>
#include <jni.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <aaudio/AAudio.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gizmondo.h"

#define TAG "GizmondoDroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define FN(n) Java_com_retroportlab_gizmondodroid_NativeLib_##n

static pthread_mutex_t win_lock = PTHREAD_MUTEX_INITIALIZER;
static ANativeWindow *window;
static int win_w, win_h;
static pthread_t thread;
static volatile int running, paused, filter;
static volatile float fps;
static char last_error[512];
static AAudioStream *audio_stream;

static volatile int touch_x, touch_y, touch_down;
static volatile uint32_t keymask;          /* bit i set = key_vk[i] held */
static const int key_vk[8] = { GIZ_VK_UP, GIZ_VK_DOWN, GIZ_VK_LEFT, GIZ_VK_RIGHT, GIZ_VK_A, GIZ_VK_B, GIZ_VK_C, GIZ_VK_START };

static int64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}

static void log_sink(const char *line) { __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line); }

/* ---------------------------------------------------------------- video */

static inline uint32_t rgbx(uint16_t p) {
    uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
    r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static inline uint32_t dim(uint32_t c, int num) {
    uint32_t r = (c & 0xFF) * num >> 8, g = ((c >> 8) & 0xFF) * num >> 8, b = ((c >> 16) & 0xFF) * num >> 8;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

static uint32_t lut565[65536];
static int lut_ready;

static void present(void) {
    if (!lut_ready) { for (uint32_t i = 0; i < 65536; i++) lut565[i] = rgbx((uint16_t)i); lut_ready = 1; }
    const uint16_t *fb = giz_screen();
    pthread_mutex_lock(&win_lock);
    if (window && fb) {
        int scale = filter ? 3 : 1;
        int w = GIZ_W * scale, h = GIZ_H * scale;
        if (w != win_w || h != win_h) {
            ANativeWindow_setBuffersGeometry(window, w, h, WINDOW_FORMAT_RGBX_8888);
            win_w = w; win_h = h;
        }
        ANativeWindow_Buffer b;
        if (ANativeWindow_lock(window, &b, NULL) == 0) {
            if (b.width >= w && b.height >= h) {
                for (int y = 0; y < GIZ_H; y++) {
                    const uint16_t *src = &fb[y * GIZ_W];
                    if (scale == 1) {
                        uint32_t *dst = (uint32_t *)b.bits + (size_t)y * b.stride;
                        for (int x = 0; x < GIZ_W; x++) dst[x] = lut565[src[x]];
                        continue;
                    }
                    for (int sy = 0; sy < 3; sy++) {
                        uint32_t *dst = (uint32_t *)b.bits + (size_t)(y * 3 + sy) * b.stride;
                        int row_dim = 256;
                        if (filter == 2 && sy == 2) row_dim = 140;
                        if (filter == 3 && sy == 2) row_dim = 190;
                        for (int x = 0; x < GIZ_W; x++) {
                            uint32_t c = lut565[src[x]];
                            uint32_t c0 = row_dim == 256 ? c : dim(c, row_dim);
                            uint32_t c2 = filter == 3 ? dim(c, row_dim * 190 >> 8) : c0;
                            dst[x * 3] = c0; dst[x * 3 + 1] = c0; dst[x * 3 + 2] = c2;
                        }
                    }
                }
            }
            ANativeWindow_unlockAndPost(window);
        }
    }
    pthread_mutex_unlock(&win_lock);
}

/* ---------------------------------------------------------------- emulator thread */

static void apply_input(void) {
    static int last_down, last_x, last_y;
    static uint32_t last_keys;
    int d = touch_down, x = touch_x, y = touch_y;
    if (d != last_down || (d && (x != last_x || y != last_y))) {
        giz_touch(x, y, d);
        last_down = d; last_x = x; last_y = y;
    }
    uint32_t k = keymask, diff = k ^ last_keys;
    for (int i = 0; i < 8; i++) if (diff & (1u << i)) giz_key(key_vk[i], (k >> i) & 1);
    last_keys = k;
}

static void *run(void *arg) {
    (void)arg;
    const int64_t frame_ns = 1000000000LL / 60;
    int64_t next = now_ns(), fps_t0 = next;
    int fps_frames = 0;
    while (running) {
        if (paused) {
            usleep(10000);
            next = fps_t0 = now_ns(); fps_frames = 0;
            continue;
        }
        apply_input();
        giz_frame();
        present();
        if (!giz_running()) { LOGI("program exited (%d)", giz_exit_code()); paused = 1; }
        fps_frames++;
        int64_t t = now_ns();
        if (t - fps_t0 >= 1000000000LL) { fps = fps_frames * 1e9f / (float)(t - fps_t0); fps_t0 = t; fps_frames = 0; }
        next += frame_ns;
        giz_set_frameskip(t > next);
        if (next > t) {
            struct timespec ts = { (time_t)(next / 1000000000LL), (long)(next % 1000000000LL) };
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
        } else if (t - next > frame_ns * 4) {
            next = t;
        }
    }
    return NULL;
}

static void stop_thread(void) {
    if (!running) return;
    running = 0;
    pthread_join(thread, NULL);
}

/* ---------------------------------------------------------------- audio */

static aaudio_data_callback_result_t audio_cb(AAudioStream *st, void *user, void *data, int32_t frames) {
    (void)st; (void)user;
    int16_t *out = data;
    memset(out, 0, (size_t)frames * 4);
    if (!paused) giz_audio_read(out, frames);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void audio_open(void) {
    if (audio_stream) return;
    AAudioStreamBuilder *b;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 44100);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_NONE);
    {
        void (*set_usage)(AAudioStreamBuilder *, aaudio_usage_t) = (void (*)(AAudioStreamBuilder *, aaudio_usage_t))dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
        if (set_usage) set_usage(b, AAUDIO_USAGE_GAME);
    }
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &audio_stream) == AAUDIO_OK) {
        giz_set_audio_rate((uint32_t)AAudioStream_getSampleRate(audio_stream));
        AAudioStream_requestStart(audio_stream);
    } else audio_stream = NULL;
    AAudioStreamBuilder_delete(b);
}

static void audio_close(void) {
    if (!audio_stream) return;
    AAudioStream_requestStop(audio_stream);
    AAudioStream_close(audio_stream);
    audio_stream = NULL;
}

/* ---------------------------------------------------------------- JNI */

static void jstr(JNIEnv *env, jstring s, char *out, size_t n) {
    out[0] = 0;
    if (!s) return;
    const char *p = (*env)->GetStringUTFChars(env, s, NULL);
    snprintf(out, n, "%s", p);
    (*env)->ReleaseStringUTFChars(env, s, p);
}

JNIEXPORT jboolean JNICALL FN(launch)(JNIEnv *env, jclass c, jstring exe, jstring sd, jstring sys, jstring args) {
    (void)c;
    stop_thread();
    audio_close();
    char e[1024], s[1024], y[1024], a[1024];
    jstr(env, exe, e, sizeof e); jstr(env, sd, s, sizeof s); jstr(env, sys, y, sizeof y); jstr(env, args, a, sizeof a);
    last_error[0] = 0;
    giz_set_log(log_sink);
    if (giz_launch(e, s, y, a, last_error, sizeof last_error) != 0) {
        if (!last_error[0]) snprintf(last_error, sizeof last_error, "Couldn't start that program");
        return JNI_FALSE;
    }
    keymask = 0; touch_down = 0;
    return JNI_TRUE;
}

JNIEXPORT jstring JNICALL FN(lastError)(JNIEnv *env, jclass c) { (void)c; return (*env)->NewStringUTF(env, last_error); }

JNIEXPORT void JNICALL FN(setSurface)(JNIEnv *env, jclass c, jobject surface) {
    (void)c;
    pthread_mutex_lock(&win_lock);
    if (window) { ANativeWindow_release(window); window = NULL; }
    win_w = win_h = 0;
    if (surface) window = ANativeWindow_fromSurface(env, surface);
    pthread_mutex_unlock(&win_lock);
    if (window && paused && giz_running()) present();
}

JNIEXPORT void JNICALL FN(start)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    audio_open();
    if (running) return;
    paused = 0;
    running = 1;
    pthread_create(&thread, NULL, run, NULL);
}

JNIEXPORT void JNICALL FN(stop)(JNIEnv *env, jclass c) {
    (void)env; (void)c;
    stop_thread();
    audio_close();
    giz_stop();
}

JNIEXPORT void JNICALL FN(setPaused)(JNIEnv *env, jclass c, jboolean p) { (void)env; (void)c; paused = p; }
JNIEXPORT void JNICALL FN(setFilter)(JNIEnv *env, jclass c, jint f) { (void)env; (void)c; filter = f; }
JNIEXPORT void JNICALL FN(setOption)(JNIEnv *env, jclass c, jint id, jint v) { (void)env; (void)c; giz_set_option(id, v); }
JNIEXPORT void JNICALL FN(setAlias)(JNIEnv *env, jclass c, jint b, jint v1, jint v2) { (void)env; (void)c; giz_set_alias(b, v1, v2); }
JNIEXPORT jfloat JNICALL FN(getFps)(JNIEnv *env, jclass c) { (void)env; (void)c; return fps; }
JNIEXPORT jboolean JNICALL FN(isRunning)(JNIEnv *env, jclass c) { (void)env; (void)c; return giz_running() ? JNI_TRUE : JNI_FALSE; }
JNIEXPORT void JNICALL FN(setTouch)(JNIEnv *env, jclass c, jint x, jint y, jboolean down) {
    (void)env; (void)c; touch_x = x; touch_y = y; touch_down = down;
}
JNIEXPORT void JNICALL FN(setKeys)(JNIEnv *env, jclass c, jint mask) { (void)env; (void)c; keymask = (uint32_t)mask; }
