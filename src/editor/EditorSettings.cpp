#include "editor/EditorSettings.h"
#include "core/Paths.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace rift {

namespace {
    std::filesystem::path settingsFile() { return Paths::userData() / "settings.json"; }
}

void EditorSettings::addRecentProject(const std::string& folder) {
    recentProjects.erase(std::remove(recentProjects.begin(), recentProjects.end(), folder), recentProjects.end());
    recentProjects.insert(recentProjects.begin(), folder);
    if (recentProjects.size() > 12) recentProjects.resize(12);
    lastProject = folder;
}

bool EditorSettings::load() {
    std::ifstream in(settingsFile());
    if (!in.is_open()) return false;
    nlohmann::json j;
    try { in >> j; } catch (const std::exception& e) {
        LOG_WARN("Settings", std::string("Ignoring unreadable settings.json: ") + e.what());
        return false;
    }
    auto get = [&](const char* key, auto& out) {
        try { if (j.contains(key)) out = j.at(key).get<std::decay_t<decltype(out)>>(); } catch (...) {}
    };
    get("recentProjects", recentProjects);
    get("lastProject", lastProject);
    get("reopenLastProject", reopenLastProject);
    get("windowX", windowX);
    get("windowY", windowY);
    get("windowWidth", windowWidth);
    get("windowHeight", windowHeight);
    get("windowMaximized", windowMaximized);
    get("showExplorer", showExplorer);
    get("showProperties", showProperties);
    get("showOutput", showOutput);
    get("showAssets", showAssets);
    get("showGui", showGui);
    get("uiScale", uiScale);
    get("scriptFontSize", scriptFontSize);
    get("showStats", showStats);
    get("layoutVersion", layoutVersion);
    get("showGrid", showGrid);
    get("gridStep", gridStep);
    get("wireframe", wireframe);
    get("shadows", shadows);
    get("vsync", vsync);
    get("cameraSpeed", cameraSpeed);
    get("mouseSensitivity", mouseSensitivity);
    get("snap", snap);
    get("snapMove", snapMove);
    get("snapRotate", snapRotate);
    get("snapScale", snapScale);
    get("localSpace", localSpace);
    get("autosaveMinutes", autosaveMinutes);
    get("discordPresence", discordPresence);
    get("discordClientId", discordClientId);
    uiScale = std::clamp(uiScale, 0.6f, 2.5f);
    scriptFontSize = std::clamp(scriptFontSize, 9.0f, 32.0f);
    cameraSpeed = std::clamp(cameraSpeed, 1.0f, 500.0f);
    mouseSensitivity = std::clamp(mouseSensitivity, 0.02f, 2.0f);
    gridStep = std::max(gridStep, 0.01f);
    windowWidth = std::clamp(windowWidth, 640, 16384);
    windowHeight = std::clamp(windowHeight, 400, 16384);
    return true;
}

bool EditorSettings::save() const {
    nlohmann::json j;
    j["recentProjects"] = recentProjects;
    j["lastProject"] = lastProject;
    j["reopenLastProject"] = reopenLastProject;
    j["windowX"] = windowX;
    j["windowY"] = windowY;
    j["windowWidth"] = windowWidth;
    j["windowHeight"] = windowHeight;
    j["windowMaximized"] = windowMaximized;
    j["showExplorer"] = showExplorer;
    j["showProperties"] = showProperties;
    j["showOutput"] = showOutput;
    j["showAssets"] = showAssets;
    j["showGui"] = showGui;
    j["uiScale"] = uiScale;
    j["scriptFontSize"] = scriptFontSize;
    j["showStats"] = showStats;
    j["layoutVersion"] = layoutVersion;
    j["showGrid"] = showGrid;
    j["gridStep"] = gridStep;
    j["wireframe"] = wireframe;
    j["shadows"] = shadows;
    j["vsync"] = vsync;
    j["cameraSpeed"] = cameraSpeed;
    j["mouseSensitivity"] = mouseSensitivity;
    j["snap"] = snap;
    j["snapMove"] = snapMove;
    j["snapRotate"] = snapRotate;
    j["snapScale"] = snapScale;
    j["localSpace"] = localSpace;
    j["autosaveMinutes"] = autosaveMinutes;
    j["discordPresence"] = discordPresence;
    j["discordClientId"] = discordClientId;
    std::ofstream out(settingsFile(), std::ios::trunc);
    if (!out.is_open()) { LOG_WARN("Settings", "Could not write settings.json"); return false; }
    out << j.dump(2);
    return true;
}

} // namespace rift
