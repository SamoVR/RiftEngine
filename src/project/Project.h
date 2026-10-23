#pragma once
//
// Project.h -- a RIFT game project: a folder with
//
//   project.json        name, start scene, window and build settings
//   scenes/<Name>/      one folder per scene: scene.json + scripts/*.lua
//   assets/             textures, models, sounds (paths in scenes are relative to the project)
//   scripts/            Lua modules shared by every scene (require("Name"))
//
// The editor opens projects; the player runs the start scene of one.
//
#include <filesystem>
#include <string>
#include <vector>

namespace rift {

struct ProjectSettings {
    std::string name = "My Game";
    std::string version = "1.0.0";
    std::string author;
    std::string startScene = "scenes/Main/scene.json"; // relative to the project folder
    std::string icon;                                  // assets/... png used for the game window / exe
    int windowWidth = 1280;
    int windowHeight = 720;
    bool fullscreen = false;
    bool resizable = true;
    bool vsync = true;
};

class Project {
public:
    // Loads <folder>/project.json. False (with a reason) if it is missing or unreadable.
    bool load(const std::filesystem::path& folder, std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;
    bool isOpen() const { return !root_.empty(); }
    void close();

    const std::filesystem::path& root() const { return root_; }
    ProjectSettings& settings() { return settings_; }
    const ProjectSettings& settings() const { return settings_; }

    std::filesystem::path startScenePath() const { return root_ / settings_.startScene; }
    std::filesystem::path scenesFolder() const { return root_ / "scenes"; }
    std::filesystem::path assetsFolder() const { return root_ / "assets"; }
    std::filesystem::path scriptsFolder() const { return root_ / "scripts"; }
    // Every scene.json under scenes/, sorted by name.
    std::vector<std::filesystem::path> scenes() const;

    // The project folder that contains `file` (nearest parent with a
    // project.json), or empty.
    static std::filesystem::path findRoot(const std::filesystem::path& file);

    enum class Template { Empty, Baseplate, Obby };
    // Creates <parent>/<name>/ with project.json, a first scene and the
    // standard folders. Fails if the folder exists and is not empty.
    static bool create(const std::filesystem::path& parent, const std::string& name, Template t,
                       std::filesystem::path* outFolder = nullptr, std::string* error = nullptr);

private:
    std::filesystem::path root_;
    ProjectSettings settings_;
};

} // namespace rift
