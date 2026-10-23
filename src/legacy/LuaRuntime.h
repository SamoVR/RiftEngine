#pragma once
//
// LuaRuntime.h -- runs the scene's Script objects with Lua 5.4 (via sol2).
//
// One LuaRuntime lives for one Play session (GameRuntime creates it on Play and
// destroys it on Stop), so every run starts from a fresh, sandboxed Lua state.
//
// The scripting API is Roblox-like: workspace, Instance.new, Vector3, Color3,
// part.Touched:Connect(...), task.wait / task.spawn, RunService.Heartbeat,
// UserInputService, Players.LocalPlayer ... See SCRIPTING.md for the full list.
//
// sol2 is heavy to compile, so it is kept out of this header (pimpl): only
// LuaRuntime.cpp includes it.
//
#include <memory>
#include <string>

class GameRuntime;
class ScriptStore;
class Object;

class LuaRuntime {
public:
    explicit LuaRuntime(GameRuntime& game);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Builds the API and starts every enabled Script object in the scene.
    // `scripts` must outlive this object.
    void start(ScriptStore& scripts);

    // Once per frame: input events, RunService.Heartbeat, task.wait wake-ups.
    void update(float dt, bool allowInput);

    // A Script object entered the world during play (cloned / re-parented).
    void scriptAddedToWorld(Object* script);

    // Touched plumbing (GameRuntime does the collision test).
    bool hasTouchedListeners(const Object* part) const;
    void fireTouched(Object* part, Object* other);

    // Syntax check without running anything. Returns "" if the source is OK,
    // otherwise "chunk:line: message".
    static std::string checkSyntax(const std::string& source, const std::string& chunkName);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
