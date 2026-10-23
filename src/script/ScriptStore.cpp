#include "script/ScriptStore.h"
#include "core/Paths.h"
#include "core/Log.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace rift {

void ScriptStore::reset(const std::string& sceneDir) {
    root_ = sceneDir;
    entries_.clear();
}

std::string ScriptStore::normalize(const std::string& relPath) {
    // "project:scripts/X.lua" -> a file in the project's shared scripts folder.
    if (relPath.rfind(kProjectPrefix, 0) == 0) {
        const std::string rest = normalize(relPath.substr(std::char_traits<char>::length(kProjectPrefix)));
        return rest.empty() ? std::string() : std::string(kProjectPrefix) + rest;
    }
    std::string p = relPath;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty() || p.front() == '/' || p.find(':') != std::string::npos) return "";
    // Reject "..", "." and empty segments.
    size_t start = 0;
    while (start <= p.size()) {
        const size_t slash = p.find('/', start);
        const std::string seg = p.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == "." || seg == "..") return "";
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return p;
}

bool ScriptStore::isValidPath(const std::string& relPath) {
    const std::string p = normalize(relPath);
    return p.size() > 4 && p.compare(p.size() - 4, 4, ".lua") == 0;
}

fs::path ScriptStore::diskPath(const std::string& relPath) const {
    if (relPath.rfind(kProjectPrefix, 0) == 0) {
        return fs::path(Paths::fromUtf8(projectRoot_)) / Paths::fromUtf8(relPath.substr(std::char_traits<char>::length(kProjectPrefix)));
    }
    return fs::path(Paths::fromUtf8(root_)) / Paths::fromUtf8(relPath);
}

bool ScriptStore::readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open()) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool ScriptStore::writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out << text;
    out.close();
    return !out.fail();
}

bool ScriptStore::has(const std::string& relPath) const {
    return entries_.count(normalize(relPath)) != 0;
}

const ScriptStore::Entry* ScriptStore::find(const std::string& relPath) const {
    auto it = entries_.find(normalize(relPath));
    return it == entries_.end() ? nullptr : &it->second;
}

bool ScriptStore::load(const std::string& relPath) {
    const std::string key = normalize(relPath);
    if (key.empty() || !isValidPath(key)) {
        LOG_WARN("ScriptStore", "Rejected script path '" + relPath + "' (must be a relative .lua path inside the scene folder).");
        return false;
    }
    if (entries_.count(key)) return true;

    Entry e;
    const fs::path p = diskPath(key);
    // An unsaved scene has no folder yet, so there is nothing on disk to read
    // (and a relative path would wrongly resolve against the working directory).
    const bool hasRoot = key.rfind(kProjectPrefix, 0) == 0 ? !projectRoot_.empty() : !root_.empty();
    if (!hasRoot || !readFile(p, e.text)) {
        LOG_WARN("ScriptStore", "Script file not found: " + p.string());
        return false;
    }
    e.diskText = e.text;
    e.onDisk = true;
    std::error_code ec;
    e.mtime = fs::last_write_time(p, ec);
    entries_[key] = std::move(e);
    return true;
}

std::string* ScriptStore::editableText(const std::string& relPath) {
    auto it = entries_.find(normalize(relPath));
    return it == entries_.end() ? nullptr : &it->second.text;
}

void ScriptStore::touch(const std::string& relPath) {
    auto it = entries_.find(normalize(relPath));
    if (it == entries_.end()) return;
    it->second.dirty = !it->second.onDisk || it->second.text != it->second.diskText;
}

bool ScriptStore::create(const std::string& relPath, const std::string& text) {
    const std::string key = normalize(relPath);
    if (key.empty() || !isValidPath(key) || entries_.count(key)) return false;
    Entry e;
    e.text = text;
    e.dirty = true;
    entries_[key] = std::move(e);
    return true;
}

void ScriptStore::forget(const std::string& relPath) {
    entries_.erase(normalize(relPath));
}

std::string ScriptStore::uniquePath(const std::string& baseName) const {
    std::string base = baseName.empty() ? "Script" : baseName;
    // Keep only filename-safe characters.
    for (char& c : base) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) c = '_';
    }
    for (int n = 1; ; ++n) {
        const std::string name = "scripts/" + base + (n == 1 ? "" : std::to_string(n)) + ".lua";
        std::error_code ec;
        if (!entries_.count(name) && (root_.empty() || !fs::exists(diskPath(name), ec))) return name;
    }
}

bool ScriptStore::anyDirty() const {
    for (const auto& [path, e] : entries_) if (e.dirty) return true;
    return false;
}

bool ScriptStore::save(const std::string& relPath) {
    auto it = entries_.find(normalize(relPath));
    if (it == entries_.end()) return false;
    Entry& e = it->second;
    if (root_.empty()) {
        LOG_WARN("ScriptStore", "Cannot save " + it->first + ": the scene has no folder yet.");
        return false;
    }
    const fs::path p = diskPath(it->first);
    if (!writeFile(p, e.text)) {
        LOG_ERROR("ScriptStore", "Failed to write " + p.string());
        return false;
    }
    e.diskText = e.text;
    e.onDisk = true;
    e.dirty = false;
    std::error_code ec;
    e.mtime = fs::last_write_time(p, ec);
    return true;
}

bool ScriptStore::saveAll(const std::string& sceneDir) {
    const bool reroot = !sceneDir.empty() && fs::path(sceneDir) != fs::path(root_);
    if (reroot) root_ = sceneDir;
    bool ok = true;
    for (auto& [path, e] : entries_) {
        if (!reroot && !e.dirty && e.onDisk) continue;
        ok = save(path) && ok;
    }
    return ok;
}

std::vector<std::string> ScriptStore::pollExternalChanges(std::vector<std::string>* conflicts) {
    std::vector<std::string> reloaded;
    for (auto& [path, e] : entries_) {
        if (!e.onDisk) continue;
        std::error_code ec;
        const fs::path p = diskPath(path);
        const auto mtime = fs::last_write_time(p, ec);
        if (ec || mtime == e.mtime) continue;

        std::string text;
        if (!readFile(p, text)) continue;
        e.mtime = mtime;
        if (text == e.diskText) continue; // touched but identical

        e.diskText = text;
        if (e.dirty) { // unsaved edits in the editor: never overwrite them silently
            if (conflicts) conflicts->push_back(path);
            continue;
        }
        e.text = text;
        reloaded.push_back(path);
    }
    return reloaded;
}

std::vector<std::string> ScriptStore::paths() const {
    std::vector<std::string> out;
    out.reserve(entries_.size());
    for (const auto& [path, e] : entries_) out.push_back(path);
    return out;
}

} // namespace rift
