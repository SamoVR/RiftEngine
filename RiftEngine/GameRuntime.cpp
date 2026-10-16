#include "GameRuntime.h"
#include "LuaRuntime.h"
#include "ScriptStore.h"
#include "Geometry.h"
#include "Logger.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>

namespace {
    constexpr const char* kPlayerName = "Player";
    const glm::vec3 kPlayerSize(0.8f, 1.8f, 0.8f);
    constexpr float kMaxPhysicsStep = 1.0f / 120.0f; // longest single physics step
    constexpr float kMaxStepDistance = 0.25f;        // longest distance any body moves in one step
    constexpr int   kMaxPhysicsSteps = 64;           // safety cap per frame

    std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
}

GameRuntime::GameRuntime(Scene& scene, Camera& camera, InputManager& input)
    : scene_(scene), camera_(camera), input_(input) {}

GameRuntime::~GameRuntime() {
    if (running_) stop();
}

// ============================================================================
//  Start / stop
// ============================================================================

void GameRuntime::start() {
    if (running_) return;
    running_ = true;
    paused_ = false;
    time_ = 0.0f;
    outcome_ = Outcome::None;
    message_.clear();
    messageTimer_ = 0.0f;
    touching_.clear();
    log_.clear();
    savedOrbit_ = camera_.getOrbit();

    snapshot_.clear();
    byId_.clear();
    created_.clear();
    detached_.clear();
    dead_.clear();
    for (Object* o : scene_.getObjects()) {
        snapshot_[o] = { o->position, o->rotation, o->scale, o->color, o->game, o->parent, o->children };
        o->game.velocity = glm::vec3(0.0f);
        byId_[o->id] = o;
    }
    objects_ = scene_.getObjects();

    spawnPlayer();
    byId_[player_->id] = player_.get();
    // Third-person view behind/above the player, keeping the editor's heading.
    camera_.setOrbit({ 14.0f, savedOrbit_.yaw, 22.0f, player_->position });
    logLine("Game started. WASD / arrows = move, Space = jump, right-drag = look around, F5 = stop.", false);

    lua_ = std::make_unique<LuaRuntime>(*this);
    if (scripts_) {
        lua_->start(*scripts_);
    } else {
        logLine("No script store attached, scripts will not run.", true);
    }
    LOG_INFO("Game", "Play mode started");
}

void GameRuntime::stop() {
    if (!running_) return;
    running_ = false;
    lua_.reset(); // closes the Lua state first: nothing can run after this

    // Everything the scripts created goes away; scene objects they only hid
    // ("destroyed") or rearranged are put back below.
    for (Object* o : created_) {
        if (scene_.contains(o)) scene_.removeObject(o);
    }
    created_.clear();
    detached_.clear();
    dead_.clear();
    objects_.clear();
    touching_.clear();
    byId_.clear();
    player_.reset();

    // Objects can't be added or removed by the editor while playing, but check
    // anyway so a stale pointer is never written through.
    for (auto& [object, s] : snapshot_) {
        if (!scene_.contains(object)) continue;
        object->position = s.position;
        object->rotation = s.rotation;
        object->scale = s.scale;
        object->color = s.color;
        object->game = s.game;
        object->game.velocity = glm::vec3(0.0f);
        object->parent = s.parent;     // scripts may have re-parented it
        object->children = s.children; // ... and changed its children (script-created ones are gone by now)
    }
    snapshot_.clear();
    camera_.setOrbit(savedOrbit_); // back to the exact editor view
    message_.clear();
    messageTimer_ = 0.0f;
    LOG_INFO("Game", "Play mode stopped, scene restored");
}

void GameRuntime::spawnPlayer() {
    // A part named "SpawnPoint" (or "Spawn") decides where the player appears.
    spawnPoint_ = glm::vec3(0.0f, 3.0f, 0.0f);
    for (Object* o : scene_.getObjects()) {
        if (o->kind != Object::Kind::Part) continue;
        const std::string n = lower(o->name);
        if (n == "spawnpoint" || n == "spawn" || n == "spawnlocation") {
            const Box b = worldBox(*o);
            spawnPoint_ = glm::vec3((b.lo.x + b.hi.x) * 0.5f, b.hi.y + kPlayerSize.y * 0.5f + 0.05f, (b.lo.z + b.hi.z) * 0.5f);
            break;
        }
    }
    player_ = std::make_unique<Object>(kPlayerName, createCubeVertices(), static_cast<Texture*>(nullptr));
    player_->scale = kPlayerSize;
    player_->color = glm::vec3(0.25f, 0.55f, 1.0f);
    player_->position = spawnPoint_;
    playerVelocity_ = glm::vec3(0.0f);
    playerGrounded_ = false;
}

// ============================================================================
//  Frame
// ============================================================================

void GameRuntime::update(float dt, bool allowPlayerInput) {
    if (!running_) return;
    if (messageTimer_ > 0.0f) {
        messageTimer_ -= dt;
        if (messageTimer_ <= 0.0f && outcome_ == Outcome::None) message_.clear();
    }
    if (paused_ || outcome_ != Outcome::None) {
        if (player_) camera_.focusOn(player_->position + glm::vec3(0.0f, 0.6f, 0.0f));
        return;
    }
    time_ += dt;

    // Scripts first (Heartbeat, task.wait wake-ups, input events), then physics.
    if (lua_) lua_->update(dt, allowPlayerInput);
    objects_ = scene_.getObjects(); // scripts may have created parts

    // Adaptive substeps. A step is limited both in time (kMaxPhysicsStep) and
    // in distance (kMaxStepDistance), so the fastest body never moves further
    // in one step than it takes to cross a thin platform; a time cap alone
    // still tunnelled at ~85 u/s. (Not a true fixed timestep: the step is
    // dt/n, so results depend slightly on frame rate. A proper fix is
    // continuous collision, planned with the physics rework.)
    float fastest = glm::length(playerVelocity_);
    for (Object* o : objects_) {
        if (o->kind == Object::Kind::Part && !o->game.anchored && !o->parent) fastest = std::max(fastest, glm::length(o->game.velocity));
    }
    fastest += std::fabs(gravity) * dt; // speed reached by the end of this frame
    const float needed = std::max(dt / kMaxPhysicsStep, fastest * dt / kMaxStepDistance);
    const int steps = std::clamp(static_cast<int>(std::ceil(needed)), 1, kMaxPhysicsSteps);
    const float h = dt / static_cast<float>(steps);
    for (int i = 0; i < steps; ++i) {
        stepPhysics(h);
        updatePlayer(h, allowPlayerInput);
    }
    detectTouches();
    flushDead();

    if (player_) camera_.focusOn(player_->position + glm::vec3(0.0f, 0.6f, 0.0f));
}

void GameRuntime::updatePlayer(float dt, bool allowInput) {
    if (!player_) return;

    glm::vec3 wish(0.0f);
    if (allowInput) {
        // Camera-relative movement on the ground plane.
        glm::vec3 forward = camera_.target - camera_.position;
        forward.y = 0.0f;
        forward = glm::length(forward) > 1e-4f ? glm::normalize(forward) : glm::vec3(0.0f, 0.0f, -1.0f);
        const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 1.0f, 0.0f)));

        if (input_.isKeyPressed(GLFW_KEY_W) || input_.isKeyPressed(GLFW_KEY_UP)) wish += forward;
        if (input_.isKeyPressed(GLFW_KEY_S) || input_.isKeyPressed(GLFW_KEY_DOWN)) wish -= forward;
        if (input_.isKeyPressed(GLFW_KEY_D) || input_.isKeyPressed(GLFW_KEY_RIGHT)) wish += right;
        if (input_.isKeyPressed(GLFW_KEY_A) || input_.isKeyPressed(GLFW_KEY_LEFT)) wish -= right;
        if (glm::length(wish) > 1e-4f) {
            wish = glm::normalize(wish);
            // Face the walking direction (yaw only).
            player_->rotation.y = glm::degrees(std::atan2(wish.x, wish.z));
        }
        if (playerGrounded_ && input_.isKeyPressed(GLFW_KEY_SPACE)) {
            playerVelocity_.y = jumpSpeed;
            playerGrounded_ = false;
        }
    }

    const float sprint = allowInput && input_.isKeyPressed(GLFW_KEY_LEFT_SHIFT) ? 1.6f : 1.0f;
    playerVelocity_.x = wish.x * walkSpeed * sprint;
    playerVelocity_.z = wish.z * walkSpeed * sprint;
    playerVelocity_.y += gravity * dt;

    player_->position += playerVelocity_ * dt;
    Box box{ player_->position - kPlayerSize * 0.5f, player_->position + kPlayerSize * 0.5f };
    playerGrounded_ = resolveCollisions(box, playerVelocity_, nullptr, 0.0f);
    player_->position = (box.lo + box.hi) * 0.5f;

    if (player_->position.y < killPlaneY) {
        logLine("Player fell out of the world, respawning.", false);
        player_->position = spawnPoint_;
        playerVelocity_ = glm::vec3(0.0f);
    }
}

// Simple, predictable physics: unanchored ROOT parts fall and are pushed out
// of solid parts along the axis of least penetration. Children of a moving
// part simply move with it (that's how the hierarchy already works).
void GameRuntime::stepPhysics(float dt) {
    for (Object* o : objects_) {
        if (o->kind != Object::Kind::Part || o->game.anchored || o->game.destroyed || o->parent) continue;

        glm::vec3& v = o->game.velocity;
        v.y += gravity * dt;
        o->position += v * dt;

        Box box = worldBox(*o);
        const glm::vec3 centerBefore = (box.lo + box.hi) * 0.5f;
        if (o->game.canCollide) {
            if (resolveCollisions(box, v, o, o->game.bounciness)) {
                v.x *= std::max(0.0f, 1.0f - 6.0f * dt); // ground friction
                v.z *= std::max(0.0f, 1.0f - 6.0f * dt);
            }
            o->position += (box.lo + box.hi) * 0.5f - centerBefore;
        }

        if (o->position.y < killPlaneY) { // fell out of the world: Roblox "destroys" it
            o->game.visible = false;
            o->game.anchored = true;
            v = glm::vec3(0.0f);
        }
    }
}

// Collision against each part's ORIENTED box. Using the axis-aligned box
// made rotated parts far bigger than they look (a spinning 8 x 0.8 bar
// blocked an 8 x 8 square), so walls/ramps/spinners felt invisible.
bool GameRuntime::resolveCollisions(Box& box, glm::vec3& velocity, const Object* self, float bounciness) {
    bool grounded = false;
    for (Object* other : objects_) {
        if (other == self || other->kind != Object::Kind::Part || other->game.destroyed || !other->game.canCollide || !other->game.visible) continue;
        if (self && (other->isChildOf(const_cast<Object*>(self)) || self->isChildOf(other))) continue;
        const Obb ob = worldObb(*other);
        glm::vec3 push;
        if (!separate(box, ob, push)) continue;
        float depth = glm::length(push);
        if (depth < 1e-6f) continue;
        glm::vec3 normal = push / depth;

        // Small ledges are stepped onto instead of blocking the walk.
        const float top = ob.center.y + std::fabs(ob.axis[0].y) * ob.half.x + std::fabs(ob.axis[1].y) * ob.half.y + std::fabs(ob.axis[2].y) * ob.half.z;
        const float stepUp = top - box.lo.y;
        if (normal.y < 0.7f && stepUp > 0.0f && stepUp < 0.35f && velocity.y <= 0.0f) {
            normal = glm::vec3(0.0f, 1.0f, 0.0f);
            depth = stepUp;
        }

        box.lo += normal * depth;
        box.hi += normal * depth;
        const float into = glm::dot(velocity, normal);
        if (into < 0.0f) {
            velocity -= (1.0f + bounciness) * into * normal;
            if (std::fabs(glm::dot(velocity, normal)) < 0.5f) velocity -= glm::dot(velocity, normal) * normal; // no micro-bounces
        }
        if (normal.y > 0.7f) grounded = true;
    }
    return grounded;
}

void GameRuntime::detectTouches() {
    if (!lua_) return;
    std::set<std::pair<const Object*, const Object*>> now;

    auto contact = [&](Object* a, Object* b) {
        const std::pair<const Object*, const Object*> key(a, b);
        now.insert(key);
        if (!touching_.count(key)) lua_->fireTouched(a, b); // fires once when the contact begins
    };

    std::vector<std::pair<Object*, Box>> boxes;
    for (Object* o : objects_) {
        if (o->kind == Object::Kind::Part && o->game.visible && !o->game.destroyed) boxes.emplace_back(o, worldBox(*o));
    }
    glm::vec3 scratch;
    const Box playerBox = player_ ? Box{ player_->position - kPlayerSize * 0.5f, player_->position + kPlayerSize * 0.5f } : Box{};

    for (size_t i = 0; i < boxes.size(); ++i) {
        Object* a = boxes[i].first;
        // Only parts somebody listens to are tested (a handler may destroy parts mid-loop).
        if (a->game.destroyed || !lua_->hasTouchedListeners(a)) continue;
        if (player_ && separate(playerBox, worldObb(*a), scratch, 0.05f)) contact(a, player_.get());
        for (size_t j = 0; j < boxes.size(); ++j) {
            if (i == j) continue;
            Object* b = boxes[j].first;
            if (b->game.destroyed || a->isChildOf(b) || b->isChildOf(a)) continue;
            if (separate(boxes[i].second, worldObb(*b), scratch, 0.02f)) contact(a, b);
        }
    }
    touching_ = std::move(now);
}

// ============================================================================
//  Object registry + script-facing API
// ============================================================================

Object* GameRuntime::findObject(uint64_t id) const {
    auto it = byId_.find(id);
    if (it == byId_.end() || it->second->game.destroyed) return nullptr;
    return it->second;
}

bool GameRuntime::isInWorld(const Object* o) const {
    if (!o) return false;
    if (o == player_.get()) return true;
    return byId_.count(o->id) != 0 && detached_.count(o->id) == 0;
}

std::vector<Object*> GameRuntime::collectSubtree(Object* root) const {
    std::vector<Object*> nodes;
    std::vector<Object*> stack{ root };
    while (!stack.empty()) {
        Object* n = stack.back();
        stack.pop_back();
        nodes.push_back(n);
        for (Object* c : n->children) stack.push_back(c);
    }
    return nodes;
}

Object* GameRuntime::createObject(const std::string& className) {
    std::string mesh;
    if (className == "Part" || className == "Block") mesh = "cube";
    else if (className == "Sphere") mesh = "sphere";
    else if (className == "Wedge") mesh = "triangle";
    else return nullptr;

    auto o = std::make_unique<Object>(className == "Block" ? "Part" : className, primitiveVertices(mesh), static_cast<Texture*>(nullptr));
    o->color = glm::vec3(0.64f);
    o->game.anchored = false; // like Roblox: a new part falls until anchored
    Object* raw = o.get();
    byId_[raw->id] = raw;
    created_.insert(raw);
    detached_[raw->id] = std::move(o);
    return raw;
}

Object* GameRuntime::cloneSubtree(Object* o) {
    if (!o || o == player_.get() || o->game.destroyed) return nullptr;
    std::function<Object*(Object*, Object*)> copyNode = [&](Object* src, Object* newParent) -> Object* {
        if (src->game.destroyed) return nullptr;
        auto copy = std::make_unique<Object>(*src); // fresh GPU buffers and a new id, no parent/children
        Object* raw = copy.get();
        byId_[raw->id] = raw;
        created_.insert(raw);
        detached_[raw->id] = std::move(copy);
        if (newParent) raw->setParent(newParent);
        for (Object* child : std::vector<Object*>(src->children)) copyNode(child, raw);
        return raw;
    };
    return copyNode(o, nullptr);
}

bool GameRuntime::setObjectParent(Object* o, Object* newParent, bool toWorld, std::string& error) {
    if (!o) { error = "no object"; return false; }
    if (o == player_.get()) { error = "the player's Character cannot be re-parented"; return false; }
    if (newParent == player_.get()) { error = "nothing can be parented to the player's Character"; return false; }
    if (newParent && (newParent == o || newParent->isChildOf(o))) {
        error = "an Instance cannot be parented under itself or one of its descendants";
        return false;
    }

    const bool oInWorld = isInWorld(o);
    const bool targetInWorld = toWorld || (newParent && isInWorld(newParent));

    std::vector<Object*> enteredWorld;
    if (targetInWorld) {
        if (!oInWorld) {
            // Detached subtree -> scene.
            for (Object* n : collectSubtree(o)) {
                auto it = detached_.find(n->id);
                if (it == detached_.end()) continue;
                scene_.addObject(std::move(it->second));
                detached_.erase(it);
                enteredWorld.push_back(n);
            }
        }
        o->setParent(newParent); // nullptr = top level (workspace)
    } else {
        if (oInWorld) {
            if (!created_.count(o)) {
                error = "objects from the scene cannot be removed from the world; use :Destroy()";
                return false;
            }
            // Scene -> detached subtree (links between its nodes are kept).
            std::vector<std::unique_ptr<Object>> nodes = scene_.extractSubtree(o);
            for (auto& n : nodes) {
                const uint64_t id = n->id;
                detached_[id] = std::move(n);
            }
        }
        o->setParent(newParent); // nullptr (Parent = nil) or another detached object
    }

    if (lua_) {
        for (Object* n : enteredWorld) {
            if (n->kind == Object::Kind::Script) lua_->scriptAddedToWorld(n);
        }
    }
    return true;
}

void GameRuntime::destroyObject(Object* o) {
    if (!o || o == player_.get()) return;
    std::vector<Object*> freeNow; // never-parented script objects: nothing else can see them
    for (Object* n : collectSubtree(o)) {
        n->game.destroyed = true;
        n->game.visible = false;
        n->game.canCollide = false;
        n->game.velocity = glm::vec3(0.0f);
        if (!created_.count(n)) continue; // scene objects are only hidden; Stop restores them
        if (detached_.count(n->id)) freeNow.push_back(n);
        else dead_.push_back(n);          // in the scene: freed at the end of the frame
    }
    if (!freeNow.empty()) {
        o->removeParent();
        for (Object* n : freeNow) {
            created_.erase(n);
            byId_.erase(n->id);
            detached_.erase(n->id); // destroys the object
        }
    }
}

void GameRuntime::flushDead() {
    if (dead_.empty()) return;
    for (Object* n : dead_) {
        created_.erase(n);
        byId_.erase(n->id);
        if (scene_.contains(n)) scene_.removeObject(n);
    }
    dead_.clear();
    objects_.clear();
}

void GameRuntime::respawnPlayer() {
    if (!player_) return;
    player_->position = spawnPoint_;
    playerVelocity_ = glm::vec3(0.0f);
}

void GameRuntime::showMessage(const std::string& text, float seconds) {
    message_ = text;
    messageTimer_ = seconds;
}

void GameRuntime::finish(bool won, const std::string& text) {
    outcome_ = won ? Outcome::Won : Outcome::Lost;
    message_ = text.empty() ? (won ? "You win!" : "Game over") : text;
    messageTimer_ = 1e9f;
    logLine(message_, false);
}

// ============================================================================
//  Helpers
// ============================================================================

GameRuntime::Box GameRuntime::worldBox(const Object& o) {
    const glm::mat4 m = o.getGlobalTransform();
    const glm::vec3 lo = o.getBoundsMin(), hi = o.getBoundsMax();
    Box b{ glm::vec3(1e30f), glm::vec3(-1e30f) };
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 corner((i & 1) ? hi.x : lo.x, (i & 2) ? hi.y : lo.y, (i & 4) ? hi.z : lo.z);
        const glm::vec3 w = glm::vec3(m * glm::vec4(corner, 1.0f));
        b.lo = glm::min(b.lo, w);
        b.hi = glm::max(b.hi, w);
    }
    return b;
}

bool GameRuntime::overlaps(const Box& a, const Box& b, float margin) {
    return a.lo.x < b.hi.x + margin && a.hi.x > b.lo.x - margin &&
           a.lo.y < b.hi.y + margin && a.hi.y > b.lo.y - margin &&
           a.lo.z < b.hi.z + margin && a.hi.z > b.lo.z - margin;
}

void GameRuntime::logLine(const std::string& text, bool isError) {
    log_.push_back({ text, isError, time_ });
    if (log_.size() > 500) log_.erase(log_.begin(), log_.begin() + 100);
    if (isError) LOG_WARN("Script", text);
}

std::string GameRuntime::checkSyntax(const std::string& source, const std::string& chunkName) {
    return LuaRuntime::checkSyntax(source, chunkName);
}

GameRuntime::Obb GameRuntime::worldObb(const Object& o) {
    const glm::mat4 m = o.getGlobalTransform();
    const glm::vec3 lo = o.getBoundsMin(), hi = o.getBoundsMax();
    Obb b;
    b.center = glm::vec3(m * glm::vec4((lo + hi) * 0.5f, 1.0f));
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 column(m[i]);
        const float length = glm::length(column);
        b.axis[i] = length > 1e-6f ? column / length : glm::vec3(i == 0, i == 1, i == 2);
        b.half[i] = (hi[i] - lo[i]) * 0.5f * length;
    }
    return b;
}

// Separating-axis test of an axis-aligned box against an oriented box (the 3
// world axes, the 3 box axes and their 9 cross products). On overlap `push`
// is the smallest move that takes `box` out of `obb`.
bool GameRuntime::separate(const Box& box, const Obb& obb, glm::vec3& push, float margin) {
    const glm::vec3 centerA = (box.lo + box.hi) * 0.5f;
    const glm::vec3 halfA = (box.hi - box.lo) * 0.5f;
    const glm::vec3 d = obb.center - centerA;
    float best = 1e30f;
    bool found = false;

    auto test = [&](glm::vec3 axis, float preference) {
        const float length = glm::length(axis);
        if (length < 1e-6f) return true; // parallel edges: no information
        axis /= length;
        const float rA = halfA.x * std::fabs(axis.x) + halfA.y * std::fabs(axis.y) + halfA.z * std::fabs(axis.z);
        float rB = 0.0f;
        for (int i = 0; i < 3; ++i) rB += obb.half[i] * std::fabs(glm::dot(obb.axis[i], axis));
        const float dist = glm::dot(d, axis);
        const float overlap = rA + rB - std::fabs(dist);
        if (overlap <= -margin) return false;
        // Face axes are preferred over edge axes of similar depth (steadier contacts).
        if (overlap * preference < best) {
            best = overlap * preference;
            push = (dist > 0.0f ? -axis : axis) * overlap;
            found = true;
        }
        return true;
    };

    const glm::vec3 world[3] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1} };
    for (const auto& a : world) if (!test(a, 1.0f)) return false;
    for (const auto& a : obb.axis) if (!test(a, 1.0f)) return false;
    for (const auto& a : world)
        for (const auto& b : obb.axis)
            if (!test(glm::cross(a, b), 1.15f)) return false;
    return found;
}
