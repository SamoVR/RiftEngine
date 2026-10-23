#pragma once
//
// LuaRuntime.h -- runs a game session's Scripts with Lua 5.4 (sol2), with a
// Roblox-style API (see docs/SCRIPTING.md for the full reference).
//
// One LuaRuntime lives for one GameSession, so every Play starts from a
// fresh, sandboxed Lua state:
//   * no io / os.execute / load / dofile / debug; memory is capped;
//   * a script that runs for ~1 s without yielding is stopped with an error;
//   * scripts address instances by id, so using a destroyed Instance raises
//     an error instead of crashing.
//
// Events (Changed, ChildAdded, Touched, ...) are DEFERRED like Roblox's
// default: handlers run right after the code that caused them yields or
// returns, never in the middle of it.
//
// sol2 is heavy to compile, so it stays out of this header (pimpl).
//
#include <cstdint>
#include <memory>
#include <string>

namespace rift {

class GameSession;
struct Instance;
struct TouchEvent;
enum class GuiEvent;

class LuaRuntime {
public:
    explicit LuaRuntime(GameSession& session);
    ~LuaRuntime();
    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // Builds the API and starts every enabled Script in the world.
    void start();

    // Frame phases (see GameSession::update).
    void beforePhysics(float dt);  // input events, RenderStepped, Stepped
    void afterPhysics(float dt);   // Heartbeat, task.wait wake-ups, tweens, Debris

    // Events from the session.
    void onTouch(const TouchEvent& e);
    void onCharacterAdded(Instance& character);
    void onCharacterRemoving(Instance& character);
    void onHumanoidDied(Instance& humanoid);
    void onHealthChanged(Instance& humanoid, float health);
    void onJumping(Instance& humanoid);
    void onRunning(Instance& humanoid, float speed);
    void onGuiEvent(Instance& element, GuiEvent e);
    void onSoundEnded(Instance& sound);

    // Diagnostics.
    size_t threadCount() const;
    size_t memoryUsed() const;

    // Syntax check without running anything ("" if OK, else "chunk:line: message").
    static std::string checkSyntax(const std::string& source, const std::string& chunkName);

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace rift
