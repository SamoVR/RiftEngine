#pragma once
//
// SceneIO.h -- scene files and instance snapshots as JSON.
//
// Scene file (format 4), one instance per line, parents before children:
//   { "format": 4, "rift": "0.9.0", "instances": [
//       {"id":"..","class":"Workspace","props":{"Gravity":40}},
//       {"id":"..","class":"Part","name":"Floor","parent":"<id>","props":{"Size":[64,1,64]}},
//       ... ] }
// Only properties that differ from the class default are written, by name,
// so files stay small, readable and diff-friendly, and new properties never
// break old files. Format 3 files (RIFT 0.8) are converted on load.
//
// The same encoding is used for undo snapshots, copy/paste and prefabs.
//
#include "scene/Scene.h"

#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <vector>

namespace rift::SceneIO {

constexpr int kFormatVersion = 4;

struct LoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    int formatVersion = 0; // version found in the file (3 = converted)
};

// ---- whole scenes
nlohmann::json sceneToJson(const Scene& scene);
// `scene` should be freshly constructed; on failure it is left as it was.
LoadResult sceneFromJson(Scene& scene, const nlohmann::json& j);
std::string sceneToText(const Scene& scene); // the file text (one instance per line)
bool saveScene(const Scene& scene, const std::filesystem::path& file, std::string* error = nullptr);
LoadResult loadScene(Scene& scene, const std::filesystem::path& file);

// ---- single instances / subtrees
// One instance's own data (no children, no parent link).
nlohmann::json instanceToJson(const Instance& instance);
// Applies the props/attrs/tags/name of `j` to an existing instance.
void applyInstanceJson(Instance& instance, const nlohmann::json& j, std::vector<std::string>* warnings = nullptr);
// `roots` and all their archivable descendants as a flat array (parents
// first). Root entries carry no "parent".
nlohmann::json subtreesToJson(const std::vector<const Instance*>& roots);
// Recreates instances from subtreesToJson output under `parent` (nullptr =
// detached). keepIds: reuse the stored ids when they are free (undo/redo);
// otherwise fresh ids (paste, prefabs). Returns the new roots.
std::vector<Instance*> subtreesFromJson(Scene& scene, const nlohmann::json& j, Instance* parent, bool keepIds,
                                        std::vector<std::string>* warnings = nullptr);

} // namespace rift::SceneIO
