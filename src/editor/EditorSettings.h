#pragma once
//
// EditorSettings.h -- the editor's preferences, saved to
// %APPDATA%\RIFT\settings.json (tolerant loading: unknown or wrongly typed
// values keep their defaults, so old files never break new versions).
//
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace rift {

struct EditorSettings {
    // Projects
    std::vector<std::string> recentProjects; // most recent first
    std::map<std::string, std::int64_t> recentOpened; // folder -> when it was last opened (Unix time)
    std::string lastProject;
    bool reopenLastProject = true;
    std::string projectsFolder;            // where new projects go ("" = Documents\RIFT Projects)

    // Window (restored on the next start)
    int windowX = -1, windowY = -1;  // -1: centred
    int windowWidth = 1600, windowHeight = 900;
    bool windowMaximized = true;

    // Interface
    float uiScale = 1.0f;
    int accent = 0;                  // theme::accentPresets()
    bool showStats = true;
    bool confirmDelete = false;      // ask before deleting objects in the Explorer
    bool showTooltips = true;
    int layoutVersion = 0;
    bool showExplorer = true, showProperties = true, showOutput = true, showAssets = true, showSourceControl = true;

    // Viewport
    bool showGrid = true;
    bool showGui = true;            // StarterGui drawn over the scene while editing
    float gridStep = 1.0f;
    bool wireframe = false;
    bool shadows = true;
    bool vsync = true;
    int msaa = 4;                   // samples: 1, 2, 4, 8
    int fpsLimit = 0;               // without V-Sync; 0 = no limit
    bool throttleInBackground = true; // ~15 FPS while another window has focus
    float cameraSpeed = 24.0f;      // studs per second
    float mouseSensitivity = 0.25f; // degrees per pixel
    float fieldOfView = 70.0f;      // editor camera, degrees
    bool invertMouseY = false;

    // Transform tools
    bool snap = true;
    float snapMove = 1.0f;
    float snapRotate = 15.0f;
    float snapScale = 0.5f;
    bool localSpace = false;

    // Script editor
    float scriptFontSize = 15.0f;
    int tabSize = 4;
    bool showWhitespace = false;
    bool showMinimap = false;
    bool autoClosePairs = true;
    std::string externalEditor;     // program for "Open in External Editor" ("" = Windows' choice for .lua)

    // Playing
    bool clearOutputOnPlay = true;
    bool pauseOnError = false;      // pause the game when a script error happens
    bool saveBeforePlay = false;    // save the scene when Play is pressed

    // Output
    bool outputTimestamps = true;
    int outputMaxLines = 2000;

    // Saving
    int autosaveMinutes = 5;        // 0 = off; autosaves go next to the scene (*.autosave.json)

    // Updates
    bool checkForUpdates = true;    // at start-up
    bool includePrereleases = false;
    std::string skippedVersion;     // "Skip this version"

    // Source control
    std::string gitPath;            // "" = find git.exe
    bool gitInitNewProjects = true;
    bool gitPushAfterCommit = false;

    // Discord
    bool discordPresence = true;
    std::int64_t discordClientId = 1363649635603972227;

    void addRecentProject(const std::string& folder);
    // Back to the defaults, except the window, recent projects and layout.
    void resetPreferences();
    bool load();
    bool save() const;
};

} // namespace rift
