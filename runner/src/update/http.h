// update/http.h -- a minimal blocking HTTPS client for the self-updater.
//
// Windows uses WinHTTP (http_win.cpp), Android java.net.HttpURLConnection
// over JNI (http_android.cpp, Bionic ships no libcurl and the platform TLS
// stack is only reachable from Java), and Linux and macOS link libcurl
// (http_curl.cpp) through find_package(CURL REQUIRED), so a build fails fast
// rather than silently shipping without update checks. Meant to be called
// from a worker thread: every call here blocks. HTTPS only: a non-https://
// URL is rejected up front. Never throws; failures surface as a non-empty
// HttpResponse::error or a false return with `error` set.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace nds::net {

// Aborts an in-flight blocking call from another thread. Without it a
// caller that wants to join its worker has to wait out the full connect /
// receive timeout (10-20 s), which on the frontend thread is a visible
// freeze. Cancelling is sticky: once cancelled, later calls handed the same
// token fail at once instead of touching the network.
class CancelToken {
public:
    CancelToken() = default;
    CancelToken(const CancelToken&) = delete;
    CancelToken& operator=(const CancelToken&) = delete;

    // Thread safe, idempotent, and safe with no request in flight.
    void Cancel();
    bool cancelled() const { return cancelled_.load(std::memory_order_acquire); }

    // Backend use only. AttachHandle hands the token ownership of the native
    // handle, so exactly one of Cancel / CloseHandle tears it down under the
    // lock; false if the token was already cancelled (the handle is closed
    // and the request must not run). CloseHandle is the normal release at
    // the end of a request.
    bool AttachHandle(void* handle);
    void CloseHandle();

private:
    std::atomic<bool> cancelled_{false};
    std::mutex mutex_;
    void* handle_ = nullptr;
};

// Called now and then during a transfer with (bytes so far, total bytes; 0
// when the server does not say). May be empty.
using ProgressFn = std::function<void(uint64_t downloaded, uint64_t total)>;

struct HttpResponse {
    long status = 0;
    std::string body;
    // Non-empty on any failure (connect, TLS, timeout, non-https URL, ...);
    // `status` means nothing when this is set.
    std::string error;

    bool ok() const { return error.empty() && status >= 200 && status < 300; }
};

// Blocking HTTPS GET.
HttpResponse HttpGet(std::string_view url, const ProgressFn& progress = {},
                     CancelToken* cancel = nullptr);

// Blocking HTTPS POST with a JSON body ("Content-Type: application/json").
HttpResponse HttpPostJson(std::string_view url, std::string_view json_body,
                          CancelToken* cancel = nullptr);

// Blocking HTTPS download of `url` straight to `dest` (overwritten if it
// exists). False with `error` set on any failure (DNS, TLS, HTTP status >=
// 400, timeout, or a filesystem error writing `dest`); `dest` is then absent
// or partial, and must not be taken as valid.
bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error,
                        CancelToken* cancel = nullptr);

}  // namespace nds::net
