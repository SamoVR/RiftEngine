#pragma once
//
// PhysicsWorld.h -- Jolt Physics for the parts in a Scene's Workspace.
//
// Bodies:
//   * Terrain: a static mesh per terrain chunk (rebuilt when the chunk changes).
//   * An ANCHORED part is its own static body. If a script (or its moving
//     parent) changes where it is, it becomes kinematic and is moved with a
//     velocity, so it pushes dynamic parts and carries the player.
//   * An UNANCHORED part is a dynamic body together with its descendant
//     parts (child parts are welded to it, whatever their own Anchored is).
//     Parts under a Model or Folder are separate bodies.
//   * CanCollide = false parts are "ghosts": they collide with nothing but
//     are still found by Touched queries and raycasts.
//
// The scene stays the source of truth: changes made through the Scene API
// (scripts, editor) are picked up through SceneObserver; transforms and
// velocities written by scripts are compared with what physics last wrote.
//
#include "scene/Scene.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rift {

struct RaycastHit {
    uint64_t instance = 0;
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f };
    float distance = 0.0f;
};

struct RaycastFilter {
    std::vector<uint64_t> instances;   // these instances and their descendants ...
    bool include = false;              // ... are the only ones hit (true) or are ignored (false)
    bool respectCanCollide = false;    // true: CanCollide = false parts are not hit
};

struct TouchEvent {
    uint64_t part = 0;   // the watched part
    uint64_t other = 0;  // the part it touches
    bool began = true;   // false: contact ended
};

class CharacterController;

class PhysicsWorld : public SceneObserver {
public:
    explicit PhysicsWorld(Scene& scene);
    ~PhysicsWorld() override;
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advances the simulation by dt seconds (split into steps of at most
    // 1/60 s) and writes dynamic parts' transforms and velocities back.
    // `beforeStep` runs before every sub-step (character movement).
    void step(float dt);

    // ---- queries
    std::optional<RaycastHit> raycast(const glm::vec3& origin, const glm::vec3& direction, const RaycastFilter& filter = {}) const;
    // Parts whose collision shape overlaps a box (world-space centre, size, Euler degrees).
    std::vector<uint64_t> partsInBox(const glm::vec3& center, const glm::vec3& size, const glm::vec3& rotation = glm::vec3(0.0f)) const;

    // ---- touch events: only watched parts are tested (scripts listening to Touched)
    void watchTouches(uint64_t partId, bool watch);
    std::vector<TouchEvent> takeTouchEvents();
    // Parts touching `partId` right now (GetTouchingParts).
    std::vector<uint64_t> touchingParts(uint64_t partId) const;

    // Dynamic parts that fell below Workspace.FallenPartsDestroyHeight
    // during the last step (the game destroys them).
    std::vector<uint64_t> takeFallenParts();

    // ---- impulses (scripts: part:ApplyImpulse)
    void applyImpulse(uint64_t partId, const glm::vec3& impulse);
    void applyAngularImpulse(uint64_t partId, const glm::vec3& impulse);
    float massOf(uint64_t partId) const;

    // ---- character (one per player). The controller is owned by the world.
    CharacterController* createCharacter(uint64_t rootPartId, const glm::vec3& feetPosition, float radius, float height);
    void destroyCharacter(CharacterController* character);

    // An instance whose parts never get bodies (a character model: its
    // controller moves it). Applies to all its descendants.
    void setIgnored(uint64_t instanceId, bool ignored);

    size_t bodyCount() const;

    // SceneObserver
    void onInstanceAdded(Instance& instance) override;
    void onInstanceRemoving(Instance& instance) override;
    void onParentChanged(Instance& instance, Instance* oldParent) override;
    void onPropertyChanged(Instance& instance, const PropertyInfo& prop) override;

    struct Impl; // Jolt types stay in the .cpp
private:
    std::unique_ptr<Impl> impl_;
};

// Moves a capsule through the world like a game character: walks up stairs
// and slopes, stands on (and rides) moving platforms, pushes dynamic parts.
class CharacterController {
public:
    struct Input {
        glm::vec3 moveDirection{ 0.0f }; // horizontal, length 0..1
        bool jump = false;
        float walkSpeed = 8.0f;
        float jumpSpeed = 12.0f;
        bool swimming = false;           // in terrain water: no gravity, Space rises
        bool anchored = false;           // a part of the character is anchored: it stays where it is
    };

    virtual ~CharacterController() = default;
    virtual void setInput(const Input& input) = 0;   // used by every sub-step until changed
    virtual glm::vec3 feetPosition() const = 0;
    virtual glm::vec3 velocity() const = 0;
    virtual void setVelocity(const glm::vec3& v) = 0;
    virtual bool onGround() const = 0;
    virtual bool justJumped() const = 0;             // a jump started during the last step
    virtual void teleport(const glm::vec3& feetPosition) = 0;
    virtual uint64_t rootPartId() const = 0;
    virtual float radius() const = 0;
    virtual float height() const = 0;
};

} // namespace rift
