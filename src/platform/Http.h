#pragma once
//
// Http.h -- HTTPS requests through WinHTTP (the system's proxy settings and
// certificates). Blocking: call from a worker thread. Used by the updater.
//
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Platform {

struct HttpResponse {
    int status = 0;        // 0: no answer (offline, DNS, TLS); see `error`
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

// GET `url`. `headers` are "Name: value" lines.
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers = {}, unsigned timeoutMs = 15000);

// Downloads `url` to `file` (written to file.part first, renamed when complete).
// `progress(received, total)` is called as data arrives (total 0 = unknown) and
// can return false to cancel.
bool httpDownload(const std::string& url, const std::string& file,
                  const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string* error);

} // namespace Platform
