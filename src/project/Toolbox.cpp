#include "project/Toolbox.h"
#include "core/Paths.h"
#include "scene/Reflection.h"

#include <algorithm>
#include <ctime>
#include <fstream>
#include <set>

namespace rift::Toolbox {

namespace fs = std::filesystem;

namespace {
    bool readJson(const fs::path& file, nlohmann::json& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        out = nlohmann::json::parse(in, nullptr, false);
        return !out.is_discarded() && out.is_object();
    }
    bool writeJson(const fs::path& file, const nlohmann::json& j, std::string* error) {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write " + Paths::toUtf8(file);
            return false;
        }
        out << j.dump(1);
        return static_cast<bool>(out);
    }
    // "assets/x.png#2" -> "assets/x.png"; engine, absolute and generated ids are not project files.
    std::string projectFile(const std::string& stored) {
        if (stored.empty() || stored.rfind(Paths::kEnginePrefix, 0) == 0 || stored.find(':') != std::string::npos || stored[0] == '/' || stored[0] == '\\')
            return {};
        const size_t hash = stored.find('#');
        std::string path = hash == std::string::npos ? stored : stored.substr(0, hash);
        if (path.find("..") != std::string::npos) return {};
        return path;
    }
    ToolboxItem itemFrom(const nlohmann::json& j, const fs::path& folder, bool builtIn) {
        ToolboxItem item;
        item.folder = folder;
        item.builtIn = builtIn;
        item.name = j.value("name", Paths::toUtf8(folder.filename()));
        item.description = j.value("description", std::string());
        item.created = j.value("created", static_cast<int64_t>(0));
        if (j.contains("instances") && j["instances"].is_array()) {
            item.objects = static_cast<int>(j["instances"].size());
            for (const auto& e : j["instances"]) {
                if (!e.contains("parent")) { item.rootClass = e.value("class", std::string()); break; }
            }
        }
        return item;
    }
}

fs::path userFolder() { return Paths::userData() / "Toolbox"; }
fs::path builtInFolder() { return Paths::engineData() / "toolbox"; }

std::vector<ToolboxItem> listFolder(const fs::path& folder, bool builtIn) {
    std::vector<ToolboxItem> items;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (!entry.is_directory(ec)) continue;
        nlohmann::json j;
        if (!readJson(entry.path() / "model.json", j) || j.value("format", std::string()) != "rift-toolbox") continue;
        items.push_back(itemFrom(j, entry.path(), builtIn));
    }
    std::sort(items.begin(), items.end(), [](const ToolboxItem& a, const ToolboxItem& b) { return a.name < b.name; });
    return items;
}

std::vector<ToolboxItem> list() {
    std::vector<ToolboxItem> items;
    try {
        items = listFolder(builtInFolder(), true);
    } catch (const std::exception&) {
        // no engine data folder (tests): no built-in items
    }
    std::vector<ToolboxItem> mine = listFolder(userFolder(), false);
    items.insert(items.end(), mine.begin(), mine.end());
    return items;
}

std::string folderName(const std::string& name) {
    std::string out;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        if (u < 32 || std::string("<>:\"/\\|?*").find(ch) != std::string::npos) continue;
        out += ch;
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(0, 1);
    if (out.size() > 60) out.resize(60);
    return out.empty() ? std::string("Model") : out;
}

std::vector<std::string> filesUsed(const nlohmann::json& snapshot) {
    std::set<std::string> files;
    if (!snapshot.is_array()) return {};
    for (const auto& e : snapshot) {
        const ClassInfo* info = classByName(e.value("class", std::string()));
        if (!info || !e.contains("props") || !e["props"].is_object()) continue;
        for (const auto& [name, value] : e["props"].items()) {
            const PropertyInfo* prop = findProperty(info->id, name);
            if (!prop || prop->type != ValueType::Asset || !value.is_string()) continue;
            const std::string file = projectFile(value.get<std::string>());
            if (!file.empty()) files.insert(file);
        }
    }
    return { files.begin(), files.end() };
}

bool save(const nlohmann::json& snapshot, const std::string& name, const std::string& description, const fs::path& projectRoot,
          ToolboxItem* out, std::string* error, const fs::path& folder) {
    if (!snapshot.is_array() || snapshot.empty()) {
        if (error) *error = "nothing to save";
        return false;
    }
    std::error_code ec;
    fs::create_directories(folder, ec);
    // A folder of its own: "Car", "Car 2", ...
    const std::string base = folderName(name);
    fs::path dir = folder / Paths::fromUtf8(base);
    for (int n = 2; fs::exists(dir, ec); ++n) dir = folder / Paths::fromUtf8(base + " " + std::to_string(n));
    if (!fs::create_directories(dir, ec)) {
        if (error) *error = "cannot create " + Paths::toUtf8(dir) + ": " + ec.message();
        return false;
    }
    // The project files it uses come along.
    std::vector<std::string> copied;
    for (const std::string& file : filesUsed(snapshot)) {
        const fs::path from = projectRoot / Paths::fromUtf8(file);
        if (!fs::is_regular_file(from, ec)) continue;
        const fs::path to = dir / "files" / Paths::fromUtf8(file);
        fs::create_directories(to.parent_path(), ec);
        if (fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec)) copied.push_back(file);
    }
    nlohmann::json j;
    j["format"] = "rift-toolbox";
    j["version"] = 1;
    j["name"] = name.empty() ? base : name;
    j["description"] = description;
    j["created"] = static_cast<int64_t>(std::time(nullptr));
    j["instances"] = snapshot;
    j["files"] = copied;
    if (!writeJson(dir / "model.json", j, error)) {
        fs::remove_all(dir, ec);
        return false;
    }
    if (out) *out = itemFrom(j, dir, false);
    return true;
}

bool load(const ToolboxItem& item, const fs::path& projectRoot, nlohmann::json& snapshot, std::string* error) {
    nlohmann::json j;
    if (!readJson(item.folder / "model.json", j) || !j.contains("instances") || !j["instances"].is_array()) {
        if (error) *error = "cannot read " + Paths::toUtf8(item.folder / "model.json");
        return false;
    }
    snapshot = j["instances"];
    // Files the project does not have yet are copied in (ones it has are kept).
    std::error_code ec;
    if (j.contains("files") && j["files"].is_array() && !projectRoot.empty()) {
        for (const auto& f : j["files"]) {
            if (!f.is_string()) continue;
            const std::string file = projectFile(f.get<std::string>());
            if (file.empty()) continue;
            const fs::path to = projectRoot / Paths::fromUtf8(file);
            if (fs::exists(to, ec)) continue;
            fs::create_directories(to.parent_path(), ec);
            fs::copy_file(item.folder / "files" / Paths::fromUtf8(file), to, fs::copy_options::skip_existing, ec);
        }
    }
    return true;
}

bool remove(const ToolboxItem& item, std::string* error) {
    if (item.builtIn) {
        if (error) *error = "built-in items cannot be deleted";
        return false;
    }
    std::error_code ec;
    fs::remove_all(item.folder, ec);
    if (ec && error) *error = ec.message();
    return !ec;
}

bool rename(ToolboxItem& item, const std::string& name, std::string* error) {
    if (item.builtIn) {
        if (error) *error = "built-in items cannot be renamed";
        return false;
    }
    nlohmann::json j;
    if (!readJson(item.folder / "model.json", j)) {
        if (error) *error = "cannot read " + Paths::toUtf8(item.folder / "model.json");
        return false;
    }
    j["name"] = name;
    if (!writeJson(item.folder / "model.json", j, error)) return false;
    item.name = name;
    return true;
}

} // namespace rift::Toolbox
