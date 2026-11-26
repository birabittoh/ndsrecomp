// Android Dynamic Performance Framework (APerformanceHint, API 33+) for the
// emulation thread: each frame reports how long its work took against the
// frame interval, so the kernel raises the core's clock (and keeps the thread
// on a big core) before a frame runs late instead of after its load average
// catches up. No-op on other platforms and on older Android.
#pragma once
#include <cstdint>

#if defined(__ANDROID__)
// Opens the session for the calling thread on the first call.
void nds_android_perf_hint_frame(uint64_t work_ns, uint64_t target_ns);
void nds_android_perf_hint_close();
#else
inline void nds_android_perf_hint_frame(uint64_t, uint64_t) {}
inline void nds_android_perf_hint_close() {}
#endif
