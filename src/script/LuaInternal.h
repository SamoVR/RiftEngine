#pragma once
//
// LuaInternal.h -- shared by the LuaRuntime*.cpp files only (includes sol2).
//
//   LuaRuntime.cpp   state, sandbox, coroutine scheduler, signals, deferred
//                    events, script start/stop, require, public API
//   LuaTypes.cpp     Vector3, Vector2, Color3, UDim, UDim2, CFrame, Enum,
//                    and Value <-> Lua conversion
//   LuaInstance.cpp  the Instance type: properties, methods, events
//   LuaServices.cpp  game, services (Players, RunService, TweenService, ...)
//
#include <sol/sol.hpp>

#include "script/LuaRuntime.h"
#include "game/GameSession.h"
#include "scene/Scene.h"
#include "scene/Reflection.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json.hpp>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rift::lua {

// ============================================================================
//  Lua-side value types
// ============================================================================
// Wrappers (not glm types directly) so sol2 binds exactly the operators below
// and nothing glm happens to define.
struct Vector3L { glm::vec3 v{ 0.0f }; };
struct Vector2L { glm::vec2 v{ 0.0f }; };
struct UDim { float scale = 0.0f, offset = 0.0f; };

// Position + rotation (Roblox CFrame). Rotation is orthonormal.
struct CFrame {
    glm::vec3 p{ 0.0f };
    glm::mat3 r{ 1.0f };
    glm::mat4 toMat4() const { glm::mat4 m(r); m[3] = glm::vec4(p, 1.0f); return m; }
    static CFrame fromMat4(const glm::mat4& m);
};

struct EnumType {
    std::string name;
    std::vector<std::pair<std::string, int>> items; // name, value
    const std::pair<std::string, int>* byName(const std::string& n) const;
    const std::pair<std::string, int>* byValue(int v) const;
};
struct EnumItem {
    const EnumType* type = nullptr;
    int value = 0;
    std::string name() const;
};
const EnumType* enumType(const std::string& name);          // nullptr if unknown
const std::vector<const EnumType*>& allEnumTypes();

struct Inst { uint64_t id = 0; };
struct AnimTrack { uint64_t id = 0; };   // an AnimationTrack (GameSession::animation())
struct KeyframeSequenceProvider {};

// ============================================================================
//  Signals
// ============================================================================
struct SignalState {
    struct Conn {
        uint64_t id = 0;
        sol::main_protected_function fn; // main-thread reference (see the Play crash fix)
        bool once = false;
        bool connected = true;
        uint64_t script = 0;             // script that connected it (disconnected when it stops)
    };
    std::vector<std::shared_ptr<Conn>> conns;
    std::vector<lua_State*> waiters;     // threads parked in :Wait()
    uint64_t nextId = 1;
    bool hasListeners() const { return !conns.empty() || !waiters.empty(); }
    void disconnect(uint64_t id);
};
struct Signal { std::shared_ptr<SignalState> s; };
struct Connection { std::weak_ptr<SignalState> s; uint64_t id = 0; };

// ============================================================================
//  Tweens
// ============================================================================
enum class Easing { Linear, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Bounce, Elastic };
enum class EasingDir { In, Out, InOut };
struct TweenInfo {
    float time = 1.0f;
    Easing style = Easing::Quad;
    EasingDir direction = EasingDir::Out;
    int repeatCount = 0;   // -1 = forever
    bool reverses = false;
    float delay = 0.0f;
};
enum class PlaybackState { Begin, Playing, Paused, Completed, Cancelled, Delayed };
struct TweenRecord {
    uint64_t id = 0;
    uint64_t instance = 0;
    TweenInfo info;
    struct Goal { std::string prop; Value from, to; bool cframe = false; CFrame cfFrom, cfTo; };
    std::vector<Goal> goals;
    PlaybackState state = PlaybackState::Begin;
    float elapsed = 0.0f;
    int cycle = 0;
    std::shared_ptr<SignalState> completed = std::make_shared<SignalState>();
};
struct TweenHandle { uint64_t id = 0; };
float ease(Easing style, EasingDir dir, float t);

// ============================================================================
//  Misc handles
// ============================================================================
struct DataModel {};
struct RunService {};
struct UserInputService {};
struct TweenService {};
struct Debris {};
struct CollectionService {};
struct HttpService {};
struct DataStoreService {};
struct DataStore { std::string name; };
struct Mouse {};
struct Camera {};
struct RaycastParams {
    std::vector<uint64_t> filter;
    bool include = false;
    bool respectCanCollide = false;
};
struct InputObject {
    int keyCode = 0;         // GLFW key, 0 if none
    int inputType = 0;       // index into Enum.UserInputType
    int state = 0;           // Enum.UserInputState
    glm::vec3 position{ 0.0f };
    glm::vec3 delta{ 0.0f };
};

// ============================================================================
//  Impl
// ============================================================================
struct SignalKey {
    uint64_t id;
    std::string name;
    bool operator==(const SignalKey& o) const { return id == o.id && name == o.name; }
};
struct SignalKeyHash {
    size_t operator()(const SignalKey& k) const { return std::hash<uint64_t>()(k.id) ^ (std::hash<std::string>()(k.name) * 31); }
};

} // namespace rift::lua

namespace rift {

struct LuaRuntime::Impl : public SceneObserver {
    GameSession& session;
    NetworkSide side;
    int client = 0;        // the player whose client this is (Client side)
    Scene& scene;          // this side's world
    GameCamera serverCamera; // workspace.CurrentCamera on the server (the client uses the session's)

    // NOTE: everything that holds Lua references is declared AFTER `lua`, so
    // it is destroyed before the state is closed.
    size_t memoryUsed = 0;
    size_t memoryLimit = 512u * 1024u * 1024u;
    // Destroyed instances that scripts still hold: a read-only snapshot, so
    // `child.Name` in a ChildRemoved / Destroying handler works like Roblox.
    // Dropped when Lua collects the handle (declared before `lua`: the
    // handles' __gc runs while the state closes).
    std::unordered_map<uint64_t, std::unique_ptr<Instance>> tombstones;
    sol::state lua;
    sol::table instanceCache;   // id -> Instance userdata (weak values): one Lua value per instance
    sol::table moduleCache;     // ModuleScript id / module name -> result
    std::unordered_set<std::string> loadingModules;

    // ---- coroutine scheduler
    struct ThreadRec {
        enum State { Running, Timed, Parked } state = Running;
        int ref = LUA_NOREF;
        uint64_t script = 0;  // owning script (killed with it)
        double sleptAt = 0.0;
        uint64_t seq = 0;
        bool kill = false;    // stop as soon as it yields (it was running when killed)
        int freshArgs = -1;   // >= 0: not started yet (task.delay); resume with this many stacked args
    };
    std::unordered_map<lua_State*, ThreadRec> threads;
    std::map<std::pair<double, uint64_t>, lua_State*> timed; // (wake time, sequence) -> thread
    uint64_t seqCounter = 0;
    double now = 0.0;
    int resumeDepth = 0;
    uint64_t currentScript = 0;                  // script whose code is running right now
    std::unordered_set<uint64_t> runningScripts; // started and not stopped

    struct ChildWait {
        lua_State* thread;
        uint64_t parent;
        std::string name;
        bool hasTimeout;
        double deadline;  // when hasTimeout
        double started;   // for the "infinite yield possible" warning
        bool warned;
    };
    std::vector<ChildWait> childWaits;

    // ---- signals and deferred events
    std::unordered_map<lua::SignalKey, std::shared_ptr<lua::SignalState>, lua::SignalKeyHash> signals;
    std::shared_ptr<lua::SignalState> heartbeat, stepped, renderStepped, inputBegan, inputEnded, inputChanged,
                                      playerAdded, mouseButton1Down, mouseButton1Up, mouseButton2Down, mouseButton2Up,
                                      mouseMove, wheelForward, wheelBackward;
    std::deque<std::function<void()>> pending;
    bool flushing = false;

    // ---- services state
    std::vector<lua::TweenRecord> tweens;
    uint64_t nextTweenId = 1;
    std::vector<std::pair<double, uint64_t>> debris; // (time, instance)
    std::unordered_map<std::string, std::shared_ptr<lua::SignalState>> tagAdded, tagRemoved;
    std::unordered_map<std::string, nlohmann::json> dataStores; // store name -> {key: value}
    int mouseBehavior = 0; // Enum.MouseBehavior
    std::unordered_map<std::string, uint64_t> renderBindings; // RunService:BindToRenderStep name -> connection id
    std::unordered_map<uint64_t, uint64_t> primaryParts; // Model id -> part id
    // AnimationTrack events (Stopped, Ended, DidLoop, KeyframeReached, "Marker:<name>"), by track id.
    std::unordered_map<uint64_t, std::unordered_map<std::string, std::shared_ptr<lua::SignalState>>> trackSignals;
    std::shared_ptr<lua::SignalState> trackSignal(uint64_t track, const std::string& name) {
        auto& s = trackSignals[track][name];
        if (!s) s = std::make_shared<lua::SignalState>();
        return s;
    }

    // ---- remotes and bindables
    // OnServerInvoke / OnClientInvoke / OnInvoke callbacks, by (instance, name).
    std::unordered_map<lua::SignalKey, sol::main_protected_function, lua::SignalKeyHash> callbacks;
    std::unordered_map<uint64_t, lua_State*> pendingCalls; // InvokeServer / InvokeClient waiting for the answer
    std::unordered_map<uint64_t, int> replyTo;             // server: which client an Invoke came from (by call id)
    sol::main_protected_function remoteRunner;              // runs a RemoteFunction handler and replies
    uint64_t nextCall = 1;

    Impl(GameSession& s, NetworkSide side, int client);
    ~Impl() override;

    lua_State* L() { return lua.lua_state(); }
    bool isClient() const { return side == NetworkSide::Client; }
    const char* sideName() const { return isClient() ? "client" : "server"; }
    // Fails with a clear message when an API is used on the wrong side.
    void requireSide(NetworkSide wanted, const char* what) const;
    GameSession::Client& me() const { return session.client(client); } // this client's player (Client side)
    // The view and camera this runtime's input and Camera API use (the server
    // borrows the focused player's, for workspace.CurrentCamera queries).
    const ViewportInput& view() const { return isClient() ? me().view : session.viewport(); }
    int viewClient() const { return isClient() ? client : session.focus(); }
    Instance* localPlayer() const;     // this side's Player (nil on the server, like Roblox)
    Instance* localCharacter() const;  // the character as this side sees it
    GameCamera& cam();                 // workspace.CurrentCamera

    // ---- errors / output
    [[noreturn]] static void fail(const std::string& message) { throw std::runtime_error(message); }
    void logScript(uint64_t script, const std::string& text, LogLevel level);
    std::string scriptLabel(uint64_t script) const;

    // ---- instances
    sol::object nil() { return sol::make_object(lua, sol::lua_nil); }
    sol::object instanceFor(uint64_t id);
    sol::object instanceOrNil(const Instance* i) { return i ? instanceFor(i->id) : nil(); }
    Instance* need(const lua::Inst& h);                 // throws if destroyed
    Instance* needPart(const lua::Inst& h, const char* member);
    sol::table toTable(const std::vector<Instance*>& list);

    // ---- signals
    std::shared_ptr<lua::SignalState> signalFor(uint64_t id, const std::string& name); // created on demand
    std::shared_ptr<lua::SignalState> findSignal(uint64_t id, const std::string& name) const;
    void queue(const std::shared_ptr<lua::SignalState>& s, std::function<int(lua_State*)> pushArgs);
    void queueInstanceEvent(uint64_t id, const std::string& name, std::function<int(lua_State*)> pushArgs);
    void fire(lua::SignalState& s, const std::function<int(lua_State*)>& pushArgs);
    void flush();
    int pushInstance(lua_State* T, uint64_t id);

    // ---- scheduler
    lua_State* newThread(uint64_t script);
    void finishThread(lua_State* T);
    void resume(lua_State* T, int nargs);
    void spawn(const sol::main_protected_function& fn, uint64_t script, const std::function<int(lua_State*)>& pushArgs);
    void runDue();
    void killScript(uint64_t script);

    // ---- scripts
    bool shouldRun(const Instance& script) const;
    void startScript(Instance& script);
    sol::object requireModule(const sol::object& target, lua_State* caller);
    sol::object runModule(const std::string& key, const std::string& path, const std::string& chunkName, lua_State* caller);

    // ---- conversions (LuaTypes.cpp)
    sol::object toLua(const Value& v, const PropertyInfo* prop);
    Value fromLua(const sol::object& o, const PropertyInfo& prop, std::string& error);
    Value attributeFromLua(const sol::object& o, std::string& error);
    sol::object attributeToLua(const Value& v);

    // ---- API construction
    void buildApi();
    void registerTypes();      // LuaTypes.cpp
    void registerInstance();   // LuaInstance.cpp
    void registerServices();   // LuaServices.cpp

    // ---- services (LuaServices.cpp)
    void updateTweens(float dt);
    void updateDebris();
    void fireInput();
    std::string dataStorePath(const std::string& name) const;
    nlohmann::json& dataStore(const std::string& name);
    void saveDataStore(const std::string& name);

    // ---- remotes (LuaServices.cpp)
    RemoteValue toRemote(const sol::object& o, int depth = 0);
    sol::object fromRemote(const RemoteValue& v);
    int pushRemoteArgs(lua_State* T, const std::vector<RemoteValue>& args);
    void handleRemote(const RemoteMessage& m);
    void reply(uint64_t call, NetworkSide to, int toClient, bool ok, std::string error, std::vector<RemoteValue> results);

    // ---- SceneObserver
    void onInstanceAdded(Instance& i) override;
    void onInstanceRemoving(Instance& i) override;
    void onParentChanged(Instance& i, Instance* oldParent) override;
    void onPropertyChanged(Instance& i, const PropertyInfo& prop) override;
    void onAttributeChanged(Instance& i, const std::string& name) override;
};

} // namespace rift
