#include "project/Project.h"
#include "scene/Scene.h"
#include "scene/SceneIO.h"
#include "core/Paths.h"
#include "core/Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace rift {

namespace fs = std::filesystem;

bool Project::load(const fs::path& folder, std::string* error) {
    const fs::path file = folder / "project.json";
    std::ifstream in(file);
    if (!in.is_open()) {
        if (error) *error = "no project.json in " + Paths::toUtf8(folder);
        return false;
    }
    nlohmann::json j;
    try {
        in >> j;
    } catch (const std::exception& e) {
        if (error) *error = std::string("project.json is not valid JSON: ") + e.what();
        return false;
    }
    ProjectSettings s;
    auto get = [&](const char* key, auto& out) {
        try { if (j.contains(key)) out = j.at(key).get<std::decay_t<decltype(out)>>(); } catch (...) {}
    };
    get("name", s.name);
    get("version", s.version);
    get("author", s.author);
    get("startScene", s.startScene);
    get("icon", s.icon);
    get("scenesFolder", s.scenesFolder);
    if (!isValidScenesFolder(s.scenesFolder)) s.scenesFolder = "scenes";
    if (j.contains("window") && j["window"].is_object()) {
        const auto& w = j["window"];
        s.windowWidth = std::clamp(w.value("width", s.windowWidth), 320, 7680);
        s.windowHeight = std::clamp(w.value("height", s.windowHeight), 240, 4320);
        s.fullscreen = w.value("fullscreen", s.fullscreen);
        s.resizable = w.value("resizable", s.resizable);
        s.vsync = w.value("vsync", s.vsync);
    }
    root_ = fs::absolute(folder);
    settings_ = s;
    return true;
}

bool Project::save(std::string* error) const {
    if (root_.empty()) { if (error) *error = "no project is open"; return false; }
    nlohmann::json j;
    j["name"] = settings_.name;
    j["version"] = settings_.version;
    if (!settings_.author.empty()) j["author"] = settings_.author;
    j["startScene"] = settings_.startScene;
    if (!settings_.icon.empty()) j["icon"] = settings_.icon;
    if (settings_.scenesFolder != "scenes") j["scenesFolder"] = settings_.scenesFolder;
    j["window"] = { { "width", settings_.windowWidth }, { "height", settings_.windowHeight },
                    { "fullscreen", settings_.fullscreen }, { "resizable", settings_.resizable }, { "vsync", settings_.vsync } };
    std::ofstream out(root_ / "project.json", std::ios::trunc);
    if (!out.is_open()) { if (error) *error = "cannot write project.json"; return false; }
    out << j.dump(2) << "\n";
    return true;
}

void Project::close() {
    root_.clear();
    settings_ = {};
}

fs::path Project::scenesFolder() const {
    return root_ / Paths::fromUtf8(isValidScenesFolder(settings_.scenesFolder) ? settings_.scenesFolder : std::string("scenes"));
}

bool Project::isValidScenesFolder(const std::string& folder) {
    if (folder.empty() || folder.size() > 200) return false;
    const fs::path p = Paths::fromUtf8(folder);
    if (p.is_absolute() || p.has_root_name() || p.has_root_directory()) return false;
    for (const auto& part : p) {
        const std::string s = Paths::toUtf8(part);
        if (s == ".." || s == ".") return false;
        for (char c : s) if (c && std::strchr(":*?\"<>|", c)) return false;
    }
    // Not into the folders that hold other things.
    const std::string first = Paths::toUtf8(*p.begin());
    return first != "assets" && first != "scripts" && first != "Builds" && first != ".git";
}

bool Project::changeScenesFolder(const std::string& folderIn, bool moveExisting, std::string* error) {
    std::string folder = folderIn;
    std::replace(folder.begin(), folder.end(), '\\', '/');
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    if (!isValidScenesFolder(folder)) {
        if (error) *error = "the scenes folder must be a folder inside the project (not assets, scripts or Builds)";
        return false;
    }
    const fs::path oldFolder = scenesFolder();
    const std::string oldRel = settings_.scenesFolder;
    const fs::path newFolder = root_ / Paths::fromUtf8(folder);
    std::error_code ec;
    if (fs::equivalent(oldFolder, newFolder, ec)) return true;
    fs::create_directories(newFolder, ec);
    if (ec) { if (error) *error = "cannot create " + Paths::toUtf8(newFolder) + ": " + ec.message(); return false; }
    if (moveExisting && fs::is_directory(oldFolder, ec)) {
        // Check every move first, so nothing moves if one would fail.
        std::vector<fs::path> items;
        for (const auto& entry : fs::directory_iterator(oldFolder, ec)) {
            if (fs::exists(newFolder / entry.path().filename(), ec)) {
                if (error) *error = Paths::toUtf8(entry.path().filename()) + " already exists in " + folder;
                return false;
            }
            items.push_back(entry.path());
        }
        for (const fs::path& item : items) {
            fs::rename(item, newFolder / item.filename(), ec);
            if (ec) { if (error) *error = "could not move " + Paths::toUtf8(item.filename()) + ": " + ec.message(); return false; }
        }
        // The start scene moved too.
        const std::string prefix = oldRel + "/";
        if (settings_.startScene.rfind(prefix, 0) == 0) settings_.startScene = folder + "/" + settings_.startScene.substr(prefix.size());
        if (fs::is_empty(oldFolder, ec)) fs::remove(oldFolder, ec);
    }
    settings_.scenesFolder = folder;
    return save(error);
}

std::vector<fs::path> Project::scenes() const {
    std::vector<fs::path> out;
    std::error_code ec;
    if (root_.empty() || !fs::is_directory(scenesFolder(), ec)) return out;
    for (const auto& entry : fs::recursive_directory_iterator(scenesFolder(), ec)) {
        if (entry.is_regular_file(ec) && entry.path().filename() == "scene.json") out.push_back(entry.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

fs::path Project::findRoot(const fs::path& file) {
    std::error_code ec;
    for (fs::path dir = fs::is_directory(file, ec) ? file : file.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        if (fs::exists(dir / "project.json", ec)) return dir;
        if (dir == dir.parent_path()) break;
    }
    return {};
}

namespace {

Instance* part(Scene& s, const char* name, glm::vec3 pos, glm::vec3 size, Color3 color, Material material = Material::Plastic) {
    Instance* p = s.create(ClassId::Part, &s.workspace());
    p->name = name;
    p->position = pos;
    p->size = size;
    p->color = color;
    p->material = material;
    return p;
}

void writeText(const fs::path& file, const std::string& text) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << text;
}

} // namespace

bool Project::create(const fs::path& parent, const std::string& name, Template t, fs::path* outFolder, std::string* error) {
    std::string folderName;
    for (char c : name) folderName += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '_' : c;
    if (folderName.empty()) folderName = "MyGame";
    const fs::path folder = parent / Paths::fromUtf8(folderName);
    std::error_code ec;
    if (fs::exists(folder, ec) && !fs::is_empty(folder, ec)) {
        if (error) *error = Paths::toUtf8(folder) + " already exists and is not empty";
        return false;
    }
    fs::create_directories(folder / "assets" / "textures", ec);
    fs::create_directories(folder / "assets" / "sounds", ec);
    fs::create_directories(folder / "assets" / "models", ec);
    fs::create_directories(folder / "scripts", ec);
    if (ec) { if (error) *error = "cannot create " + Paths::toUtf8(folder) + ": " + ec.message(); return false; }

    Project p;
    p.root_ = fs::absolute(folder);
    p.settings_.name = name.empty() ? "My Game" : name;
    p.settings_.startScene = "scenes/Main/scene.json";
    if (!p.save(error)) return false;

    // The first scene.
    Scene scene;
    if (t != Template::Empty) {
        Instance* base = part(scene, "Baseplate", { 0, -0.5f, 0 }, { 128, 1, 128 }, { 0.36f, 0.38f, 0.40f }, Material::Concrete);
        (void)base;
        Instance* spawn = scene.create(ClassId::SpawnLocation, &scene.workspace());
        spawn->position = { 0, 0.2f, 0 };
    }
    if (t == Template::Obby) {
        const Color3 colors[] = { { 0.95f, 0.35f, 0.35f }, { 0.98f, 0.70f, 0.25f }, { 0.40f, 0.80f, 0.45f }, { 0.35f, 0.60f, 0.98f }, { 0.65f, 0.45f, 0.95f } };
        for (int i = 0; i < 5; ++i) {
            Instance* step = part(scene, "Platform", { 0.0f, 1.0f + i * 1.6f, -8.0f - i * 6.0f }, { 4, 1, 4 }, colors[i]);
            step->material = Material::SmoothPlastic;
        }
        Instance* finish = part(scene, "Finish", { 0.0f, 9.0f, -40.0f }, { 6, 1, 6 }, { 1.0f, 0.84f, 0.2f }, Material::Neon);
        Instance* script = scene.create(ClassId::Script, finish);
        script->name = "FinishScript";
        script->source = "scripts/FinishScript.lua";
        writeText(folder / "scenes" / "Main" / "scripts" / "FinishScript.lua",
            "-- Touching the finish pad wins the game.\n"
            "local pad = script.Parent\n"
            "\n"
            "pad.Touched:Connect(function(hit)\n"
            "    local humanoid = hit.Parent:FindFirstChildOfClass(\"Humanoid\")\n"
            "    if humanoid then\n"
            "        game:Win(\"You made it in \" .. math.floor(time()) .. \" seconds!\")\n"
            "    end\n"
            "end)\n");
    }
    if (!SceneIO::saveScene(scene, folder / "scenes" / "Main" / "scene.json", error)) return false;

    writeText(folder / "scripts" / "README.txt",
        "Lua modules shared by every scene. A file here named Utils.lua is loaded with\n"
        "    local Utils = require(\"Utils\")\n"
        "from any script in the project.\n");
    if (outFolder) *outFolder = p.root_;
    LOG_INFO("Project", "Created project " + Paths::toUtf8(p.root_));
    return true;
}

} // namespace rift
