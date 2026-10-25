#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// In-memory copies of the Lua files that belong to the open scene, keyed by
// their path RELATIVE to the scene folder ("scripts/Coin.lua").
//
//  * The editor edits the text here; the runtime runs the text from here, so
//    you can test unsaved changes. Save writes the files next to scene.json.
//  * Files are plain .lua on disk, so any external editor / git works.
//    pollExternalChanges() picks up edits made outside the engine (hot reload).
//  * Paths come from scene files, so they are validated: relative, inside the
//    scene folder, no "..". Anything else is rejected.
//
// No OpenGL / ImGui dependency, so it is unit-tested on its own.
namespace rift {

class ScriptStore {
public:
    struct Entry {
        std::string text;      // what the editor shows and the runtime runs
        std::string diskText;  // what is currently on disk (valid if onDisk)
        bool onDisk = false;
        bool dirty = false;    // text differs from diskText (or never saved)
        std::filesystem::file_time_type mtime{};
    };

    // Forget everything and point at a new scene folder.
    void reset(const std::string& sceneDir);
    const std::string& root() const { return root_; }
    // The project folder: "project:scripts/X.lua" paths live there (shared modules).
    static constexpr const char* kProjectPrefix = "project:";
    // "engine:scripts/X.lua": a built-in script shipped with RIFT (data/scripts),
    // read-only (the default character's Animate).
    static constexpr const char* kEnginePrefix = "engine:";
    static bool isEnginePath(const std::string& path) { return path.rfind(kEnginePrefix, 0) == 0; }
    void setProjectRoot(const std::string& projectDir) { projectRoot_ = projectDir; }
    const std::string& projectRoot() const { return projectRoot_; }

    // True if `relPath` is a safe relative path ending in .lua.
    static bool isValidPath(const std::string& relPath);
    // Normalizes separators to '/'. Returns "" if the path is not valid.
    static std::string normalize(const std::string& relPath);

    bool has(const std::string& relPath) const;
    const Entry* find(const std::string& relPath) const;
    // Reads the file from the scene folder into the store (no-op if present).
    // Returns false if the path is invalid or the file cannot be read.
    bool load(const std::string& relPath);

    // Editing. editableText() hands out the buffer; call touch() after
    // changing it so the dirty flag is updated.
    std::string* editableText(const std::string& relPath);
    void touch(const std::string& relPath);
    // Creates a new in-memory script (dirty until saved). False if it exists/invalid.
    bool create(const std::string& relPath, const std::string& text);
    // Drops it from the store; the file on disk is left alone.
    void forget(const std::string& relPath);
    // A path like "scripts/Name.lua" that is neither in the store nor on disk.
    std::string uniquePath(const std::string& baseName) const;

    bool anyDirty() const;
    // Writes every script into `sceneDir` (the current root if empty). Passing
    // a different folder is "Save As": all files are written there and the
    // store re-roots. Returns false if any file could not be written.
    bool saveAll(const std::string& sceneDir = std::string());
    bool save(const std::string& relPath);

    // Reloads scripts whose file changed on disk and which have no unsaved
    // edits. Scripts that changed on disk AND have unsaved edits are listed in
    // `conflicts` and left alone. Returns the reloaded paths.
    std::vector<std::string> pollExternalChanges(std::vector<std::string>* conflicts = nullptr);

    std::vector<std::string> paths() const;
    // Where a script's file is on disk.
    std::filesystem::path diskPath(const std::string& relPath) const;

private:
    std::string root_;
    std::string projectRoot_;
    std::map<std::string, Entry> entries_;

    static bool readFile(const std::filesystem::path& p, std::string& out);
    static bool writeFile(const std::filesystem::path& p, const std::string& text);
};

} // namespace rift
