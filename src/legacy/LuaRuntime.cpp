// LuaRuntime.cpp -- see LuaRuntime.h. Build notes:
//   * Lua is compiled as C++ and SOL_USING_CXX_LUA is defined, so a script error
//     is a real C++ exception and destructors of the bindings run while it
//     unwinds (compiled as C, Lua would longjmp over them and leak).
//   * SOL_ALL_SAFETIES_ON turns wrong argument types into Lua errors.
// SOL_ALL_SAFETIES_ON is defined for the whole build (Rift.props).
#include <sol/sol.hpp>

#include "script/LuaRuntime.h"
#include "game/GameRuntime.h"
#include "script/ScriptStore.h"
#include "core/Log.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {

// ============================================================================
//  Value types (Vector3, Color3) -- immutable value objects, like Roblox's
// ============================================================================
struct Vec3 {
    float x = 0, y = 0, z = 0;
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 unit() const { const float l = length(); return l > 1e-8f ? Vec3{ x / l, y / l, z / l } : Vec3{}; }
    float dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    Vec3 cross(const Vec3& o) const { return { y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x }; }
    Vec3 lerp(const Vec3& o, float t) const { return { x + (o.x - x) * t, y + (o.y - y) * t, z + (o.z - z) * t }; }
};
glm::vec3 toGlm(const Vec3& v) { return { v.x, v.y, v.z }; }
Vec3 fromGlm(const glm::vec3& v) { return { v.x, v.y, v.z }; }

struct Col3 { float r = 0, g = 0, b = 0; };

std::string numberText(float v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", static_cast<double>(v));
    return buf;
}

// ============================================================================
//  Handles. Lua never holds a raw Object*: it holds an id, resolved on every
//  access, so using a destroyed Instance is an error, not a crash.
// ============================================================================
struct Inst { uint64_t id = 0; }; // id 0 = workspace

struct SignalState {
    struct Conn {
        uint64_t id = 0;
        // MUST be a main-thread reference. A plain sol::protected_function
        // remembers the coroutine that called :Connect(); once that script
        // finished, Lua collected the coroutine and the next copy/destroy of
        // the handler dereferenced freed memory (the crash a few seconds after Play).
        sol::main_protected_function fn;
        bool once = false;
        bool connected = true;
        std::string label; // script that connected it (for error messages)
    };
    // shared_ptr: fire() snapshots the list every frame, and copying the
    // pointers is free while copying Lua references costs a registry ref each.
    std::vector<std::shared_ptr<Conn>> conns;
    std::vector<lua_State*> waiters; // threads parked in :Wait()
    uint64_t nextId = 1;
    bool hasListeners() const { return !conns.empty() || !waiters.empty(); }
    void disconnect(uint64_t id) {
        for (auto it = conns.begin(); it != conns.end(); ++it) {
            if ((*it)->id == id) { (*it)->connected = false; conns.erase(it); return; }
        }
    }
};
struct Signal { std::shared_ptr<SignalState> s; };
struct Connection { std::weak_ptr<SignalState> s; uint64_t id = 0; };

struct InstSignals {
    std::shared_ptr<SignalState> touched, childAdded, destroying;
};

// Tag types for the service objects.
struct GameSvc {};
struct PlayersSvc {};
struct PlayerHandle {};
struct RunSvc {};
struct InputSvc {};

// Runs inside every Lua instruction-count hook: kills runaway scripts.
std::chrono::steady_clock::time_point g_deadline;
void timeLimitHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() > g_deadline) {
        luaL_error(L, "script ran too long without yielding (infinite loop? use task.wait())");
    }
}
constexpr double kScriptBudgetSeconds = 1.0; // per resume of a coroutine
constexpr int kMaxResumeDepth = 48;          // signal handlers that fire signals that ...

const char* kPrelude = R"LUA(
function task.defer(f, ...)
    local args = table.pack(...)
    return task.spawn(function() task.wait(); f(table.unpack(args, 1, args.n)) end)
end
function task.delay(seconds, f, ...)
    local args = table.pack(...)
    return task.spawn(function() task.wait(seconds); f(table.unpack(args, 1, args.n)) end)
end
function math.clamp(x, lo, hi) return math.max(lo, math.min(hi, x)) end
function math.sign(x) if x > 0 then return 1 elseif x < 0 then return -1 else return 0 end end
function math.round(x) if x >= 0 then return math.floor(x + 0.5) else return -math.floor(-x + 0.5) end end
function math.lerp(a, b, t) return a + (b - a) * t end
)LUA";

std::string lowerCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Enum.KeyCode names -> GLFW key codes.
const std::vector<std::pair<std::string, int>>& keyTable() {
    static const std::vector<std::pair<std::string, int>> table = [] {
        std::vector<std::pair<std::string, int>> t;
        for (char c = 'A'; c <= 'Z'; ++c) t.emplace_back(std::string(1, c), GLFW_KEY_A + (c - 'A'));
        const char* digits[] = { "Zero", "One", "Two", "Three", "Four", "Five", "Six", "Seven", "Eight", "Nine" };
        for (int i = 0; i < 10; ++i) t.emplace_back(digits[i], GLFW_KEY_0 + i);
        t.emplace_back("Space", GLFW_KEY_SPACE);
        t.emplace_back("LeftShift", GLFW_KEY_LEFT_SHIFT);
        t.emplace_back("RightShift", GLFW_KEY_RIGHT_SHIFT);
        t.emplace_back("LeftControl", GLFW_KEY_LEFT_CONTROL);
        t.emplace_back("RightControl", GLFW_KEY_RIGHT_CONTROL);
        t.emplace_back("LeftAlt", GLFW_KEY_LEFT_ALT);
        t.emplace_back("Return", GLFW_KEY_ENTER);
        t.emplace_back("Tab", GLFW_KEY_TAB);
        t.emplace_back("Escape", GLFW_KEY_ESCAPE);
        t.emplace_back("Backspace", GLFW_KEY_BACKSPACE);
        t.emplace_back("Up", GLFW_KEY_UP);
        t.emplace_back("Down", GLFW_KEY_DOWN);
        t.emplace_back("Left", GLFW_KEY_LEFT);
        t.emplace_back("Right", GLFW_KEY_RIGHT);
        for (int i = 1; i <= 12; ++i) t.emplace_back("F" + std::to_string(i), GLFW_KEY_F1 + (i - 1));
        return t;
    }();
    return table;
}

} // namespace

// ============================================================================
//  Impl
// ============================================================================
struct LuaRuntime::Impl {
    GameRuntime& game;
    ScriptStore* scripts = nullptr;

    // NOTE: members that hold sol references are declared AFTER `lua`, so they
    // are destroyed before the Lua state is closed.
    sol::state lua;
    sol::table instanceCache;   // id -> Instance userdata (weak values): same object, same Lua value
    sol::table moduleCache;     // require() results
    sol::environment moduleEnv;
    std::unordered_set<std::string> loadingModules;

    // ---- coroutine scheduler
    struct ThreadRec {
        enum State { Running, Timed, Parked } state = Running;
        int ref = LUA_NOREF;
        std::string label;
        double sleptAt = 0.0;
        uint64_t seq = 0;
    };
    std::unordered_map<lua_State*, ThreadRec> threads;
    std::map<std::pair<double, uint64_t>, lua_State*> timed; // (wake time, sequence) -> thread
    uint64_t seqCounter = 0;
    double now = 0.0;
    int resumeDepth = 0;
    bool allowInput = true;

    // ---- signals
    std::unordered_map<uint64_t, InstSignals> instSignals;
    std::shared_ptr<SignalState> heartbeat = std::make_shared<SignalState>();
    std::shared_ptr<SignalState> inputBegan = std::make_shared<SignalState>();
    std::shared_ptr<SignalState> inputEnded = std::make_shared<SignalState>();
    std::vector<bool> keyWasDown;

    explicit Impl(GameRuntime& g) : game(g) {}
    ~Impl() {
        // Drop everything that references Lua values before the state closes.
        for (auto& [state, rec] : threads) luaL_unref(lua.lua_state(), LUA_REGISTRYINDEX, rec.ref);
        threads.clear();
        timed.clear();
    }

    lua_State* L() { return lua.lua_state(); }

    // ------------------------------------------------------------------ errors
    [[noreturn]] static void fail(const std::string& message) { throw std::runtime_error(message); }

    Object* need(const Inst& i) {
        Object* o = game.findObject(i.id);
        if (!o) fail("this Instance was destroyed (or no longer exists)");
        return o;
    }
    Object* needPart(const Inst& i, const char* what) {
        Object* o = need(i);
        if (o->kind != Object::Kind::Part) fail(std::string("a ") + (o->kind == Object::Kind::Script ? "Script" : "this object") + " has no " + what);
        return o;
    }

    std::string labelOf(lua_State* state) const {
        auto it = threads.find(state);
        return it == threads.end() ? std::string() : it->second.label;
    }

    void logScript(const std::string& label, const std::string& text, bool isError) {
        game.logLine(label.empty() ? text : "[" + label + "] " + text, isError);
    }

    // --------------------------------------------------------------- instances
    // A nil that is bound to this Lua state (a state-less sol::object crashes on get_type()).
    sol::object nilObject() { return sol::make_object(lua, sol::lua_nil); }

    sol::object instanceFor(uint64_t id) {
        sol::object cached = instanceCache[static_cast<lua_Integer>(id)];
        if (cached.valid() && cached.get_type() != sol::type::lua_nil) return cached;
        sol::object fresh = sol::make_object(lua, Inst{ id });
        instanceCache[static_cast<lua_Integer>(id)] = fresh;
        return fresh;
    }
    sol::object instanceOrNil(const Object* o) {
        return o ? instanceFor(o->id) : nilObject();
    }

    static bool isAlive(const Object* o) { return o && !o->game.destroyed; }

    std::vector<Object*> childrenOf(const Inst& i) {
        std::vector<Object*> out;
        if (i.id == 0) {
            for (Object* o : game.scene().getObjects()) if (!o->parent && isAlive(o)) out.push_back(o);
        } else {
            for (Object* c : need(i)->children) if (isAlive(c)) out.push_back(c);
        }
        return out;
    }

    void collectDescendants(const Inst& i, std::vector<Object*>& out) {
        for (Object* c : childrenOf(i)) {
            out.push_back(c);
            collectDescendants(Inst{ c->id }, out);
        }
    }

    sol::table toTable(const std::vector<Object*>& objects) {
        sol::table t = lua.create_table();
        for (size_t k = 0; k < objects.size(); ++k) t[k + 1] = instanceFor(objects[k]->id);
        return t;
    }

    static const char* className(const Object* o) { return o->kind == Object::Kind::Script ? "Script" : "Part"; }

    bool isA(const Inst& i, const std::string& cls) {
        if (cls == "Instance") return true;
        if (i.id == 0) return cls == "Workspace" || cls == "Model";
        const Object* o = need(i);
        if (o->kind == Object::Kind::Script) return cls == "Script" || cls == "LuaSourceContainer";
        return cls == "Part" || cls == "BasePart" || cls == "PVInstance";
    }

    std::string fullName(const Inst& i) {
        if (i.id == 0) return "Workspace";
        std::vector<std::string> parts;
        for (const Object* o = need(i); o; o = o->parent) parts.push_back(o->name);
        std::string out = "Workspace";
        for (auto it = parts.rbegin(); it != parts.rend(); ++it) out += "." + *it;
        return out;
    }

    sol::object findChild(const Inst& i, const std::string& name, bool recursive) {
        std::vector<Object*> pool;
        if (recursive) collectDescendants(i, pool); else pool = childrenOf(i);
        for (Object* o : pool) if (o->name == name) return instanceFor(o->id);
        return nilObject();
    }

    // ------------------------------------------------------------- properties
    sol::object getParent(const Inst& i) {
        if (i.id == 0) return nilObject();
        const Object* o = need(i);
        if (o->parent) return instanceFor(o->parent->id);
        return game.isInWorld(o) ? instanceFor(0) : nilObject();
    }

    void setParent(const Inst& i, sol::object value, lua_State* callingThread) {
        if (i.id == 0) fail("Workspace cannot be parented");
        Object* o = need(i);
        Object* newParent = nullptr;
        bool toWorld = false;
        if (value.get_type() == sol::type::lua_nil) {
            toWorld = false;
        } else if (value.is<Inst>()) {
            const Inst target = value.as<Inst>();
            if (target.id == 0) toWorld = true;
            else newParent = need(target);
        } else {
            fail("Parent must be an Instance or nil");
        }
        std::string error;
        if (!game.setObjectParent(o, newParent, toWorld, error)) fail(error);
        // ChildAdded on the new parent (workspace = id 0).
        const uint64_t parentId = toWorld ? 0 : (newParent ? newParent->id : UINT64_MAX);
        if (parentId != UINT64_MAX) {
            auto it = instSignals.find(parentId);
            if (it != instSignals.end() && it->second.childAdded && it->second.childAdded->hasListeners()) {
                const uint64_t childId = o->id;
                fire(*it->second.childAdded, [this, childId](lua_State* T) { instanceFor(childId).push(T); return 1; });
            }
        }
        (void)callingThread;
    }

    Signal signalFor(const Inst& i, std::shared_ptr<SignalState> InstSignals::* member) {
        if (i.id != 0) need(i);
        InstSignals& s = instSignals[i.id];
        if (!(s.*member)) (s.*member) = std::make_shared<SignalState>();
        return Signal{ s.*member };
    }

    void destroyInstance(const Inst& i) {
        if (i.id == 0) fail("Workspace cannot be destroyed");
        Object* o = need(i);
        if (o == game.playerObject()) fail("the player's Character cannot be destroyed");
        // Destroying fires first, for the object and everything under it.
        std::vector<Object*> nodes{ o };
        collectDescendants(i, nodes);
        for (Object* n : nodes) {
            auto it = instSignals.find(n->id);
            if (it != instSignals.end() && it->second.destroying && it->second.destroying->hasListeners()) {
                fire(*it->second.destroying, [](lua_State*) { return 0; });
            }
        }
        game.destroyObject(o);
    }

    sol::object cloneInstance(const Inst& i) {
        Object* o = need(i);
        Object* copy = game.cloneSubtree(o);
        if (!copy) fail("this Instance cannot be cloned");
        return instanceFor(copy->id);
    }

    // ------------------------------------------------------------ coroutines
    lua_State* newThread(const std::string& label) {
        lua_State* T = lua_newthread(L());
        ThreadRec rec;
        rec.ref = luaL_ref(L(), LUA_REGISTRYINDEX); // anchors the thread; pops it
        rec.label = label;
        threads[T] = rec;
        return T;
    }

    void finishThread(lua_State* T) {
        auto it = threads.find(T);
        if (it == threads.end()) return;
        if (it->second.state == ThreadRec::Timed) {
            for (auto t = timed.begin(); t != timed.end(); ++t) {
                if (t->second == T) { timed.erase(t); break; }
            }
        }
        luaL_unref(L(), LUA_REGISTRYINDEX, it->second.ref);
        threads.erase(it);
    }

    // Resumes `T` with `nargs` values already on its stack.
    void resume(lua_State* T, int nargs) {
        auto it = threads.find(T);
        if (it == threads.end()) { lua_pop(T, nargs); return; }
        const std::string label = it->second.label;
        if (resumeDepth >= kMaxResumeDepth) {
            lua_pop(T, nargs);
            logScript(label, "too many nested events (a handler keeps triggering itself?)", true);
            finishThread(T);
            return;
        }
        it->second.state = ThreadRec::Running;
        ++resumeDepth;
        const auto savedDeadline = g_deadline;
        g_deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kScriptBudgetSeconds));

        int nres = 0;
        const int status = lua_resume(T, L(), nargs, &nres);

        g_deadline = savedDeadline;
        --resumeDepth;
        it = threads.find(T);
        if (it == threads.end()) return; // finished itself some other way

        if (status == LUA_YIELD) {
            if (nres >= 1 && lua_isnumber(T, -nres)) {
                const double seconds = std::max(0.0, lua_tonumber(T, -nres));
                it->second.state = ThreadRec::Timed;
                it->second.sleptAt = now;
                it->second.seq = ++seqCounter;
                timed[{ now + seconds, it->second.seq }] = T;
            } else {
                it->second.state = ThreadRec::Parked; // waits for a signal
            }
            lua_pop(T, nres);
        } else if (status == LUA_OK) {
            lua_pop(T, nres);
            finishThread(T);
        } else {
            std::string message = lua_tostring(T, -1) ? lua_tostring(T, -1) : "unknown error";
            luaL_traceback(L(), T, message.c_str(), 0);
            std::string traceback = lua_tostring(L(), -1) ? lua_tostring(L(), -1) : message;
            lua_pop(L(), 1);
            // Keep the log readable: the error plus the first few stack lines.
            size_t cut = 0;
            for (int lines = 0; lines < 5 && cut != std::string::npos; ++lines) cut = traceback.find('\n', cut + 1);
            if (cut != std::string::npos) traceback.resize(cut);
            logScript(label, traceback, true);
            finishThread(T);
        }
    }

    // Starts `fn(args...)` as a new coroutine and runs it until its first yield.
    void spawn(const sol::main_protected_function& fn, const std::string& label, const std::function<int(lua_State*)>& pushArgs) {
        lua_State* T = newThread(label);
        fn.push(T);
        const int n = pushArgs ? pushArgs(T) : 0;
        resume(T, n);
    }

    void runDue() {
        std::vector<lua_State*> due;
        while (!timed.empty() && timed.begin()->first.first <= now) {
            due.push_back(timed.begin()->second);
            timed.erase(timed.begin());
        }
        for (lua_State* T : due) {
            auto it = threads.find(T);
            if (it == threads.end() || it->second.state != ThreadRec::Timed) continue;
            lua_pushnumber(T, now - it->second.sleptAt); // task.wait() returns the time waited
            resume(T, 1);
        }
    }

    // ---------------------------------------------------------------- signals
    void fire(SignalState& s, const std::function<int(lua_State*)>& pushArgs) {
        std::vector<lua_State*> waiters = std::move(s.waiters);
        s.waiters.clear();
        for (lua_State* T : waiters) {
            if (!threads.count(T)) continue;
            const int n = pushArgs(T);
            resume(T, n);
        }
        const std::vector<std::shared_ptr<SignalState::Conn>> snapshot = s.conns; // handlers may connect/disconnect
        for (const auto& c : snapshot) {
            if (!c->connected) continue; // disconnected by an earlier handler in this same fire
            if (c->once) s.disconnect(c->id);
            spawn(c->fn, c->label, pushArgs);
        }
    }

    // ------------------------------------------------------------------ input
    int keyFrom(const sol::object& key) {
        if (key.is<double>()) return static_cast<int>(key.as<double>());
        if (key.is<std::string>()) {
            const std::string wanted = lowerCase(key.as<std::string>());
            for (const auto& [name, code] : keyTable()) if (lowerCase(name) == wanted) return code;
            fail("unknown key '" + key.as<std::string>() + "'");
        }
        fail("key must be Enum.KeyCode.X or a key name like \"W\"");
    }

    void pollInput() {
        const auto& keys = keyTable();
        if (keyWasDown.size() != keys.size()) keyWasDown.assign(keys.size(), false);
        const bool listening = inputBegan->hasListeners() || inputEnded->hasListeners();
        for (size_t k = 0; k < keys.size(); ++k) {
            const bool down = allowInput && game.input().isKeyPressed(keys[k].second);
            if (down == keyWasDown[k]) continue;
            keyWasDown[k] = down;
            if (!listening) continue;
            const int code = keys[k].second;
            auto push = [this, code](lua_State* T) {
                sol::table input = lua.create_table();
                input["KeyCode"] = code;
                input["UserInputType"] = "Keyboard";
                input.push(T);
                lua_pushboolean(T, 0); // gameProcessed
                return 2;
            };
            fire(down ? *inputBegan : *inputEnded, push);
        }
    }

    // ------------------------------------------------------------------ start
    static std::string baseName(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }

    void startScript(Object* o) {
        if (!o || o->kind != Object::Kind::Script || !o->scriptEnabled || o->game.destroyed) return;
        if (o->scriptPath.empty()) { logScript(o->name, "Script has no source file", true); return; }
        if (!scripts || !scripts->load(o->scriptPath)) {
            logScript(o->name, "script file not found: " + o->scriptPath, true);
            return;
        }
        const ScriptStore::Entry* entry = scripts->find(o->scriptPath);
        if (!entry) return;

        lua_State* T = newThread(o->name);
        const std::string chunk = "@" + o->scriptPath;
        if (luaL_loadbufferx(T, entry->text.data(), entry->text.size(), chunk.c_str(), "t") != LUA_OK) {
            logScript(o->name, std::string("syntax error: ") + (lua_tostring(T, -1) ? lua_tostring(T, -1) : "?"), true);
            lua_pop(T, 1);
            finishThread(T);
            return;
        }
        // Each script gets its own globals table that falls back to the shared API.
        sol::environment env(lua, sol::create, lua.globals());
        env["script"] = instanceFor(o->id);
        env.push(T);
        lua_setupvalue(T, -2, 1); // _ENV
        resume(T, 0);
    }

    sol::object requireModule(const std::string& nameIn, lua_State* Lc) {
        const std::string name = nameIn;
        sol::object cached = moduleCache[name];
        if (cached.valid() && cached.get_type() != sol::type::lua_nil) return cached;
        if (loadingModules.count(name)) fail("circular require of '" + name + "'");

        std::string module = name;
        std::replace(module.begin(), module.end(), '.', '/'); // require("lib.math") -> scripts/lib/math.lua
        const std::string path = "scripts/" + module + ".lua";
        if (!ScriptStore::isValidPath(path) || !scripts || !scripts->load(path)) {
            fail("module '" + name + "' not found (looked for " + path + ")");
        }
        const ScriptStore::Entry* entry = scripts->find(path);
        const std::string chunk = "@" + path;
        if (luaL_loadbufferx(Lc, entry->text.data(), entry->text.size(), chunk.c_str(), "t") != LUA_OK) {
            const std::string err = lua_tostring(Lc, -1) ? lua_tostring(Lc, -1) : "?";
            lua_pop(Lc, 1);
            fail("error loading module '" + name + "': " + err);
        }
        moduleEnv.push(Lc);
        lua_setupvalue(Lc, -2, 1);
        loadingModules.insert(name);
        try {
            lua_call(Lc, 0, 1); // modules cannot yield; an error propagates to the requiring script
        } catch (...) {
            loadingModules.erase(name);
            throw;
        }
        loadingModules.erase(name);
        sol::object result = sol::stack::get<sol::object>(Lc, -1);
        lua_pop(Lc, 1);
        if (result.get_type() == sol::type::lua_nil) result = sol::make_object(lua, true);
        moduleCache[name] = result;
        return result;
    }

    // ----------------------------------------------------------------- the API
    void buildApi();
    void begin(ScriptStore& store);
};

// ============================================================================
//  API construction
// ============================================================================
void LuaRuntime::Impl::buildApi() {
    // Only safe libraries: no io, no package, no debug. `os` is trimmed below.
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string,
                       sol::lib::table, sol::lib::utf8, sol::lib::os);
    for (const char* name : { "dofile", "loadfile", "load", "loadstring", "collectgarbage", "rawset", "rawget" }) lua[name] = sol::lua_nil;
    sol::table os = lua["os"];
    for (const char* name : { "execute", "remove", "rename", "exit", "getenv", "tmpname", "setlocale" }) os[name] = sol::lua_nil;
    sol::table str = lua["string"];
    str["dump"] = sol::lua_nil;

    lua_sethook(L(), timeLimitHook, LUA_MASKCOUNT, 20000);

    // Weak-valued cache so an Instance is the same Lua value every time.
    instanceCache = lua.create_table();
    sol::table weak = lua.create_table();
    weak["__mode"] = "v";
    instanceCache[sol::metatable_key] = weak;
    moduleCache = lua.create_table();
    moduleEnv = sol::environment(lua, sol::create, lua.globals());

    // ------------------------------------------------------------ Vector3 / Color3
    lua.new_usertype<Vec3>("Vector3",
        "new", sol::factories([](sol::optional<float> x, sol::optional<float> y, sol::optional<float> z) {
            return Vec3{ x.value_or(0.0f), y.value_or(0.0f), z.value_or(0.0f) };
        }),
        "X", sol::readonly(&Vec3::x), "Y", sol::readonly(&Vec3::y), "Z", sol::readonly(&Vec3::z),
        "Magnitude", sol::property(&Vec3::length),
        "Unit", sol::property(&Vec3::unit),
        "Dot", &Vec3::dot, "Cross", &Vec3::cross, "Lerp", &Vec3::lerp,
        "zero", sol::var(Vec3{ 0, 0, 0 }), "one", sol::var(Vec3{ 1, 1, 1 }),
        "xAxis", sol::var(Vec3{ 1, 0, 0 }), "yAxis", sol::var(Vec3{ 0, 1, 0 }), "zAxis", sol::var(Vec3{ 0, 0, 1 }),
        sol::meta_function::addition, [](const Vec3& a, const Vec3& b) { return Vec3{ a.x + b.x, a.y + b.y, a.z + b.z }; },
        sol::meta_function::subtraction, [](const Vec3& a, const Vec3& b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; },
        sol::meta_function::multiplication, sol::overload(
            [](const Vec3& a, const Vec3& b) { return Vec3{ a.x * b.x, a.y * b.y, a.z * b.z }; },
            [](const Vec3& a, float s) { return Vec3{ a.x * s, a.y * s, a.z * s }; },
            [](float s, const Vec3& a) { return Vec3{ a.x * s, a.y * s, a.z * s }; }),
        sol::meta_function::division, sol::overload(
            [](const Vec3& a, const Vec3& b) { return Vec3{ a.x / b.x, a.y / b.y, a.z / b.z }; },
            [](const Vec3& a, float s) { return Vec3{ a.x / s, a.y / s, a.z / s }; }),
        sol::meta_function::unary_minus, [](const Vec3& a) { return Vec3{ -a.x, -a.y, -a.z }; },
        sol::meta_function::equal_to, [](const Vec3& a, const Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; },
        sol::meta_function::to_string, [](const Vec3& a) { return numberText(a.x) + ", " + numberText(a.y) + ", " + numberText(a.z); });

    lua.new_usertype<Col3>("Color3",
        "new", sol::factories([](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b) {
            return Col3{ std::clamp(r.value_or(0.0f), 0.0f, 1.0f), std::clamp(g.value_or(0.0f), 0.0f, 1.0f), std::clamp(b.value_or(0.0f), 0.0f, 1.0f) };
        }),
        "fromRGB", sol::factories([](sol::optional<float> r, sol::optional<float> g, sol::optional<float> b) {
            return Col3{ std::clamp(r.value_or(0.0f) / 255.0f, 0.0f, 1.0f), std::clamp(g.value_or(0.0f) / 255.0f, 0.0f, 1.0f), std::clamp(b.value_or(0.0f) / 255.0f, 0.0f, 1.0f) };
        }),
        "fromHSV", sol::factories([](float h, float s, float v) {
            h = h - std::floor(h);
            const float c = v * s, hp = h * 6.0f;
            const float x = c * (1.0f - std::fabs(std::fmod(hp, 2.0f) - 1.0f));
            float r = 0, g = 0, b = 0;
            if (hp < 1) { r = c; g = x; } else if (hp < 2) { r = x; g = c; } else if (hp < 3) { g = c; b = x; }
            else if (hp < 4) { g = x; b = c; } else if (hp < 5) { r = x; b = c; } else { r = c; b = x; }
            const float m = v - c;
            return Col3{ r + m, g + m, b + m };
        }),
        "R", sol::readonly(&Col3::r), "G", sol::readonly(&Col3::g), "B", sol::readonly(&Col3::b),
        "Lerp", [](const Col3& a, const Col3& b, float t) { return Col3{ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t }; },
        sol::meta_function::equal_to, [](const Col3& a, const Col3& b) { return a.r == b.r && a.g == b.g && a.b == b.b; },
        sol::meta_function::to_string, [](const Col3& a) { return numberText(a.r) + ", " + numberText(a.g) + ", " + numberText(a.b); });

    // ------------------------------------------------------------ signals
    lua.new_usertype<Connection>("RBXScriptConnection", sol::no_constructor,
        "Disconnect", [](Connection& c) {
            if (auto s = c.s.lock()) s->disconnect(c.id);
        },
        "Connected", sol::property([](Connection& c) {
            auto s = c.s.lock();
            return s && std::any_of(s->conns.begin(), s->conns.end(), [&](const auto& x) { return x->id == c.id; });
        }));

    auto connect = [this](Signal& sig, sol::main_protected_function fn, bool once, sol::this_state ts) {
        auto c = std::make_shared<SignalState::Conn>();
        c->id = sig.s->nextId++;
        c->fn = std::move(fn);
        c->once = once;
        c->label = labelOf(ts.L);
        sig.s->conns.push_back(c);
        return Connection{ sig.s, c->id };
    };
    lua.new_usertype<Signal>("RBXScriptSignal", sol::no_constructor,
        "Connect", [connect](Signal& sig, sol::main_protected_function fn, sol::this_state ts) { return connect(sig, std::move(fn), false, ts); },
        "Once", [connect](Signal& sig, sol::main_protected_function fn, sol::this_state ts) { return connect(sig, std::move(fn), true, ts); },
        "Wait", sol::yielding([](Signal& sig, sol::this_state ts) { sig.s->waiters.push_back(ts.L); }));

    // ------------------------------------------------------------ Instance
    lua.new_usertype<Inst>("Instance", sol::no_constructor,
        "new", sol::factories([this](const std::string& cls, sol::optional<sol::object> parent) -> sol::object {
            if (cls == "Script") fail("Scripts are created in the editor (Insert > Script), not with Instance.new");
            Object* o = game.createObject(cls);
            if (!o) fail("unknown class '" + cls + "' (Part, Sphere, Wedge)");
            sol::object handle = instanceFor(o->id);
            if (parent && parent->get_type() != sol::type::lua_nil) setParent(Inst{ o->id }, *parent, nullptr);
            return handle;
        }),

        "Name", sol::property(
            [this](Inst& i) -> std::string { return i.id == 0 ? "Workspace" : need(i)->name; },
            [this](Inst& i, const std::string& v) { if (i.id == 0) fail("Workspace cannot be renamed"); need(i)->name = v; }),
        "ClassName", sol::property([this](Inst& i) -> std::string { return i.id == 0 ? "Workspace" : className(need(i)); }),
        "Parent", sol::property(
            [this](Inst& i) { return getParent(i); },
            [this](Inst& i, sol::object v, sol::this_state ts) { setParent(i, v, ts.L); }),

        "Position", sol::property(
            [this](Inst& i) { return fromGlm(needPart(i, "Position")->position); },
            [this](Inst& i, const Vec3& v) { needPart(i, "Position")->position = toGlm(v); }),
        "WorldPosition", sol::property([this](Inst& i) { return fromGlm(glm::vec3(needPart(i, "Position")->getGlobalTransform()[3])); }),
        "Rotation", sol::property(
            [this](Inst& i) { return fromGlm(needPart(i, "Rotation")->rotation); },
            [this](Inst& i, const Vec3& v) { needPart(i, "Rotation")->rotation = toGlm(v); }),
        "Size", sol::property(
            [this](Inst& i) { return fromGlm(needPart(i, "Size")->scale); },
            [this](Inst& i, const Vec3& v) { needPart(i, "Size")->scale = glm::max(toGlm(v), glm::vec3(0.001f)); }),
        "Color", sol::property(
            [this](Inst& i) { const glm::vec3 c = needPart(i, "Color")->color; return Col3{ c.r, c.g, c.b }; },
            [this](Inst& i, const Col3& c) { needPart(i, "Color")->color = glm::vec3(c.r, c.g, c.b); }),
        "Anchored", sol::property(
            [this](Inst& i) { return needPart(i, "Anchored")->game.anchored; },
            [this](Inst& i, bool v) { Object* o = needPart(i, "Anchored"); o->game.anchored = v; if (v) o->game.velocity = glm::vec3(0.0f); }),
        "CanCollide", sol::property(
            [this](Inst& i) { return needPart(i, "CanCollide")->game.canCollide; },
            [this](Inst& i, bool v) { needPart(i, "CanCollide")->game.canCollide = v; }),
        "Visible", sol::property(
            [this](Inst& i) { return needPart(i, "Visible")->game.visible; },
            [this](Inst& i, bool v) { needPart(i, "Visible")->game.visible = v; }),
        "Bounciness", sol::property(
            [this](Inst& i) { return needPart(i, "Bounciness")->game.bounciness; },
            [this](Inst& i, float v) { needPart(i, "Bounciness")->game.bounciness = std::clamp(v, 0.0f, 1.0f); }),
        "Velocity", sol::property(
            [this](Inst& i) {
                Object* o = needPart(i, "Velocity");
                return fromGlm(o == game.playerObject() ? game.playerVelocity() : o->game.velocity);
            },
            [this](Inst& i, const Vec3& v) {
                Object* o = needPart(i, "Velocity");
                (o == game.playerObject() ? game.playerVelocity() : o->game.velocity) = toGlm(v);
            }),
        "Gravity", sol::property(
            [this](Inst& i) { if (i.id != 0) fail("only Workspace has Gravity"); return game.gravity; },
            [this](Inst& i, float v) { if (i.id != 0) fail("only Workspace has Gravity"); game.gravity = v; }),
        "Enabled", sol::property(
            [this](Inst& i) { Object* o = need(i); if (o->kind != Object::Kind::Script) fail("only Scripts have Enabled"); return o->scriptEnabled; },
            [](Inst&, bool) { fail("Enabled cannot be changed while the game runs"); }),

        "GetChildren", [this](Inst& i) { return toTable(childrenOf(i)); },
        "GetDescendants", [this](Inst& i) { std::vector<Object*> all; collectDescendants(i, all); return toTable(all); },
        "FindFirstChild", [this](Inst& i, const std::string& name, sol::optional<bool> recursive) { return findChild(i, name, recursive.value_or(false)); },
        "FindFirstChildOfClass", [this](Inst& i, const std::string& cls) -> sol::object {
            for (Object* c : childrenOf(i)) if (cls == className(c)) return instanceFor(c->id);
            return nilObject();
        },
        "IsA", [this](Inst& i, const std::string& cls) { return isA(i, cls); },
        "IsDescendantOf", [this](Inst& i, Inst& ancestor) {
            if (ancestor.id == 0) return i.id != 0 && game.isInWorld(need(i));
            const Object* a = need(ancestor);
            return i.id != 0 && need(i)->isChildOf(const_cast<Object*>(a));
        },
        "GetFullName", [this](Inst& i) { return fullName(i); },
        "Destroy", [this](Inst& i) { destroyInstance(i); },
        "Clone", [this](Inst& i) { return cloneInstance(i); },

        "Touched", sol::property([this](Inst& i) { return signalFor(i, &InstSignals::touched); }),
        "ChildAdded", sol::property([this](Inst& i) { return signalFor(i, &InstSignals::childAdded); }),
        "Destroying", sol::property([this](Inst& i) { return signalFor(i, &InstSignals::destroying); }),

        sol::meta_function::equal_to, [](const Inst& a, const Inst& b) { return a.id == b.id; },
        sol::meta_function::to_string, [this](const Inst& i) -> std::string {
            if (i.id == 0) return "Workspace";
            const Object* o = game.findObject(i.id);
            return o ? o->name : "<destroyed>";
        },
        sol::meta_function::new_index, [this](Inst& i, sol::stack_object key, sol::stack_object) {
            const std::string name = key.is<std::string>() ? key.as<std::string>() : "?";
            fail("'" + name + "' is not a valid property of " + (i.id == 0 ? "Workspace" : className(need(i))) + " \"" + (i.id == 0 ? "Workspace" : need(i)->name) + "\"");
        },
        // Roblox-style: part.Child finds a child by name. Unknown names are an
        // error so a typo does not silently become nil.
        sol::meta_function::index, [this](Inst& i, sol::stack_object key, sol::this_state ts) -> sol::object {
            if (key.is<std::string>()) {
                const std::string name = key.as<std::string>();
                sol::object child = findChild(i, name, false);
                if (child.get_type() != sol::type::lua_nil) return child;
                fail("'" + name + "' is not a valid member of " + (i.id == 0 ? "Workspace" : className(need(i))) + " \"" + (i.id == 0 ? "Workspace" : need(i)->name) + "\"");
            }
            (void)ts;
            return nilObject();
        });

    // ------------------------------------------------------------ globals
    lua["workspace"] = Inst{ 0 };

    sol::table task = lua.create_named_table("task");
    task["wait"] = sol::yielding([](sol::optional<double> seconds) { return std::max(0.0, seconds.value_or(0.0)); });
    lua["wait"] = task["wait"];
    task["spawn"] = [this](sol::protected_function fn, sol::variadic_args va, sol::this_state ts) {
        lua_State* caller = ts.L;
        lua_State* T = newThread(labelOf(caller));
        fn.push(T);
        const int n = static_cast<int>(va.size());
        for (auto arg : va) lua_pushvalue(caller, arg.stack_index());
        lua_xmove(caller, T, n);
        resume(T, n);
    };

    auto stringify = [](lua_State* Lc, sol::variadic_args va) {
        std::string out;
        bool first = true;
        for (auto arg : va) {
            if (!first) out += '\t';
            first = false;
            lua_pushvalue(Lc, arg.stack_index());
            size_t len = 0;
            const char* text = luaL_tolstring(Lc, -1, &len);
            out.append(text, len);
            lua_pop(Lc, 2);
        }
        return out;
    };
    lua["print"] = [this, stringify](sol::variadic_args va, sol::this_state ts) { logScript(labelOf(ts.L), stringify(ts.L, va), false); };
    lua["warn"] = [this, stringify](sol::variadic_args va, sol::this_state ts) { logScript(labelOf(ts.L), stringify(ts.L, va), true); };
    lua["typeof"] = [](sol::object v, sol::this_state ts) -> std::string {
        if (v.is<Inst>()) return "Instance";
        if (v.is<Vec3>()) return "Vector3";
        if (v.is<Col3>()) return "Color3";
        if (v.is<Signal>()) return "RBXScriptSignal";
        if (v.is<Connection>()) return "RBXScriptConnection";
        return lua_typename(ts.L, static_cast<int>(v.get_type())); // sol::type mirrors the LUA_T* constants
    };
    lua["time"] = [this]() { return static_cast<double>(game.elapsed()); };
    lua["require"] = [this](const std::string& name, sol::this_state ts) { return requireModule(name, ts.L); };

    // ------------------------------------------------------------ Enum
    sol::table keyCodes = lua.create_table();
    for (const auto& [name, code] : keyTable()) keyCodes[name] = code;
    sol::table enumTable = lua.create_named_table("Enum");
    enumTable["KeyCode"] = keyCodes;

    // ------------------------------------------------------------ services
    lua.new_usertype<RunSvc>("RunService", sol::no_constructor,
        "Heartbeat", sol::property([this](RunSvc&) { return Signal{ heartbeat }; }),
        "IsRunning", [](RunSvc&) { return true; });
    lua.new_usertype<InputSvc>("UserInputService", sol::no_constructor,
        "IsKeyDown", [this](InputSvc&, sol::object key) { return allowInput && game.input().isKeyPressed(keyFrom(key)); },
        "InputBegan", sol::property([this](InputSvc&) { return Signal{ inputBegan }; }),
        "InputEnded", sol::property([this](InputSvc&) { return Signal{ inputEnded }; }));
    lua.new_usertype<PlayerHandle>("Player", sol::no_constructor,
        "Name", sol::property([](PlayerHandle&) { return std::string("Player"); }),
        "Character", sol::property([this](PlayerHandle&) -> sol::object { return instanceOrNil(game.playerObject()); }),
        "LoadCharacter", [this](PlayerHandle&) { game.respawnPlayer(); },
        sol::meta_function::equal_to, [](const PlayerHandle&, const PlayerHandle&) { return true; });
    lua.new_usertype<PlayersSvc>("Players", sol::no_constructor,
        "LocalPlayer", sol::property([](PlayersSvc&) { return PlayerHandle{}; }),
        "GetPlayers", [this](PlayersSvc&) { sol::table t = lua.create_table(); t[1] = PlayerHandle{}; return t; });

    lua.new_usertype<GameSvc>("DataModel", sol::no_constructor,
        "GetService", [this](GameSvc&, const std::string& name) -> sol::object {
            if (name == "Workspace") return instanceFor(0);
            if (name == "Players") return sol::make_object(lua, PlayersSvc{});
            if (name == "RunService") return sol::make_object(lua, RunSvc{});
            if (name == "UserInputService") return sol::make_object(lua, InputSvc{});
            fail("unknown service '" + name + "' (Workspace, Players, RunService, UserInputService)");
        },
        "Workspace", sol::property([this](GameSvc&) { return instanceFor(0); }),
        "Players", sol::property([](GameSvc&) { return PlayersSvc{}; }),
        "RunService", sol::property([](GameSvc&) { return RunSvc{}; }),
        "UserInputService", sol::property([](GameSvc&) { return InputSvc{}; }),
        // Engine extras (not in Roblox): on-screen message and win / lose.
        "Message", [this](GameSvc&, const std::string& text, sol::optional<float> seconds) { game.showMessage(text, seconds.value_or(3.0f)); },
        "Win", [this](GameSvc&, sol::optional<std::string> text) { game.finish(true, text.value_or("")); },
        "Lose", [this](GameSvc&, sol::optional<std::string> text) { game.finish(false, text.value_or("")); });
    lua["game"] = GameSvc{};
    lua["Players"] = PlayersSvc{};
    lua["RunService"] = RunSvc{};
    lua["UserInputService"] = InputSvc{};

    g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto result = lua.safe_script(kPrelude, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        LOG_ERROR("Lua", std::string("prelude failed: ") + err.what());
    }
}

void LuaRuntime::Impl::begin(ScriptStore& store) {
    scripts = &store;
    buildApi();
    for (Object* o : game.scene().getObjects()) startScript(o);
}

// ============================================================================
//  Public API
// ============================================================================
LuaRuntime::LuaRuntime(GameRuntime& game) : impl_(std::make_unique<Impl>(game)) {}
LuaRuntime::~LuaRuntime() = default;

void LuaRuntime::start(ScriptStore& scripts) { impl_->begin(scripts); }

void LuaRuntime::update(float dt, bool allowInput) {
    Impl& s = *impl_;
    s.now += dt;
    s.allowInput = allowInput;
    s.pollInput();
    if (s.heartbeat->hasListeners()) {
        s.fire(*s.heartbeat, [dt](lua_State* T) { lua_pushnumber(T, dt); return 1; });
    }
    s.runDue();
}

void LuaRuntime::scriptAddedToWorld(Object* script) { impl_->startScript(script); }

bool LuaRuntime::hasTouchedListeners(const Object* part) const {
    auto it = impl_->instSignals.find(part->id);
    return it != impl_->instSignals.end() && it->second.touched && it->second.touched->hasListeners();
}

void LuaRuntime::fireTouched(Object* part, Object* other) {
    Impl& s = *impl_;
    if (!part || !other || !Impl::isAlive(part) || !Impl::isAlive(other)) return;
    auto it = s.instSignals.find(part->id);
    if (it == s.instSignals.end() || !it->second.touched || !it->second.touched->hasListeners()) return;
    const uint64_t otherId = other->id;
    s.fire(*it->second.touched, [&s, otherId](lua_State* T) { s.instanceFor(otherId).push(T); return 1; });
}

std::string LuaRuntime::checkSyntax(const std::string& source, const std::string& chunkName) {
    lua_State* L = luaL_newstate();
    if (!L) return "";
    const std::string chunk = "@" + chunkName;
    std::string error;
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
    }
    lua_close(L);
    return error;
}
