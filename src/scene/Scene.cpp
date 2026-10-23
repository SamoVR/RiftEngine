#include "scene/Scene.h"
#include "core/Log.h"
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>

Scene::Scene() {}

void Scene::addObject(std::unique_ptr<Object> object) {
    if (object) objects_.push_back(std::move(object));
}

void Scene::addInternalObject(std::unique_ptr<Object> object) {
    if (object) internalObjects_.push_back(std::move(object));
}

void Scene::detachFromHierarchy(Object* object) {
    if (!object) return;

    object->removeParent();

    // Children are detached, not deleted: silently destroying everything
    // inside a "group" as a side effect of deleting the group would be a
    // nasty surprise in an editor.
    for (Object* child : std::vector<Object*>(object->children)) { // copy: removeChild mutates the list
        object->removeChild(child);
    }
}

bool Scene::removeObjectAt(size_t index) {
    if (index >= objects_.size()) {
        LOG_WARN("Scene", "removeObjectAt: index " + std::to_string(index) + " out of range");
        return false;
    }
    detachFromHierarchy(objects_[index].get());
    objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool Scene::removeObject(Object* object) {
    if (!object) return false;

    auto it = std::find_if(objects_.begin(), objects_.end(),
        [object](const std::unique_ptr<Object>& p) { return p.get() == object; });

    if (it == objects_.end()) {
        LOG_WARN("Scene", "removeObject: object not found in scene");
        return false;
    }
    detachFromHierarchy(object);
    objects_.erase(it);
    return true;
}

void Scene::clear() {
    objects_.clear(); // unique_ptr destructors free everything
}

void Scene::clearAll() {
    objects_.clear();
    internalObjects_.clear();
}

bool Scene::contains(const Object* object) const {
    return indexOf(object) != npos;
}

size_t Scene::indexOf(const Object* object) const {
    for (size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].get() == object) return i;
    }
    return npos;
}

std::unique_ptr<Object> Scene::extractObject(Object* object, size_t* outIndex) {
    const size_t index = indexOf(object);
    if (index == npos) return nullptr;
    if (outIndex) *outIndex = index;
    detachFromHierarchy(object);
    std::unique_ptr<Object> owned = std::move(objects_[index]);
    objects_.erase(objects_.begin() + static_cast<std::ptrdiff_t>(index));
    return owned;
}

std::vector<std::unique_ptr<Object>> Scene::extractSubtree(Object* root) {
    std::vector<std::unique_ptr<Object>> out;
    if (!root || !contains(root)) return out;
    std::vector<Object*> nodes;
    std::vector<Object*> stack{ root };
    while (!stack.empty()) {
        Object* n = stack.back();
        stack.pop_back();
        nodes.push_back(n);
        for (Object* c : n->children) stack.push_back(c);
    }
    root->removeParent(); // cut only the link to the outside
    for (Object* n : nodes) {
        auto it = std::find_if(objects_.begin(), objects_.end(),
            [n](const std::unique_ptr<Object>& p) { return p.get() == n; });
        if (it == objects_.end()) continue;
        out.push_back(std::move(*it));
        objects_.erase(it);
    }
    return out;
}

void Scene::insertObjectAt(size_t index, std::unique_ptr<Object> object) {
    if (!object) return;
    index = std::min(index, objects_.size());
    objects_.insert(objects_.begin() + static_cast<std::ptrdiff_t>(index), std::move(object));
}

nlohmann::json Scene::buildJson() const {
    nlohmann::json j;
    j["objects"] = nlohmann::json::array();
    for (const auto& obj : objects_) j["objects"].push_back(obj->toJSON());
    return j;
}

std::string Scene::toJsonString() const {
    return buildJson().dump();
}

std::vector<Object*> Scene::getObjects() const {
    std::vector<Object*> view;
    view.reserve(objects_.size());
    for (const auto& ptr : objects_) view.push_back(ptr.get());
    return view;
}

std::vector<Object*> Scene::getInternalObjects() const {
    std::vector<Object*> view;
    view.reserve(internalObjects_.size());
    for (const auto& ptr : internalObjects_) view.push_back(ptr.get());
    return view;
}

Object* Scene::findByName(const std::string& name) const {
    for (const auto& ptr : objects_) {
        if (ptr->name == name) return ptr.get();
    }
    return nullptr;
}

Object* Scene::findById(uint64_t id) const {
    for (const auto& ptr : objects_) {
        if (ptr->id == id) return ptr.get();
    }
    return nullptr;
}

bool Scene::saveToFile(const std::string& filename) const {
    const nlohmann::json j = buildJson();

    std::error_code dirError;
    const std::filesystem::path parentDir = std::filesystem::path(filename).parent_path();
    if (!parentDir.empty()) std::filesystem::create_directories(parentDir, dirError);

    std::ofstream outFile(filename);
    if (!outFile.is_open()) {
        LOG_ERROR("Scene", "Failed to open file for saving: " + filename);
        return false;
    }
    // One object per line: small, and still readable / diff-friendly.
    std::string text = "{\n  \"format\": 3,\n  \"objects\": [\n";
    const auto& objects = j["objects"];
    for (size_t i = 0; i < objects.size(); ++i) {
        text += "    " + objects[i].dump() + (i + 1 < objects.size() ? ",\n" : "\n");
    }
    text += "  ]\n}\n";
    outFile << text;
    outFile.close();
    if (outFile.fail()) {
        LOG_ERROR("Scene", "Error while writing scene file: " + filename);
        return false;
    }
    LOG_INFO("Scene", "Saved " + std::to_string(objects_.size()) + " object(s) to " + filename);
    return true;
}

bool Scene::loadFromFile(const std::string& filename, Texture* sharedTexture) {
    std::ifstream inFile(filename);
    if (!inFile.is_open()) {
        LOG_ERROR("Scene", "Failed to open file for loading: " + filename);
        return false;
    }

    nlohmann::json j;
    try {
        inFile >> j;
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Scene", "Failed to parse scene file '" + filename + "': " + e.what());
        return false;
    }

    if (!j.is_object() || j.value("format", 0) != 3) {
        LOG_ERROR("Scene", "'" + filename + "' is not a format 3 scene (this build only reads format 3).");
        return false;
    }
    if (!j.contains("objects") || !j["objects"].is_array()) {
        LOG_ERROR("Scene", "Scene file '" + filename + "' has no valid 'objects' array; nothing loaded.");
        return false;
    }

    // Build the whole new object list first and only swap it in once parsing
    // has succeeded, so a malformed file can no longer wipe the current scene.
    std::vector<std::unique_ptr<Object>> loaded;
    std::unordered_map<uint64_t, Object*> idToObject;
    std::vector<std::pair<Object*, uint64_t>> pendingParents; // (child, parent id)

    try {
        for (const auto& objJson : j["objects"]) {
            if (!objJson.is_object() || !objJson.contains("name")) {
                LOG_WARN("Scene", "Skipping an object entry with no 'name' in " + filename);
                continue;
            }

            auto obj = std::make_unique<Object>(objJson["name"].get<std::string>(), std::vector<Vertex>(), nullptr);
            obj->fromJSON(objJson);

            // Ids must be unique; a hand-edited or merged file may repeat one.
            if (idToObject.count(obj->id) != 0) {
                LOG_WARN("Scene", "Duplicate object id for '" + obj->name + "' in " + filename + " -- assigned a new id.");
                obj->id = Object::newId();
            }

            // Shared (non-owned) texture: re-attach instead of reloading.
            if (obj->kind == Object::Kind::Part && objJson.contains("texture") && !objJson.value("textureOwned", true)) {
                const std::string savedPath = objJson["texture"].get<std::string>();
                if (sharedTexture && sharedTexture->isValid() && sharedTexture->getFilePath() == Texture::normalizePath(savedPath)) {
                    obj->setTexture(sharedTexture);
                } else {
                    obj->setOwnedTexture(std::make_unique<Texture>(savedPath));
                }
            }

            idToObject[obj->id] = obj.get();
            if (objJson.contains("parent") && objJson["parent"].is_string()) {
                pendingParents.emplace_back(obj.get(), Object::idFromString(objJson["parent"].get<std::string>()));
            }
            loaded.push_back(std::move(obj));
        }
    } catch (const nlohmann::json::exception& e) {
        LOG_ERROR("Scene", "Malformed scene file '" + filename + "': " + e.what() + " -- current scene left untouched.");
        return false;
    }

    // Parents are resolved after every object exists (a child may appear
    // before its parent in the file).
    for (const auto& link : pendingParents) {
        auto parentIt = idToObject.find(link.second);
        if (parentIt == idToObject.end()) {
            LOG_WARN("Scene", "'" + link.first->name + "' references a missing parent -- left unparented.");
            continue;
        }
        link.first->setParent(parentIt->second); // rejects cycles
    }

    objects_ = std::move(loaded); // old objects freed here
    LOG_INFO("Scene", "Loaded " + std::to_string(objects_.size()) + " object(s) from " + filename);
    return true;
}
