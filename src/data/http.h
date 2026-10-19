#pragma once
#include <string>

namespace hermes {
struct HttpRequest {
    std::wstring method=L"GET";
    std::wstring url;
    std::string body;
    std::string contentType="application/json";
    std::string cookie;                       // full Cookie header value when the site needs a session
    unsigned timeoutMs=15000;
};
struct HttpResult {
    bool transportOk=false;                   // WinHTTP completed; HTTP status may still be an error
    unsigned status=0;
    std::string body;
    std::string error;                        // transport-level description when transportOk is false
    unsigned elapsedMs=0;
    std::string DurationText() const;
};
// Blocking single request. The data layer calls this from its worker threads, never from the UI
// thread, so the message loop stays free while a site is slow.
HttpResult HttpFetch(const HttpRequest& request);
}
