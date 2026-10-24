#include "editor/EditorSettings.h"
#include "core/Paths.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <ctime>
#include <fstream>

namespace rift {

namespace {
    std::filesystem::path settingsFile() { return Paths::userData() / "settings.json"; }

    // Every saved setting once, for both loading and saving.
    template <class Settings, class F> void visit(Settings& s, F&& f) {
        f("recentProjects", s.recentProjects);
        f("recentOpened", s.recentOpened);
        f("lastProject", s.lastProject);
        f("projectsFolder", s.projectsFolder);
        f("reopenLastProject", s.reopenLastProject);
        f("windowX", s.windowX);
        f("windowY", s.windowY);
        f("windowWidth", s.windowWidth);
        f("windowHeight", s.windowHeight);
        f("windowMaximized", s.windowMaximized);
        f("uiScale", s.uiScale);
        f("accent", s.accent);
        f("showStats", s.showStats);
        f("confirmDelete", s.confirmDelete);
        f("showTooltips", s.showTooltips);
        f("layoutVersion", s.layoutVersion);
        f("showExplorer", s.showExplorer);
        f("showProperties", s.showProperties);
        f("showOutput", s.showOutput);
        f("showAssets", s.showAssets);
        f("showSourceControl", s.showSourceControl);
        f("showGrid", s.showGrid);
        f("showGui", s.showGui);
        f("gridStep", s.gridStep);
        f("wireframe", s.wireframe);
        f("shadows", s.shadows);
        f("vsync", s.vsync);
        f("msaa", s.msaa);
        f("fpsLimit", s.fpsLimit);
        f("throttleInBackground", s.throttleInBackground);
        f("cameraSpeed", s.cameraSpeed);
        f("mouseSensitivity", s.mouseSensitivity);
        f("fieldOfView", s.fieldOfView);
        f("invertMouseY", s.invertMouseY);
        f("snap", s.snap);
        f("snapMove", s.snapMove);
        f("snapRotate", s.snapRotate);
        f("snapScale", s.snapScale);
        f("localSpace", s.localSpace);
        f("scriptFontSize", s.scriptFontSize);
        f("tabSize", s.tabSize);
        f("showWhitespace", s.showWhitespace);
        f("showMinimap", s.showMinimap);
        f("autoClosePairs", s.autoClosePairs);
        f("externalEditor", s.externalEditor);
        f("clearOutputOnPlay", s.clearOutputOnPlay);
        f("pauseOnError", s.pauseOnError);
        f("saveBeforePlay", s.saveBeforePlay);
        f("outputTimestamps", s.outputTimestamps);
        f("outputMaxLines", s.outputMaxLines);
        f("autosaveMinutes", s.autosaveMinutes);
        f("checkForUpdates", s.checkForUpdates);
        f("includePrereleases", s.includePrereleases);
        f("skippedVersion", s.skippedVersion);
        f("gitPath", s.gitPath);
        f("gitInitNewProjects", s.gitInitNewProjects);
        f("gitPushAfterCommit", s.gitPushAfterCommit);
        f("discordPresence", s.discordPresence);
        f("discordClientId", s.discordClientId);
    }
}

void EditorSettings::addRecentProject(const std::string& folder) {
    recentProjects.erase(std::remove(recentProjects.begin(), recentProjects.end(), folder), recentProjects.end());
    recentProjects.insert(recentProjects.begin(), folder);
    if (recentProjects.size() > 24) recentProjects.resize(24);
    lastProject = folder;
    recentOpened[folder] = static_cast<std::int64_t>(std::time(nullptr));
    // Forget times of projects that left the list.
    for (auto it = recentOpened.begin(); it != recentOpened.end();) {
        if (std::find(recentProjects.begin(), recentProjects.end(), it->first) == recentProjects.end()) it = recentOpened.erase(it);
        else ++it;
    }
}

void EditorSettings::resetPreferences() {
    EditorSettings fresh;
    fresh.recentProjects = recentProjects;
    fresh.recentOpened = recentOpened;
    fresh.lastProject = lastProject;
    fresh.projectsFolder = projectsFolder;
    fresh.windowX = windowX;
    fresh.windowY = windowY;
    fresh.windowWidth = windowWidth;
    fresh.windowHeight = windowHeight;
    fresh.windowMaximized = windowMaximized;
    fresh.layoutVersion = layoutVersion;
    fresh.showExplorer = showExplorer;
    fresh.showProperties = showProperties;
    fresh.showOutput = showOutput;
    fresh.showAssets = showAssets;
    fresh.showSourceControl = showSourceControl;
    fresh.discordClientId = discordClientId;
    *this = fresh;
}

bool EditorSettings::load() {
    std::ifstream in(settingsFile());
    if (!in.is_open()) return false;
    nlohmann::json j;
    try { in >> j; } catch (const std::exception& e) {
        LOG_WARN("Settings", std::string("Ignoring unreadable settings.json: ") + e.what());
        return false;
    }
    visit(*this, [&](const char* key, auto& out) {
        try { if (j.contains(key)) out = j.at(key).get<std::decay_t<decltype(out)>>(); } catch (...) {}
    });
    uiScale = std::clamp(uiScale, 0.6f, 2.5f);
    scriptFontSize = std::clamp(scriptFontSize, 9.0f, 32.0f);
    cameraSpeed = std::clamp(cameraSpeed, 1.0f, 500.0f);
    mouseSensitivity = std::clamp(mouseSensitivity, 0.02f, 2.0f);
    fieldOfView = std::clamp(fieldOfView, 30.0f, 110.0f);
    gridStep = std::max(gridStep, 0.01f);
    tabSize = std::clamp(tabSize, 1, 8);
    msaa = msaa >= 8 ? 8 : msaa >= 4 ? 4 : msaa >= 2 ? 2 : 1;
    fpsLimit = std::clamp(fpsLimit, 0, 1000);
    outputMaxLines = std::clamp(outputMaxLines, 100, 100000);
    windowWidth = std::clamp(windowWidth, 640, 16384);
    windowHeight = std::clamp(windowHeight, 400, 16384);
    return true;
}

bool EditorSettings::save() const {
    nlohmann::json j;
    visit(*this, [&](const char* key, const auto& value) { j[key] = value; });
    std::ofstream out(settingsFile(), std::ios::trunc);
    if (!out.is_open()) { LOG_WARN("Settings", "Could not write settings.json"); return false; }
    out << j.dump(2);
    return true;
}

} // namespace rift
