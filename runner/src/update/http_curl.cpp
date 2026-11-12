// update/http_curl.cpp -- the libcurl backend of update/http.h (Linux and
// macOS).
#include "update/http.h"

#include <curl/curl.h>

#include <fstream>
#include <mutex>

namespace nds::net {

// libcurl easy handles may only be touched by the thread running
// curl_easy_perform, so cancelling here is only the flag: the transfer's
// progress callback polls it and aborts (see ProgressCallback).
void CancelToken::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true, std::memory_order_release);
}

bool CancelToken::AttachHandle(void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_ = handle;
    return !cancelled_.load(std::memory_order_acquire);
}

void CancelToken::CloseHandle() {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_ = nullptr;
}

namespace {

constexpr long kConnectTimeoutSec = 10;
constexpr long kTotalTimeoutSec = 60;
// A release is a few hundred MB, too big for a total cap on a slow line:
// a download has none, and gives up instead once it stalls.
constexpr long kDownloadTimeoutSec = 0;
constexpr long kStallSeconds = 60;
constexpr long kMaxRedirects = 5;
constexpr const char* kUserAgent = "ndsrecomp-http/1.0";

// curl_global_init / cleanup are process wide and not safe to call
// concurrently; every request gets a fresh easy handle, but the global init
// only has to happen once.
void EnsureGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

size_t WriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// A short count makes curl fail the transfer with CURLE_WRITE_ERROR.
size_t WriteFileCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::ofstream*>(userdata);
    out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    return *out ? size * nmemb : 0;
}

struct ProgressState {
    const ProgressFn* fn = nullptr;
    CancelToken* cancel = nullptr;
};

int ProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                     curl_off_t, curl_off_t) {
    auto* state = static_cast<ProgressState*>(clientp);
    if (state->fn && *state->fn) {
        (*state->fn)(static_cast<uint64_t>(dlnow),
                     static_cast<uint64_t>(dltotal));
    }
    // Non-zero aborts the transfer with CURLE_ABORTED_BY_CALLBACK. curl runs
    // this about once a second, connect phase included, which keeps a
    // cancelled request from sitting out the connect timeout.
    return (state->cancel && state->cancel->cancelled()) ? 1 : 0;
}

// curl only calls ProgressCallback with progress meters on, so a request
// that can be cancelled needs them even when nobody wants the progress.
void InstallProgressAndCancel(CURL* curl, ProgressState* state) {
    if (!state->cancel && !(state->fn && *state->fn)) return;
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, state);
}

bool IsHttpsUrl(std::string_view url) { return url.substr(0, 8) == "https://"; }

// The options every request shares.
void SetCommonOptions(CURL* curl, const std::string& url, long timeout_sec) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, kMaxRedirects);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
}

}  // namespace

HttpResponse HttpGet(std::string_view url, const ProgressFn& progress,
                     CancelToken* cancel) {
    HttpResponse response;
    if (cancel && cancel->cancelled()) {
        response.error = "cancelled";
        return response;
    }
    if (!IsHttpsUrl(url)) {
        response.error = "only https:// URLs are supported";
        return response;
    }
    EnsureGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "curl_easy_init failed";
        return response;
    }

    const std::string url_str(url);
    SetCommonOptions(curl, url_str, kTotalTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    ProgressState state{&progress, cancel};
    InstallProgressAndCancel(curl, &state);

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        response.error = curl_easy_strerror(result);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = status;
    }
    curl_easy_cleanup(curl);
    return response;
}

HttpResponse HttpPostJson(std::string_view url, std::string_view json_body,
                          CancelToken* cancel) {
    HttpResponse response;
    if (cancel && cancel->cancelled()) {
        response.error = "cancelled";
        return response;
    }
    if (!IsHttpsUrl(url)) {
        response.error = "only https:// URLs are supported";
        return response;
    }
    EnsureGlobalInit();

    CURL* curl = curl_easy_init();
    if (!curl) {
        response.error = "curl_easy_init failed";
        return response;
    }

    const std::string url_str(url);
    curl_slist* headers =
        curl_slist_append(nullptr, "Content-Type: application/json");
    SetCommonOptions(curl, url_str, kTotalTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(json_body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

    ProgressState state{nullptr, cancel};
    InstallProgressAndCancel(curl, &state);

    const CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        response.error = curl_easy_strerror(result);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = status;
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error,
                        CancelToken* cancel) {
    if (cancel && cancel->cancelled()) {
        error = "cancelled";
        return false;
    }
    if (!IsHttpsUrl(url)) {
        error = "only https:// URLs are supported";
        return false;
    }
    EnsureGlobalInit();

    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open destination file for writing";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }

    const std::string url_str(url);
    SetCommonOptions(curl, url_str, kDownloadTimeoutSec);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, kStallSeconds);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out);

    ProgressState state{&progress, cancel};
    InstallProgressAndCancel(curl, &state);

    const CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    out.close();

    if (result != CURLE_OK) {
        error = curl_easy_strerror(result);
        return false;
    }
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

}  // namespace nds::net
