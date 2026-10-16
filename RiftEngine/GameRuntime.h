#pragma once
//
// GameRuntime.h -- play mode ("Play" / "Stop" in the toolbar, like Roblox
// Studio's Play button). While running it
//   * spawns a player character (WASD / Space, the camera follows it),
//   * simulates physics for parts with Anchored = off (gravity, collisions,
//     bounciness),
//   * runs the scene's Script objects with Lua (see LuaRuntime.h).
// On Stop everything the game changed is put back exactly as it was, so the
// edited scene is never affected by a test run: properties, hierarchy,
// "destroyed" objects, and anything the scripts created are all undone.
//
#include "Scene.h"
#include "Camera.h"
#include "InputManager.h"

#include <glm/glm.hpp>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ScriptStore;
class LuaRuntime;

class GameRuntime {
public:
    GameRuntime(Scene& scene, Camera& camera, InputManager& input);
    ~GameRuntime();

    // Scripts are read from here (the editor's in-memory copies, so unsaved
    // edits can be tested). Must be set before start().
    void setScriptStore(ScriptStore* store) { scripts_ = store; }

    void start();
    void stop();
    bool isRunning() const { return running_; }
    void togglePause() { paused_ = !paused_; }
    bool isPaused() const { return paused_; }

    // allowPlayerInput: false while the user types in a UI field.
    void update(float deltaTime, bool allowPlayerInput);

    // ---- output for the UI
    struct LogLine { std::string text; bool isError; float time; };
    const std::vector<LogLine>& log() const { return log_; }
    void clearLog() { log_.clear(); }
    const std::string& message() const { return message_; }
    float messageTimeLeft() const { return messageTimer_; }
    enum class Outcome { None, Won, Lost };
    Outcome outcome() const { return outcome_; }
    float elapsed() const { return time_; }
    const Object* player() const { return player_.get(); }

    // Checks a Lua script for syntax errors without running it (script editor).
    // Returns "" when OK, otherwise "chunkName:line: message".
    static std::string checkSyntax(const std::string& source, const std::string& chunkName = "script");

    // ---- tuning (shown in Settings > Game)
    float gravity = -30.0f;
    float walkSpeed = 8.0f;
    float jumpSpeed = 12.0f;
    float killPlaneY = -60.0f;

    // =======================================================================
    //  Script-facing API (what LuaRuntime calls). Objects are addressed by
    //  stable id, never by raw pointer, so a script holding a reference to a
    //  destroyed object gets a clear error instead of a crash.
    // =======================================================================
    Scene& scene() { return scene_; }
    InputManager& input() { return input_; }
    void logLine(const std::string& text, bool isError);

    // nullptr if the id is unknown or the object was destroyed.
    Object* findObject(uint64_t id) const;
    Object* playerObject() { return player_.get(); }
    glm::vec3& playerVelocity() { return playerVelocity_; }
    // Is the object part of the running world (scene or player)?
    bool isInWorld(const Object* o) const;

    // New parts start detached (not in the world) until given a Parent, like
    // Instance.new in Roblox. Returns nullptr for an unknown class name.
    Object* createObject(const std::string& className);
    // Deep copy (detached). Scripts in the copy start when it enters the world.
    Object* cloneSubtree(Object* o);
    // newParent == nullptr && toWorld: workspace.  newParent == nullptr && !toWorld: Parent = nil.
    bool setObjectParent(Object* o, Object* newParent, bool toWorld, std::string& error);
    // Objects created by scripts are freed; objects from the scene are hidden
    // and restored on Stop.
    void destroyObject(Object* o);

    void respawnPlayer();
    void showMessage(const std::string& text, float seconds);
    void finish(bool won, const std::string& text);

    // World-space axis-aligned bounds (also used by the editor to drop new parts onto surfaces).
    struct Box { glm::vec3 lo, hi; };
    static Box worldBox(const Object& o);
    static bool overlaps(const Box& a, const Box& b, float margin = 0.0f);
    struct Obb { glm::vec3 center; glm::vec3 axis[3]; glm::vec3 half; };
    static Obb worldObb(const Object& o);
    // Separating-axis test; on overlap `push` moves `box` out of `obb`.
    static bool separate(const Box& box, const Obb& obb, glm::vec3& push, float margin = 0.0f);

private:
    Scene& scene_;
    Camera& camera_;
    InputManager& input_;
    ScriptStore* scripts_ = nullptr;

    bool running_ = false;
    bool paused_ = false;
    float time_ = 0.0f;

    // Everything play mode may change, captured on Play and restored on Stop
    // (including the hierarchy, which scripts can rearrange).
    struct Snapshot {
        glm::vec3 position, rotation, scale, color;
        Object::GameProps game;
        Object* parent = nullptr;
        std::vector<Object*> children;
    };
    std::unordered_map<Object*, Snapshot> snapshot_;
    Camera::Orbit savedOrbit_{};

    std::unique_ptr<LuaRuntime> lua_;
    std::set<std::pair<const Object*, const Object*>> touching_; // contacts last frame (Touched fires on the rising edge)

    // Player character: owned here (not in the Explorer, never saved); Engine draws it.
    std::unique_ptr<Object> player_;
    glm::vec3 playerVelocity_{ 0.0f };
    glm::vec3 spawnPoint_{ 0.0f, 3.0f, 0.0f };
    bool playerGrounded_ = false;

    std::vector<LogLine> log_;
    std::vector<Object*> objects_; // scene snapshot, refreshed once per update
    std::string message_;
    float messageTimer_ = 0.0f;
    Outcome outcome_ = Outcome::None;

    // Object registry for the scripting API.
    std::unordered_map<uint64_t, Object*> byId_;                        // everything addressable (scene + player + created)
    std::unordered_set<Object*> created_;                               // made by scripts during this session
    std::unordered_map<uint64_t, std::unique_ptr<Object>> detached_;    // created, not in the world yet
    std::vector<Object*> dead_;                                         // created + destroyed, freed at end of frame

    void spawnPlayer();
    void updatePlayer(float dt, bool allowInput);
    void stepPhysics(float dt);
    // Pushes `box` (moving by `velocity`) out of every solid part except `self`.
    // Returns true if it ended up standing on something.
    bool resolveCollisions(Box& box, glm::vec3& velocity, const Object* self, float bounciness);
    void detectTouches();
    void flushDead();
    std::vector<Object*> collectSubtree(Object* root) const;
};
