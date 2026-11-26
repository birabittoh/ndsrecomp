// android_perf_hint.cpp -- see android_perf_hint.h.
#include "android_perf_hint.h"

#include <dlfcn.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// The NDK declares these only from API 33; the runner targets 28, so they are
// looked up at run time.
struct Manager;
struct Session;
using GetManager = Manager* (*)();
using CreateSession = Session* (*)(Manager*, const int32_t*, size_t, int64_t);
using UpdateTarget = int (*)(Session*, int64_t);
using ReportActual = int (*)(Session*, int64_t);
using CloseSession = void (*)(Session*);

struct Api {
    CreateSession create = nullptr;
    UpdateTarget update = nullptr;
    ReportActual report = nullptr;
    CloseSession close = nullptr;
    Manager* manager = nullptr;
};

Api load() {
    Api api;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return api;
    const auto get = reinterpret_cast<GetManager>(
        dlsym(lib, "APerformanceHint_getManager"));
    api.create = reinterpret_cast<CreateSession>(
        dlsym(lib, "APerformanceHint_createSession"));
    api.update = reinterpret_cast<UpdateTarget>(
        dlsym(lib, "APerformanceHint_updateTargetWorkDuration"));
    api.report = reinterpret_cast<ReportActual>(
        dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    api.close = reinterpret_cast<CloseSession>(
        dlsym(lib, "APerformanceHint_closeSession"));
    if (get && api.create && api.update && api.report && api.close)
        api.manager = get();
    return api;
}

Api g_api;
Session* g_session = nullptr;
bool g_tried = false;
int64_t g_target_ns = 0;

}  // namespace

void nds_android_perf_hint_frame(uint64_t work_ns, uint64_t target_ns) {
    if (!g_tried) {
        g_tried = true;
        // NDS_ANDROID_PERF_HINT=0 (debug.ndsrecomp.env) leaves it off.
        const char* env = std::getenv("NDS_ANDROID_PERF_HINT");
        if (!env || std::strcmp(env, "0") != 0) g_api = load();
        if (g_api.manager) {
            const int32_t tid = static_cast<int32_t>(gettid());
            g_target_ns = static_cast<int64_t>(target_ns);
            g_session = g_api.create(g_api.manager, &tid, 1, g_target_ns);
        }
        std::fprintf(stderr, "[android] performance hint session: %s\n",
                     g_session ? "on" : "unavailable");
    }
    if (!g_session || work_ns == 0) return;
    if (static_cast<int64_t>(target_ns) != g_target_ns) {
        g_target_ns = static_cast<int64_t>(target_ns);
        g_api.update(g_session, g_target_ns);
    }
    g_api.report(g_session, static_cast<int64_t>(work_ns));
}

void nds_android_perf_hint_close() {
    if (g_session) g_api.close(g_session);
    g_session = nullptr;
    g_tried = false;
}
