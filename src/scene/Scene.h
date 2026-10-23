#pragma once
//
// Scene.h -- owns a tree of Instances (Roblox's DataModel).
//
// The root of the tree is a fixed set of services (Workspace, Lighting,
// ReplicatedStorage, ...). Everything else lives under one of them, or is
// "detached" (created by a script and not parented yet, like Parent = nil).
//
// Play mode runs on a clone() of the edited scene: the game can change or
// destroy anything, and Stop simply throws the clone away.
//
#include "scene/Instance.h"
#include "scene/Reflection.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rift {

constexpr int kServiceCount = static_cast<int>(ClassId::SoundService) + 1;

// Notified about changes made through the Scene API (scripts use these for
// ChildAdded / Destroying / Changed; the editor for refreshes).
class SceneObserver {
public:
    virtual ~SceneObserver() = default;
    virtual void onInstanceAdded(Instance&) {}                       // created in, or moved into, the tree
    virtual void onInstanceRemoving(Instance&) {}                    // about to be destroyed (instance and each descendant)
    virtual void onParentChanged(Instance&, Instance* /*oldParent*/) {}
    virtual void onPropertyChanged(Instance&, const PropertyInfo&) {}
    virtual void onAttributeChanged(Instance&, const std::string& /*name*/) {}
};

class Scene {
public:
    static constexpr size_t npos = static_cast<size_t>(-1);

    Scene();
    ~Scene();
    Scene(const Scene&) = delete;
    Scene& operator=(const Scene&) = delete;

    // Deep copy with the same ids (Play mode).
    std::unique_ptr<Scene> clone() const;

    // ---- services
    Instance* service(ClassId cls) const;
    Instance& workspace() const { return *services_[static_cast<int>(ClassId::Workspace)]; }
    Instance& lighting() const { return *services_[static_cast<int>(ClassId::Lighting)]; }
    Instance& starterPlayer() const { return *services_[static_cast<int>(ClassId::StarterPlayer)]; }
    const std::array<Instance*, kServiceCount>& services() const { return services_; }

    // ---- create / destroy
    // A new instance with its class defaults. `parent` == nullptr leaves it
    // detached. `id` 0 picks a fresh id; a used id is replaced by a fresh one.
    Instance* create(ClassId cls, Instance* parent = nullptr, uint64_t id = 0);
    // Copies `source` (from this or another scene) with its archivable
    // descendants, detached, with new ids (or the same ids if keepIds and free).
    Instance* cloneSubtree(const Instance& source, bool keepIds = false);
    // Destroys the instance and its descendants. Services cannot be destroyed.
    void destroy(Instance* instance);
    // Destroys everything except the services (and resets nothing else).
    void clear();

    // ---- hierarchy
    // Moves `instance` under `newParent` at sibling position `index` (npos =
    // last). nullptr detaches it. Refuses cycles and moving services.
    bool setParent(Instance* instance, Instance* newParent, size_t index = npos, std::string* error = nullptr);
    bool canParent(const Instance* instance, const Instance* newParent, std::string* error = nullptr) const;
    bool isInTree(const Instance* instance) const;          // under a service (not detached)
    Instance* serviceOf(const Instance* instance) const;    // the service at the top of its branch
    size_t siblingIndex(const Instance* instance) const;    // position in parent's children (npos for roots)

    // ---- lookup / iteration
    Instance* find(uint64_t id) const;
    // Gives an instance another id (loaders keep the ids stored in files,
    // including the services'). Fails if the id is 0 or taken.
    bool reassignId(Instance* instance, uint64_t newId);
    size_t size() const { return instances_.size(); }
    // Every instance, detached ones included (no particular order).
    void forEach(const std::function<void(Instance&)>& fn) const;
    // Depth-first over the attached tree, services first, siblings in order.
    void forEachInTree(const std::function<void(Instance&)>& fn) const;
    static void forEachDescendant(Instance& root, const std::function<void(Instance&)>& fn); // excludes root

    // ---- property / attribute changes with notification
    bool setProperty(Instance& instance, const PropertyInfo& prop, const Value& value, std::string* error = nullptr);
    bool setProperty(Instance& instance, const std::string& propName, const Value& value, std::string* error = nullptr);
    void setAttribute(Instance& instance, const std::string& name, const Value& value);
    // For code that writes fields directly (gizmo, physics) and wants listeners told.
    void notifyPropertyChanged(Instance& instance, const PropertyInfo& prop);

    void addObserver(SceneObserver* observer);
    void removeObserver(SceneObserver* observer);

    // Incremented by every change made through this API (cheap dirty checks).
    uint64_t revision() const { return revision_; }
    void bumpRevision() { ++revision_; }

private:
    std::unordered_map<uint64_t, std::unique_ptr<Instance>> instances_;
    std::array<Instance*, kServiceCount> services_{};
    std::vector<SceneObserver*> observers_;
    uint64_t revision_ = 0;

    Instance* adopt(std::unique_ptr<Instance> instance);
    void detachFromParent(Instance* instance);
    void destroyRecursive(Instance* instance);
};

} // namespace rift
