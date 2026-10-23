#include "core/Log.h"
#include "core/Paths.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace Log {

namespace {
    struct State {
        std::recursive_mutex mutex; // recursive: a listener may log
        std::ofstream file;
        std::string path;
        bool opened = false;
        bool inListener = false; // a listener that logs must not recurse forever
        std::map<int, Listener> listeners;
        int nextListener = 1;
    };
    State& state() {
        static State s;
        return s;
    }

    void openFile(State& s, const std::string& appName) {
        if (s.opened) return;
        s.opened = true;
        try {
            const auto dir = Paths::userData() / "logs";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            const auto current = dir / Paths::fromUtf8(appName + ".log");
            const auto previous = dir / Paths::fromUtf8(appName + ".previous.log");
            if (std::filesystem::exists(current, ec)) std::filesystem::rename(current, previous, ec);
            s.file.open(current, std::ios::out | std::ios::trunc);
            s.path = Paths::toUtf8(current);
        } catch (...) {
            // No log file is not a reason to fail: OutputDebugString still works.
        }
    }

    const char* label(LogLevel level) {
        switch (level) {
            case LogLevel::Info:    return "INFO ";
            case LogLevel::Warning: return "WARN ";
            case LogLevel::Error:   return "ERROR";
        }
        return "?????";
    }

    std::string timestamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tmBuf{};
#ifdef _WIN32
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tmBuf);
        return buf;
    }
}

void init(const std::string& appName) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    openFile(s, appName);
}

void shutdown() {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    if (s.file.is_open()) {
        s.file << "----- end of session -----\n";
        s.file.close();
    }
    s.listeners.clear();
}

void write(LogLevel level, const std::string& tag, const std::string& message) {
    State& s = state();
    std::unique_lock<std::recursive_mutex> lock(s.mutex);
    openFile(s, "RIFT");
    const std::string line = "[" + timestamp() + "] [" + label(level) + "] [" + tag + "] " + message;
    if (s.file.is_open()) s.file << line << std::endl;
#ifdef _WIN32
    OutputDebugStringA((line + "\n").c_str());
#endif
    if (s.inListener || s.listeners.empty()) return;
    s.inListener = true;
    const auto listeners = s.listeners; // a listener may remove itself
    for (const auto& [id, fn] : listeners) {
        try { fn(level, tag, message); } catch (...) {}
    }
    s.inListener = false;
}

int addListener(Listener listener) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    const int id = s.nextListener++;
    s.listeners[id] = std::move(listener);
    return id;
}

void removeListener(int id) {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    s.listeners.erase(id);
}

std::string filePath() {
    State& s = state();
    std::lock_guard<std::recursive_mutex> lock(s.mutex);
    return s.path;
}

} // namespace Log
