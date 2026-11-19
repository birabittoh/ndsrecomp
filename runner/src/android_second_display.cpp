// android_second_display.cpp -- see android_second_display.h.
//
// Two threads meet here: the UI thread (surface and touch callbacks from the
// activity's Presentation) and the render thread (the frontend's frame loop,
// which owns the swap chain made on the surface). Android requires that a
// surface is no longer drawn to once surfaceDestroyed returns, so a UI thread
// replacing a surface the render thread has picked up waits until the render
// thread says it let go of it, with a timeout so a stalled frame cannot hang
// the UI. Each surface holds its own ANativeWindow reference until then.
#include "android_second_display.h"

#include <SDL3/SDL_system.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <jni.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

namespace {

struct Retired {
    ANativeWindow* window;
    uint64_t generation;  // the generation that replaced it
};

std::mutex g_mutex;
std::condition_variable g_released_cv;
ANativeWindow* g_window = nullptr;
uint64_t g_generation = 0;         // bumped whenever g_window changes
uint64_t g_window_generation = 0;  // the generation g_window arrived with
uint64_t g_seen = 0;               // last generation the render thread took
uint64_t g_released = 0;           // last generation it released up to
bool g_attached = false;           // a render thread is taking surfaces
std::vector<Retired> g_retired;
std::deque<NdsSecondDisplayTouch> g_touches;
constexpr size_t kMaxTouches = 256;
bool g_started = false;

void release_retired_locked(uint64_t up_to) {
    for (auto it = g_retired.begin(); it != g_retired.end();) {
        if (it->generation <= up_to) {
            ANativeWindow_release(it->window);
            it = g_retired.erase(it);
        } else {
            ++it;
        }
    }
}

void JNICALL native_surface(JNIEnv* env, jclass, jobject surface, jint width,
                            jint height) {
    ANativeWindow* window =
        surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    std::unique_lock<std::mutex> lock(g_mutex);
    if (window && window == g_window) {
        // surfaceChanged on the same surface: the swap chain follows the
        // size by itself, and the new reference is one too many.
        ANativeWindow_release(window);
        std::fprintf(stderr, "[second-display] surface resized to %dx%d\n",
                     int(width), int(height));
        return;
    }
    ANativeWindow* const old = g_window;
    const uint64_t old_generation = g_window_generation;
    g_window = window;
    g_window_generation = ++g_generation;
    std::fprintf(stderr, "[second-display] surface %s (%dx%d, generation %llu)\n",
                 window ? "ready" : "gone", int(width), int(height),
                 static_cast<unsigned long long>(g_generation));
    if (!old) return;
    g_retired.push_back({old, g_generation});
    if (!g_attached || g_seen < old_generation) {
        // The render thread never took it: nothing to wait for.
        release_retired_locked(g_generation);
        return;
    }
    const uint64_t wanted = g_generation;
    if (!g_released_cv.wait_for(lock, std::chrono::seconds(1), [&] {
            return !g_attached || g_released >= wanted;
        })) {
        std::fprintf(stderr, "[second-display] render thread did not let go of "
                             "the old surface within 1 s\n");
        return;  // released on the render thread's next frame
    }
    release_retired_locked(g_attached ? g_released : g_generation);
}

void JNICALL native_touch(JNIEnv*, jclass, jint action, jfloat x, jfloat y,
                          jint width, jint height) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_touches.size() >= kMaxTouches) g_touches.pop_front();
    NdsSecondDisplayTouch touch;
    touch.action = int(action);
    touch.x = x;
    touch.y = y;
    touch.width = int(width);
    touch.height = int(height);
    g_touches.push_back(touch);
}

bool clear_exception(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

// Calls a no-argument void method of the activity; false if it has none.
bool call_activity(const char* name) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = env->GetMethodID(cls, name, "()V");
    bool ok = false;
    if (!clear_exception(env) && method) {
        env->CallVoidMethod(activity, method);
        ok = !clear_exception(env);
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return ok;
}

}  // namespace

bool nds_second_display_start() {
    if (g_started) return true;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) {
        std::fprintf(stderr, "[second-display] no JNI environment or activity\n");
        return false;
    }
    static const JNINativeMethod methods[] = {
        {"nativeSecondDisplaySurface", "(Landroid/view/Surface;II)V",
         reinterpret_cast<void*>(native_surface)},
        {"nativeSecondDisplayTouch", "(IFFII)V",
         reinterpret_cast<void*>(native_touch)},
    };
    jclass cls = env->GetObjectClass(activity);
    const jint registered = env->RegisterNatives(cls, methods, 2);
    const bool failed = clear_exception(env) || registered != JNI_OK;
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    if (failed) {
        std::fprintf(stderr, "[second-display] the activity has no second-display "
                             "natives; single display only\n");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_attached = true;
    }
    if (!call_activity("startSecondDisplay")) {
        std::fprintf(stderr, "[second-display] the activity has no "
                             "startSecondDisplay(); single display only\n");
        std::lock_guard<std::mutex> lock(g_mutex);
        g_attached = false;
        return false;
    }
    g_started = true;
    return true;
}

void nds_second_display_stop() {
    if (!g_started) return;
    g_started = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_attached = false;
        g_seen = 0;
        release_retired_locked(UINT64_MAX);
        g_touches.clear();
    }
    g_released_cv.notify_all();
    call_activity("stopSecondDisplay");
}

ANativeWindow* nds_second_display_surface(uint64_t* generation) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_seen = g_generation;
    *generation = g_generation;
    return g_window;
}

void nds_second_display_release(uint64_t generation) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (generation > g_released) g_released = generation;
        release_retired_locked(g_released);
    }
    g_released_cv.notify_all();
}

bool nds_second_display_pop_touch(NdsSecondDisplayTouch* touch) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_touches.empty()) return false;
    *touch = g_touches.front();
    g_touches.pop_front();
    return true;
}
