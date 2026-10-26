#pragma once
//
// Toolbox.h -- the asset library: models saved from any project (a folder per
// item in the user's RIFT folder) and the ones that come with RIFT
// (data/toolbox). An item is
//   model.json      {"format":"rift-toolbox", "name", "description", "created",
//                    "instances": [...], "files": ["assets/..."]}
//   files/...       copies of the project files the instances use (images,
//                   meshes, sounds, animations), at their project paths
//   thumbnail.png   a picture of it (made by the editor when it is saved)
// "instances" is the copy/paste encoding (SceneIO::subtreesToJson) with each
// script's text in "_text", so an item goes into any project.
//
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rift {

struct ToolboxItem {
    std::string name, description;
    std::filesystem::path folder;   // the item's folder
    bool builtIn = false;
    std::string rootClass;          // the first root's class: "Model", "Part", ...
    int objects = 0;                // instances in it
    int64_t created = 0;            // unix seconds
    std::filesystem::path thumbnail() const { return folder / "thumbnail.png"; }
};

namespace Toolbox {

std::filesystem::path userFolder();     // <user data>/Toolbox
std::filesystem::path builtInFolder();  // <engine data>/toolbox

// Built-in items first, then the user's; each group by name.
std::vector<ToolboxItem> list();
std::vector<ToolboxItem> listFolder(const std::filesystem::path& folder, bool builtIn);

// A new item in `folder` from a copy/paste snapshot (Editor::copySnapshot).
// The project files it uses are copied along.
bool save(const nlohmann::json& snapshot, const std::string& name, const std::string& description, const std::filesystem::path& projectRoot,
          ToolboxItem* out, std::string* error, const std::filesystem::path& folder = userFolder());
// The item's snapshot; the files it uses are copied into the project (where
// they are not there already).
bool load(const ToolboxItem& item, const std::filesystem::path& projectRoot, nlohmann::json& snapshot, std::string* error);
bool remove(const ToolboxItem& item, std::string* error);
bool rename(ToolboxItem& item, const std::string& name, std::string* error);

// The project files (relative paths) the snapshot's asset properties name.
std::vector<std::string> filesUsed(const nlohmann::json& snapshot);
// A folder name for an item called `name` ("My Car!" -> "My Car").
std::string folderName(const std::string& name);

} // namespace Toolbox
} // namespace rift
