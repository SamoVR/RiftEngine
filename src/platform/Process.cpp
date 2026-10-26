#include "platform/Process.h"

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

#include <algorithm>
#include <chrono>
#include <cwctype>

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
    std::string narrow(const std::wstring& w) {
        if (w.empty()) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
        return s;
    }

    // One argument quoted so that CommandLineToArgvW (and the C runtime) read
    // it back unchanged: backslashes only matter in front of a quote.
    std::wstring quote(const std::wstring& arg) {
        if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
        std::wstring out = L"\"";
        for (size_t i = 0; i < arg.size(); ++i) {
            size_t slashes = 0;
            while (i < arg.size() && arg[i] == L'\\') { ++slashes; ++i; }
            if (i == arg.size()) { out.append(slashes * 2, L'\\'); break; }
            if (arg[i] == L'"') { out.append(slashes * 2 + 1, L'\\'); out += L'"'; }
            else { out.append(slashes, L'\\'); out += arg[i]; }
        }
        out += L'"';
        return out;
    }

    std::wstring lower(std::wstring s) {
        for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
        return s;
    }

    // The current environment with `changes` ("NAME=value") applied, as the
    // double-NUL-terminated block CreateProcessW expects.
    std::wstring environmentBlock(const std::vector<std::string>& changes) {
        std::vector<std::wstring> vars;
        if (wchar_t* env = GetEnvironmentStringsW()) {
            for (const wchar_t* p = env; *p; p += wcslen(p) + 1) vars.emplace_back(p);
            FreeEnvironmentStringsW(env);
        }
        for (const std::string& change : changes) {
            const std::wstring w = widen(change);
            const std::wstring name = lower(w.substr(0, w.find(L'=') + 1));
            vars.erase(std::remove_if(vars.begin(), vars.end(), [&](const std::wstring& v) { return lower(v.substr(0, name.size())) == name; }), vars.end());
            vars.push_back(w);
        }
        std::wstring block;
        for (const std::wstring& v : vars) { block += v; block.push_back(L'\0'); }
        block.push_back(L'\0');
        return block;
    }
}

std::string findOnPath(const std::string& program) {
    wchar_t found[MAX_PATH * 2];
    const std::wstring name = widen(program);
    if (SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH * 2, found, nullptr) == 0) return {};
    return narrow(found);
}

std::string localAppDataFolder() {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) out = narrow(path);
    CoTaskMemFree(path);
    return out;
}

ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& arguments, const std::string& workingFolder,
                         unsigned timeoutMs, const std::vector<std::string>& environment, bool separateErrors) {
    ProcessResult result;
    std::string path = exe;
    if (path.find_first_of("\\/") == std::string::npos) {
        path = findOnPath(exe);
        if (path.empty()) { result.output = exe + " was not found"; return result; }
    }

    // One pipe for stdout and stderr; the child gets the write end.
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) { result.output = "could not create a pipe"; return result; }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    // A second pipe for stderr when the caller parses stdout (warnings must not mix in).
    HANDLE errRead = nullptr, errWrite = nullptr;
    if (separateErrors) {
        if (!CreatePipe(&errRead, &errWrite, &sa, 0)) { CloseHandle(readPipe); CloseHandle(writePipe); result.output = "could not create a pipe"; return result; }
        SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);
    }
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul;
    si.hStdOutput = writePipe;
    si.hStdError = separateErrors ? errWrite : writePipe;
    PROCESS_INFORMATION pi{};

    std::wstring commandLine = quote(widen(path));
    for (const std::string& a : arguments) commandLine += L' ' + quote(widen(a));
    std::wstring env = environmentBlock(environment);
    const std::wstring dir = widen(workingFolder);
    const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                        env.data(), dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    CloseHandle(writePipe); // only the child writes now
    if (errWrite) CloseHandle(errWrite);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!created) {
        CloseHandle(readPipe);
        if (errRead) CloseHandle(errRead);
        result.output = "could not start " + path;
        return result;
    }
    result.started = true;

    // Read while it runs (a full pipe would block the child), until it exits.
    const auto start = std::chrono::steady_clock::now();
    char buffer[4096];
    auto drainPipe = [&](HANDLE pipe, std::string& into) {
        DWORD available = 0;
        while (PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD got = 0;
            if (!ReadFile(pipe, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) || got == 0) break;
            into.append(buffer, got);
        }
    };
    auto drain = [&] {
        drainPipe(readPipe, result.output);
        if (errRead) drainPipe(errRead, result.errors);
    };
    for (;;) {
        drain();
        if (WaitForSingleObject(pi.hProcess, 15) == WAIT_OBJECT_0) break;
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        if (timeoutMs && elapsed > static_cast<long long>(timeoutMs)) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            result.timedOut = true;
            break;
        }
    }
    drain();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exitCode = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    if (errRead) CloseHandle(errRead);
    return result;
}

#else
ProcessResult runProcess(const std::string& exe, const std::vector<std::string>&, const std::string&, unsigned, const std::vector<std::string>&, bool) {
    ProcessResult r;
    r.output = exe + " cannot be started on this platform";
    return r;
}
std::string findOnPath(const std::string&) { return {}; }
std::string localAppDataFolder() { return {}; }
#endif

} // namespace Platform
