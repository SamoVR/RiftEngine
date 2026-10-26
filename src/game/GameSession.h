#pragma once
//
// GameSession.h -- one run of the game (Play in the editor, or the shipped
// player). It plays on a CLONE of the scene, so the edited scene is never
// touched and stopping is simply destroying the session.
//
// Like Roblox, a run has a server and clients in one process:
//
//   server   the authoritative world: Scripts run here, physics and the
//            character controllers simulate it, DataStores live here.
//   clients  one per player: a copy of the world (see Replicator) with the
//            player's LocalScripts, GUI (PlayerGui), camera and Backpack bar.
//
// Options::playerCount > 1 simulates several players (Roblox Studio's local
// server test): every player has a character and a client; the one in
// focus() gets the keyboard and mouse, is drawn, and is heard.
//
// Each update():  input -> clients' RenderStepped / Stepped -> characters +
// tools + physics -> touches / deaths / fallen parts -> replication ->
// Heartbeat, waits -> remote messages -> cameras -> GUI -> sounds.
//
#include "scene/Scene.h"
#include "game/Animation.h"
#include "game/GameCamera.h"
#include "game/GuiSystem.h"
#include "script/RemoteValue.h"
#include "core/Log.h"

#include <array>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rift {

class Input;
class ScriptStore;
class PhysicsWorld;
class CharacterController;
class LuaRuntime;
class AudioSystem;
class Replicator;

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
        std::string playerName = "Player";     // with several players: Player1, Player2, ...
        int playerCount = 1;                   // simulated players (1..8)
        bool spawnCharacter = true;
    };
    enum class Outcome { None, Won, Lost };
    struct LogLine {
        std::string text;
        LogLevel level = LogLevel::Info;
        double time = 0.0;
        uint64_t script = 0; // the script that printed it (0 = engine)
        NetworkSide side = NetworkSide::Server;
        int client = 0;      // which player's client (side == Client)
    };

    // One slot of a player's Backpack bar (screen pixels, relative to the view).
    struct HotbarSlot {
        uint64_t tool = 0;   // 0: an empty slot (shown while the inventory is open)
        int index = 0;       // 0..9
        int number = 0;      // the key that equips it (1..9, 0 for the tenth)
        bool equipped = false;
        glm::vec2 pos{ 0.0f }, size{ 0.0f };
    };
    // The inventory the ` key opens (Roblox's backpack): every tool in a grid
    // with a search box, above the bar; tools are dragged into the bar's slots.
    struct InventoryView {
        bool open = false;
        glm::vec2 pos{ 0.0f }, size{ 0.0f };             // the window
        glm::vec2 searchPos{ 0.0f }, searchSize{ 0.0f };
        glm::vec2 gridPos{ 0.0f }, gridSize{ 0.0f };     // cells outside are scrolled away
        struct Cell { uint64_t tool = 0; int hotbarIndex = -1; bool equipped = false; glm::vec2 pos{ 0.0f }, size{ 0.0f }; };
        std::vector<Cell> cells;
        int toolCount = 0;                                // before the search
        int maxScroll = 0;                                // rows that can be scrolled away
        // The button beside the bar that opens it.
        bool button = false;
        glm::vec2 buttonPos{ 0.0f }, buttonSize{ 0.0f };
    };

    // A line of the chat.
    struct ChatMessage {
        std::string speaker;   // the player's name ("" for a system message)
        uint64_t playerId = 0;
        std::string text;
        double time = 0.0;
        Color3 color{ 1.0f, 1.0f, 1.0f }; // the name's colour (a system message: the text's)
    };
    // A speech bubble over a character's head (or any part).
    struct ChatBubble {
        uint64_t part = 0;
        std::string text;
        double time = 0.0;
    };
    // The ProximityPrompt a player sees (view pixels), for drawing and clicking.
    struct PromptView {
        bool visible = false;
        uint64_t prompt = 0;
        glm::vec2 pos{ 0.0f }, size{ 0.0f }; // the box around the prompt's point
        float progress = 0.0f;               // a hold under way: 0..1
        std::string key, action, object;
    };
    // Where the chat goes (view pixels): the messages, top left, with the box to type in under them.
    struct ChatView {
        bool enabled = false, open = false;
        glm::vec2 pos{ 0.0f }, size{ 0.0f };            // the messages
        glm::vec2 inputPos{ 0.0f }, inputSize{ 0.0f };  // the box
        float textSize = 16.0f;
        float idle = 0.0f;                               // seconds since the last message or use
    };

    // One player: its client world and everything that belongs to it.
    struct Client {
        Client();
        ~Client();
        int index = 0;
        uint64_t playerId = 0, playerGuiId = 0, backpackId = 0; // same ids on every side
        std::unique_ptr<Scene> scene;
        std::unique_ptr<Replicator> replicator;
        std::unique_ptr<LuaRuntime> lua;
        std::unique_ptr<GuiSystem> gui;
        GameCamera camera;
        ViewportInput view;           // only the client in focus gets the real one
        bool mouseLock = false, scriptMouseLock = false;
        bool backpackBar = true;      // StarterGui:SetCoreGuiEnabled(Backpack, ...)
        bool playerList = true;       // StarterGui:SetCoreGuiEnabled(PlayerList, ...): the leaderboard
        bool chatWindow = true;       // SetCoreGuiEnabled(Chat, ...) / ChatWindowConfiguration.Enabled
        bool bubbleChat = true;       // BubbleChatConfiguration.Enabled: bubbles over heads

        // Chat: what this player has seen, and the box being typed in.
        std::vector<ChatMessage> chat;
        bool chatOpen = false;
        std::string chatDraft;
        double chatActivity = -1000.0;

        // Character (server instances)
        uint64_t characterId = 0, humanoidId = 0, rootPartId = 0;
        glm::vec3 rootOffset{ 0.0f }, rootRotation{ 0.0f }; // the root part's place in the character model
        // Workspace.Camera in this client's world, and what it was last set to
        // (a change since then came from the Properties panel or a script).
        uint64_t cameraId = 0;
        CFrameValue cameraFrame;
        float cameraFov = 0.0f;
        int cameraKind = -1;
        CharacterController* controller = nullptr;
        float lastHealth = 0.0f, respawnTimer = 0.0f, facingYaw = 0.0f, runningSpeed = -1.0f;
        bool dead = false, guiFilled = false, walkTarget = false;
        glm::vec3 walkTo{ 0.0f };
        std::vector<uint64_t> deathPieces;

        // Tools: the bar's ten numbered slots (0 = empty), every tool in the
        // order it arrived (the inventory), what is held, the mouse button.
        std::array<uint64_t, 10> hotbarTools{};
        std::vector<uint64_t> inventory;
        bool inventoryOpen = false, searchFocused = false, overInventory = false;
        std::string inventorySearch;
        int inventoryScroll = 0;      // rows scrolled away
        uint64_t dragTool = 0;        // pressed on a slot or cell; dragging once it moves
        int dragFrom = -1;            // the bar slot it came from (-1: the inventory)
        bool dragging = false;
        glm::vec2 dragStart{ 0.0f };
        uint64_t equipped = 0;
        bool activating = false;
        std::unordered_map<uint64_t, glm::mat4> toolOffsets; // other parts of the held tool, relative to its Handle

        // Seats: the one sat on, how high the hips are above the root, no sitting again until then.
        uint64_t seatId = 0;
        float seatHip = 0.25f;
        double seatCooldownUntil = 0.0;
        // The ProximityPrompt shown, and its key (or click) being held.
        uint64_t prompt = 0;
        glm::vec3 promptPosition{ 0.0f };
        float promptHold = 0.0f;
        bool promptPressed = false, promptHolding = false, promptTriggered = false;

        // Humanoid state (Enum.HumanoidStateType) for StateChanged and the Animate script.
        HumanoidState state = HumanoidState::Running;
        bool swimming = false;
        float swimSpeed = -1.0f;
    };

    GameSession(const Scene& editScene, ScriptStore& scripts, Input& input, Options options);
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    // Spawns the players and starts the scripts.
    void start();
    // `view` is the client in focus's view; the others get no input.
    void update(float dt, const ViewportInput& view);

    // ---- players / clients
    int clientCount() const { return static_cast<int>(clients_.size()); }
    Client& client(int index) { return *clients_[static_cast<size_t>(index)]; }
    const Client& client(int index) const { return *clients_[static_cast<size_t>(index)]; }
    int focus() const { return focus_; }
    void setFocus(int index); // which player gets input, is drawn and heard
    Client& focused() { return client(focus_); }
    const Client& focused() const { return client(focus_); }
    Client* clientOfPlayer(uint64_t playerId);
    Client* clientOfCharacter(uint64_t characterId);
    Client* clientOfHumanoid(uint64_t humanoidId);

    // ---- the worlds (client = the one in focus)
    Scene& scene() { return *scene_; }                 // the server's
    const Scene& scene() const { return *scene_; }
    Scene& clientScene() { return *focused().scene; }
    const Scene& clientScene() const { return *focused().scene; }
    Scene& scene(NetworkSide side) { return side == NetworkSide::Server ? *scene_ : clientScene(); }
    LuaRuntime& lua() { return *lua_; }                // runs Scripts
    LuaRuntime& clientLua() { return *focused().lua; } // runs the focused player's LocalScripts

    // ---- state
    PhysicsWorld& physics() { return *physics_; }
    AnimationSystem& animation() { return *animation_; }
    // Humanoid:GetState() for a player's Humanoid (Running for anyone else's).
    HumanoidState humanoidState(uint64_t humanoidId) const;
    ScriptStore& scripts() { return scripts_; }
    Input& input() { return input_; }
    GuiSystem& gui() { return *focused().gui; }
    GameCamera& camera() { return focused().camera; }
    const GameCamera& camera() const { return focused().camera; }
    // The focused player's character while its camera is in first person (it
    // is not drawn then, so the camera does not look out of the head); 0 otherwise.
    uint64_t firstPersonCharacter() const;
    AudioSystem* audio() const { return options_.audio; }
    const Options& options() const { return options_; }
    const ViewportInput& viewport() const { return focused().view; }
    double time() const { return time_; }
    bool paused() const { return paused_; }
    void setPaused(bool paused) { paused_ = paused; }
    // First-person / MouseBehavior.LockCenter: the host should hide and lock the cursor.
    bool wantsMouseLock() const { return focused().mouseLock; }
    // The focused player is typing into a text box of the game (Esc is the box's).
    bool typing() const { return focused().searchFocused || focused().chatOpen; }
    void setMouseLockRequested(int client, bool lock);

    // ---- the focused player (ids are the same on every side)
    uint64_t playerId() const { return focused().playerId; }
    uint64_t characterId() const { return focused().characterId; }
    uint64_t humanoidId() const { return focused().humanoidId; }
    uint64_t rootPartId() const { return focused().rootPartId; }
    Instance* player() const;     // server instances
    Instance* character() const;
    Instance* humanoid() const;
    Instance* rootPart() const;
    void loadCharacter(uint64_t playerId);                          // (re)spawn now
    void moveCharacterTo(uint64_t humanoidId, const glm::vec3& target); // Humanoid:MoveTo

    // ---- tools (server side; clients ask through these too)
    void equipTool(int client, uint64_t tool);  // from the Backpack (or the world) into the hand
    void unequipTools(int client);              // back into the Backpack
    void dropTool(int client);                  // into the world (CanBeDropped)
    void activateTool(uint64_t tool, bool down);// Activated / Deactivated on the server and its owner
    std::vector<HotbarSlot> hotbar(int client) const;
    InventoryView inventory(int client) const;
    void setInventoryOpen(int client, bool open);
    // Puts a tool in a bar slot (0..9; -1 takes it off the bar). A tool already there swaps places.
    void assignHotbar(int client, uint64_t tool, int index);

    // ---- seats and prompts
    void sit(uint64_t humanoidId, uint64_t seatId); // Seat:Sit(humanoid)
    PromptView promptView(int client) const;

    // ---- chat
    // `client` says `text`: Player.Chatted on the server, a line for everyone and a bubble over the head.
    void chat(int client, const std::string& text);
    // A line without a speaker, for one player (-1: everyone).
    void systemMessage(int client, const std::string& text, const Color3& color = Color3(0.8f, 0.8f, 0.85f));
    void showBubble(uint64_t part, const std::string& text);
    const std::deque<ChatBubble>& bubbles() const { return bubbles_; }
    ChatView chatView(int client) const;
    void setChatOpen(int client, bool open);
    static Color3 nameColor(const std::string& name);

    // ---- client <-> server messages (delivered next frame)
    void postRemote(RemoteMessage message);

    // ---- output
    void log(LogLevel level, const std::string& text, uint64_t script = 0, NetworkSide side = NetworkSide::Server, int client = 0);
    const std::deque<LogLine>& logLines() const { return log_; }
    uint64_t logCounter() const { return logCounter_; } // grows with every line (UI auto-scroll)
    void clearLog() { log_.clear(); }
    void showMessage(const std::string& text, float seconds);
    const std::string& message() const { return message_; }
    void finish(bool won, const std::string& text);
    Outcome outcome() const { return outcome_; }

    // ---- sounds (Sound instances; what the focused player hears)
    void playSound(Instance& sound);
    void stopSound(Instance& sound);
    void pauseSound(Instance& sound, bool paused);
    bool soundPlaying(const Instance& sound) const;
    float soundTime(const Instance& sound) const;
    float soundLength(const Instance& sound) const;

    // ---- camera helpers (screen = view pixels)
    bool screenPointToRay(int client, const glm::vec2& point, glm::vec3& origin, glm::vec3& direction) const;
    glm::vec3 worldToScreen(int client, const glm::vec3& world) const; // z = depth (<= 0: behind the camera)
    bool screenPointToRay(const glm::vec2& point, glm::vec3& origin, glm::vec3& direction) const { return screenPointToRay(focus_, point, origin, direction); }
    glm::vec3 worldToScreen(const glm::vec3& world) const { return worldToScreen(focus_, world); }

private:
    std::unique_ptr<Scene> scene_;   // server
    std::vector<std::unique_ptr<Client>> clients_;
    int focus_ = 0;
    ScriptStore& scripts_;
    Input& input_;
    Options options_;
    std::unique_ptr<PhysicsWorld> physics_;
    std::unique_ptr<AnimationSystem> animation_;
    std::unique_ptr<LuaRuntime> lua_;

    double time_ = 0.0;
    bool started_ = false;
    bool paused_ = false;

    // client <-> server
    std::vector<RemoteMessage> remotes_;

    // output
    std::deque<LogLine> log_;
    uint64_t logCounter_ = 0;
    std::string message_;
    float messageTimer_ = 0.0f;
    Outcome outcome_ = Outcome::None;

    // sounds of the focused client's world
    struct SoundState { uint32_t handle = 0; bool paused = false; };
    std::unordered_map<uint64_t, SoundState> sounds_;

    // speech bubbles over heads
    std::deque<ChatBubble> bubbles_;

    // tools that were just dropped: nobody picks them up again for a moment
    std::unordered_map<uint64_t, double> pickupBlockedUntil_;

    void addClient(int index);
    void spawnCharacter(Client& c);
    void destroyCharacter(Client& c);
    void fillPlayerGui(Client& c, bool respawn);
    void fillBackpack(Client& c);
    void setUpClientPlayer(Client& c);
    glm::vec3 spawnPosition() const;
    void handleInput(Client& c);
    void updateCharacter(Client& c, float dt);
    void afterPhysics(float dt);
    void placeCharacter(Client& c, float dt);
    void updateTools(Client& c);
    void pickUpTools();
    void updateSlots(Client& c);
    void sitOn(Client& c, Instance& humanoid, Instance& seat);
    void standUp(Client& c);
    void updatePrompt(Client& c, bool typing, bool& overGui);
    float frameDt_ = 0.0f;
    void updateCamera(Client& c, float dt);
    Instance* cameraObject(Client& c); // (re)makes the client's Workspace.Camera
    void updateSounds();
    void stopAllSounds();
    void deliverRemotes();
    Instance* toolOwnerCharacter(const Instance& tool) const;
    // The server's runtime, then every client's.
    template <class F> void forAll(F&& f);
};

} // namespace rift
