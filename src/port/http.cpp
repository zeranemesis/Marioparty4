#include "port/http.hpp"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#define PARTYBOARD_HTTP_WINHTTP 1
#include <windows.h>
#include <winhttp.h>
#elif defined(__APPLE__)
#define PARTYBOARD_HTTP_APPLE 1
extern "C" int PartyBoard_HttpPostApple(const char* url, const void* body, size_t bodyLength,
    const char* contentType, const char* userAgent, char** outBody, size_t* outLength, char** outError);
#elif defined(__ANDROID__) || defined(ANDROID)
#define PARTYBOARD_HTTP_ANDROID 1
#include <SDL3/SDL_system.h>
#include <jni.h>
#elif defined(PARTYBOARD_HAVE_CURL)
#define PARTYBOARD_HTTP_CURL 1
#include <curl/curl.h>
#endif

namespace partyboard::http {

namespace {
constexpr long kTimeoutSeconds = 30;
}

#if defined(PARTYBOARD_HTTP_WINHTTP)

namespace {

std::wstring widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), length);
    return result;
}

std::string last_error(const char* what) {
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

// Closes a WinHTTP handle when it goes out of scope.
struct Handle {
    HINTERNET h = nullptr;
    ~Handle() {
        if (h != nullptr) {
            WinHttpCloseHandle(h);
        }
    }
};

} // namespace

bool available() { return true; }

Response post(const std::string& url, const std::string& body, const std::string& contentType,
    const std::string& userAgent) {
    Response response;
    const std::wstring wideUrl = widen(url);

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)) {
        response.error = last_error("WinHttpCrackUrl");
        return response;
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo != nullptr) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    Handle session{WinHttpOpen(widen(userAgent).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0)};
    if (session.h == nullptr) {
        response.error = last_error("WinHttpOpen");
        return response;
    }
    const int timeoutMs = static_cast<int>(kTimeoutSeconds * 1000);
    WinHttpSetTimeouts(session.h, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    Handle connection{WinHttpConnect(session.h, host.c_str(), parts.nPort, 0)};
    if (connection.h == nullptr) {
        response.error = last_error("WinHttpConnect");
        return response;
    }
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    Handle request{WinHttpOpenRequest(connection.h, L"POST", path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags)};
    if (request.h == nullptr) {
        response.error = last_error("WinHttpOpenRequest");
        return response;
    }

    const std::wstring headers = L"Content-Type: " + widen(contentType) + L"\r\n";
    if (!WinHttpSendRequest(request.h, headers.c_str(), static_cast<DWORD>(-1),
            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0)
        || !WinHttpReceiveResponse(request.h, nullptr)) {
        response.error = last_error("WinHttpSendRequest");
        return response;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        response.error = last_error("WinHttpQueryHeaders");
        return response;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available)) {
            response.error = last_error("WinHttpQueryDataAvailable");
            return response;
        }
        if (available == 0) {
            break;
        }
        const size_t offset = response.body.size();
        response.body.resize(offset + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, response.body.data() + offset, available, &read)) {
            response.error = last_error("WinHttpReadData");
            response.body.clear();
            return response;
        }
        response.body.resize(offset + read);
    }
    response.status = static_cast<int>(status);
    return response;
}

#elif defined(PARTYBOARD_HTTP_APPLE)

bool available() { return true; }

Response post(const std::string& url, const std::string& body, const std::string& contentType,
    const std::string& userAgent) {
    Response response;
    char* outBody = nullptr;
    size_t outLength = 0;
    char* outError = nullptr;
    response.status = PartyBoard_HttpPostApple(url.c_str(), body.data(), body.size(), contentType.c_str(),
        userAgent.c_str(), &outBody, &outLength, &outError);
    if (outBody != nullptr) {
        response.body.assign(outBody, outLength);
        std::free(outBody);
    }
    if (outError != nullptr) {
        response.error = outError;
        std::free(outError);
    }
    return response;
}

#elif defined(PARTYBOARD_HTTP_ANDROID)

namespace {

// Clears a pending Java exception, reporting whether there was one.
bool failed(JNIEnv* env) {
    if (!env->ExceptionCheck()) {
        return false;
    }
    env->ExceptionClear();
    return true;
}

jstring utf(JNIEnv* env, const std::string& text) { return env->NewStringUTF(text.c_str()); }

} // namespace

bool available() { return true; }

Response post(const std::string& url, const std::string& body, const std::string& contentType,
    const std::string& userAgent) {
    Response response;
    // SDL attaches the calling thread to the VM if it is not already.
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    if (env == nullptr) {
        response.error = "no JNI environment";
        return response;
    }
    // Every local reference below lives in this frame and is released with it,
    // which matters on a worker thread that stays attached to the VM.
    if (env->PushLocalFrame(64) != 0) {
        failed(env);
        response.error = "PushLocalFrame failed";
        return response;
    }

    auto done = [&](const char* error) {
        failed(env);
        if (error != nullptr) {
            response.error = error;
        }
        env->PopLocalFrame(nullptr);
        return response;
    };

    jclass urlClass = env->FindClass("java/net/URL");
    if (urlClass == nullptr || failed(env)) {
        return done("java.net.URL unavailable");
    }
    jobject urlObject = env->NewObject(urlClass, env->GetMethodID(urlClass, "<init>", "(Ljava/lang/String;)V"),
        utf(env, url));
    if (urlObject == nullptr || failed(env)) {
        return done("invalid URL");
    }
    jobject connection = env->CallObjectMethod(urlObject,
        env->GetMethodID(urlClass, "openConnection", "()Ljava/net/URLConnection;"));
    if (connection == nullptr || failed(env)) {
        return done("openConnection failed");
    }

    jclass httpClass = env->FindClass("java/net/HttpURLConnection");
    const jint timeoutMs = static_cast<jint>(kTimeoutSeconds * 1000);
    env->CallVoidMethod(connection, env->GetMethodID(httpClass, "setConnectTimeout", "(I)V"), timeoutMs);
    env->CallVoidMethod(connection, env->GetMethodID(httpClass, "setReadTimeout", "(I)V"), timeoutMs);
    env->CallVoidMethod(connection, env->GetMethodID(httpClass, "setRequestMethod", "(Ljava/lang/String;)V"),
        utf(env, "POST"));
    env->CallVoidMethod(connection, env->GetMethodID(httpClass, "setDoOutput", "(Z)V"), JNI_TRUE);
    const jmethodID setHeader =
        env->GetMethodID(httpClass, "setRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V");
    env->CallVoidMethod(connection, setHeader, utf(env, "Content-Type"), utf(env, contentType));
    env->CallVoidMethod(connection, setHeader, utf(env, "User-Agent"), utf(env, userAgent));
    if (failed(env)) {
        return done("configuring the request failed");
    }

    jobject output = env->CallObjectMethod(connection,
        env->GetMethodID(httpClass, "getOutputStream", "()Ljava/io/OutputStream;"));
    if (output == nullptr || failed(env)) {
        return done("connection failed");
    }
    jclass outputClass = env->FindClass("java/io/OutputStream");
    jbyteArray payload = env->NewByteArray(static_cast<jsize>(body.size()));
    env->SetByteArrayRegion(payload, 0, static_cast<jsize>(body.size()), reinterpret_cast<const jbyte*>(body.data()));
    env->CallVoidMethod(output, env->GetMethodID(outputClass, "write", "([B)V"), payload);
    env->CallVoidMethod(output, env->GetMethodID(outputClass, "close", "()V"));
    if (failed(env)) {
        return done("sending the request failed");
    }

    const jint status = env->CallIntMethod(connection, env->GetMethodID(httpClass, "getResponseCode", "()I"));
    if (failed(env)) {
        return done("no response");
    }
    // An error status carries its body on the error stream instead.
    jobject input = env->CallObjectMethod(connection,
        env->GetMethodID(httpClass, status >= 400 ? "getErrorStream" : "getInputStream", "()Ljava/io/InputStream;"));
    if (failed(env)) {
        input = nullptr;
    }
    if (input != nullptr) {
        jclass inputClass = env->FindClass("java/io/InputStream");
        const jmethodID read = env->GetMethodID(inputClass, "read", "([B)I");
        jbyteArray chunk = env->NewByteArray(16384);
        for (;;) {
            const jint count = env->CallIntMethod(input, read, chunk);
            if (failed(env)) {
                return done("reading the response failed");
            }
            if (count <= 0) {
                break;
            }
            const size_t offset = response.body.size();
            response.body.resize(offset + static_cast<size_t>(count));
            env->GetByteArrayRegion(chunk, 0, count, reinterpret_cast<jbyte*>(response.body.data() + offset));
        }
        env->CallVoidMethod(input, env->GetMethodID(inputClass, "close", "()V"));
    }
    env->CallVoidMethod(connection, env->GetMethodID(httpClass, "disconnect", "()V"));
    response.status = static_cast<int>(status);
    return done(nullptr);
}

#elif defined(PARTYBOARD_HTTP_CURL)

namespace {

size_t append(char* data, size_t size, size_t count, void* userdata) {
    static_cast<std::string*>(userdata)->append(data, size * count);
    return size * count;
}

} // namespace

bool available() { return true; }

Response post(const std::string& url, const std::string& body, const std::string& contentType,
    const std::string& userAgent) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    Response response;
    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        response.error = "curl_easy_init failed";
        return response;
    }
    const std::string header = "Content-Type: " + contentType;
    curl_slist* headers = curl_slist_append(nullptr, header.c_str());
    char errorBuffer[CURL_ERROR_SIZE] = {};

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);

    const CURLcode result = curl_easy_perform(curl);
    if (result == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        response.status = static_cast<int>(status);
    } else {
        response.error = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(result);
        response.body.clear();
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

#else

bool available() { return false; }

Response post(const std::string&, const std::string&, const std::string&, const std::string&) {
    Response response;
    response.error = "this build has no HTTPS backend";
    return response;
}

#endif

} // namespace partyboard::http
