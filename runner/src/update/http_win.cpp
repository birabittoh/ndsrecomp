// update/http_win.cpp -- the WinHTTP backend of update/http.h.
#include "update/http.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace nds::net {

// Closing the request handle makes whichever WinHTTP call is blocked on it
// (send, receive, read) fail at once, which is the only way out of a
// connect that would otherwise run the full timeout.
void CancelToken::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true, std::memory_order_release);
    if (handle_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
        handle_ = nullptr;
    }
}

bool CancelToken::AttachHandle(void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (cancelled_.load(std::memory_order_acquire)) {
        WinHttpCloseHandle(static_cast<HINTERNET>(handle));
        return false;
    }
    handle_ = handle;
    return true;
}

void CancelToken::CloseHandle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) {
        WinHttpCloseHandle(static_cast<HINTERNET>(handle_));
        handle_ = nullptr;
    }
}

namespace {

constexpr DWORD kConnectTimeoutMs = 10000;
constexpr DWORD kResolveTimeoutMs = 10000;
constexpr DWORD kSendTimeoutMs = 10000;
constexpr DWORD kReceiveTimeoutMs = 20000;

struct ParsedUrl {
    bool ok = false;
    std::wstring host;
    std::wstring path;  // with the query
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
};

std::wstring Widen(std::string_view s) {
    if (s.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                        static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        out.data(), len);
    return out;
}

ParsedUrl ParseHttpsUrl(std::string_view url) {
    ParsedUrl result;
    if (url.substr(0, 8) != "https://") return result;
    const std::wstring wide = Widen(url);

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    wchar_t host_buf[256]{};
    wchar_t path_buf[4096]{};
    components.lpszHostName = host_buf;
    components.dwHostNameLength = static_cast<DWORD>(std::size(host_buf));
    components.lpszUrlPath = path_buf;
    components.dwUrlPathLength = static_cast<DWORD>(std::size(path_buf));
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(wide.c_str(), static_cast<DWORD>(wide.size()), 0,
                         &components)) {
        return result;
    }
    result.host = host_buf;
    result.path = path_buf;
    result.port = components.nPort ? components.nPort
                                   : INTERNET_DEFAULT_HTTPS_PORT;
    result.ok = true;
    return result;
}

class Session {
public:
    Session() {
        handle_ = WinHttpOpen(L"ndsrecomp-http/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (handle_) {
            WinHttpSetTimeouts(handle_, kResolveTimeoutMs, kConnectTimeoutMs,
                               kSendTimeoutMs, kReceiveTimeoutMs);
        }
    }
    ~Session() {
        if (handle_) WinHttpCloseHandle(handle_);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    HINTERNET get() const { return handle_; }

private:
    HINTERNET handle_ = nullptr;
};

// One HTTPS request. `method` is L"GET" or L"POST"; `body` is only sent for
// POST. Streams the response body through `on_data` (0 or more chunks) and
// returns the final status or error.
HttpResponse DoRequest(std::string_view url, const wchar_t* method,
                       std::string_view body, const char* content_type,
                       const std::function<void(const uint8_t*, size_t)>& on_data,
                       const ProgressFn& progress, CancelToken* cancel) {
    HttpResponse response;
    if (cancel && cancel->cancelled()) {
        response.error = "cancelled";
        return response;
    }
    const ParsedUrl parsed = ParseHttpsUrl(url);
    if (!parsed.ok) {
        response.error = "only https:// URLs are supported";
        return response;
    }

    static Session session;
    if (!session.get()) {
        response.error = "WinHttpOpen failed";
        return response;
    }

    HINTERNET connect =
        WinHttpConnect(session.get(), parsed.host.c_str(), parsed.port, 0);
    if (!connect) {
        response.error = "WinHttpConnect failed";
        return response;
    }

    HINTERNET request = WinHttpOpenRequest(
        connect, method, parsed.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        response.error = "WinHttpOpenRequest failed";
        return response;
    }

    // While attached the token owns `request`, so exactly one of the two
    // closes it and a Cancel() landing mid-request aborts the blocked call.
    if (cancel && !cancel->AttachHandle(request)) {
        WinHttpCloseHandle(connect);
        response.error = "cancelled";
        return response;
    }
    auto close_request = [&] {
        if (cancel) {
            cancel->CloseHandle();
        } else {
            WinHttpCloseHandle(request);
        }
    };

    std::wstring headers;
    if (content_type) {
        headers = L"Content-Type: ";
        headers += Widen(content_type);
        headers += L"\r\n";
    }

    BOOL sent = WinHttpSendRequest(
        request,
        headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
        headers.empty() ? 0 : static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0);
    if (sent) sent = WinHttpReceiveResponse(request, nullptr);
    if (!sent) {
        response.error =
            (cancel && cancel->cancelled())
                ? "cancelled"
                : "request failed (network/TLS error, code " +
                      std::to_string(GetLastError()) + ")";
        close_request();
        WinHttpCloseHandle(connect);
        return response;
    }

    DWORD status_code = 0;
    DWORD status_size = sizeof(status_code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_STATUS_CODE,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
                        WINHTTP_NO_HEADER_INDEX);
    response.status = static_cast<long>(status_code);

    DWORD content_length = 0;
    DWORD length_size = sizeof(content_length);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_FLAG_NUMBER | WINHTTP_QUERY_CONTENT_LENGTH,
                        WINHTTP_HEADER_NAME_BY_INDEX, &content_length,
                        &length_size, WINHTTP_NO_HEADER_INDEX);

    uint64_t downloaded = 0;
    std::vector<uint8_t> buffer(65536);
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
            break;
        if (buffer.size() < available) buffer.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &read) ||
            read == 0) {
            break;
        }
        downloaded += read;
        if (on_data) on_data(buffer.data(), read);
        if (progress) progress(downloaded, content_length);
    }

    close_request();
    WinHttpCloseHandle(connect);
    return response;
}

}  // namespace

HttpResponse HttpGet(std::string_view url, const ProgressFn& progress,
                     CancelToken* cancel) {
    std::string body;
    HttpResponse response = DoRequest(
        url, L"GET", {}, nullptr,
        [&](const uint8_t* data, size_t len) {
            body.append(reinterpret_cast<const char*>(data), len);
        },
        progress, cancel);
    response.body = std::move(body);
    return response;
}

HttpResponse HttpPostJson(std::string_view url, std::string_view json_body,
                          CancelToken* cancel) {
    std::string body;
    HttpResponse response = DoRequest(
        url, L"POST", json_body, "application/json",
        [&](const uint8_t* data, size_t len) {
            body.append(reinterpret_cast<const char*>(data), len);
        },
        {}, cancel);
    response.body = std::move(body);
    return response;
}

bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error,
                        CancelToken* cancel) {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open destination file for writing";
        return false;
    }
    const HttpResponse response = DoRequest(
        url, L"GET", {}, nullptr,
        [&](const uint8_t* data, size_t len) {
            out.write(reinterpret_cast<const char*>(data),
                      static_cast<std::streamsize>(len));
        },
        progress, cancel);
    out.close();
    if (!response.error.empty()) {
        error = response.error;
        return false;
    }
    if (response.status < 200 || response.status >= 300) {
        error = "HTTP " + std::to_string(response.status);
        return false;
    }
    return true;
}

}  // namespace nds::net
