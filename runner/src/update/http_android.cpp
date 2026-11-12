// update/http_android.cpp -- the java.net.HttpURLConnection backend of
// update/http.h, driven over JNI. Bionic ships no libcurl and the platform
// TLS stack is only reachable from Java.
#include "update/http.h"

#include <SDL3/SDL_system.h>
#include <jni.h>

#include <fstream>
#include <vector>

namespace nds::net {

// The handle is a global ref to the HttpURLConnection. disconnect() is the
// documented way to abort a blocked read from another thread; the worker's
// read then throws and unwinds normally.
void CancelToken::Cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_.store(true, std::memory_order_release);
    if (!handle_) return;
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto conn = static_cast<jobject>(handle_);
    handle_ = nullptr;
    if (!env) return;
    jclass cls = env->GetObjectClass(conn);
    jmethodID disconnect = env->GetMethodID(cls, "disconnect", "()V");
    env->CallVoidMethod(conn, disconnect);
    env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteGlobalRef(conn);
}

bool CancelToken::AttachHandle(void* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    handle_ = handle;
    return !cancelled_.load(std::memory_order_acquire);
}

void CancelToken::CloseHandle() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (handle_) {
        if (auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv()))
            env->DeleteGlobalRef(static_cast<jobject>(handle_));
        handle_ = nullptr;
    }
}

namespace {

constexpr jint kConnectTimeoutMs = 10 * 1000;
constexpr jint kReadTimeoutMs = 20 * 1000;
constexpr jint kDownloadReadTimeoutMs = 300 * 1000;
constexpr jsize kChunkSize = 64 * 1024;
constexpr const char* kUserAgent = "ndsrecomp-http/1.0";

bool IsHttpsUrl(std::string_view url) {
    return url.size() >= 8 && url.substr(0, 8) == "https://";
}

// Local refs pile up across a long read loop; scope them per request.
class LocalFrame {
public:
    explicit LocalFrame(JNIEnv* env) : env_(env) { env_->PushLocalFrame(32); }
    ~LocalFrame() { env_->PopLocalFrame(nullptr); }
    LocalFrame(const LocalFrame&) = delete;
    LocalFrame& operator=(const LocalFrame&) = delete;

private:
    JNIEnv* env_;
};

// The pending exception as text, cleared; empty if none.
std::string TakeException(JNIEnv* env) {
    if (!env->ExceptionCheck()) return {};
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    std::string text = "java exception";
    jclass cls = env->GetObjectClass(ex);
    jmethodID to_string = env->GetMethodID(cls, "toString", "()Ljava/lang/String;");
    if (auto str = static_cast<jstring>(env->CallObjectMethod(ex, to_string))) {
        if (const char* utf = env->GetStringUTFChars(str, nullptr)) {
            text = utf;
            env->ReleaseStringUTFChars(str, utf);
        }
    }
    env->ExceptionClear();
    return text;
}

using SinkFn = std::function<bool(const char* data, size_t size)>;

struct Request {
    std::string_view url;
    const char* method = "GET";
    std::string_view body;
    jint read_timeout_ms = kReadTimeoutMs;
    const ProgressFn* progress = nullptr;
    CancelToken* cancel = nullptr;
};

// Runs one request, streaming the response body into `sink`. The HTTP
// status, or 0 with `error` set.
long Perform(const Request& req, const SinkFn& sink, std::string& error) {
    if (req.cancel && req.cancel->cancelled()) {
        error = "cancelled";
        return 0;
    }
    if (!IsHttpsUrl(req.url)) {
        error = "only https:// URLs are supported";
        return 0;
    }
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (!env) {
        error = "no JNI environment";
        return 0;
    }
    LocalFrame frame(env);

    auto fail = [&](const char* what) {
        const std::string ex = TakeException(env);
        error = ex.empty() ? what : ex;
        return 0L;
    };

    jclass url_cls = env->FindClass("java/net/URL");
    jclass conn_cls = env->FindClass("java/net/HttpURLConnection");
    if (!url_cls || !conn_cls) return fail("java.net classes unavailable");
    jstring jurl = env->NewStringUTF(std::string(req.url).c_str());
    jobject url_obj = env->NewObject(
        url_cls, env->GetMethodID(url_cls, "<init>", "(Ljava/lang/String;)V"),
        jurl);
    if (!url_obj) return fail("bad URL");
    jobject conn = env->CallObjectMethod(
        url_obj,
        env->GetMethodID(url_cls, "openConnection", "()Ljava/net/URLConnection;"));
    if (!conn || env->ExceptionCheck()) return fail("openConnection failed");

    auto call_void = [&](const char* name, const char* sig, auto... args) {
        env->CallVoidMethod(conn, env->GetMethodID(conn_cls, name, sig), args...);
    };
    call_void("setConnectTimeout", "(I)V", kConnectTimeoutMs);
    call_void("setReadTimeout", "(I)V", req.read_timeout_ms);
    call_void("setInstanceFollowRedirects", "(Z)V", JNI_TRUE);
    call_void("setRequestMethod", "(Ljava/lang/String;)V",
              env->NewStringUTF(req.method));
    call_void("setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
              env->NewStringUTF("User-Agent"), env->NewStringUTF(kUserAgent));
    if (env->ExceptionCheck()) return fail("request setup failed");

    // Own the connection through the token so Cancel() can disconnect it.
    jobject conn_ref = env->NewGlobalRef(conn);
    if (req.cancel && !req.cancel->AttachHandle(conn_ref)) {
        // Cancel() already released the ref.
        error = "cancelled";
        return 0;
    }
    struct Detach {
        CancelToken* cancel;
        JNIEnv* env;
        jobject ref;
        ~Detach() {
            if (cancel) {
                cancel->CloseHandle();
            } else {
                env->DeleteGlobalRef(ref);
            }
        }
    } detach{req.cancel, env, conn_ref};

    if (!req.body.empty()) {
        call_void("setDoOutput", "(Z)V", JNI_TRUE);
        call_void("setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V",
                  env->NewStringUTF("Content-Type"),
                  env->NewStringUTF("application/json"));
        jobject out = env->CallObjectMethod(
            conn,
            env->GetMethodID(conn_cls, "getOutputStream", "()Ljava/io/OutputStream;"));
        if (!out || env->ExceptionCheck()) return fail("getOutputStream failed");
        jclass out_cls = env->GetObjectClass(out);
        jbyteArray bytes = env->NewByteArray(static_cast<jsize>(req.body.size()));
        env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(req.body.size()),
                                reinterpret_cast<const jbyte*>(req.body.data()));
        env->CallVoidMethod(out, env->GetMethodID(out_cls, "write", "([B)V"), bytes);
        env->CallVoidMethod(out, env->GetMethodID(out_cls, "close", "()V"));
        if (env->ExceptionCheck()) return fail("request body write failed");
    }

    const jint status = env->CallIntMethod(
        conn, env->GetMethodID(conn_cls, "getResponseCode", "()I"));
    if (env->ExceptionCheck()) return fail("connect failed");
    const jlong total = env->CallLongMethod(
        conn, env->GetMethodID(conn_cls, "getContentLengthLong", "()J"));
    env->ExceptionClear();

    // Error responses (>= 400) keep their body on the error stream.
    const char* stream_getter = status >= 400 ? "getErrorStream" : "getInputStream";
    jobject in = env->CallObjectMethod(
        conn, env->GetMethodID(conn_cls, stream_getter, "()Ljava/io/InputStream;"));
    if (env->ExceptionCheck()) return fail("getInputStream failed");
    if (in) {
        jclass in_cls = env->GetObjectClass(in);
        jmethodID read = env->GetMethodID(in_cls, "read", "([BII)I");
        jmethodID close = env->GetMethodID(in_cls, "close", "()V");
        jbyteArray chunk = env->NewByteArray(kChunkSize);
        std::vector<char> buffer(static_cast<size_t>(kChunkSize));
        uint64_t downloaded = 0;
        for (;;) {
            if (req.cancel && req.cancel->cancelled()) {
                error = "cancelled";
                return 0;
            }
            const jint n = env->CallIntMethod(in, read, chunk, 0, kChunkSize);
            if (env->ExceptionCheck()) return fail("read failed");
            if (n < 0) break;
            env->GetByteArrayRegion(chunk, 0, n,
                                    reinterpret_cast<jbyte*>(buffer.data()));
            if (!sink(buffer.data(), static_cast<size_t>(n))) {
                error = "write failed";
                return 0;
            }
            downloaded += static_cast<uint64_t>(n);
            if (req.progress && *req.progress) {
                (*req.progress)(downloaded,
                                total > 0 ? static_cast<uint64_t>(total) : 0);
            }
        }
        env->CallVoidMethod(in, close);
        env->ExceptionClear();
    }
    call_void("disconnect", "()V");
    env->ExceptionClear();
    return status;
}

HttpResponse Fetch(const Request& req) {
    HttpResponse response;
    auto sink = [&](const char* data, size_t size) {
        response.body.append(data, size);
        return true;
    };
    response.status = Perform(req, sink, response.error);
    return response;
}

}  // namespace

HttpResponse HttpGet(std::string_view url, const ProgressFn& progress,
                     CancelToken* cancel) {
    Request req;
    req.url = url;
    req.progress = &progress;
    req.cancel = cancel;
    return Fetch(req);
}

HttpResponse HttpPostJson(std::string_view url, std::string_view json_body,
                          CancelToken* cancel) {
    Request req;
    req.url = url;
    req.method = "POST";
    req.body = json_body;
    req.cancel = cancel;
    return Fetch(req);
}

bool HttpDownloadToFile(std::string_view url, const std::filesystem::path& dest,
                        const ProgressFn& progress, std::string& error,
                        CancelToken* cancel) {
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "failed to open destination file for writing";
        return false;
    }
    Request req;
    req.url = url;
    req.read_timeout_ms = kDownloadReadTimeoutMs;
    req.progress = &progress;
    req.cancel = cancel;
    auto sink = [&](const char* data, size_t size) {
        out.write(data, static_cast<std::streamsize>(size));
        return static_cast<bool>(out);
    };
    const long status = Perform(req, sink, error);
    out.close();
    if (!error.empty()) return false;
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

}  // namespace nds::net
