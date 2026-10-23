#pragma once
//
// Log.h -- engine-wide logging.
//
// Every line goes to:
//   1. <user data>/logs/<app>.log (flushed per line, so the cause of a crash is
//      on disk even if the process dies right after; the previous session is
//      kept as <app>.previous.log),
//   2. OutputDebugString (Visual Studio's Output window),
//   3. any registered listeners (the editor's Output panel).
//
#include <functional>
#include <string>

enum class LogLevel { Info, Warning, Error };

namespace Log {

// Opens the log file. Call once at start-up, after Paths::setUserDataFolderName.
// Logging before init() still works (file opened lazily with the default name).
void init(const std::string& appName);
void shutdown();

void write(LogLevel level, const std::string& tag, const std::string& message);

// Listener receives every line (any thread; the call is serialised).
// Returns an id for removeListener.
using Listener = std::function<void(LogLevel, const std::string& tag, const std::string& message)>;
int addListener(Listener listener);
void removeListener(int id);

// Full path of the current log file (for "Open log" in the UI).
std::string filePath();

} // namespace Log

#define LOG_INFO(tag, msg)  ::Log::write(LogLevel::Info,    tag, (msg))
#define LOG_WARN(tag, msg)  ::Log::write(LogLevel::Warning, tag, (msg))
#define LOG_ERROR(tag, msg) ::Log::write(LogLevel::Error,   tag, (msg))
