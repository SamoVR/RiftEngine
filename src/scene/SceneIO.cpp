#include "scene/SceneIO.h"
#include "core/Log.h"
#include "core/TransformUtil.h"
#include "core/Version.h"

#include <cctype>
#include <fstream>
#include <functional>
#include <sstream>
#include <unordered_map>

namespace rift::SceneIO {

using nlohmann::json;

// ============================================================================
//  Instances
// ============================================================================

json instanceToJson(const Instance& inst) {
    json j;
    j["id"] = idToString(inst.id);
    j["class"] = classInfo(inst.cls).name;
    if (!inst.isService()) j["name"] = inst.name;

    json props = json::object();
    for (const PropertyInfo& p : classInfo(inst.cls).props) {
        if (!p.has(PF_Serialize) || p.name == "Name") continue;
        const Value v = p.get(inst);
        if (valuesEqual(v, defaultValue(inst.cls, p))) continue;
        props[p.name] = toJson(v);
    }
    if (!props.empty()) j["props"] = std::move(props);

    if (!inst.attributes.empty()) {
        json attrs = json::object();
        for (const Attribute& a : inst.attributes) attrs[a.name] = toTaggedJson(a.value);
        j["attrs"] = std::move(attrs);
    }
    if (!inst.tags.empty()) j["tags"] = inst.tags;
    return j;
}

void applyInstanceJson(Instance& inst, const json& j, std::vector<std::string>* warnings) {
    if (!inst.isService() && j.contains("name") && j["name"].is_string()) inst.name = j["name"].get<std::string>();
    if (j.contains("props") && j["props"].is_object()) {
        for (auto it = j["props"].begin(); it != j["props"].end(); ++it) {
            const PropertyInfo* p = findProperty(inst.cls, it.key());
            if (!p) {
                if (warnings) warnings->push_back(classInfo(inst.cls).name + " has no property '" + it.key() + "' (ignored)");
                continue;
            }
            const Value v = fromJson(it.value(), p->type);
            std::string error;
            if (typeOf(v) == ValueType::None || !setProperty(inst, *p, v, &error)) {
                if (warnings) warnings->push_back(inst.name + "." + p->name + ": unreadable value (ignored)");
            }
        }
    }
    if (j.contains("attrs") && j["attrs"].is_object()) {
        for (auto it = j["attrs"].begin(); it != j["attrs"].end(); ++it) {
            const Value v = fromTaggedJson(it.value());
            if (typeOf(v) != ValueType::None) inst.setAttribute(it.key(), v);
        }
    }
    if (j.contains("tags") && j["tags"].is_array()) {
        for (const auto& t : j["tags"]) if (t.is_string()) inst.addTag(t.get<std::string>());
    }
}

json subtreesToJson(const std::vector<const Instance*>& roots) {
    json out = json::array();
    std::function<void(const Instance&, const Instance*)> add = [&](const Instance& inst, const Instance* parent) {
        if (!inst.archivable && parent) return;
        json j = instanceToJson(inst);
        if (parent) j["parent"] = idToString(parent->id);
        out.push_back(std::move(j));
        for (const Instance* c : inst.children) add(*c, &inst);
    };
    for (const Instance* r : roots) if (r && !r->isService()) add(*r, nullptr);
    return out;
}

std::vector<Instance*> subtreesFromJson(Scene& scene, const json& j, Instance* parent, bool keepIds, std::vector<std::string>* warnings) {
    std::vector<Instance*> roots;
    if (!j.is_array()) return roots;
    std::unordered_map<uint64_t, Instance*> byFileId;
    for (const json& e : j) {
        if (!e.is_object() || !e.contains("class") || !e["class"].is_string()) continue;
        const ClassInfo* ci = classByName(e["class"].get<std::string>());
        if (!ci || ci->service) {
            if (warnings) warnings->push_back("Unknown class '" + e["class"].get<std::string>() + "' (skipped)");
            continue;
        }
        const uint64_t fileId = e.contains("id") && e["id"].is_string() ? idFromString(e["id"].get<std::string>()) : 0;
        Instance* inst = scene.create(ci->id, nullptr, keepIds ? fileId : 0);
        if (!inst) continue;
        applyInstanceJson(*inst, e, warnings);
        if (fileId) byFileId[fileId] = inst;

        Instance* target = parent;
        bool isRoot = true;
        if (e.contains("parent") && e["parent"].is_string()) {
            auto it = byFileId.find(idFromString(e["parent"].get<std::string>()));
            if (it != byFileId.end()) { target = it->second; isRoot = false; }
        }
        if (target) scene.setParent(inst, target);
        if (isRoot) roots.push_back(inst);
    }
    return roots;
}

// ============================================================================
//  Scenes (format 4)
// ============================================================================

json sceneToJson(const Scene& scene) {
    json j;
    j["format"] = kFormatVersion;
    j["rift"] = RIFT_VERSION_STRING;
    json list = json::array();
    for (Instance* s : scene.services()) list.push_back(instanceToJson(*s));
    for (Instance* s : scene.services()) {
        std::vector<const Instance*> roots(s->children.begin(), s->children.end());
        json sub = subtreesToJson(roots);
        for (json& e : sub) {
            if (!e.contains("parent")) e["parent"] = idToString(s->id);
            list.push_back(std::move(e));
        }
    }
    j["instances"] = std::move(list);
    return j;
}

namespace {

LoadResult loadFormat4(Scene& scene, const json& j) {
    LoadResult r;
    r.formatVersion = 4;
    if (!j.contains("instances") || !j["instances"].is_array()) { r.error = "the file has no 'instances' list"; return r; }

    std::unordered_map<uint64_t, Instance*> byFileId;
    struct Pending { Instance* child; uint64_t parentId; };
    std::vector<Pending> pending;

    for (const json& e : j["instances"]) {
        if (!e.is_object() || !e.contains("class") || !e["class"].is_string()) { r.warnings.push_back("Skipped a malformed instance entry"); continue; }
        const std::string cls = e["class"].get<std::string>();
        const ClassInfo* ci = classByName(cls);
        if (!ci) { r.warnings.push_back("Unknown class '" + cls + "' (skipped with its children)"); continue; }
        const uint64_t fileId = e.contains("id") && e["id"].is_string() ? idFromString(e["id"].get<std::string>()) : 0;

        if (ci->service) {
            Instance* s = scene.service(ci->id);
            scene.reassignId(s, fileId); // stable ids: save -> load -> save gives the same file
            applyInstanceJson(*s, e, &r.warnings);
            if (fileId) byFileId[fileId] = s;
            continue;
        }
        Instance* inst = scene.create(ci->id, nullptr, fileId);
        applyInstanceJson(*inst, e, &r.warnings);
        if (fileId) byFileId[fileId] = inst;
        const uint64_t parentId = e.contains("parent") && e["parent"].is_string() ? idFromString(e["parent"].get<std::string>()) : 0;
        auto it = byFileId.find(parentId);
        if (it != byFileId.end()) scene.setParent(inst, it->second);
        else pending.push_back({ inst, parentId });
    }
    // A child listed before its parent (hand-edited file): resolve now.
    for (const Pending& p : pending) {
        auto it = byFileId.find(p.parentId);
        if (it != byFileId.end() && scene.setParent(p.child, it->second)) continue;
        if (p.parentId != 0) r.warnings.push_back("'" + p.child->name + "' has a missing parent; it was moved to Workspace");
        scene.setParent(p.child, p.child->isGui() ? scene.service(ClassId::StarterGui)
                               : p.child->isScript() && !p.child->parent ? scene.service(ClassId::ScriptService)
                               : &scene.workspace());
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------------------
//  Format 3 (RIFT 0.8): a flat "objects" list of Part/Script. Parent scale
//  used to be inherited, so world transforms are recomputed and re-split
//  into the new frame + Size model.
// ---------------------------------------------------------------------------
glm::vec3 vec3Of(const json& j, const glm::vec3& fallback) {
    if (j.is_array() && j.size() == 3 && j[0].is_number() && j[1].is_number() && j[2].is_number())
        return { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
    return fallback;
}

LoadResult loadFormat3(Scene& scene, const json& j) {
    LoadResult r;
    r.formatVersion = 3;
    if (!j.contains("objects") || !j["objects"].is_array()) { r.error = "the file has no 'objects' list"; return r; }

    struct Old {
        const json* data;
        Instance* inst = nullptr;
        uint64_t id = 0, parentId = 0;
        glm::mat4 local{ 1.0f };
        bool isScript = false;
    };
    std::vector<Old> objects;
    std::unordered_map<uint64_t, size_t> index;
    for (const json& e : j["objects"]) {
        if (!e.is_object()) continue;
        Old o;
        o.data = &e;
        o.id = e.contains("id") && e["id"].is_string() ? idFromString(e["id"].get<std::string>()) : 0;
        o.parentId = e.contains("parent") && e["parent"].is_string() ? idFromString(e["parent"].get<std::string>()) : 0;
        o.isScript = e.value("class", std::string("Part")) == "Script";
        if (!o.isScript) {
            o.local = TransformUtil::compose(vec3Of(e.value("position", json()), glm::vec3(0.0f)),
                                             vec3Of(e.value("rotation", json()), glm::vec3(0.0f)),
                                             vec3Of(e.value("scale", json()), glm::vec3(1.0f)));
        }
        if (o.id) index[o.id] = objects.size();
        objects.push_back(o);
    }

    // Old world matrices (scale inherited, as the old renderer did).
    std::function<glm::mat4(size_t, int)> oldWorld = [&](size_t i, int depth) -> glm::mat4 {
        const Old& o = objects[i];
        auto it = index.find(o.parentId);
        if (it == index.end() || depth > 64 || objects[it->second].isScript) return o.local;
        return oldWorld(it->second, depth + 1) * o.local;
    };

    // Create every instance first.
    for (Old& o : objects) {
        const json& e = *o.data;
        const std::string name = e.value("name", std::string("Part"));
        if (o.isScript) {
            o.inst = scene.create(ClassId::Script, nullptr, o.id);
            o.inst->name = name;
            o.inst->source = e.value("source", std::string());
            o.inst->enabled = e.value("enabled", true);
            continue;
        }
        std::string lower = name;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        const bool spawn = lower == "spawnpoint" || lower == "spawn" || lower == "spawnlocation";
        o.inst = scene.create(spawn ? ClassId::SpawnLocation : ClassId::Part, nullptr, o.id);
        Instance& p = *o.inst;
        p.name = name;
        const std::string mesh = e.value("mesh", std::string("cube"));
        p.shape = mesh == "sphere" ? PartShape::Ball : mesh == "triangle" ? PartShape::Wedge : PartShape::Block;
        p.color = Color3::from(vec3Of(e.value("color", json()), glm::vec3(1.0f)));
        if (e.contains("texture") && e["texture"].is_string()) {
            p.texture.path = e["texture"].get<std::string>();
            p.textureScale = glm::vec2(0.0f); // the old renderer stretched textures over each face
            p.color = Color3(1.0f, 1.0f, 1.0f); // textures replaced the colour before
        }
        if (e.contains("game") && e["game"].is_object()) {
            const json& g = e["game"];
            p.anchored = g.value("anchored", true);
            p.canCollide = g.value("canCollide", true);
            p.visible = g.value("visible", true);
            p.elasticity = g.value("bounciness", 0.0f);
        }
        if (mesh == "custom") r.warnings.push_back("'" + name + "' used a custom mesh, which format 4 stores as a MeshPart asset; it was replaced by a block");
    }

    // Parent links, then transforms (parents are placed before their children).
    for (Old& o : objects) {
        auto it = index.find(o.parentId);
        Instance* parent = it != index.end() ? objects[it->second].inst : nullptr;
        if (parent && parent->isScript()) parent = nullptr;
        if (!parent || !scene.setParent(o.inst, parent)) {
            scene.setParent(o.inst, o.isScript ? scene.service(ClassId::ScriptService) : &scene.workspace());
        }
    }
    std::function<void(Instance&)> place = [&](Instance& inst) {
        auto it = index.find(inst.id);
        if (it != index.end() && !objects[it->second].isScript) inst.setWorldTransform(oldWorld(it->second, 0));
        for (Instance* c : inst.children) place(*c);
    };
    for (Instance* c : std::vector<Instance*>(scene.workspace().children)) place(*c);

    // Old scenes had no lighting / spawn settings: keep the old look (flat
    // ambient-heavy light, no shadows) close to what they were built with.
    scene.lighting().castShadow = true;
    r.ok = true;
    return r;
}

} // namespace

LoadResult sceneFromJson(Scene& scene, const json& j) {
    if (!j.is_object()) return { false, "not a scene file (expected a JSON object)" };
    const int format = j.value("format", 0);
    if (format == 3 || format == 4) {
        scene.clear(); // the file replaces everything, the default containers included
        LoadResult r = format == 4 ? loadFormat4(scene, j) : loadFormat3(scene, j);
        if (r.ok) scene.ensureDefaults(); // older files predate StarterPlayerScripts & co.
        return r;
    }
    if (format > 4) return { false, "this scene was saved by a newer version of RIFT (format " + std::to_string(format) + ")" };
    return { false, "unsupported scene format " + std::to_string(format) };
}

std::string sceneToText(const Scene& scene) {
    const json j = sceneToJson(scene);
    std::ostringstream out;
    out << "{\n  \"format\": " << j["format"].dump() << ",\n  \"rift\": " << j["rift"].dump() << ",\n  \"instances\": [\n";
    const json& list = j["instances"];
    for (size_t i = 0; i < list.size(); ++i) out << "    " << list[i].dump() << (i + 1 < list.size() ? ",\n" : "\n");
    out << "  ]\n}\n";
    return out.str();
}

bool saveScene(const Scene& scene, const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    // Write to a temporary file first and swap it in: a crash or full disk
    // mid-write must never leave a half-written scene behind.
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) { if (error) *error = "cannot write " + file.string(); return false; }
        out << sceneToText(scene);
        out.close();
        if (out.fail()) { if (error) *error = "error while writing " + file.string(); std::filesystem::remove(tmp, ec); return false; }
    }
    std::filesystem::rename(tmp, file, ec);
    if (ec) {
        // rename over an existing file can fail on some file systems; copy instead.
        std::filesystem::copy_file(tmp, file, std::filesystem::copy_options::overwrite_existing, ec);
        std::filesystem::remove(tmp, ec);
        if (ec) { if (error) *error = "cannot replace " + file.string() + ": " + ec.message(); return false; }
    }
    // The terrain's voxels: a binary file next to the scene (none when there is no terrain).
    const std::filesystem::path terrainFile = file.parent_path() / "terrain.bin";
    if (scene.terrain().empty()) {
        std::filesystem::remove(terrainFile, ec);
    } else {
        std::filesystem::path terrainTmp = terrainFile;
        terrainTmp += ".tmp";
        std::string terrainError;
        if (!scene.terrain().save(terrainTmp, &terrainError)) { if (error) *error = terrainError; std::filesystem::remove(terrainTmp, ec); return false; }
        std::filesystem::rename(terrainTmp, terrainFile, ec);
        if (ec) {
            std::filesystem::copy_file(terrainTmp, terrainFile, std::filesystem::copy_options::overwrite_existing, ec);
            std::filesystem::remove(terrainTmp, ec);
        }
    }
    return true;
}

LoadResult loadScene(Scene& scene, const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in.is_open()) return { false, "cannot open " + file.string() };
    json j;
    try {
        in >> j;
    } catch (const json::exception& e) {
        return { false, std::string("not valid JSON: ") + e.what() };
    }
    LoadResult r;
    try {
        r = sceneFromJson(scene, j);
    } catch (const std::exception& e) {
        return { false, std::string("malformed scene: ") + e.what() };
    }
    if (!r.ok) return r;
    std::error_code ec;
    const std::filesystem::path terrainFile = file.parent_path() / "terrain.bin";
    if (std::filesystem::exists(terrainFile, ec)) {
        std::string error;
        if (!scene.terrain().load(terrainFile, &error)) r.warnings.push_back("terrain.bin: " + error + " (the terrain was not loaded)");
    }
    return r;
}

} // namespace rift::SceneIO
