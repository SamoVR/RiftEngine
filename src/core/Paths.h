#pragma once
//
// Paths.h -- where RIFT finds its files. Nothing in the engine resolves paths
// against the current working directory any more (launching from Explorer, a
// shortcut, the VS debugger or a shipped game folder all behave the same).
//
//  * Engine data (shaders, fonts, icons, default textures): the "data" folder
//    next to the executable. Development builds (build\bin\Debug) find the
//    repository's data\ folder by walking up from the executable.
//  * User data (settings.json, imgui.ini, logs): %APPDATA%\RIFT (or the
//    game's own folder for the player, see setUserDataFolderName).
//  * Project assets: relative to the open project's root folder. Scene files
//    store paths like "assets/textures/wood.png", so projects can be moved.
//
// All strings are UTF-8 (the executable manifest sets the UTF-8 code page).
//
#include <filesystem>
#include <string>

namespace Paths {

namespace fs = std::filesystem;

// Folder containing the running executable.
const fs::path& exeDir();

// The engine's data folder. Throws std::runtime_error if it cannot be found.
const fs::path& engineData();
fs::path shader(const std::string& fileName);   // data/shaders/<file>
fs::path font(const std::string& fileName);     // data/fonts/<file>
fs::path icon(const std::string& fileName);     // data/icons/<file>
fs::path engineTexture(const std::string& fileName); // data/textures/<file>

// Per-user writable folder, created on first use.
void setUserDataFolderName(const std::string& name); // default "RIFT"
const fs::path& userData();

// Root folder of the open project ("" when none is open).
void setProjectRoot(const fs::path& root);
const fs::path& projectRoot();

// Prefix of stored paths that point into the engine's data folder
// ("engine:textures/checker.png"), so built-in assets stay portable too.
inline constexpr const char* kEnginePrefix = "engine:";

// Turns a path stored in a scene file into an absolute path: "engine:..." is
// resolved in the data folder, absolute paths are returned as they are, and
// relative ones are resolved against the project root (or the data folder).
fs::path resolveAsset(const std::string& stored);
// The inverse: "assets/x.png" when `path` lies inside the project,
// "engine:..." inside the data folder, otherwise the absolute path.
// Always uses forward slashes.
std::string toStored(const fs::path& path);

// UTF-8 <-> path (fs::path(std::string) would use the ANSI code page on
// systems where the manifest's UTF-8 setting is not honoured).
fs::path fromUtf8(const std::string& s);
std::string toUtf8(const fs::path& p);

} // namespace Paths
