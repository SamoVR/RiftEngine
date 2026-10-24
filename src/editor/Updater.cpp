#include "editor/Updater.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Http.h"
#include "platform/Platform.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace rift {

namespace fs = std::filesystem;

Updater::~Updater() {
    cancel_ = true;
    join();
}

void Updater::join() {
    if (worker_.joinable()) worker_.join();
}

void Updater::fail(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = message;
    }
    LOG_WARN("Updater", message);
    state_ = State::Failed;
}

Updater::Release Updater::release() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return release_;
}

std::string Updater::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

int Updater::compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& v) {
        std::vector<long> out;
        size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            long n = 0;
            while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = n * 10 + (v[i++] - '0');
            out.push_back(n);
            if (i < v.size() && v[i] == '.') ++i;
            else break;
        }
        return out;
    };
    const std::vector<long> x = parts(a), y = parts(b);
    for (size_t k = 0; k < std::max(x.size(), y.size()); ++k) {
        const long p = k < x.size() ? x[k] : 0, q = k < y.size() ? y[k] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

bool Updater::installed() {
    std::error_code ec;
    return fs::exists(Paths::exeDir() / "Uninstall.exe", ec);
}

void Updater::check(bool includePrereleases) {
    if (busy()) return;
    join();
    cancel_ = false;
    state_ = State::Checking;
    worker_ = std::thread([this, includePrereleases] {
        const std::string api = std::string("https://api.github.com/repos/") + RIFT_GITHUB_REPO + (includePrereleases ? "/releases?per_page=15" : "/releases/latest");
        const Platform::HttpResponse r = Platform::httpGet(api, { "Accept: application/vnd.github+json", "User-Agent: RIFT/" RIFT_VERSION_STRING,
                                                                   "X-GitHub-Api-Version: 2022-11-28" });
        if (cancel_) { state_ = State::Idle; return; }
        if (r.status == 404) { state_ = State::UpToDate; return; } // no release published yet
        if (!r.ok()) { fail("Could not check for updates: " + r.error); return; }

        nlohmann::json j = nlohmann::json::parse(r.body, nullptr, false);
        if (j.is_discarded()) { fail("Could not check for updates: unexpected answer from GitHub"); return; }
        // The list (pre-releases included) is newest first; drafts are skipped.
        const nlohmann::json* pick = nullptr;
        if (j.is_array()) {
            for (const auto& rel : j) {
                if (rel.value("draft", false)) continue;
                pick = &rel;
                break;
            }
        } else if (j.is_object()) {
            pick = &j;
        }
        if (!pick) { state_ = State::UpToDate; return; }

        Release rel;
        rel.version = pick->value("tag_name", std::string());
        if (!rel.version.empty() && (rel.version[0] == 'v' || rel.version[0] == 'V')) rel.version.erase(0, 1);
        rel.title = pick->value("name", std::string());
        rel.notes = pick->value("body", std::string());
        rel.page = pick->value("html_url", std::string());
        rel.published = pick->value("published_at", std::string()).substr(0, 10);
        rel.prerelease = pick->value("prerelease", false);
        if (pick->contains("assets") && (*pick)["assets"].is_array()) {
            for (const auto& asset : (*pick)["assets"]) {
                std::string name = asset.value("name", std::string());
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".exe") == 0 && lower.find("setup") != std::string::npos) {
                    rel.setupUrl = asset.value("browser_download_url", std::string());
                    rel.setupSize = asset.value("size", std::uint64_t(0));
                    break;
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            release_ = rel;
            error_.clear();
        }
        const bool newer = !rel.version.empty() && compareVersions(rel.version, RIFT_VERSION_STRING) > 0;
        LOG_INFO("Updater", "Latest release: " + (rel.version.empty() ? std::string("none") : rel.version) + (newer ? " (newer)" : ""));
        if (newer) news_ = true;
        state_ = newer ? State::Available : State::UpToDate;
    });
}

void Updater::download() {
    if (state() != State::Available) return;
    join();
    const Release rel = release();
    if (rel.setupUrl.empty()) { fail("This release has no setup to download; get it from " + rel.page); return; }
    cancel_ = false;
    progress_ = 0.0f;
    downloaded_ = 0;
    state_ = State::Downloading;
    const fs::path folder = fs::temp_directory_path() / "RIFT Updates";
    setupFile_ = Paths::toUtf8(folder / ("RIFT-Setup-" + rel.version + ".exe"));
    worker_ = std::thread([this, rel] {
        std::string error;
        const bool ok = Platform::httpDownload(rel.setupUrl, setupFile_, [this, &rel](std::uint64_t got, std::uint64_t total) {
            if (!total) total = rel.setupSize;
            downloaded_ = got;
            progress_ = total ? static_cast<float>(static_cast<double>(got) / static_cast<double>(total)) : 0.0f;
            return !cancel_.load();
        }, &error);
        if (cancel_) { state_ = State::Available; return; }
        if (!ok) { fail("Download failed: " + error); return; }
        progress_ = 1.0f;
        state_ = State::Ready;
    });
}

void Updater::cancel() {
    cancel_ = true;
}

bool Updater::install() {
    if (state() != State::Ready) return false;
#ifdef _WIN32
    // An installed RIFT is updated where it is, silently, and started again
    // once this editor has closed; otherwise the setup shows its window.
    std::vector<std::string> args;
    if (installed()) {
        args = { "/update", "/silent", "/dir", Paths::toUtf8(Paths::exeDir()), "/wait", std::to_string(GetCurrentProcessId()), "/launch" };
    }
    if (!Platform::launch(setupFile_, args)) {
        fail("Could not start the setup (" + setupFile_ + ")");
        return false;
    }
    return true;
#else
    return false;
#endif
}

} // namespace rift
