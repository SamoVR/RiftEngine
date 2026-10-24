#include "platform/Http.h"
#include "core/Paths.h"

#ifdef _WIN32
#include <Windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <filesystem>
#include <fstream>
#include <memory>

namespace Platform {

#ifdef _WIN32
namespace {
    std::wstring widen(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    struct Handle {
        HINTERNET h = nullptr;
        explicit Handle(HINTERNET handle = nullptr) : h(handle) {}
        ~Handle() { if (h) WinHttpCloseHandle(h); }
        Handle(const Handle&) = delete;
        Handle& operator=(const Handle&) = delete;
        explicit operator bool() const { return h != nullptr; }
    };

    std::string lastError(const char* what) {
        const DWORD code = GetLastError();
        std::string text = what;
        switch (code) {
            case ERROR_WINHTTP_NAME_NOT_RESOLVED: return text + ": no internet connection (the server name could not be resolved)";
            case ERROR_WINHTTP_CANNOT_CONNECT: return text + ": could not connect";
            case ERROR_WINHTTP_TIMEOUT: return text + ": timed out";
            case ERROR_WINHTTP_SECURE_FAILURE: return text + ": secure connection failed";
            default: return text + " (error " + std::to_string(code) + ")";
        }
    }

    // An open request whose response headers have arrived.
    struct Request {
        Handle session, connection, request;
        int status = 0;
        std::uint64_t length = 0; // Content-Length, 0 if unknown
    };

    bool openRequest(Request& r, const std::string& url, const std::vector<std::string>& headers, unsigned timeoutMs, std::string& error) {
        const std::wstring wurl = widen(url);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        wchar_t host[256] = {}, path[2048] = {};
        parts.lpszHostName = host;
        parts.dwHostNameLength = 256;
        parts.lpszUrlPath = path;
        parts.dwUrlPathLength = 2048;
        wchar_t extra[2048] = {};
        parts.lpszExtraInfo = extra;
        parts.dwExtraInfoLength = 2048;
        if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) { error = "not a valid address: " + url; return false; }

        r.session.h = WinHttpOpen(L"RIFT", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!r.session) r.session.h = WinHttpOpen(L"RIFT", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!r.session) { error = lastError("could not start WinHTTP"); return false; }
        const int t = static_cast<int>(timeoutMs);
        WinHttpSetTimeouts(r.session.h, t, t, t, t);
        r.connection.h = WinHttpConnect(r.session.h, host, parts.nPort, 0);
        if (!r.connection) { error = lastError("could not connect"); return false; }
        const std::wstring target = std::wstring(path) + extra;
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        r.request.h = WinHttpOpenRequest(r.connection.h, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!r.request) { error = lastError("could not create the request"); return false; }
        std::wstring headerText;
        for (const std::string& h : headers) headerText += widen(h) + L"\r\n";
        if (!WinHttpSendRequest(r.request.h, headerText.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerText.c_str(), static_cast<DWORD>(-1L),
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
            || !WinHttpReceiveResponse(r.request.h, nullptr)) {
            error = lastError("request failed");
            return false;
        }
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(r.request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        r.status = static_cast<int>(status);
        wchar_t lengthText[32] = {};
        size = sizeof(lengthText);
        if (WinHttpQueryHeaders(r.request.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lengthText, &size, WINHTTP_NO_HEADER_INDEX))
            r.length = std::wcstoull(lengthText, nullptr, 10);
        return true;
    }

    // Reads the body in pieces; `sink` returns false to stop.
    bool readBody(Request& r, const std::function<bool(const char*, size_t)>& sink, std::string& error) {
        std::vector<char> buffer(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(r.request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got)) {
                error = lastError("download interrupted");
                return false;
            }
            if (got == 0) return true;
            if (!sink(buffer.data(), got)) return false;
        }
    }
}

HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers, unsigned timeoutMs) {
    HttpResponse response;
    Request r;
    if (!openRequest(r, url, headers, timeoutMs, response.error)) return response;
    response.status = r.status;
    readBody(r, [&](const char* data, size_t n) { response.body.append(data, n); return true; }, response.error);
    if (!response.ok() && response.error.empty()) response.error = "the server answered " + std::to_string(r.status);
    return response;
}

bool httpDownload(const std::string& url, const std::string& file,
                  const std::function<bool(std::uint64_t, std::uint64_t)>& progress, std::string* error) {
    std::string err;
    Request r;
    auto fail = [&](const std::string& e) { if (error) *error = e; return false; };
    if (!openRequest(r, url, { "Accept: application/octet-stream" }, 30000, err)) return fail(err);
    if (r.status < 200 || r.status >= 300) return fail("the server answered " + std::to_string(r.status));

    const std::filesystem::path target = Paths::fromUtf8(file);
    std::filesystem::path part = target;
    part += ".part";
    std::error_code ec;
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    bool cancelled = false;
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return fail("cannot write " + Paths::toUtf8(part));
        std::uint64_t received = 0;
        const bool ok = readBody(r, [&](const char* data, size_t n) {
            out.write(data, static_cast<std::streamsize>(n));
            received += n;
            if (progress && !progress(received, r.length)) { cancelled = true; return false; }
            return static_cast<bool>(out);
        }, err);
        if (!ok || !out) {
            out.close();
            std::filesystem::remove(part, ec);
            return fail(cancelled ? "cancelled" : err.empty() ? "could not write the file" : err);
        }
        if (r.length && received != r.length) {
            out.close();
            std::filesystem::remove(part, ec);
            return fail("the download is incomplete");
        }
    }
    std::filesystem::remove(target, ec);
    std::filesystem::rename(part, target, ec);
    if (ec) return fail("cannot rename the download: " + ec.message());
    return true;
}

#else
HttpResponse httpGet(const std::string&, const std::vector<std::string>&, unsigned) {
    HttpResponse r;
    r.error = "HTTP is not available on this platform";
    return r;
}
bool httpDownload(const std::string&, const std::string&, const std::function<bool(std::uint64_t, std::uint64_t)>&, std::string* error) {
    if (error) *error = "HTTP is not available on this platform";
    return false;
}
#endif

} // namespace Platform
