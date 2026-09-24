#pragma once

#include <string>

// A minimal HTTPS client: one blocking POST, backed by each platform's own
// networking stack so that no TLS library has to be built or shipped.
//
//   Windows              WinHTTP
//   macOS, iOS, tvOS     NSURLSession        (src/port/http_apple.m)
//   Android              HttpURLConnection   (through JNI)
//   Linux and others     libcurl             (when found at configure time)
//
// It exists for RetroAchievements, whose server only speaks HTTPS. It blocks,
// so callers run it on a worker thread, never on the game thread.
namespace partyboard::http {

struct Response {
    // The HTTP status, or 0 when no response was received at all (no network,
    // TLS failure, timeout, or no backend on this platform).
    int status = 0;
    std::string body;
    // Why status is 0; empty otherwise.
    std::string error;
};

// Whether this build has a working backend.
bool available();

Response post(const std::string& url, const std::string& body, const std::string& contentType,
    const std::string& userAgent);

} // namespace partyboard::http
