#pragma once
//
// GameSession.h -- one run of the game (Play in the editor, or the shipped
// player). It plays on a CLONE of the scene, so the edited scene is never
// touched and stopping is simply destroying the session.
//
// Each update():  input -> scripts (RenderStepped, Stepped) -> character +
// physics -> touches / deaths / fallen parts -> scripts (Heartbeat, waits)
// -> camera -> GUI -> sounds.
//
#include "scene/Scene.h"
#include "game/GameCamera.h"
#include "game/GuiSystem.h"
#include "core/Log.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

namespace rift {

class Input;
class ScriptStore;
class PhysicsWorld;
class CharacterController;
class LuaRuntime;
class AudioSystem;

// What the host knows about the area the game is drawn in.
struct ViewportInput {
    glm::vec2 size{ 1280.0f, 720.0f }; // pixels
    glm::vec2 mouse{ 0.0f };           // relative to the view's top-left corner
    bool hovered = true;               // the mouse is over the view
    bool keyboard = true;              // key presses go to the game
};

class GameSession {
public:
    struct Options {
        std::filesystem::path dataStoreFolder; // DataStoreService files; empty = kept in memory
        AudioSystem* audio = nullptr;          // null: silent (tests)
        bool isStudio = true;                  // RunService:IsStudio()
        std::string playerName = "Player";
        bool spawnCharacter = true;
    };
    enum class Outcome { None, Won, Lost };
    struct LogLine {
        std::string text;
        LogLevel level = LogLevel::Info;
        double time = 0.0;
        uint64_t script = 0; // the script that printed it (0 = engine)
    };

    GameSession(const Scene& editScene, ScriptStore& scripts, Input& input, Options options);
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Spawns the player and starts the scripts.
    void start();
    void update(float dt, const ViewportInput& view);

    // ---- state
    Scene& scene() { return *scene_; }
    const Scene& scene() const { return *scene_; }
    PhysicsWorld& physics() { return *physics_; }
    ScriptStore& scripts() { return scripts_; }
    Input& input() { return input_; }
    LuaRuntime& lua() { return *lua_; }
    GuiSystem& gui() { return gui_; }
    GameCamera& camera() { return camera_; }
    const GameCamera& camera() const { return camera_; }
    AudioSystem* audio() const { return options_.audio; }
    const Options& options() const { return options_; }
    const ViewportInput& viewport() const { return view_; }
    double time() const { return time_; }
    bool paused() const { return paused_; }
    void setPaused(bool paused) { paused_ = paused; }
    // First-person / MouseBehavior.LockCenter: the host should hide and lock the cursor.
    bool wantsMouseLock() const { return mouseLock_; }
    void setMouseLockRequested(bool lock) { scriptMouseLock_ = lock; }

    // ---- player
    Instance* player() const;
    Instance* character() const;
    Instance* humanoid() const;
    Instance* rootPart() const;
    void loadCharacter();           // (re)spawn now
    void moveCharacterTo(const glm::vec3& target); // Humanoid:MoveTo

    // ---- output
    void log(LogLevel level, const std::string& text, uint64_t script = 0);
    const std::deque<LogLine>& logLines() const { return log_; }
    uint64_t logCounter() const { return logCounter_; } // grows with every line (UI auto-scroll)
    void clearLog() { log_.clear(); }
    void showMessage(const std::string& text, float seconds);
    const std::string& message() const { return message_; }
    void finish(bool won, const std::string& text);
    Outcome outcome() const { return outcome_; }

    // ---- sounds (Sound instances)
    void playSound(Instance& sound);
    void stopSound(Instance& sound);
    void pauseSound(Instance& sound, bool paused);
    bool soundPlaying(const Instance& sound) const;
    float soundTime(const Instance& sound) const;
    float soundLength(const Instance& sound) const;

    // ---- camera helpers (screen = view pixels)
    bool screenPointToRay(const glm::vec2& point, glm::vec3& origin, glm::vec3& direction) const;
    glm::vec3 worldToScreen(const glm::vec3& world) const; // z = depth (<= 0: behind the camera)

private:
    std::unique_ptr<Scene> scene_;
    ScriptStore& scripts_;
    Input& input_;
    Options options_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<LuaRuntime> lua_;
    GuiSystem gui_;
    GameCamera camera_;
    ViewportInput view_;

    double time_ = 0.0;
    bool started_ = false;
    bool paused_ = false;
    bool mouseLock_ = false;
    bool scriptMouseLock_ = false;

    // player / character
    uint64_t playerId_ = 0, characterId_ = 0, humanoidId_ = 0, rootPartId_ = 0;
    CharacterController* controller_ = nullptr;
    float lastHealth_ = 0.0f;
    bool dead_ = false;
    float respawnTimer_ = 0.0f;
    bool walkTarget_ = false;
    glm::vec3 walkTo_{ 0.0f };
    float facingYaw_ = 0.0f;
    float runningSpeed_ = -1.0f;
    std::vector<uint64_t> deathPieces_;     // limbs of the last character that died

    // output
    std::deque<LogLine> log_;
    uint64_t logCounter_ = 0;
    std::string message_;
    float messageTimer_ = 0.0f;
    Outcome outcome_ = Outcome::None;

    // sounds
    struct SoundState { uint32_t handle = 0; bool paused = false; };
    std::unordered_map<uint64_t, SoundState> sounds_;

    void spawnCharacter();
    void destroyCharacter();
    glm::vec3 spawnPosition() const;
    void updateCharacter(float dt);
    void afterPhysics(float dt);
    void updateCamera(float dt);
    void updateSounds();
};

} // namespace rift
