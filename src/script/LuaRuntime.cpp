// LuaRuntime.cpp -- the core of the scripting runtime (see LuaInternal.h for
// how the files are split). Build notes:
//   * Lua is compiled as C++ and SOL_USING_CXX_LUA is defined (Rift.props), so
//     a script error is a C++ exception and destructors run while it unwinds.
//   * SOL_ALL_SAFETIES_ON turns wrong argument types into Lua errors.
#include "script/LuaInternal.h"
#include "script/ScriptStore.h"
#include "physics/PhysicsWorld.h"
#include "core/Log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

namespace rift {

using namespace rift::lua;

namespace {

// Runaway-script guard: the instruction-count hook checks a deadline that is
// set before every resume. (One game session runs at a time.)
std::chrono::steady_clock::time_point g_deadline = std::chrono::steady_clock::time_point::max();
constexpr double kScriptBudgetSeconds = 1.0;
constexpr int kMaxResumeDepth = 64;
constexpr size_t kMaxEventsPerFlush = 20000;

void timeLimitHook(lua_State* L, lua_Debug*) {
    if (std::chrono::steady_clock::now() > g_deadline) {
        luaL_error(L, "script ran for more than %d second without yielding (an endless loop? add task.wait())", static_cast<int>(kScriptBudgetSeconds));
    }
}

// Memory cap: a script that allocates without bound gets a "not enough
// memory" error instead of taking the editor down.
void* limitedAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* impl = static_cast<LuaRuntime::Impl*>(ud);
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        impl->memoryUsed -= old;
        std::free(ptr);
        return nullptr;
    }
    if (nsize > old && impl->memoryUsed + (nsize - old) > impl->memoryLimit) return nullptr;
    void* p = std::realloc(ptr, nsize);
    if (p) impl->memoryUsed = impl->memoryUsed - old + nsize;
    return p;
}

int luaPanic(lua_State* L) {
    const char* message = lua_tostring(L, -1);
    LOG_ERROR("Lua", std::string("unprotected error: ") + (message ? message : "?"));
    throw std::runtime_error(message ? message : "Lua panic");
}

const char* kPrelude = R"LUA(
function math.clamp(x, lo, hi) if x < lo then return lo elseif x > hi then return hi else return x end end
function math.sign(x) if x > 0 then return 1 elseif x < 0 then return -1 else return 0 end end
function math.round(x) if x >= 0 then return math.floor(x + 0.5) else return -math.floor(-x + 0.5) end end
function math.lerp(a, b, t) return a + (b - a) * t end
function math.map(x, a, b, c, d) return c + (x - a) * (d - c) / (b - a) end
function table.find(t, value, init)
    for i = init or 1, #t do if t[i] == value then return i end end
    return nil
end
function table.clear(t) for k in pairs(t) do t[k] = nil end end
function table.clone(t) local c = {} for k, v in pairs(t) do c[k] = v end return c end
function string.split(s, sep)
    sep = sep or ","
    local out, start = {}, 1
    if sep == "" then for i = 1, #s do out[i] = s:sub(i, i) end return out end
    while true do
        local i, j = string.find(s, sep, start, true)
        if not i then out[#out + 1] = s:sub(start) return out end
        out[#out + 1] = s:sub(start, i - 1)
        start = j + 1
    end
end
)LUA";

} // namespace

void SignalState::disconnect(uint64_t id) {
    for (auto it = conns.begin(); it != conns.end(); ++it) {
        if ((*it)->id == id) { (*it)->connected = false; conns.erase(it); return; }
    }
}

// ============================================================================
//  Impl lifetime
// ============================================================================
LuaRuntime::Impl::Impl(GameSession& s, NetworkSide sd)
    : session(s), side(sd), scene(s.scene(sd)), lua(luaPanic, limitedAlloc, this) {
    heartbeat = std::make_shared<SignalState>();
    stepped = std::make_shared<SignalState>();
    renderStepped = std::make_shared<SignalState>();
    inputBegan = std::make_shared<SignalState>();
    inputEnded = std::make_shared<SignalState>();
    inputChanged = std::make_shared<SignalState>();
    playerAdded = std::make_shared<SignalState>();
    mouseButton1Down = std::make_shared<SignalState>();
    mouseButton1Up = std::make_shared<SignalState>();
    mouseButton2Down = std::make_shared<SignalState>();
    mouseButton2Up = std::make_shared<SignalState>();
    mouseMove = std::make_shared<SignalState>();
    wheelForward = std::make_shared<SignalState>();
    wheelBackward = std::make_shared<SignalState>();
    // The scene is observed from start() on (buildApi): the server's scripts run
    // first and already change the client's world before the client starts.
}

LuaRuntime::Impl::~Impl() {
    scene.removeObserver(this);
    // Drop every Lua reference before the state closes (members after `lua`
    // are destroyed first anyway; threads are registry refs).
    pending.clear();
    callbacks.clear();
    pendingCalls.clear();
    remoteRunner = sol::main_protected_function();
    for (auto& [state, rec] : threads) luaL_unref(L(), LUA_REGISTRYINDEX, rec.ref);
    threads.clear();
    timed.clear();
    signals.clear();
    tweens.clear();
    tagAdded.clear();
    tagRemoved.clear();
    for (auto* s : { &heartbeat, &stepped, &renderStepped, &inputBegan, &inputEnded, &inputChanged, &playerAdded,
                     &mouseButton1Down, &mouseButton1Up, &mouseButton2Down, &mouseButton2Up, &mouseMove, &wheelForward, &wheelBackward }) {
        s->reset();
    }
}

// ============================================================================
//  Output
// ============================================================================
std::string LuaRuntime::Impl::scriptLabel(uint64_t script) const {
    const Instance* s = scene.find(script);
    return s ? s->fullName() : std::string();
}

void LuaRuntime::Impl::logScript(uint64_t script, const std::string& text, LogLevel level) {
    session.log(level, text, script, side);
}

void LuaRuntime::Impl::requireSide(NetworkSide wanted, const char* what) const {
    if (side == wanted) return;
    fail(std::string(what) + (wanted == NetworkSide::Client ? " can only be used from a LocalScript (on the client)"
                                                             : " can only be used from a Script (on the server)"));
}

Instance* LuaRuntime::Impl::localPlayer() const { return isClient() ? scene.find(session.playerId()) : nullptr; }
Instance* LuaRuntime::Impl::localCharacter() const { return session.characterId() ? scene.find(session.characterId()) : nullptr; }
GameCamera& LuaRuntime::Impl::cam() { return isClient() ? session.camera() : serverCamera; }

// ============================================================================
//  Instances
// ============================================================================
sol::object LuaRuntime::Impl::instanceFor(uint64_t id) {
    const lua_Integer key = static_cast<lua_Integer>(id);
    sol::object cached = instanceCache[key];
    if (cached.valid() && cached.get_type() == sol::type::userdata) return cached;
    sol::object fresh = sol::make_object(lua, Inst{ id });
    instanceCache[key] = fresh;
    return fresh;
}

Instance* LuaRuntime::Impl::need(const Inst& h) {
    if (Instance* i = scene.find(h.id)) return i;
    auto t = tombstones.find(h.id);
    if (t != tombstones.end()) return t->second.get();
    fail("this Instance no longer exists");
}

Instance* LuaRuntime::Impl::needPart(const Inst& h, const char* member) {
    Instance* i = need(h);
    if (!i->isBasePart()) fail(std::string(member) + " is not a valid member of " + classInfo(i->cls).name + " \"" + i->name + "\"");
    return i;
}

sol::table LuaRuntime::Impl::toTable(const std::vector<Instance*>& list) {
    sol::table t = lua.create_table(static_cast<int>(list.size()), 0);
    for (size_t k = 0; k < list.size(); ++k) t[k + 1] = instanceFor(list[k]->id);
    return t;
}

int LuaRuntime::Impl::pushInstance(lua_State* T, uint64_t id) {
    instanceFor(id).push(T);
    return 1;
}

// ============================================================================
//  Signals
// ============================================================================
std::shared_ptr<SignalState> LuaRuntime::Impl::signalFor(uint64_t id, const std::string& name) {
    auto& slot = signals[SignalKey{ id, name }];
    if (!slot) {
        slot = std::make_shared<SignalState>();
        // Touch tests only run for parts somebody listens to.
        if (name == "Touched" || name == "TouchEnded") session.physics().watchTouches(id, true);
    }
    return slot;
}

std::shared_ptr<SignalState> LuaRuntime::Impl::findSignal(uint64_t id, const std::string& name) const {
    auto it = signals.find(SignalKey{ id, name });
    return it == signals.end() ? nullptr : it->second;
}

void LuaRuntime::Impl::queue(const std::shared_ptr<SignalState>& s, std::function<int(lua_State*)> pushArgs) {
    if (!s || !s->hasListeners()) return;
    pending.push_back([this, s, push = std::move(pushArgs)]() { fire(*s, push); });
}

void LuaRuntime::Impl::queueInstanceEvent(uint64_t id, const std::string& name, std::function<int(lua_State*)> pushArgs) {
    queue(findSignal(id, name), std::move(pushArgs));
}

void LuaRuntime::Impl::fire(SignalState& s, const std::function<int(lua_State*)>& pushArgs) {
    std::vector<lua_State*> waiters;
    waiters.swap(s.waiters);
    for (lua_State* T : waiters) {
        if (!threads.count(T)) continue;
        resume(T, pushArgs ? pushArgs(T) : 0);
    }
    const auto snapshot = s.conns; // handlers may connect / disconnect
    for (const auto& c : snapshot) {
        if (!c->connected) continue;
        if (c->once) s.disconnect(c->id);
        spawn(c->fn, c->script, pushArgs);
    }
}

void LuaRuntime::Impl::flush() {
    if (flushing) return;
    flushing = true;
    size_t handled = 0;
    while (!pending.empty() && handled < kMaxEventsPerFlush) {
        auto action = std::move(pending.front());
        pending.pop_front();
        action();
        ++handled;
    }
    if (!pending.empty() && handled >= kMaxEventsPerFlush) {
        // Events that keep causing events: the rest waits for the next frame.
        LOG_WARN("Lua", "Too many chained events in one frame; the rest run next frame.");
    }
    flushing = false;
}

// ============================================================================
//  Scheduler
// ============================================================================
lua_State* LuaRuntime::Impl::newThread(uint64_t script) {
    lua_State* T = lua_newthread(L());
    ThreadRec rec;
    rec.ref = luaL_ref(L(), LUA_REGISTRYINDEX); // anchors the thread (pops it)
    rec.script = script;
    threads[T] = rec;
    return T;
}

void LuaRuntime::Impl::finishThread(lua_State* T) {
    auto it = threads.find(T);
    if (it == threads.end()) return;
    if (it->second.state == ThreadRec::Timed) {
        for (auto t = timed.begin(); t != timed.end(); ++t) {
            if (t->second == T) { timed.erase(t); break; }
        }
    }
    childWaits.erase(std::remove_if(childWaits.begin(), childWaits.end(), [T](const ChildWait& w) { return w.thread == T; }), childWaits.end());
    luaL_unref(L(), LUA_REGISTRYINDEX, it->second.ref);
    threads.erase(it);
}

void LuaRuntime::Impl::resume(lua_State* T, int nargs) {
    auto it = threads.find(T);
    if (it == threads.end()) { lua_pop(T, nargs); return; }
    const uint64_t script = it->second.script;
    if (resumeDepth >= kMaxResumeDepth) {
        lua_pop(T, nargs);
        logScript(script, "too many nested events (a handler keeps triggering itself?)", LogLevel::Error);
        finishThread(T);
        return;
    }
    if (lua_status(T) != LUA_OK && lua_status(T) != LUA_YIELD) { lua_pop(T, nargs); finishThread(T); return; }

    it->second.state = ThreadRec::Running;
    ++resumeDepth;
    const uint64_t previousScript = currentScript;
    currentScript = script;
    const auto savedDeadline = g_deadline;
    g_deadline = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(kScriptBudgetSeconds));

    int nres = 0;
    int status = LUA_OK;
    try {
        status = lua_resume(T, L(), nargs, &nres);
    } catch (const std::exception& e) {
        // A C++ exception that escaped Lua's own handling (should not happen).
        lua_settop(T, 0);
        lua_pushstring(T, e.what());
        status = LUA_ERRRUN;
    }

    g_deadline = savedDeadline;
    currentScript = previousScript;
    --resumeDepth;

    it = threads.find(T);
    if (it != threads.end() && it->second.kill) {
        lua_settop(T, 0);
        finishThread(T);
        it = threads.end();
    }
    if (it != threads.end()) {
        if (status == LUA_YIELD) {
            if (nres >= 1 && lua_type(T, -nres) == LUA_TNUMBER) {
                const double seconds = std::max(0.0, lua_tonumber(T, -nres));
                it->second.state = ThreadRec::Timed;
                it->second.sleptAt = now;
                it->second.seq = ++seqCounter;
                timed[{ now + seconds, it->second.seq }] = T;
            } else {
                it->second.state = ThreadRec::Parked; // waiting for a signal / child
            }
            lua_pop(T, nres);
        } else if (status == LUA_OK) {
            lua_pop(T, nres);
            finishThread(T);
        } else {
            const char* raw = lua_tostring(T, -1);
            std::string message = raw ? raw : "error object is not a string";
            luaL_traceback(L(), T, message.c_str(), 1);
            std::string traceback = lua_tostring(L(), -1) ? lua_tostring(L(), -1) : message;
            lua_pop(L(), 1);
            // Keep the Output readable: the message plus a few stack lines.
            size_t cut = 0;
            for (int lines = 0; lines < 5 && cut != std::string::npos; ++lines) cut = traceback.find('\n', cut + 1);
            if (cut != std::string::npos) traceback.resize(cut);
            logScript(script, traceback, LogLevel::Error);
            finishThread(T);
        }
    }
    if (resumeDepth == 0) flush(); // deferred events run once the code yielded or returned
}

void LuaRuntime::Impl::spawn(const sol::main_protected_function& fn, uint64_t script, const std::function<int(lua_State*)>& pushArgs) {
    lua_State* T = newThread(script);
    fn.push(T);
    const int n = pushArgs ? pushArgs(T) : 0;
    resume(T, n);
}

void LuaRuntime::Impl::runDue() {
    std::vector<lua_State*> due;
    while (!timed.empty() && timed.begin()->first.first <= now) {
        due.push_back(timed.begin()->second);
        timed.erase(timed.begin());
    }
    for (lua_State* T : due) {
        auto it = threads.find(T);
        if (it == threads.end() || it->second.state != ThreadRec::Timed) continue;
        if (it->second.freshArgs >= 0) { // task.delay: start the function with its own arguments
            const int n = it->second.freshArgs;
            it->second.freshArgs = -1;
            resume(T, n);
            continue;
        }
        lua_pushnumber(T, now - it->second.sleptAt); // task.wait() returns the time actually waited
        resume(T, 1);
    }
    // WaitForChild timeouts and "infinite yield" warnings.
    const std::vector<ChildWait> waits = childWaits;
    for (const ChildWait& w : waits) {
        if (w.hasTimeout && now >= w.deadline) {
            childWaits.erase(std::remove_if(childWaits.begin(), childWaits.end(), [&](const ChildWait& x) { return x.thread == w.thread; }), childWaits.end());
            lua_pushnil(w.thread);
            resume(w.thread, 1);
        } else if (!w.hasTimeout && !w.warned && now - w.started > 5.0) {
            for (ChildWait& x : childWaits) if (x.thread == w.thread) x.warned = true;
            const Instance* parent = scene.find(w.parent);
            auto t = threads.find(w.thread);
            logScript(t != threads.end() ? t->second.script : 0,
                      "Infinite yield possible on '" + (parent ? parent->fullName() : std::string("?")) + ":WaitForChild(\"" + w.name + "\")'", LogLevel::Warning);
        }
    }
}

void LuaRuntime::Impl::killScript(uint64_t script) {
    runningScripts.erase(script);
    std::vector<lua_State*> owned;
    for (const auto& [T, rec] : threads) if (rec.script == script) owned.push_back(T);
    for (lua_State* T : owned) {
        // A thread that is running right now (the script stopping itself, or
        // a handler further up the C stack) is finished when it yields.
        ThreadRec& rec = threads[T];
        if (rec.state == ThreadRec::Running) rec.kill = true;
        else finishThread(T);
    }
    // Its connections die with it (Roblox does the same).
    auto prune = [script](SignalState& s) {
        for (auto it = s.conns.begin(); it != s.conns.end();) {
            if ((*it)->script == script) { (*it)->connected = false; it = s.conns.erase(it); }
            else ++it;
        }
    };
    for (auto& [key, s] : signals) prune(*s);
    for (auto* s : { &heartbeat, &stepped, &renderStepped, &inputBegan, &inputEnded, &inputChanged, &playerAdded,
                     &mouseButton1Down, &mouseButton1Up, &mouseButton2Down, &mouseButton2Up, &mouseMove, &wheelForward, &wheelBackward }) {
        prune(**s);
    }
    for (auto& t : tweens) prune(*t.completed);
}

// ============================================================================
//  Scripts
// ============================================================================
bool LuaRuntime::Impl::shouldRun(const Instance& s) const {
    if (!s.enabled || !scene.isInTree(&s)) return false;
    if (isClient()) {
        // LocalScripts run in what belongs to the local player: PlayerGui,
        // PlayerScripts, Backpack and the character.
        if (s.cls != ClassId::LocalScript) return false;
        const Instance* player = scene.find(session.playerId());
        const Instance* character = session.characterId() ? scene.find(session.characterId()) : nullptr;
        return (player && s.isDescendantOf(player)) || (character && s.isDescendantOf(character));
    }
    // Scripts run in the world, in ScriptService and under the players
    // (character scripts); storage and the Starter* services hold templates.
    if (s.cls != ClassId::Script) return false;
    const Instance* service = scene.serviceOf(&s);
    return service && (service->cls == ClassId::Workspace || service->cls == ClassId::ScriptService || service->cls == ClassId::Players);
}

void LuaRuntime::Impl::startScript(Instance& s) {
    if (runningScripts.count(s.id) || !shouldRun(s)) return;
    runningScripts.insert(s.id);
    if (s.source.empty()) { logScript(s.id, s.fullName() + " has no source file", LogLevel::Error); return; }
    ScriptStore& store = session.scripts();
    if (!store.load(s.source)) {
        logScript(s.id, s.fullName() + ": script file not found (" + s.source + ")", LogLevel::Error);
        return;
    }
    const ScriptStore::Entry* entry = store.find(s.source);
    if (!entry) return;

    lua_State* T = newThread(s.id);
    const std::string chunk = "=" + s.fullName();
    if (luaL_loadbufferx(T, entry->text.data(), entry->text.size(), chunk.c_str(), "t") != LUA_OK) {
        logScript(s.id, std::string(lua_tostring(T, -1) ? lua_tostring(T, -1) : "syntax error"), LogLevel::Error);
        lua_pop(T, 1);
        finishThread(T);
        return;
    }
    // Each script has its own globals, falling back to the shared API.
    sol::environment env(lua, sol::create, lua.globals());
    env["script"] = instanceFor(s.id);
    env.push(T);
    lua_setupvalue(T, -2, 1); // _ENV
    resume(T, 0);
}

sol::object LuaRuntime::Impl::runModule(const std::string& key, const std::string& path, const std::string& chunkName, lua_State* caller) {
    sol::object cached = moduleCache[key];
    if (cached.valid() && cached.get_type() != sol::type::lua_nil) return cached;
    if (loadingModules.count(key)) fail("circular require of " + chunkName);

    ScriptStore& store = session.scripts();
    if (!ScriptStore::isValidPath(path) || !store.load(path)) fail("module " + chunkName + " not found (" + path + ")");
    const ScriptStore::Entry* entry = store.find(path);
    const std::string chunk = "=" + chunkName;
    if (luaL_loadbufferx(caller, entry->text.data(), entry->text.size(), chunk.c_str(), "t") != LUA_OK) {
        const std::string err = lua_tostring(caller, -1) ? lua_tostring(caller, -1) : "?";
        lua_pop(caller, 1);
        fail("error loading module " + chunkName + ": " + err);
    }
    sol::environment env(lua, sol::create, lua.globals());
    if (key.rfind("id:", 0) == 0) env["script"] = instanceFor(std::strtoull(key.c_str() + 3, nullptr, 10));
    env.push(caller);
    lua_setupvalue(caller, -2, 1);
    loadingModules.insert(key);
    try {
        lua_call(caller, 0, 1); // modules run to completion (they cannot yield)
    } catch (...) {
        loadingModules.erase(key);
        throw;
    }
    loadingModules.erase(key);
    sol::object result = sol::stack::get<sol::object>(caller, -1);
    lua_pop(caller, 1);
    if (result.get_type() == sol::type::lua_nil) fail("module " + chunkName + " must return a value");
    moduleCache[key] = result;
    return moduleCache[key];
}

sol::object LuaRuntime::Impl::requireModule(const sol::object& target, lua_State* caller) {
    if (target.is<Inst>()) {
        Instance* m = need(target.as<Inst>());
        if (m->cls != ClassId::ModuleScript) fail("require() expects a ModuleScript, got a " + classInfo(m->cls).name);
        return runModule("id:" + std::to_string(m->id), m->source, m->fullName(), caller);
    }
    if (target.is<std::string>()) {
        // require("Utils") / require("lib.Utils") loads scripts/Utils.lua next to
        // the scene, or else from the project's shared scripts folder.
        std::string name = target.as<std::string>();
        std::string path = name;
        std::replace(path.begin(), path.end(), '.', '/');
        const std::string local = "scripts/" + path + ".lua";
        const std::string shared = std::string(ScriptStore::kProjectPrefix) + "scripts/" + path + ".lua";
        ScriptStore& store = session.scripts();
        const bool useLocal = store.has(local) || store.load(local) || !store.load(shared);
        return runModule("name:" + name, useLocal ? local : shared, name, caller);
    }
    fail("require() expects a ModuleScript or a module name");
}

// ============================================================================
//  Scene events -> Lua signals (deferred)
// ============================================================================
void LuaRuntime::Impl::onInstanceAdded(Instance& i) {
    const uint64_t id = i.id;
    if (i.cls == (isClient() ? ClassId::LocalScript : ClassId::Script)) {
        pending.push_back([this, id]() { if (Instance* s = scene.find(id)) startScript(*s); });
    }
    for (const std::string& tag : i.tags) {
        auto it = tagAdded.find(tag);
        if (it != tagAdded.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
    }
}

void LuaRuntime::Impl::onInstanceRemoving(Instance& i) {
    const uint64_t id = i.id;
    // Keep a snapshot if a script holds this instance.
    sol::object handle = instanceCache[static_cast<lua_Integer>(id)];
    if (handle.valid() && handle.get_type() == sol::type::userdata) {
        auto copy = std::make_unique<Instance>(i);
        copy->parent = nullptr;
        copy->children.clear();
        copy->scene = nullptr;
        tombstones[id] = std::move(copy);
    }
    if (i.isScript()) killScript(id);
    queueInstanceEvent(id, "Destroying", nullptr);
    for (const std::string& tag : i.tags) {
        auto it = tagRemoved.find(tag);
        if (it != tagRemoved.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
    }
    // Its signals go with it (pending events hold their own reference).
    for (auto it = signals.begin(); it != signals.end();) {
        if (it->first.id == id) it = signals.erase(it); else ++it;
    }
    primaryParts.erase(id);
}

void LuaRuntime::Impl::onParentChanged(Instance& i, Instance* oldParent) {
    const uint64_t id = i.id;
    auto pushChild = [this, id](lua_State* T) { return pushInstance(T, id); };
    if (oldParent) {
        queueInstanceEvent(oldParent->id, "ChildRemoved", pushChild);
        for (Instance* a = oldParent; a; a = a->parent) queueInstanceEvent(a->id, "DescendantRemoving", pushChild);
    }
    if (Instance* p = i.parent) {
        queueInstanceEvent(p->id, "ChildAdded", pushChild);
        for (Instance* a = p; a; a = a->parent) queueInstanceEvent(a->id, "DescendantAdded", pushChild);
        // Threads waiting in WaitForChild for this name.
        for (const ChildWait& w : std::vector<ChildWait>(childWaits)) {
            if (w.parent != p->id || w.name != i.name) continue;
            lua_State* T = w.thread;
            childWaits.erase(std::remove_if(childWaits.begin(), childWaits.end(), [T](const ChildWait& x) { return x.thread == T; }), childWaits.end());
            pending.push_back([this, T, id]() { if (threads.count(T)) { pushInstance(T, id); resume(T, 1); } });
        }
    }
    const uint64_t parentId = i.parent ? i.parent->id : 0;
    auto pushAncestry = [this, id, parentId](lua_State* T) {
        pushInstance(T, id);
        if (parentId) pushInstance(T, parentId); else lua_pushnil(T);
        return 2;
    };
    queueInstanceEvent(id, "AncestryChanged", pushAncestry);
    Scene::forEachDescendant(i, [&](Instance& d) { queueInstanceEvent(d.id, "AncestryChanged", pushAncestry); });
    queueInstanceEvent(id, "Changed", [](lua_State* T) { lua_pushstring(T, "Parent"); return 1; });
    queueInstanceEvent(id, "prop:Parent", nullptr);

    // A script moved somewhere it runs (out of storage into the world, into the character, ...).
    if (scene.isInTree(&i)) {
        const ClassId runnable = isClient() ? ClassId::LocalScript : ClassId::Script;
        std::vector<uint64_t> starts;
        if (i.cls == runnable) starts.push_back(id);
        Scene::forEachDescendant(i, [&](Instance& d) { if (d.cls == runnable) starts.push_back(d.id); });
        for (uint64_t sid : starts) pending.push_back([this, sid]() { if (Instance* s = scene.find(sid)) startScript(*s); });
    }
}

void LuaRuntime::Impl::onPropertyChanged(Instance& i, const PropertyInfo& prop) {
    const uint64_t id = i.id;
    if (i.isValueObject() && prop.name == "Value") {
        // ValueBase.Changed passes the new value (Roblox).
        const Value v = prop.get(i);
        const PropertyInfo* p = &prop;
        queueInstanceEvent(id, "Changed", [this, v, p](lua_State* T) { toLua(v, p).push(T); return 1; });
    } else {
        const std::string name = prop.name;
        queueInstanceEvent(id, "Changed", [name](lua_State* T) { lua_pushstring(T, name.c_str()); return 1; });
    }
    queueInstanceEvent(id, "prop:" + prop.name, nullptr);

    if ((i.cls == ClassId::Script || i.cls == ClassId::LocalScript) && prop.name == "Enabled") {
        if (!i.enabled) killScript(id);
        else pending.push_back([this, id]() { if (Instance* s = scene.find(id)) startScript(*s); });
    }
    if (prop.name == "Name" && i.parent) {
        // A child renamed to what WaitForChild is waiting for.
        for (const ChildWait& w : std::vector<ChildWait>(childWaits)) {
            if (w.parent != i.parent->id || w.name != i.name) continue;
            lua_State* T = w.thread;
            childWaits.erase(std::remove_if(childWaits.begin(), childWaits.end(), [T](const ChildWait& x) { return x.thread == T; }), childWaits.end());
            pending.push_back([this, T, id]() { if (threads.count(T)) { pushInstance(T, id); resume(T, 1); } });
        }
    }
}

void LuaRuntime::Impl::onAttributeChanged(Instance& i, const std::string& name) {
    queueInstanceEvent(i.id, "AttributeChanged", [name](lua_State* T) { lua_pushstring(T, name.c_str()); return 1; });
    queueInstanceEvent(i.id, "attr:" + name, nullptr);
}

// ============================================================================
//  API
// ============================================================================
void LuaRuntime::Impl::buildApi() {
    // Safe libraries only: no io, package, debug; os and string trimmed below.
    lua.open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::math, sol::lib::string,
                       sol::lib::table, sol::lib::utf8, sol::lib::os);
    for (const char* name : { "dofile", "loadfile", "load", "loadstring", "collectgarbage", "rawset", "rawget", "rawequal", "rawlen" }) {
        lua[name] = sol::lua_nil;
    }
    sol::table os = lua["os"];
    for (const char* name : { "execute", "remove", "rename", "exit", "getenv", "tmpname", "setlocale" }) os[name] = sol::lua_nil;
    sol::table str = lua["string"];
    str["dump"] = sol::lua_nil;

    lua_sethook(L(), timeLimitHook, LUA_MASKCOUNT, 10000);

    instanceCache = lua.create_table();
    sol::table weak = lua.create_table();
    weak["__mode"] = "v";
    instanceCache[sol::metatable_key] = weak;
    moduleCache = lua.create_table();

    registerTypes();
    registerInstance();
    registerServices();

    g_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const auto result = lua.safe_script(kPrelude, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        LOG_ERROR("Lua", std::string("prelude failed: ") + err.what());
    }
    g_deadline = std::chrono::steady_clock::time_point::max();
    scene.addObserver(this);
}

// ============================================================================
//  Public API
// ============================================================================
LuaRuntime::LuaRuntime(GameSession& session, NetworkSide side) : impl_(std::make_unique<Impl>(session, side)) {}
NetworkSide LuaRuntime::side() const { return impl_->side; }

void LuaRuntime::onRemoteMessage(const RemoteMessage& message) {
    impl_->handleRemote(message);
    impl_->flush();
}
LuaRuntime::~LuaRuntime() = default;

void LuaRuntime::start() {
    Impl& s = *impl_;
    s.buildApi();
    std::vector<Instance*> scripts;
    const ClassId runnable = s.isClient() ? ClassId::LocalScript : ClassId::Script;
    s.scene.forEachInTree([&](Instance& i) { if (i.cls == runnable) scripts.push_back(&i); });
    for (Instance* script : scripts) {
        if (s.scene.find(script->id)) s.startScript(*script); // an earlier script may have destroyed it
    }
    // Players.PlayerAdded fires for the local player once every script had the chance to connect.
    if (Instance* player = s.scene.find(s.session.playerId())) {
        const uint64_t id = player->id;
        s.queue(s.playerAdded, [&s, id](lua_State* T) { return s.pushInstance(T, id); });
    }
    s.flush();
}

void LuaRuntime::beforePhysics(float dt) {
    Impl& s = *impl_;
    if (s.isClient()) {
        s.fireInput();
        if (s.renderStepped->hasListeners()) s.queue(s.renderStepped, [dt](lua_State* T) { lua_pushnumber(T, dt); return 1; });
    }
    if (s.stepped->hasListeners()) {
        const double t = s.session.time();
        s.queue(s.stepped, [t, dt](lua_State* T) { lua_pushnumber(T, t); lua_pushnumber(T, dt); return 2; });
    }
    s.flush();
}

void LuaRuntime::afterPhysics(float dt) {
    Impl& s = *impl_;
    s.now += dt;
    if (s.heartbeat->hasListeners()) s.queue(s.heartbeat, [dt](lua_State* T) { lua_pushnumber(T, dt); return 1; });
    s.flush();
    s.runDue();
    s.updateTweens(dt);
    s.updateDebris();
    s.flush();
}

void LuaRuntime::onTouch(const TouchEvent& e) {
    Impl& s = *impl_;
    const uint64_t other = e.other;
    s.queueInstanceEvent(e.part, e.began ? "Touched" : "TouchEnded", [&s, other](lua_State* T) { return s.pushInstance(T, other); });
}

void LuaRuntime::onCharacterAdded(Instance& character) {
    Impl& s = *impl_;
    const uint64_t id = character.id;
    s.queueInstanceEvent(s.session.playerId(), "CharacterAdded", [&s, id](lua_State* T) { return s.pushInstance(T, id); });
    s.flush();
}

void LuaRuntime::onCharacterRemoving(Instance& character) {
    Impl& s = *impl_;
    const uint64_t id = character.id;
    s.queueInstanceEvent(s.session.playerId(), "CharacterRemoving", [&s, id](lua_State* T) { return s.pushInstance(T, id); });
}

void LuaRuntime::onHumanoidDied(Instance& humanoid) { impl_->queueInstanceEvent(humanoid.id, "Died", nullptr); }

void LuaRuntime::onHealthChanged(Instance& humanoid, float health) {
    impl_->queueInstanceEvent(humanoid.id, "HealthChanged", [health](lua_State* T) { lua_pushnumber(T, health); return 1; });
}

void LuaRuntime::onJumping(Instance& humanoid) {
    impl_->queueInstanceEvent(humanoid.id, "Jumping", [](lua_State* T) { lua_pushboolean(T, 1); return 1; });
}

void LuaRuntime::onRunning(Instance& humanoid, float speed) {
    impl_->queueInstanceEvent(humanoid.id, "Running", [speed](lua_State* T) { lua_pushnumber(T, speed); return 1; });
}

void LuaRuntime::onGuiEvent(Instance& element, GuiEvent e) {
    Impl& s = *impl_;
    const uint64_t id = element.id;
    switch (e) {
        case GuiEvent::MouseEnter: s.queueInstanceEvent(id, "MouseEnter", nullptr); break;
        case GuiEvent::MouseLeave: s.queueInstanceEvent(id, "MouseLeave", nullptr); break;
        case GuiEvent::MouseButton1Down: s.queueInstanceEvent(id, "MouseButton1Down", nullptr); break;
        case GuiEvent::MouseButton1Up: s.queueInstanceEvent(id, "MouseButton1Up", nullptr); break;
        case GuiEvent::Activated:
            s.queueInstanceEvent(id, "Activated", nullptr);
            s.queueInstanceEvent(id, "MouseButton1Click", nullptr);
            break;
    }
}

void LuaRuntime::onSoundEnded(Instance& sound) {
    const std::string soundId = sound.soundId.path;
    impl_->queueInstanceEvent(sound.id, "Ended", [soundId](lua_State* T) { lua_pushstring(T, soundId.c_str()); return 1; });
}

size_t LuaRuntime::threadCount() const { return impl_->threads.size(); }
size_t LuaRuntime::memoryUsed() const { return impl_->memoryUsed; }

std::string LuaRuntime::checkSyntax(const std::string& source, const std::string& chunkName) {
    lua_State* L = luaL_newstate();
    if (!L) return "";
    const std::string chunk = "=" + chunkName;
    std::string error;
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        error = lua_tostring(L, -1) ? lua_tostring(L, -1) : "syntax error";
    }
    lua_close(L);
    return error;
}

} // namespace rift
