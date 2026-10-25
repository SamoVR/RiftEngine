#include "project/Exporter.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace rift {

namespace fs = std::filesystem;

namespace {
    constexpr const char* kMarker = "rift-build.json"; // marks a folder as a RIFT build (safe to replace)

    bool inside(const fs::path& path, const fs::path& folder) {
        std::error_code ec;
        const fs::path rel = fs::relative(path, folder, ec);
        return !ec && !rel.empty() && *rel.begin() != "..";
    }

    struct Copier {
        ExportResult& result;
        bool copyFile(const fs::path& from, const fs::path& to) {
            std::error_code ec;
            fs::create_directories(to.parent_path(), ec);
            if (!fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) {
                result.error = "Could not copy " + Paths::toUtf8(from) + ": " + ec.message();
                return false;
            }
            ++result.files;
            result.bytes += fs::file_size(to, ec);
            return true;
        }
        // Every file under `from` (recursively) whose name passes `keep`.
        bool copyTree(const fs::path& from, const fs::path& to, bool (*keep)(const fs::path&) = nullptr) {
            std::error_code ec;
            if (!fs::exists(from, ec)) return true;
            for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (!it->is_regular_file(ec)) continue;
                if (keep && !keep(it->path())) continue;
                if (!copyFile(it->path(), to / fs::relative(it->path(), from, ec))) return false;
            }
            return true;
        }
    };

    bool notAutosave(const fs::path& p) { return p.filename() != "scene.autosave.json"; }
}

std::string executableName(const std::string& gameName) {
    std::string out;
    for (char c : gameName) {
        if (static_cast<unsigned char>(c) < 32 || std::strchr("\\/:*?\"<>|", c)) continue;
        out += c;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Game") : out;
}

ExportResult exportGame(const Project& project, const ExportOptions& o) {
    ExportResult r;
    std::error_code ec;
    if (!project.isOpen()) { r.error = "No project is open"; return r; }
    if (!fs::is_regular_file(o.playerExe, ec)) { r.error = "RiftPlayer.exe was not found (" + Paths::toUtf8(o.playerExe) + "). Build the RiftPlayer project first."; return r; }
    if (!fs::is_directory(o.engineData / "shaders", ec)) { r.error = "The engine's data folder is missing (" + Paths::toUtf8(o.engineData) + ")"; return r; }
    if (o.outputFolder.empty()) { r.error = "Choose a folder for the build"; return r; }

    const fs::path out = fs::absolute(o.outputFolder);
    const fs::path root = fs::absolute(project.root());
    // The build must not land inside what it copies (or contain the project).
    for (const fs::path& sub : { project.scenesFolder(), root / "assets", root / "scripts" }) {
        if (inside(out, sub) || fs::equivalent(out, sub, ec)) { r.error = "The build folder cannot be inside the project's " + Paths::toUtf8(sub.filename()) + " folder"; return r; }
    }
    if (fs::equivalent(out, root, ec) || inside(root, out)) { r.error = "The build folder cannot be the project folder itself"; return r; }

    // An earlier build is replaced; anything else must be empty.
    if (fs::exists(out, ec) && !fs::is_empty(out, ec)) {
        std::ifstream markerFile(out / kMarker);
        if (!markerFile.is_open()) { r.error = "The folder " + Paths::toUtf8(out) + " is not empty. Pick an empty folder (or an earlier build of this game)."; return r; }
        nlohmann::json marker;
        try { markerFile >> marker; } catch (...) {}
        markerFile.close();
        fs::remove_all(out / "game", ec);
        fs::remove_all(out / "data", ec);
        if (marker.contains("exe") && marker["exe"].is_string()) fs::remove(out / Paths::fromUtf8(marker["exe"].get<std::string>()), ec);
        fs::remove(out / kMarker, ec);
    }
    fs::create_directories(out, ec);
    if (ec) { r.error = "Cannot create " + Paths::toUtf8(out) + ": " + ec.message(); return r; }

    Copier copy{ r };
    // The player, renamed after the game.
    const std::string exeName = executableName(project.settings().name) + ".exe";
    r.executable = out / Paths::fromUtf8(exeName);
    if (!copy.copyFile(o.playerExe, r.executable)) return r;

    // Engine data: shaders, the interface font, icons, built-in textures.
    const fs::path data = out / "data";
    if (!copy.copyTree(o.engineData / "shaders", data / "shaders")) return r;
    if (!copy.copyTree(o.engineData / "textures", data / "textures")) return r;
    // Built-in scripts and animations (the default character's Animate).
    for (const char* folder : { "scripts", "animations" }) {
        std::error_code exists;
        if (fs::exists(o.engineData / folder, exists) && !copy.copyTree(o.engineData / folder, data / folder)) return r;
    }
    for (const char* f : { "Inter-Regular.ttf", "Inter-SemiBold.ttf", "Inter-LICENSE.txt" })
        if (!copy.copyFile(o.engineData / "fonts" / f, data / "fonts" / f)) return r;
    if (!copy.copyFile(o.engineData / "icons" / "rift.png", data / "icons" / "rift.png")) return r;

    // The game itself.
    const fs::path game = out / "game";
    if (!copy.copyFile(root / "project.json", game / "project.json")) return r;
    const std::string scenes = project.settings().scenesFolder;
    if (!copy.copyTree(project.scenesFolder(), game / Paths::fromUtf8(scenes), notAutosave)) return r;
    if (!copy.copyTree(root / "assets", game / "assets")) return r;
    if (!copy.copyTree(root / "scripts", game / "scripts")) return r;

    // Marker: lets the next build replace this one.
    nlohmann::json marker;
    marker["exe"] = exeName;
    marker["game"] = project.settings().name;
    marker["version"] = project.settings().version;
    marker["rift"] = RIFT_VERSION_STRING;
    marker["built"] = static_cast<int64_t>(std::time(nullptr));
    std::ofstream(out / kMarker) << marker.dump(2);

    if (!project.settings().icon.empty()) {
        const fs::path icon = root / Paths::fromUtf8(project.settings().icon);
        if (!setExecutableIcon(r.executable, icon)) LOG_WARN("Export", "Could not use " + project.settings().icon + " as the game's icon");
    }
    r.ok = true;
    LOG_INFO("Export", "Built " + project.settings().name + " into " + Paths::toUtf8(out) + " (" + std::to_string(r.files) + " files)");
    return r;
}

// ---------------------------------------------------------------------------
//  Executable icon
// ---------------------------------------------------------------------------
#ifdef _WIN32
namespace {
#pragma pack(push, 2)
    struct GroupIconHeader { WORD reserved; WORD type; WORD count; };
    struct GroupIconEntry { BYTE width, height, colors, reserved; WORD planes, bitCount; DWORD bytes; WORD id; };
#pragma pack(pop)

    // Loads `png`, scales it to fit 256x256, and encodes it as PNG again (icons store PNG data).
    bool iconImage(const fs::path& png, std::vector<unsigned char>& out) {
        int w = 0, h = 0, n = 0;
        stbi_set_flip_vertically_on_load(false); // textures turn this on for OpenGL
        unsigned char* pixels = stbi_load(Paths::toUtf8(png).c_str(), &w, &h, &n, 4);
        if (!pixels) return false;
        const int size = 256;
        std::vector<unsigned char> square(static_cast<size_t>(size) * size * 4, 0);
        // Box-filter into a centred square, keeping the image's shape.
        const float scale = std::max(w, h) / static_cast<float>(size);
        const int dw = std::max(1, static_cast<int>(w / scale)), dh = std::max(1, static_cast<int>(h / scale));
        const int ox = (size - dw) / 2, oy = (size - dh) / 2;
        for (int y = 0; y < dh; ++y) {
            for (int x = 0; x < dw; ++x) {
                const int x0 = static_cast<int>(x * scale), y0 = static_cast<int>(y * scale);
                const int x1 = std::max(x0 + 1, static_cast<int>((x + 1) * scale)), y1 = std::max(y0 + 1, static_cast<int>((y + 1) * scale));
                unsigned sum[4] = { 0, 0, 0, 0 }, count = 0;
                for (int sy = y0; sy < std::min(y1, h); ++sy)
                    for (int sx = x0; sx < std::min(x1, w); ++sx) {
                        const unsigned char* p = pixels + (static_cast<size_t>(sy) * w + sx) * 4;
                        for (int c = 0; c < 4; ++c) sum[c] += p[c];
                        ++count;
                    }
                unsigned char* d = square.data() + (static_cast<size_t>(y + oy) * size + (x + ox)) * 4;
                for (int c = 0; c < 4; ++c) d[c] = static_cast<unsigned char>(count ? sum[c] / count : 0);
            }
        }
        stbi_image_free(pixels);
        out.clear();
        stbi_write_png_to_func([](void* ctx, void* data, int bytes) {
            auto* v = static_cast<std::vector<unsigned char>*>(ctx);
            v->insert(v->end(), static_cast<unsigned char*>(data), static_cast<unsigned char*>(data) + bytes);
        }, &out, size, size, 4, square.data(), size * 4);
        return !out.empty();
    }
}

bool setExecutableIcon(const fs::path& exe, const fs::path& png) {
    std::vector<unsigned char> image;
    if (!iconImage(png, image)) return false;

    // The languages the existing icon group was compiled with.
    std::vector<WORD> languages;
    if (HMODULE module = LoadLibraryExW(exe.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE)) {
        EnumResourceLanguagesW(module, RT_GROUP_ICON, MAKEINTRESOURCEW(101), [](HMODULE, LPCWSTR, LPCWSTR, WORD lang, LONG_PTR param) -> BOOL {
            reinterpret_cast<std::vector<WORD>*>(param)->push_back(lang);
            return TRUE;
        }, reinterpret_cast<LONG_PTR>(&languages));
        FreeLibrary(module);
    }
    if (languages.empty()) languages.push_back(MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US));

    std::vector<unsigned char> group(sizeof(GroupIconHeader) + sizeof(GroupIconEntry));
    auto* header = reinterpret_cast<GroupIconHeader*>(group.data());
    header->reserved = 0;
    header->type = 1;
    header->count = 1;
    auto* entry = reinterpret_cast<GroupIconEntry*>(group.data() + sizeof(GroupIconHeader));
    *entry = GroupIconEntry{ 0, 0, 0, 0, 1, 32, static_cast<DWORD>(image.size()), 1 };

    HANDLE update = BeginUpdateResourceW(exe.c_str(), FALSE);
    if (!update) return false;
    bool ok = true;
    for (WORD lang : languages) {
        ok &= UpdateResourceW(update, RT_ICON, MAKEINTRESOURCEW(1), lang, image.data(), static_cast<DWORD>(image.size())) != FALSE;
        ok &= UpdateResourceW(update, RT_GROUP_ICON, MAKEINTRESOURCEW(101), lang, group.data(), static_cast<DWORD>(group.size())) != FALSE;
    }
    return EndUpdateResourceW(update, ok ? FALSE : TRUE) != FALSE && ok;
}
#else
bool setExecutableIcon(const fs::path&, const fs::path&) { return false; }
#endif

} // namespace rift
