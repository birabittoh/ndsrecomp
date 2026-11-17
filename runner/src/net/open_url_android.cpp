// Android's browser handoff for recomp-net's Discord sign-in. recomp-net's own
// rnet_open_url (src/auth/rnet_open_url.c) looks for xdg-open and the like,
// which Android does not have; this one takes its place at link time (the
// library is static, so its version is never pulled in). It asks the activity
// for a Custom Tab (the browser's trimmed-down view over the game) and falls
// back to SDL, which starts the full browser through an intent.
#if defined(__ANDROID__)

#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_system.h>
#include <jni.h>

#include <cstdio>
#include <cstring>

namespace {

// DQ9RecompiledActivity.openCustomTab(String) -> boolean. True when the Tab
// was asked for (it opens on the UI thread, so a failure there is its own
// fallback to a browser intent).
bool OpenCustomTab(const char* url) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return false;
    bool ok = false;
    env->PushLocalFrame(8);
    jclass cls = env->GetObjectClass(activity);
    jmethodID method = cls ? env->GetMethodID(cls, "openCustomTab",
                                              "(Ljava/lang/String;)Z")
                           : nullptr;
    if (method) {
        jstring text = env->NewStringUTF(url);
        ok = env->CallBooleanMethod(activity, method, text) == JNI_TRUE;
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        ok = false;
    }
    env->PopLocalFrame(nullptr);
    return ok;
}

}  // namespace

extern "C" int rnet_open_url(const char* url) {
    if (!url || (std::strncmp(url, "http://", 7) != 0 &&
                 std::strncmp(url, "https://", 8) != 0)) {
        std::fprintf(stderr,
                     "rnet_account: refusing to open a non-http(s) URL\n");
        return 0;
    }
    if (OpenCustomTab(url)) return 1;
    std::fprintf(stderr, "rnet_account: no Custom Tab, using the browser\n");
    if (SDL_OpenURL(url)) return 1;
    std::fprintf(stderr, "rnet_account: could not launch a browser (%s); "
                         "visit:\n  %s\n", SDL_GetError(), url);
    return 0;
}

#endif
