#include "scene/Scene.h"

#include <algorithm>

namespace rift {

Scene::Scene() {
    for (int i = 0; i < kServiceCount; ++i) {
        auto service = std::make_unique<Instance>();
        service->cls = static_cast<ClassId>(i);
        service->id = newInstanceId();
        service->scene = this;
        applyClassDefaults(*service);
        services_[i] = service.get();
        instances_.emplace(service->id, std::move(service));
    }
}

Scene::~Scene() = default;

std::unique_ptr<Scene> Scene::clone() const {
    auto copy = std::make_unique<Scene>();
    // Drop the fresh services; they are replaced by copies of ours.
    copy->instances_.clear();

    std::unordered_map<const Instance*, Instance*> map;
    map.reserve(instances_.size());
    for (const auto& [id, original] : instances_) {
        auto c = std::make_unique<Instance>(*original);
        c->scene = copy.get();
        map[original.get()] = c.get();
        copy->instances_.emplace(id, std::move(c));
    }
    for (auto& [id, c] : copy->instances_) {
        if (c->parent) c->parent = map.at(c->parent);
        for (Instance*& child : c->children) child = map.at(child);
    }
    for (int i = 0; i < kServiceCount; ++i) copy->services_[i] = map.at(services_[i]);
    copy->revision_ = revision_;
    return copy;
}

Instance* Scene::service(ClassId cls) const {
    const int i = static_cast<int>(cls);
    return i < kServiceCount ? services_[i] : nullptr;
}

Instance* Scene::adopt(std::unique_ptr<Instance> instance) {
    if (instance->id == 0 || instances_.count(instance->id)) instance->id = newInstanceId();
    while (instances_.count(instance->id)) instance->id = newInstanceId();
    instance->scene = this;
    Instance* raw = instance.get();
    instances_.emplace(raw->id, std::move(instance));
    ++revision_;
    return raw;
}

Instance* Scene::create(ClassId cls, Instance* parent, uint64_t id) {
    if (static_cast<int>(cls) < kServiceCount) return nullptr; // services are fixed
    auto instance = std::make_unique<Instance>();
    instance->cls = cls;
    instance->id = id;
    applyClassDefaults(*instance);
    Instance* raw = adopt(std::move(instance));
    if (parent && !setParent(raw, parent)) {
        destroy(raw);
        return nullptr;
    }
    return raw;
}

Instance* Scene::cloneSubtree(const Instance& source, bool keepIds) {
    if (source.isService() || !source.archivable) return nullptr;
    std::function<Instance*(const Instance&)> copyNode = [&](const Instance& src) -> Instance* {
        auto c = std::make_unique<Instance>(src);
        c->parent = nullptr;
        c->children.clear();
        c->id = keepIds ? src.id : 0;
        Instance* raw = adopt(std::move(c));
        for (const Instance* child : src.children) {
            if (!child->archivable) continue;
            if (Instance* childCopy = copyNode(*child)) {
                childCopy->parent = raw;
                raw->children.push_back(childCopy);
            }
        }
        return raw;
    };
    return copyNode(source);
}

void Scene::detachFromParent(Instance* instance) {
    if (Instance* p = instance->parent) {
        auto& siblings = p->children;
        siblings.erase(std::remove(siblings.begin(), siblings.end(), instance), siblings.end());
        instance->parent = nullptr;
    }
}

void Scene::destroyRecursive(Instance* instance) {
    for (SceneObserver* o : std::vector<SceneObserver*>(observers_)) o->onInstanceRemoving(*instance);
    const std::vector<Instance*> kids = instance->children; // observers may not change the tree, but be safe
    for (Instance* child : kids) {
        child->parent = nullptr;
        destroyRecursive(child);
    }
    instances_.erase(instance->id); // frees it
}

void Scene::destroy(Instance* instance) {
    if (!instance || instance->isService() || instance->scene != this) return;
    detachFromParent(instance);
    destroyRecursive(instance);
    ++revision_;
}

void Scene::clear() {
    for (Instance* s : services_) {
        const std::vector<Instance*> kids = s->children;
        for (Instance* child : kids) destroy(child);
    }
    // Detached leftovers (script-created instances never parented).
    std::vector<Instance*> loose;
    for (auto& [id, inst] : instances_) if (!inst->isService() && !inst->parent) loose.push_back(inst.get());
    for (Instance* inst : loose) destroy(inst);
}

bool Scene::canParent(const Instance* instance, const Instance* newParent, std::string* error) const {
    auto fail = [&](const char* msg) { if (error) *error = msg; return false; };
    if (!instance) return fail("no instance");
    if (instance->isService()) return fail("services cannot be moved");
    if (newParent == instance) return fail("an instance cannot be its own parent");
    if (newParent && newParent->isDescendantOf(instance)) return fail("an instance cannot be parented under one of its descendants");
    if (newParent && newParent->scene != this) return fail("the new parent belongs to another scene");
    return true;
}

bool Scene::setParent(Instance* instance, Instance* newParent, size_t index, std::string* error) {
    if (!canParent(instance, newParent, error)) return false;
    Instance* oldParent = instance->parent;
    const bool wasInTree = isInTree(instance);

    detachFromParent(instance);
    if (newParent) {
        auto& siblings = newParent->children;
        if (index >= siblings.size()) siblings.push_back(instance);
        else siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(index), instance);
        instance->parent = newParent;
    }
    ++revision_;

    if (oldParent != newParent) {
        for (SceneObserver* o : std::vector<SceneObserver*>(observers_)) o->onParentChanged(*instance, oldParent);
    }
    // Entering the tree (from detached) counts as being added, for it and its descendants.
    if (!wasInTree && isInTree(instance)) {
        std::vector<Instance*> entered{ instance };
        forEachDescendant(*instance, [&](Instance& d) { entered.push_back(&d); });
        for (Instance* e : entered) {
            for (SceneObserver* o : std::vector<SceneObserver*>(observers_)) o->onInstanceAdded(*e);
        }
    }
    return true;
}

bool Scene::isInTree(const Instance* instance) const {
    if (!instance) return false;
    const Instance* top = instance;
    while (top->parent) top = top->parent;
    return top->isService() && top->scene == this;
}

Instance* Scene::serviceOf(const Instance* instance) const {
    if (!instance) return nullptr;
    const Instance* top = instance;
    while (top->parent) top = top->parent;
    return top->isService() ? const_cast<Instance*>(top) : nullptr;
}

size_t Scene::siblingIndex(const Instance* instance) const {
    if (!instance || !instance->parent) return npos;
    const auto& siblings = instance->parent->children;
    auto it = std::find(siblings.begin(), siblings.end(), instance);
    return it == siblings.end() ? npos : static_cast<size_t>(it - siblings.begin());
}

Instance* Scene::find(uint64_t id) const {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : it->second.get();
}

bool Scene::reassignId(Instance* instance, uint64_t newId) {
    if (!instance || newId == 0 || instance->scene != this) return false;
    if (newId == instance->id) return true;
    if (instances_.count(newId)) return false;
    auto node = instances_.extract(instance->id);
    node.key() = newId;
    instance->id = newId;
    instances_.insert(std::move(node));
    return true;
}

void Scene::forEach(const std::function<void(Instance&)>& fn) const {
    std::vector<Instance*> all;
    all.reserve(instances_.size());
    for (const auto& [id, inst] : instances_) all.push_back(inst.get());
    for (Instance* inst : all) fn(*inst);
}

void Scene::forEachDescendant(Instance& root, const std::function<void(Instance&)>& fn) {
    std::vector<Instance*> stack(root.children.rbegin(), root.children.rend());
    while (!stack.empty()) {
        Instance* n = stack.back();
        stack.pop_back();
        fn(*n);
        for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(*it);
    }
}

void Scene::forEachInTree(const std::function<void(Instance&)>& fn) const {
    for (Instance* s : services_) {
        fn(*s);
        forEachDescendant(*s, fn);
    }
}

bool Scene::setProperty(Instance& instance, const PropertyInfo& prop, const Value& value, std::string* error) {
    const Value before = prop.get(instance);
    if (!rift::setProperty(instance, prop, value, error)) return false;
    if (!valuesEqual(before, prop.get(instance))) notifyPropertyChanged(instance, prop);
    return true;
}

bool Scene::setProperty(Instance& instance, const std::string& propName, const Value& value, std::string* error) {
    const PropertyInfo* prop = findProperty(instance.cls, propName);
    if (!prop) {
        if (error) *error = "'" + propName + "' is not a property of " + classInfo(instance.cls).name;
        return false;
    }
    return setProperty(instance, *prop, value, error);
}

void Scene::setAttribute(Instance& instance, const std::string& name, const Value& value) {
    const Value* old = instance.attribute(name);
    if (old && valuesEqual(*old, value)) return;
    if (!old && typeOf(value) == ValueType::None) return;
    instance.setAttribute(name, value);
    ++revision_;
    for (SceneObserver* o : std::vector<SceneObserver*>(observers_)) o->onAttributeChanged(instance, name);
}

void Scene::notifyPropertyChanged(Instance& instance, const PropertyInfo& prop) {
    ++revision_;
    for (SceneObserver* o : std::vector<SceneObserver*>(observers_)) o->onPropertyChanged(instance, prop);
}

void Scene::addObserver(SceneObserver* observer) {
    if (observer && std::find(observers_.begin(), observers_.end(), observer) == observers_.end()) observers_.push_back(observer);
}

void Scene::removeObserver(SceneObserver* observer) {
    observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());
}

} // namespace rift
