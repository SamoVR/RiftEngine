#pragma once
//
// EditorSettings.h -- the editor's preferences, saved to
// %APPDATA%\RIFT\settings.json (tolerant loading: unknown or wrongly typed
// values keep their defaults, so old files never break new versions).
//
#include <cstdint>
#include <string>
#include <vector>

namespace rift {

struct EditorSettings {
    // Projects
    std::vector<std::string> recentProjects; // most recent first
    std::string lastProject;
    bool reopenLastProject = true;
    std::string projectsFolder;            // where new projects go ("" = Documents\RIFT Projects)

    // Window (restored on the next start)
    int windowX = -1, windowY = -1;  // -1: centred
    int windowWidth = 1600, windowHeight = 900;
    bool windowMaximized = true;

    // Interface
    float uiScale = 1.0f;
    float scriptFontSize = 15.0f;
    bool showStats = true;
    int layoutVersion = 0;
    bool showExplorer = true, showProperties = true, showOutput = true, showAssets = true;

    // Viewport
    bool showGrid = true;
    bool showGui = true;            // StarterGui drawn over the scene while editing
    float gridStep = 1.0f;
    bool wireframe = false;
    bool shadows = true;
    bool vsync = true;
    float cameraSpeed = 24.0f;      // studs per second
    float mouseSensitivity = 0.25f; // degrees per pixel

    // Transform tools
    bool snap = true;
    float snapMove = 1.0f;
    float snapRotate = 15.0f;
    float snapScale = 0.5f;
    bool localSpace = false;

    // Saving
    int autosaveMinutes = 5;        // 0 = off; autosaves go next to the scene (*.autosave.json)

    // Discord
    bool discordPresence = true;
    std::int64_t discordClientId = 1363649635603972227;

    void addRecentProject(const std::string& folder);
    bool load();
    bool save() const;
};

} // namespace rift
