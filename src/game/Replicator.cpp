#include "game/Replicator.h"

namespace rift {

namespace {
    // Copies the reflected properties and the attributes / tags of `from` onto `to`.
    void copyState(const Instance& from, Instance& to) {
        for (const PropertyInfo& p : classInfo(from.cls).props) p.set(to, p.get(from));
        to.attributes = from.attributes;
        to.tags = from.tags;
    }

    bool sameTransform(const Instance& a, const glm::vec3& p, const glm::vec3& r) {
        return a.position == p && a.rotation == r;
    }
}

Replicator::Replicator(Scene& server, Scene& client) : server_(server), client_(client) {
    serverObserver_.r = this;
    clientObserver_.r = this;
}

Replicator::~Replicator() {
    if (!started_) return;
    server_.removeObserver(&serverObserver_);
    client_.removeObserver(&clientObserver_);
}

bool Replicator::replicates(const Scene& server, const Instance& i) {
    const Instance* service = server.serviceOf(&i);
    return service && service->cls != ClassId::ServerStorage && service->cls != ClassId::ScriptService;
}

void Replicator::start() {
    ++applying_;
    // Services keep the server's ids; their contents are copied fresh.
    for (int s = 0; s < kServiceCount; ++s) {
        const Instance* src = server_.services()[s];
        Instance* dst = client_.services()[s];
        for (Instance* c : std::vector<Instance*>(dst->children)) client_.destroy(c);
        if (dst->id != src->id) client_.reassignId(dst, src->id);
        copyState(*src, *dst);
        if (!replicates(server_, *src)) continue;
        for (const Instance* child : src->children) place(*child);
    }
    --applying_;
    server_.addObserver(&serverObserver_);
    client_.addObserver(&clientObserver_);
    started_ = true;
}

void Replicator::remember(const Instance& i) {
    if (i.hasTransform()) sent_[i.id] = { i.position, i.rotation, i.velocity, i.angularVelocity };
    for (const Instance* c : i.children) remember(*c);
}

void Replicator::place(const Instance& i) {
    Instance* existing = client_.find(i.id);
    if (!server_.isInTree(&i) || !replicates(server_, i)) {
        if (existing) {
            ++applying_;
            client_.destroy(existing);
            --applying_;
        }
        return;
    }
    // The parent is not on the client (yet): it brings this along when it arrives.
    Instance* parent = i.parent ? client_.find(i.parent->id) : nullptr;
    if (!parent) return;
    ++applying_;
    if (existing) {
        if (existing->parent != parent) client_.setParent(existing, parent, server_.siblingIndex(&i));
    } else if (Instance* copy = client_.cloneSubtree(i, true, true)) {
        client_.setParent(copy, parent, server_.siblingIndex(&i));
        remember(i);
    }
    --applying_;
}

void Replicator::added(Instance& i) {
    if (applying_) return;
    place(i);
}

void Replicator::removing(Instance& i) {
    sent_.erase(i.id);
    if (applying_) return;
    if (Instance* c = client_.find(i.id)) {
        ++applying_;
        client_.destroy(c);
        --applying_;
    }
}

void Replicator::parentChanged(Instance& i, Instance*) {
    if (applying_) return;
    place(i);
}

void Replicator::propertyChanged(Instance& i, const PropertyInfo& p) {
    if (applying_) return;
    Instance* c = client_.find(i.id);
    if (!c) return;
    ++applying_;
    client_.setProperty(*c, p, p.get(i));
    --applying_;
    if (i.hasTransform()) sent_[i.id] = { i.position, i.rotation, i.velocity, i.angularVelocity };
}

void Replicator::attributeChanged(Instance& i, const std::string& name) {
    if (applying_) return;
    Instance* c = client_.find(i.id);
    if (!c) return;
    const Value* v = i.attribute(name);
    ++applying_;
    client_.setAttribute(*c, name, v ? *v : Value{});
    --applying_;
}

void Replicator::tagsChanged(Instance& i) {
    if (applying_) return;
    if (Instance* c = client_.find(i.id)) c->tags = i.tags;
}

void Replicator::clientPropertyChanged(Instance& i, const PropertyInfo& p) {
    if (applying_ || !character_) return;
    // Only the character the client owns goes back to the server.
    const Instance* character = client_.find(character_);
    if (!character || (i.id != character_ && !i.isDescendantOf(character))) return;
    const bool transform = (i.isBasePart() || i.cls == ClassId::Model)
        && (p.name == "Position" || p.name == "Rotation" || p.name == "Velocity" || p.name == "AngularVelocity");
    const bool movement = i.cls == ClassId::Humanoid && (p.name == "WalkSpeed" || p.name == "JumpPower" || p.name == "Jump");
    if (!transform && !movement) return;
    Instance* s = server_.find(i.id);
    if (!s) return;
    ++applying_;
    server_.setProperty(*s, p, p.get(i));
    --applying_;
}

void Replicator::syncTransforms() {
    // Parts the physics moved (no property events): send what changed since last time.
    Scene::forEachDescendant(server_.workspace(), [&](Instance& s) {
        if (s.cls == ClassId::Humanoid) {
            if (Instance* c = client_.find(s.id)) {
                c->moveDirection = s.moveDirection;
                c->jump = s.jump;
            }
            return;
        }
        if (!s.hasTransform()) return;
        Instance* c = client_.find(s.id);
        if (!c) return;
        auto it = sent_.find(s.id);
        if (it != sent_.end() && sameTransform(s, it->second.position, it->second.rotation)
            && s.velocity == it->second.velocity && s.angularVelocity == it->second.angular) return;
        c->position = s.position;
        c->rotation = s.rotation;
        c->velocity = s.velocity;
        c->angularVelocity = s.angularVelocity;
        sent_[s.id] = { s.position, s.rotation, s.velocity, s.angularVelocity };
    });
}

} // namespace rift
