#pragma once
//
// Updater.h -- keeps RIFT up to date from GitHub Releases: checks for a newer
// version in the background, downloads its setup and starts it. An installed
// RIFT (Uninstall.exe next to the editor) is updated silently in place and
// restarted; a copy that was not installed gets the normal setup window.
//
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace rift {

class Updater {
public:
    enum class State { Idle, Checking, UpToDate, Available, Downloading, Ready, Failed };

    struct Release {
        std::string version;   // "1.2.0" (the tag without the leading v)
        std::string title;
        std::string notes;     // Markdown from the release
        std::string page;      // the release on GitHub
        std::string setupUrl;  // the setup .exe asset
        std::uint64_t setupSize = 0;
        std::string published; // "2026-10-25"
        bool prerelease = false;
    };

    Updater() = default;
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;

    // Looks for a release newer than this build (in the background).
    void check(bool includePrereleases);
    // Downloads the setup of the release found by check().
    void download();
    void cancel();
    // Starts the downloaded setup. True: the editor should now quit.
    bool install();

    State state() const { return state_.load(); }
    bool busy() const { const State s = state(); return s == State::Checking || s == State::Downloading; }
    Release release() const;
    std::string error() const;
    float progress() const { return progress_.load(); } // 0..1 while downloading
    std::uint64_t downloaded() const { return downloaded_.load(); }
    // True once after a check that found an update (for a one-time notice).
    bool takeNewsFlag() { return news_.exchange(false); }

    // Is RIFT installed (by the setup) where this editor runs from?
    static bool installed();
    // -1 / 0 / 1 like strcmp, numerically per dot-separated part ("1.10.0" > "1.9.2").
    static int compareVersions(const std::string& a, const std::string& b);

private:
    std::thread worker_;
    std::atomic<State> state_{ State::Idle };
    std::atomic<float> progress_{ 0.0f };
    std::atomic<std::uint64_t> downloaded_{ 0 };
    std::atomic<bool> cancel_{ false };
    std::atomic<bool> news_{ false };
    mutable std::mutex mutex_;
    Release release_;
    std::string error_;
    std::string setupFile_;

    void join();
    void fail(const std::string& message);
};

} // namespace rift
