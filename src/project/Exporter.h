#pragma once
//
// Exporter.h -- File > Build Game: turns a project into a folder anyone can
// run without the editor.
//
//   <out>/<Game Name>.exe   RiftPlayer.exe, renamed (and given the game's icon)
//   <out>/data/             the engine files the player needs (shaders, fonts, ...)
//   <out>/game/             project.json, scenes/, assets/, scripts/
//
// Building into a folder that holds an earlier build replaces it; any other
// non-empty folder is refused, so nothing of the user's is ever deleted.
//
#include "project/Project.h"

#include <filesystem>
#include <functional>
#include <string>

namespace rift {

struct ExportOptions {
    std::filesystem::path outputFolder;
    std::filesystem::path playerExe;   // RiftPlayer.exe to copy
    std::filesystem::path engineData;  // the editor's data folder
};

struct ExportResult {
    bool ok = false;
    std::string error;
    std::filesystem::path executable; // the game's .exe
    int files = 0;                    // files copied
    uintmax_t bytes = 0;
};

// A file name made from the game's name ("My Game!" -> "My Game").
std::string executableName(const std::string& gameName);

ExportResult exportGame(const Project& project, const ExportOptions& options);

// Puts a PNG into the exe as its icon (Explorer, taskbar). Windows only;
// false if the image cannot be used (the default RIFT icon stays).
bool setExecutableIcon(const std::filesystem::path& exe, const std::filesystem::path& png);

} // namespace rift
