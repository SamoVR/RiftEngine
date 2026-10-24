#include "scene/Instance.h"
#include "core/TransformUtil.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace rift {

bool Instance::isService() const { return cls <= ClassId::SoundService; }

bool Instance::isBasePart() const {
    return cls == ClassId::Part || cls == ClassId::SpawnLocation || cls == ClassId::MeshPart;
}

bool Instance::hasTransform() const {
    return isBasePart() || cls == ClassId::Model || isLight();
}

bool Instance::isScript() const { return cls == ClassId::Script || cls == ClassId::LocalScript || cls == ClassId::ModuleScript; }
bool Instance::isRemote() const { return cls >= ClassId::RemoteEvent && cls <= ClassId::BindableFunction; }
bool Instance::isFixedContainer() const {
    return cls == ClassId::StarterPlayerScripts || cls == ClassId::StarterCharacterScripts || cls == ClassId::PlayerGui
        || cls == ClassId::PlayerScripts || cls == ClassId::Backpack;
}

bool Instance::isGui() const { return cls >= ClassId::ScreenGui && cls <= ClassId::ImageButton; }
bool Instance::isGuiObject() const { return cls > ClassId::ScreenGui && cls <= ClassId::ImageButton; }
bool Instance::isLight() const { return cls == ClassId::PointLight || cls == ClassId::SpotLight; }
bool Instance::isValueObject() const { return cls >= ClassId::IntValue && cls <= ClassId::Color3Value; }

glm::mat4 Instance::localFrame() const {
    if (!hasTransform()) return glm::mat4(1.0f);
    return TransformUtil::compose(position, rotation, glm::vec3(1.0f));
}

glm::mat4 Instance::parentWorldFrame() const {
    glm::mat4 m(1.0f);
    for (const Instance* p = parent; p; p = p->parent) {
        if (p->hasTransform()) m = p->localFrame() * m;
    }
    return m;
}

glm::mat4 Instance::worldFrame() const { return parentWorldFrame() * localFrame(); }

glm::mat4 Instance::worldTransform() const {
    const glm::mat4 frame = worldFrame();
    return isBasePart() ? frame * glm::scale(glm::mat4(1.0f), size) : frame;
}

glm::vec3 Instance::worldPosition() const { return glm::vec3(worldFrame()[3]); }

glm::vec3 Instance::worldRotation() const {
    glm::vec3 p, r, s;
    TransformUtil::decompose(worldFrame(), p, r, s);
    return r;
}

void Instance::setWorldFrame(const glm::vec3& worldPos, const glm::vec3& worldRotDeg) {
    // Frames are rigid (no scale), so the inverse is exact and cheap.
    const glm::mat4 local = glm::inverse(parentWorldFrame()) * TransformUtil::compose(worldPos, worldRotDeg, glm::vec3(1.0f));
    glm::vec3 s;
    TransformUtil::decompose(local, position, rotation, s);
}

void Instance::setWorldTransform(const glm::mat4& world) {
    glm::vec3 p, r, s;
    TransformUtil::decompose(world, p, r, s);
    setWorldFrame(p, r);
    if (isBasePart()) size = glm::abs(s);
}

bool Instance::isDescendantOf(const Instance* ancestor) const {
    for (const Instance* p = parent; p; p = p->parent) {
        if (p == ancestor) return true;
    }
    return false;
}

Instance* Instance::findFirstChild(const std::string& childName) const {
    for (Instance* c : children) if (c->name == childName) return c;
    return nullptr;
}

const Value* Instance::attribute(const std::string& attrName) const {
    for (const Attribute& a : attributes) if (a.name == attrName) return &a.value;
    return nullptr;
}

void Instance::setAttribute(const std::string& attrName, const Value& v) {
    auto it = std::find_if(attributes.begin(), attributes.end(), [&](const Attribute& a) { return a.name == attrName; });
    if (typeOf(v) == ValueType::None) {
        if (it != attributes.end()) attributes.erase(it);
        return;
    }
    if (it != attributes.end()) it->value = v;
    else attributes.push_back({ attrName, v });
}

bool Instance::hasTag(const std::string& tag) const {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

void Instance::addTag(const std::string& tag) {
    if (!tag.empty() && !hasTag(tag)) tags.push_back(tag);
}

void Instance::removeTag(const std::string& tag) {
    tags.erase(std::remove(tags.begin(), tags.end(), tag), tags.end());
}

std::string Instance::fullName() const {
    std::string out = name;
    for (const Instance* p = parent; p; p = p->parent) out = p->name + "." + out;
    return out;
}

uint64_t newInstanceId() {
    static std::mt19937_64 rng{ std::random_device{}() ^ static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) };
    uint64_t v;
    do { v = rng(); } while (v == 0);
    return v;
}

std::string idToString(uint64_t id) {
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

uint64_t idFromString(const std::string& s) {
    if (s.empty() || s.size() > 16) return 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 16);
    return (end && *end == '\0') ? static_cast<uint64_t>(v) : 0;
}

} // namespace rift
