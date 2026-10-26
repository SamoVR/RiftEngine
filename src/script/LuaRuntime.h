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
#include "script/RemoteValue.h"

#include <cstdint>
#include <memory>
#include <string>

namespace rift {

class GameSession;
struct Instance;
struct TouchEvent;
struct AnimEvent;
enum class GuiEvent;

// What a Tool reports to the scripts (GameSession decides when).
enum class ToolEvent { Equipped, Unequipped, Activated, Deactivated };
// What a ProximityPrompt reports: Triggered, TriggerEnded, PromptButtonHoldBegan / Ended, PromptShown / Hidden.
enum class PromptEvent { Triggered, TriggerEnded, HoldBegan, HoldEnded, Shown, Hidden };

class LuaRuntime {
public:
    // `side`: the server runs Scripts on the server's world, a client the
    // LocalScripts of player `client` on that player's world (see GameSession).
    LuaRuntime(GameSession& session, NetworkSide side, int client = 0);
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
    void onCharacterAdded(uint64_t player, Instance& character);
    void onCharacterRemoving(uint64_t player, Instance& character);
    void onToolEvent(Instance& tool, ToolEvent e);
    void onHumanoidDied(Instance& humanoid);
    void onSeated(Instance& humanoid, bool active, uint64_t seat);     // Humanoid.Seated
    void onPromptEvent(Instance& prompt, PromptEvent e, Instance* player);
    void onHealthChanged(Instance& humanoid, float health);
    void onJumping(Instance& humanoid);
    void onRunning(Instance& humanoid, float speed);
    void onSwimming(Instance& humanoid, float speed);
    // Enum.HumanoidStateType values (also fires FreeFalling when the fall starts / ends).
    void onHumanoidStateChanged(Instance& humanoid, int oldState, int newState);
    // An AnimationTrack this side loaded: Stopped, Ended, DidLoop, KeyframeReached, markers.
    void onAnimationEvent(const AnimEvent& e);
    void onGuiEvent(Instance& element, GuiEvent e);
    void onSoundEnded(Instance& sound);
    // Chat: Player.Chatted (server), TextChatService.SendingMessage (the speaker's
    // client) and MessageReceived (every client; speaker "" for a system message).
    void onPlayerChatted(uint64_t player, const std::string& text);
    void onChatSending(const std::string& text);
    void onChatMessage(const std::string& speaker, uint64_t player, const std::string& text);
    // A RemoteEvent / RemoteFunction message for this side (GameSession delivers them).
    void onRemoteMessage(const RemoteMessage& message);
    NetworkSide side() const;
    int client() const; // which player's client (Client side)

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
