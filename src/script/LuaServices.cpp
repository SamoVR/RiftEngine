// LuaServices.cpp -- globals and services: game, task, print, require,
// RunService, UserInputService, Mouse, Camera, TweenService, Debris,
// CollectionService, HttpService and DataStoreService.
#include "script/LuaInternal.h"
#include "script/ScriptStore.h"
#include "physics/PhysicsWorld.h"
#include "platform/Input.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/TransformUtil.h"

#include <GLFW/glfw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <random>

namespace rift {

using namespace rift::lua;

// ============================================================================
//  Easing
// ============================================================================
float lua::ease(Easing style, EasingDir dir, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float pi = 3.14159265358979f;
    auto in = [&](float x) -> float {
        switch (style) {
            case Easing::Linear: return x;
            case Easing::Sine: return 1.0f - std::cos(x * pi * 0.5f);
            case Easing::Quad: return x * x;
            case Easing::Cubic: return x * x * x;
            case Easing::Quart: return x * x * x * x;
            case Easing::Quint: return x * x * x * x * x;
            case Easing::Exponential: return x <= 0.0f ? 0.0f : std::pow(2.0f, 10.0f * x - 10.0f);
            case Easing::Circular: return 1.0f - std::sqrt(std::max(0.0f, 1.0f - x * x));
            case Easing::Back: { const float c1 = 1.70158f, c3 = c1 + 1.0f; return c3 * x * x * x - c1 * x * x; }
            case Easing::Elastic: {
                if (x <= 0.0f || x >= 1.0f) return x;
                return -std::pow(2.0f, 10.0f * x - 10.0f) * std::sin((x * 10.0f - 10.75f) * (2.0f * pi / 3.0f));
            }
            case Easing::Bounce: {
                auto out = [](float y) {
                    const float n1 = 7.5625f, d1 = 2.75f;
                    if (y < 1.0f / d1) return n1 * y * y;
                    if (y < 2.0f / d1) { y -= 1.5f / d1; return n1 * y * y + 0.75f; }
                    if (y < 2.5f / d1) { y -= 2.25f / d1; return n1 * y * y + 0.9375f; }
                    y -= 2.625f / d1;
                    return n1 * y * y + 0.984375f;
                };
                return 1.0f - out(1.0f - x);
            }
        }
        return x;
    };
    switch (dir) {
        case EasingDir::In: return in(t);
        case EasingDir::Out: return 1.0f - in(1.0f - t);
        case EasingDir::InOut: return t < 0.5f ? in(t * 2.0f) * 0.5f : 1.0f - in((1.0f - t) * 2.0f) * 0.5f;
    }
    return t;
}

namespace {

Value lerpValue(const Value& a, const Value& b, float t) {
    if (typeOf(a) != typeOf(b)) return t < 1.0f ? a : b;
    switch (typeOf(a)) {
        case ValueType::Float: return std::get<float>(a) + (std::get<float>(b) - std::get<float>(a)) * t;
        case ValueType::Int: return static_cast<int>(std::lround(std::get<int>(a) + (std::get<int>(b) - std::get<int>(a)) * t));
        case ValueType::Vector3: return glm::mix(std::get<glm::vec3>(a), std::get<glm::vec3>(b), t);
        case ValueType::Vector2: return glm::mix(std::get<glm::vec2>(a), std::get<glm::vec2>(b), t);
        case ValueType::Color3: {
            const Color3 x = std::get<Color3>(a), y = std::get<Color3>(b);
            return Color3(x.r + (y.r - x.r) * t, x.g + (y.g - x.g) * t, x.b + (y.b - x.b) * t);
        }
        case ValueType::UDim2: {
            const UDim2 x = std::get<UDim2>(a), y = std::get<UDim2>(b);
            return UDim2{ x.xScale + (y.xScale - x.xScale) * t, x.xOffset + (y.xOffset - x.xOffset) * t,
                          x.yScale + (y.yScale - x.yScale) * t, x.yOffset + (y.yOffset - x.yOffset) * t };
        }
        default: return t < 1.0f ? a : b; // booleans, enums, strings jump at the end
    }
}

std::string sanitizeFileName(const std::string& name) {
    std::string out;
    for (char c : name) out += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
    return out.empty() ? "default" : out;
}

} // namespace

// ============================================================================
//  JSON <-> Lua (HttpService, DataStores)
// ============================================================================
namespace {

nlohmann::json luaToJson(const sol::object& o, int depth) {
    if (depth > 64) throw std::runtime_error("table is nested too deeply (or contains itself)");
    switch (o.get_type()) {
        case sol::type::lua_nil: case sol::type::none: return nullptr;
        case sol::type::boolean: return o.as<bool>();
        case sol::type::number: {
            const double d = o.as<double>();
            if (std::isfinite(d) && d == std::floor(d) && std::abs(d) < 9.0e15) return static_cast<int64_t>(d);
            return d;
        }
        case sol::type::string: return o.as<std::string>();
        case sol::type::table: {
            sol::table t = o.as<sol::table>();
            const size_t n = t.size();
            size_t count = 0;
            for (const auto& kv : t) { (void)kv; ++count; }
            if (n > 0 && n == count) {
                nlohmann::json arr = nlohmann::json::array();
                for (size_t i = 1; i <= n; ++i) arr.push_back(luaToJson(t[i], depth + 1));
                return arr;
            }
            nlohmann::json obj = nlohmann::json::object();
            for (const auto& kv : t) {
                std::string key;
                if (kv.first.get_type() == sol::type::string) key = kv.first.as<std::string>();
                else if (kv.first.get_type() == sol::type::number) key = std::to_string(static_cast<long long>(kv.first.as<double>()));
                else throw std::runtime_error("table keys must be strings or numbers");
                obj[key] = luaToJson(kv.second, depth + 1);
            }
            return obj;
        }
        default:
            throw std::runtime_error(std::string("cannot store a ") + sol::type_name(o.lua_state(), o.get_type()) + " as JSON");
    }
}

sol::object jsonToLua(sol::state_view lua, const nlohmann::json& j) {
    if (j.is_null()) return sol::make_object(lua, sol::lua_nil);
    if (j.is_boolean()) return sol::make_object(lua, j.get<bool>());
    if (j.is_number_integer()) return sol::make_object(lua, j.get<int64_t>());
    if (j.is_number()) return sol::make_object(lua, j.get<double>());
    if (j.is_string()) return sol::make_object(lua, j.get<std::string>());
    sol::table t = lua.create_table();
    if (j.is_array()) {
        for (size_t i = 0; i < j.size(); ++i) t[i + 1] = jsonToLua(lua, j[i]);
    } else {
        for (auto it = j.begin(); it != j.end(); ++it) t[it.key()] = jsonToLua(lua, it.value());
    }
    return t;
}

} // namespace

// ============================================================================
//  DataStores (saved per game in Options::dataStoreFolder)
// ============================================================================
std::string LuaRuntime::Impl::dataStorePath(const std::string& name) const {
    const auto& folder = session.options().dataStoreFolder;
    if (folder.empty()) return {};
    return Paths::toUtf8(folder / Paths::fromUtf8(sanitizeFileName(name) + ".json"));
}

nlohmann::json& LuaRuntime::Impl::dataStore(const std::string& name) {
    auto it = dataStores.find(name);
    if (it != dataStores.end()) return it->second;
    nlohmann::json data = nlohmann::json::object();
    const std::string path = dataStorePath(name);
    if (!path.empty()) {
        std::ifstream in(Paths::fromUtf8(path));
        if (in.is_open()) {
            try { in >> data; } catch (...) { LOG_WARN("DataStore", "Unreadable data store file " + path + " (starting empty)"); data = nlohmann::json::object(); }
            if (!data.is_object()) data = nlohmann::json::object();
        }
    }
    return dataStores.emplace(name, std::move(data)).first->second;
}

void LuaRuntime::Impl::saveDataStore(const std::string& name) {
    const std::string path = dataStorePath(name);
    if (path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(Paths::fromUtf8(path).parent_path(), ec);
    std::ofstream out(Paths::fromUtf8(path), std::ios::trunc);
    if (!out.is_open()) { LOG_WARN("DataStore", "Cannot write " + path); return; }
    out << dataStore(name).dump(2);
}

// ============================================================================
//  Tweens and Debris (per frame)
// ============================================================================
void LuaRuntime::Impl::updateTweens(float dt) {
    for (TweenRecord& tw : tweens) {
        if (tw.state != PlaybackState::Playing && tw.state != PlaybackState::Delayed) continue;
        Instance* inst = scene.find(tw.instance);
        if (!inst) { tw.state = PlaybackState::Cancelled; continue; }
        tw.elapsed += dt;
        if (tw.state == PlaybackState::Delayed) {
            if (tw.elapsed < tw.info.delay) continue;
            tw.elapsed -= tw.info.delay;
            tw.state = PlaybackState::Playing;
        }
        const float duration = std::max(tw.info.time, 1e-4f);
        const float cycleLength = tw.info.reverses ? duration * 2.0f : duration;
        bool done = false;
        while (tw.elapsed >= cycleLength) {
            if (tw.info.repeatCount >= 0 && tw.cycle >= tw.info.repeatCount) { done = true; break; }
            tw.elapsed -= cycleLength;
            ++tw.cycle;
        }
        float t = done ? (tw.info.reverses ? 0.0f : 1.0f) : tw.elapsed / duration;
        if (!done && tw.info.reverses && t > 1.0f) t = 2.0f - t;
        const float a = ease(tw.info.style, tw.info.direction, t);
        for (const auto& g : tw.goals) {
            if (g.cframe) {
                glm::vec3 p, r, s;
                const glm::vec3 pos = glm::mix(g.cfFrom.p, g.cfTo.p, a);
                const glm::quat q = glm::slerp(glm::quat_cast(g.cfFrom.r), glm::quat_cast(g.cfTo.r), a);
                glm::mat4 m = glm::mat4_cast(q);
                TransformUtil::decompose(m, p, r, s);
                inst->setWorldFrame(pos, r);
                if (const PropertyInfo* prop = findProperty(inst->cls, "Position")) scene.notifyPropertyChanged(*inst, *prop);
            } else if (const PropertyInfo* prop = findProperty(inst->cls, g.prop)) {
                scene.setProperty(*inst, *prop, lerpValue(g.from, g.to, a));
            }
        }
        if (done) {
            tw.state = PlaybackState::Completed;
            const EnumType* stateType = enumType("PlaybackState");
            queue(tw.completed, [this, stateType](lua_State* T) {
                sol::make_object(lua, EnumItem{ stateType, static_cast<int>(PlaybackState::Completed) }).push(T);
                return 1;
            });
        }
    }
    // Finished tweens nobody can observe any more are dropped.
    tweens.erase(std::remove_if(tweens.begin(), tweens.end(), [](const TweenRecord& t) {
        return (t.state == PlaybackState::Completed || t.state == PlaybackState::Cancelled) && !t.completed->hasListeners() && t.completed.use_count() == 1;
    }), tweens.end());
}

void LuaRuntime::Impl::updateDebris() {
    if (debris.empty()) return;
    std::vector<uint64_t> expired;
    debris.erase(std::remove_if(debris.begin(), debris.end(), [&](const auto& d) {
        if (d.first > now) return false;
        expired.push_back(d.second);
        return true;
    }), debris.end());
    for (uint64_t id : expired) if (Instance* i = scene.find(id)) scene.destroy(i);
}

// ============================================================================
//  Input -> UserInputService / Mouse events
// ============================================================================
void LuaRuntime::Impl::fireInput() {
    const Input& in = session.input();
    const ViewportInput& view = session.viewport();
    const bool overGui = session.gui().mouseOverGui();

    auto pushInput = [this](InputObject io, bool processed) {
        return [this, io, processed](lua_State* T) {
            sol::make_object(lua, io).push(T);
            lua_pushboolean(T, processed ? 1 : 0);
            return 2;
        };
    };
    if (view.keyboard) {
        for (int key : in.keysPressed()) queue(inputBegan, pushInput(InputObject{ key, 1, 0 }, false));
        for (int key : in.keysReleased()) queue(inputEnded, pushInput(InputObject{ key, 1, 2 }, false));
    }
    const glm::vec3 mousePos(view.mouse, 0.0f);
    if (view.hovered) {
        static const int kTypes[3] = { 2, 3, 4 }; // MouseButton1..3 in Enum.UserInputType
        for (int b = 0; b < 3; ++b) {
            if (in.mousePressed(b)) {
                InputObject io{ 0, kTypes[b], 0, mousePos };
                queue(inputBegan, pushInput(io, overGui));
                if (!overGui && b == 0) queue(mouseButton1Down, nullptr);
                if (!overGui && b == 1) queue(mouseButton2Down, nullptr);
            }
            if (in.mouseReleased(b)) {
                InputObject io{ 0, kTypes[b], 2, mousePos };
                queue(inputEnded, pushInput(io, overGui));
                if (b == 0) queue(mouseButton1Up, nullptr);
                if (b == 1) queue(mouseButton2Up, nullptr);
            }
        }
        if (glm::length(in.mouseDelta()) > 0.0f) {
            InputObject io{ 0, 5, 1, mousePos, glm::vec3(in.mouseDelta(), 0.0f) };
            queue(inputChanged, pushInput(io, overGui));
            queue(mouseMove, nullptr);
        }
        if (in.scroll() != 0.0f) {
            InputObject io{ 0, 6, 1, glm::vec3(view.mouse, in.scroll()) };
            queue(inputChanged, pushInput(io, overGui));
            queue(in.scroll() > 0.0f ? wheelForward : wheelBackward, nullptr);
        }
    }
}

// ============================================================================
//  Registration
// ============================================================================
void LuaRuntime::Impl::registerServices() {
    // ------------------------------------------------------------ output / misc globals
    auto stringify = [](lua_State* Lc, sol::variadic_args va) {
        std::string out;
        bool first = true;
        for (auto arg : va) {
            if (!first) out += ' ';
            first = false;
            lua_pushvalue(Lc, arg.stack_index());
            size_t len = 0;
            const char* text = luaL_tolstring(Lc, -1, &len);
            out.append(text, len);
            lua_pop(Lc, 2);
        }
        return out;
    };
    lua["print"] = [this, stringify](sol::variadic_args va, sol::this_state ts) { logScript(currentScript, stringify(ts.L, va), LogLevel::Info); };
    lua["warn"] = [this, stringify](sol::variadic_args va, sol::this_state ts) { logScript(currentScript, stringify(ts.L, va), LogLevel::Warning); };
    lua["typeof"] = [](sol::object v, sol::this_state ts) -> std::string {
        if (v.is<Inst>()) return "Instance";
        if (v.is<Vector3L>()) return "Vector3";
        if (v.is<Vector2L>()) return "Vector2";
        if (v.is<Color3>()) return "Color3";
        if (v.is<UDim>()) return "UDim";
        if (v.is<UDim2>()) return "UDim2";
        if (v.is<CFrame>()) return "CFrame";
        if (v.is<EnumItem>()) return "EnumItem";
        if (v.is<Signal>()) return "RBXScriptSignal";
        if (v.is<Connection>()) return "RBXScriptConnection";
        if (v.is<RaycastParams>()) return "RaycastParams";
        if (v.is<TweenInfo>()) return "TweenInfo";
        if (v.is<TweenHandle>()) return "Tween";
        if (v.is<InputObject>()) return "InputObject";
        return lua_typename(ts.L, static_cast<int>(v.get_type()));
    };
    lua["time"] = [this]() { return now; };
    lua["elapsedTime"] = [this]() { return now; };
    lua["tick"] = []() {
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    };
    lua["require"] = [this](sol::object target, sol::this_state ts) { return requireModule(target, ts.L); };
    lua["shared"] = lua.create_table();

    // ------------------------------------------------------------ task
    sol::table task = lua.create_named_table("task");
    task["wait"] = sol::yielding([](sol::optional<double> seconds) { return std::max(0.0, seconds.value_or(0.0)); });
    lua["wait"] = task["wait"];

    // Starts fn(...) (or resumes a thread) now; returns the thread.
    auto spawnNow = [this](sol::object fnOrThread, sol::variadic_args va, sol::this_state ts, bool deferred) -> sol::object {
        lua_State* caller = ts.L;
        lua_State* T = nullptr;
        if (fnOrThread.get_type() == sol::type::thread) {
            fnOrThread.push(caller);
            T = lua_tothread(caller, -1);
            lua_pop(caller, 1);
            if (!threads.count(T)) fail("task.spawn: that thread is not managed by the scheduler");
        } else if (fnOrThread.get_type() == sol::type::function) {
            T = newThread(currentScript);
            fnOrThread.push(T);
        } else {
            fail("task.spawn expects a function or a thread");
        }
        const int n = static_cast<int>(va.size());
        for (auto arg : va) lua_pushvalue(caller, arg.stack_index());
        lua_xmove(caller, T, n);
        lua_rawgeti(caller, LUA_REGISTRYINDEX, threads[T].ref);
        sol::object handle = sol::stack::pop<sol::object>(caller);
        if (deferred) {
            threads[T].state = ThreadRec::Parked;
            pending.push_back([this, T, n]() { if (threads.count(T)) resume(T, n); });
        } else {
            resume(T, n);
        }
        return handle;
    };
    task["spawn"] = [spawnNow](sol::object f, sol::variadic_args va, sol::this_state ts) { return spawnNow(f, va, ts, false); };
    task["defer"] = [spawnNow](sol::object f, sol::variadic_args va, sol::this_state ts) { return spawnNow(f, va, ts, true); };
    lua["spawn"] = task["defer"];
    task["delay"] = [this](double seconds, sol::protected_function fn, sol::variadic_args va, sol::this_state ts) -> sol::object {
        lua_State* caller = ts.L;
        lua_State* T = newThread(currentScript);
        fn.push(T);
        const int n = static_cast<int>(va.size());
        for (auto arg : va) lua_pushvalue(caller, arg.stack_index());
        lua_xmove(caller, T, n);
        ThreadRec& rec = threads[T];
        rec.state = ThreadRec::Timed;
        rec.freshArgs = n;
        rec.sleptAt = now;
        rec.seq = ++seqCounter;
        timed[{ now + std::max(0.0, seconds), rec.seq }] = T;
        lua_rawgeti(caller, LUA_REGISTRYINDEX, rec.ref);
        return sol::stack::pop<sol::object>(caller);
    };
    lua["delay"] = task["delay"];
    task["cancel"] = [this](sol::object thread, sol::this_state ts) {
        if (thread.get_type() != sol::type::thread) fail("task.cancel expects a thread");
        thread.push(ts.L);
        lua_State* T = lua_tothread(ts.L, -1);
        lua_pop(ts.L, 1);
        auto it = threads.find(T);
        if (it == threads.end()) return;
        if (it->second.state == ThreadRec::Running) it->second.kill = true;
        else finishThread(T);
    };

    // ------------------------------------------------------------ RaycastParams / TweenInfo / InputObject
    lua.new_usertype<RaycastParams>("RaycastParams",
        "new", sol::factories([]() { return RaycastParams{}; }),
        "FilterDescendantsInstances", sol::property(
            [this](RaycastParams& p) {
                std::vector<Instance*> list;
                for (uint64_t id : p.filter) if (Instance* i = scene.find(id)) list.push_back(i);
                return toTable(list);
            },
            [](RaycastParams& p, sol::table list) {
                p.filter.clear();
                for (const auto& kv : list) if (kv.second.is<Inst>()) p.filter.push_back(kv.second.as<Inst>().id);
            }),
        "FilterType", sol::property(
            [](RaycastParams& p) { return EnumItem{ enumType("RaycastFilterType"), p.include ? 1 : 0 }; },
            [](RaycastParams& p, const EnumItem& e) { p.include = e.value == 1; }),
        "RespectCanCollide", &RaycastParams::respectCanCollide,
        "IgnoreWater", sol::property([](RaycastParams&) { return true; }, [](RaycastParams&, bool) {}),
        "AddToFilter", [](RaycastParams& p, sol::object what) {
            if (what.is<Inst>()) { p.filter.push_back(what.as<Inst>().id); return; }
            if (what.get_type() == sol::type::table) {
                for (const auto& kv : what.as<sol::table>()) if (kv.second.is<Inst>()) p.filter.push_back(kv.second.as<Inst>().id);
            }
        });

    lua.new_usertype<TweenInfo>("TweenInfo",
        "new", sol::factories([](sol::optional<float> time, sol::optional<EnumItem> style, sol::optional<EnumItem> dir,
                                 sol::optional<int> repeatCount, sol::optional<bool> reverses, sol::optional<float> delay) {
            TweenInfo info;
            info.time = std::max(0.0f, time.value_or(1.0f));
            if (style) info.style = static_cast<Easing>(style->value);
            if (dir) info.direction = static_cast<EasingDir>(dir->value);
            info.repeatCount = repeatCount.value_or(0);
            info.reverses = reverses.value_or(false);
            info.delay = std::max(0.0f, delay.value_or(0.0f));
            return info;
        }),
        "Time", sol::readonly(&TweenInfo::time),
        "RepeatCount", sol::readonly(&TweenInfo::repeatCount),
        "Reverses", sol::readonly(&TweenInfo::reverses),
        "DelayTime", sol::readonly(&TweenInfo::delay),
        "EasingStyle", sol::property([](TweenInfo& t) { return EnumItem{ enumType("EasingStyle"), static_cast<int>(t.style) }; }),
        "EasingDirection", sol::property([](TweenInfo& t) { return EnumItem{ enumType("EasingDirection"), static_cast<int>(t.direction) }; }));

    lua.new_usertype<InputObject>("InputObject", sol::no_constructor,
        "KeyCode", sol::property([](InputObject& io) {
            const EnumType* t = enumType("KeyCode");
            return EnumItem{ t, t->byValue(io.keyCode) ? io.keyCode : 0 };
        }),
        "UserInputType", sol::property([](InputObject& io) { return EnumItem{ enumType("UserInputType"), io.inputType }; }),
        "UserInputState", sol::property([](InputObject& io) { return EnumItem{ enumType("UserInputState"), io.state }; }),
        "Position", sol::property([](InputObject& io) { return Vector3L{ io.position }; }),
        "Delta", sol::property([](InputObject& io) { return Vector3L{ io.delta }; }));

    // ------------------------------------------------------------ Tweens
    lua.new_usertype<TweenHandle>("Tween", sol::no_constructor,
        "Play", [this](TweenHandle& h) {
            for (TweenRecord& t : tweens) {
                if (t.id != h.id) continue;
                Instance* inst = scene.find(t.instance);
                if (!inst) fail("the tweened Instance was destroyed");
                if (t.state == PlaybackState::Paused) { t.state = PlaybackState::Playing; return; }
                for (auto& g : t.goals) {
                    if (g.cframe) g.cfFrom = CFrame::fromMat4(inst->worldFrame());
                    else if (const PropertyInfo* p = findProperty(inst->cls, g.prop)) g.from = p->get(*inst);
                }
                // A newer tween on the same property takes over (Roblox cancels the old one).
                for (TweenRecord& other : tweens) {
                    if (other.id == t.id || other.instance != t.instance || (other.state != PlaybackState::Playing && other.state != PlaybackState::Delayed)) continue;
                    for (const auto& g : t.goals) {
                        for (const auto& og : other.goals) if (og.prop == g.prop) other.state = PlaybackState::Cancelled;
                    }
                }
                t.elapsed = 0.0f;
                t.cycle = 0;
                t.state = t.info.delay > 0.0f ? PlaybackState::Delayed : PlaybackState::Playing;
                return;
            }
        },
        "Pause", [this](TweenHandle& h) { for (TweenRecord& t : tweens) if (t.id == h.id && t.state == PlaybackState::Playing) t.state = PlaybackState::Paused; },
        "Cancel", [this](TweenHandle& h) {
            for (TweenRecord& t : tweens) {
                if (t.id != h.id) continue;
                t.state = PlaybackState::Cancelled;
                const EnumType* st = enumType("PlaybackState");
                queue(t.completed, [this, st](lua_State* T) { sol::make_object(lua, EnumItem{ st, static_cast<int>(PlaybackState::Cancelled) }).push(T); return 1; });
            }
        },
        "PlaybackState", sol::property([this](TweenHandle& h) {
            for (const TweenRecord& t : tweens) if (t.id == h.id) return EnumItem{ enumType("PlaybackState"), static_cast<int>(t.state) };
            return EnumItem{ enumType("PlaybackState"), static_cast<int>(PlaybackState::Completed) };
        }),
        "Completed", sol::property([this](TweenHandle& h) {
            for (const TweenRecord& t : tweens) if (t.id == h.id) return Signal{ t.completed };
            return Signal{ std::make_shared<SignalState>() };
        }));

    lua.new_usertype<TweenService>("TweenService", sol::no_constructor,
        "Create", [this](TweenService&, const Inst& h, const TweenInfo& info, sol::table goals) {
            Instance* inst = need(h);
            TweenRecord rec;
            rec.id = nextTweenId++;
            rec.instance = inst->id;
            rec.info = info;
            for (const auto& kv : goals) {
                if (kv.first.get_type() != sol::type::string) fail("tween goals are property = value pairs");
                const std::string name = kv.first.as<std::string>();
                TweenRecord::Goal g;
                if (name == "CFrame" && inst->hasTransform()) {
                    if (!kv.second.is<CFrame>()) fail("CFrame goal must be a CFrame");
                    g.prop = "Position";
                    g.cframe = true;
                    g.cfTo = kv.second.as<CFrame>();
                } else {
                    const std::string target = name == "Orientation" ? "Rotation" : name;
                    const PropertyInfo* p = findProperty(inst->cls, target);
                    if (!p || !p->has(PF_ScriptWrite)) fail("'" + name + "' cannot be tweened on " + classInfo(inst->cls).name);
                    std::string error;
                    g.prop = target;
                    g.to = fromLua(kv.second, *p, error);
                    if (!error.empty()) fail(error);
                }
                rec.goals.push_back(std::move(g));
            }
            tweens.push_back(std::move(rec));
            return TweenHandle{ tweens.back().id };
        },
        "GetValue", [](TweenService&, float alpha, const EnumItem& style, const EnumItem& dir) {
            return ease(static_cast<Easing>(style.value), static_cast<EasingDir>(dir.value), alpha);
        });

    // ------------------------------------------------------------ Debris / CollectionService
    lua.new_usertype<Debris>("Debris", sol::no_constructor,
        "AddItem", [this](Debris&, const Inst& h, sol::optional<double> lifetime) {
            debris.emplace_back(now + std::max(0.0, lifetime.value_or(10.0)), h.id);
        });

    lua.new_usertype<CollectionService>("CollectionService", sol::no_constructor,
        "GetTagged", [this](CollectionService&, const std::string& tag) {
            std::vector<Instance*> list;
            scene.forEachInTree([&](Instance& i) { if (i.hasTag(tag)) list.push_back(&i); });
            return toTable(list);
        },
        "HasTag", [this](CollectionService&, const Inst& h, const std::string& tag) { return need(h)->hasTag(tag); },
        "AddTag", [this](CollectionService&, const Inst& h, const std::string& tag) {
            Instance* i = need(h);
            if (i->hasTag(tag)) return;
            i->addTag(tag);
            auto it = tagAdded.find(tag);
            const uint64_t id = i->id;
            if (it != tagAdded.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
        },
        "RemoveTag", [this](CollectionService&, const Inst& h, const std::string& tag) {
            Instance* i = need(h);
            if (!i->hasTag(tag)) return;
            i->removeTag(tag);
            auto it = tagRemoved.find(tag);
            const uint64_t id = i->id;
            if (it != tagRemoved.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
        },
        "GetTags", [this](CollectionService&, const Inst& h) {
            sol::table t = lua.create_table();
            int n = 0;
            for (const std::string& tag : need(h)->tags) t[++n] = tag;
            return t;
        },
        "GetInstanceAddedSignal", [this](CollectionService&, const std::string& tag) {
            auto& s = tagAdded[tag];
            if (!s) s = std::make_shared<SignalState>();
            return Signal{ s };
        },
        "GetInstanceRemovedSignal", [this](CollectionService&, const std::string& tag) {
            auto& s = tagRemoved[tag];
            if (!s) s = std::make_shared<SignalState>();
            return Signal{ s };
        });

    // ------------------------------------------------------------ HttpService (JSON only; no network)
    lua.new_usertype<HttpService>("HttpService", sol::no_constructor,
        "JSONEncode", [](HttpService&, sol::object value) {
            try { return luaToJson(value, 0).dump(); }
            catch (const std::exception& e) { fail(std::string("JSONEncode: ") + e.what()); }
        },
        "JSONDecode", [this](HttpService&, const std::string& text) {
            nlohmann::json j;
            try { j = nlohmann::json::parse(text); }
            catch (const std::exception& e) { fail(std::string("JSONDecode: ") + e.what()); }
            return jsonToLua(lua, j);
        },
        "GenerateGUID", [](HttpService&, sol::optional<bool> wrap) {
            static std::mt19937_64 rng{ std::random_device{}() };
            const uint64_t a = rng(), b = rng();
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%08X-%04X-4%03X-%04X-%012llX",
                          static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>(a & 0xFFF),
                          static_cast<unsigned>(((b >> 48) & 0x3FFF) | 0x8000), static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
            return wrap.value_or(true) ? "{" + std::string(buf) + "}" : std::string(buf);
        });

    // ------------------------------------------------------------ DataStoreService (local files)
    lua.new_usertype<DataStore>("DataStore", sol::no_constructor,
        "GetAsync", [this](DataStore& ds, const std::string& key) -> sol::object {
            const nlohmann::json& data = dataStore(ds.name);
            auto it = data.find(key);
            return it == data.end() ? nil() : jsonToLua(lua, *it);
        },
        "SetAsync", [this](DataStore& ds, const std::string& key, sol::object value) {
            nlohmann::json j;
            try { j = luaToJson(value, 0); } catch (const std::exception& e) { fail(std::string("SetAsync: ") + e.what()); }
            dataStore(ds.name)[key] = std::move(j);
            saveDataStore(ds.name);
        },
        "UpdateAsync", [this](DataStore& ds, const std::string& key, sol::protected_function transform) -> sol::object {
            nlohmann::json& data = dataStore(ds.name);
            auto it = data.find(key);
            sol::object old = it == data.end() ? nil() : jsonToLua(lua, *it);
            sol::protected_function_result r = transform(old);
            if (!r.valid()) { sol::error err = r; fail(std::string("UpdateAsync: ") + err.what()); }
            sol::object result = r;
            if (result.get_type() == sol::type::lua_nil) return old; // nil = cancel the update
            try { data[key] = luaToJson(result, 0); } catch (const std::exception& e) { fail(std::string("UpdateAsync: ") + e.what()); }
            saveDataStore(ds.name);
            return result;
        },
        "IncrementAsync", [this](DataStore& ds, const std::string& key, sol::optional<double> delta) {
            nlohmann::json& data = dataStore(ds.name);
            double value = 0.0;
            auto it = data.find(key);
            if (it != data.end() && it->is_number()) value = it->get<double>();
            value += delta.value_or(1.0);
            data[key] = value == std::floor(value) ? nlohmann::json(static_cast<int64_t>(value)) : nlohmann::json(value);
            saveDataStore(ds.name);
            return value;
        },
        "RemoveAsync", [this](DataStore& ds, const std::string& key) -> sol::object {
            nlohmann::json& data = dataStore(ds.name);
            auto it = data.find(key);
            if (it == data.end()) return nil();
            sol::object old = jsonToLua(lua, *it);
            data.erase(it);
            saveDataStore(ds.name);
            return old;
        });
    lua.new_usertype<DataStoreService>("DataStoreService", sol::no_constructor,
        "GetDataStore", [](DataStoreService&, const std::string& name, sol::optional<std::string> scope) {
            return DataStore{ scope && !scope->empty() ? name + "_" + *scope : name };
        });

    // ------------------------------------------------------------ RunService
    lua.new_usertype<RunService>("RunService", sol::no_constructor,
        "Heartbeat", sol::property([this](RunService&) { return Signal{ heartbeat }; }),
        "Stepped", sol::property([this](RunService&) { return Signal{ stepped }; }),
        "RenderStepped", sol::property([this](RunService&) { return Signal{ renderStepped }; }),
        "IsRunning", [](RunService&) { return true; },
        "IsStudio", [this](RunService&) { return session.options().isStudio; },
        "IsClient", [](RunService&) { return true; },
        "IsServer", [](RunService&) { return true; },
        "IsEdit", [](RunService&) { return false; },
        "BindToRenderStep", [this](RunService&, const std::string& name, int, sol::main_protected_function fn) {
            auto c = std::make_shared<SignalState::Conn>();
            c->id = renderStepped->nextId++;
            c->fn = std::move(fn);
            c->script = currentScript;
            renderStepped->conns.push_back(c);
            if (auto old = renderBindings.find(name); old != renderBindings.end()) renderStepped->disconnect(old->second);
            renderBindings[name] = c->id;
        },
        "UnbindFromRenderStep", [this](RunService&, const std::string& name) {
            auto it = renderBindings.find(name);
            if (it == renderBindings.end()) return;
            renderStepped->disconnect(it->second);
            renderBindings.erase(it);
        });

    // ------------------------------------------------------------ UserInputService
    auto keyOf = [](const sol::object& key) -> int {
        if (key.is<EnumItem>()) return key.as<EnumItem>().value;
        if (key.get_type() == sol::type::string) {
            const int k = keyFromName(key.as<std::string>());
            if (k < 0) fail("unknown key '" + key.as<std::string>() + "'");
            return k;
        }
        if (key.get_type() == sol::type::number) return static_cast<int>(key.as<double>());
        fail("expected Enum.KeyCode.X");
    };
    lua.new_usertype<UserInputService>("UserInputService", sol::no_constructor,
        "IsKeyDown", [this, keyOf](UserInputService&, sol::object key) { return session.viewport().keyboard && session.input().keyDown(keyOf(key)); },
        "IsMouseButtonPressed", [this](UserInputService&, const EnumItem& type) {
            const int button = type.value - 2; // MouseButton1 = 2 in Enum.UserInputType
            return session.viewport().hovered && session.input().mouseDown(button);
        },
        "GetMouseLocation", [this](UserInputService&) { return Vector2L{ session.viewport().mouse }; },
        "GetMouseDelta", [this](UserInputService&) { return Vector2L{ session.input().mouseDelta() }; },
        "GetKeysPressed", [this](UserInputService&) {
            sol::table t = lua.create_table();
            int n = 0;
            for (const auto& [name, code] : keyCodeTable()) {
                if (session.input().keyDown(code)) t[++n] = InputObject{ code, 1, 0 };
            }
            return t;
        },
        "MouseBehavior", sol::property(
            [this](UserInputService&) { return EnumItem{ enumType("MouseBehavior"), mouseBehavior }; },
            [this](UserInputService&, const EnumItem& e) { mouseBehavior = e.value; session.setMouseLockRequested(e.value == 1); }),
        "MouseIconEnabled", sol::property([](UserInputService&) { return true; }, [](UserInputService&, bool) {}),
        "KeyboardEnabled", sol::property([](UserInputService&) { return true; }),
        "MouseEnabled", sol::property([](UserInputService&) { return true; }),
        "TouchEnabled", sol::property([](UserInputService&) { return false; }),
        "GamepadEnabled", sol::property([](UserInputService&) { return false; }),
        "InputBegan", sol::property([this](UserInputService&) { return Signal{ inputBegan }; }),
        "InputEnded", sol::property([this](UserInputService&) { return Signal{ inputEnded }; }),
        "InputChanged", sol::property([this](UserInputService&) { return Signal{ inputChanged }; }));

    // ------------------------------------------------------------ Mouse (Player:GetMouse())
    auto mouseRay = [this](glm::vec3& origin, glm::vec3& dir) { return session.screenPointToRay(session.viewport().mouse, origin, dir); };
    auto mouseHit = [this, mouseRay](uint64_t* target) -> CFrame {
        glm::vec3 origin, dir;
        CFrame cf;
        if (!mouseRay(origin, dir)) return cf;
        RaycastFilter filter;
        if (Instance* character = session.character()) filter.instances.push_back(character->id);
        const auto hit = session.physics().raycast(origin, dir * 1000.0f, filter);
        cf.p = hit ? hit->position : origin + dir * 1000.0f;
        cf.r = glm::mat3_cast(session.camera().rotation); // faces the way the camera looks
        if (target) *target = hit ? hit->instance : 0;
        return cf;
    };
    lua.new_usertype<Mouse>("Mouse", sol::no_constructor,
        "Hit", sol::property([mouseHit](Mouse&) { return mouseHit(nullptr); }),
        "Target", sol::property([this, mouseHit](Mouse&) -> sol::object {
            uint64_t target = 0;
            mouseHit(&target);
            return target ? instanceFor(target) : nil();
        }),
        "X", sol::property([this](Mouse&) { return session.viewport().mouse.x; }),
        "Y", sol::property([this](Mouse&) { return session.viewport().mouse.y; }),
        "ViewSizeX", sol::property([this](Mouse&) { return session.viewport().size.x; }),
        "ViewSizeY", sol::property([this](Mouse&) { return session.viewport().size.y; }),
        "UnitRay", sol::property([this, mouseRay](Mouse&) {
            glm::vec3 origin(0.0f), dir(0.0f, 0.0f, -1.0f);
            mouseRay(origin, dir);
            sol::table ray = lua.create_table();
            ray["Origin"] = Vector3L{ origin };
            ray["Direction"] = Vector3L{ dir };
            return ray;
        }),
        "Icon", sol::property([](Mouse&) { return std::string(); }, [](Mouse&, const std::string&) {}),
        "Button1Down", sol::property([this](Mouse&) { return Signal{ mouseButton1Down }; }),
        "Button1Up", sol::property([this](Mouse&) { return Signal{ mouseButton1Up }; }),
        "Button2Down", sol::property([this](Mouse&) { return Signal{ mouseButton2Down }; }),
        "Button2Up", sol::property([this](Mouse&) { return Signal{ mouseButton2Up }; }),
        "Move", sol::property([this](Mouse&) { return Signal{ mouseMove }; }),
        "WheelForward", sol::property([this](Mouse&) { return Signal{ wheelForward }; }),
        "WheelBackward", sol::property([this](Mouse&) { return Signal{ wheelBackward }; }));

    // ------------------------------------------------------------ Camera (workspace.CurrentCamera)
    auto screenRay = [this](float x, float y) {
        glm::vec3 origin(0.0f), dir(0.0f, 0.0f, -1.0f);
        session.screenPointToRay({ x, y }, origin, dir);
        sol::table ray = lua.create_table();
        ray["Origin"] = Vector3L{ origin };
        ray["Direction"] = Vector3L{ dir };
        return ray;
    };
    auto worldToScreen = [this](const Vector3L& p) {
        const glm::vec3 s = session.worldToScreen(p.v);
        const glm::vec2 size = session.viewport().size;
        const bool onScreen = s.z > 0.0f && s.x >= 0.0f && s.y >= 0.0f && s.x <= size.x && s.y <= size.y;
        return std::make_tuple(Vector3L{ s }, onScreen);
    };
    lua.new_usertype<Camera>("Camera", sol::no_constructor,
        "CFrame", sol::property(
            [this](Camera&) { return CFrame::fromMat4(session.camera().frame()); },
            [this](Camera&, const CFrame& cf) { session.camera().setFrame(cf.toMat4()); }),
        "Focus", sol::property([this](Camera&) {
            CFrame cf;
            Instance* root = session.rootPart();
            cf.p = root ? root->worldPosition() : session.camera().position + session.camera().lookVector() * 10.0f;
            return cf;
        }),
        "FieldOfView", sol::property(
            [this](Camera&) { return session.camera().fieldOfView; },
            [this](Camera&, float fov) { session.camera().fieldOfView = std::clamp(fov, 1.0f, 120.0f); }),
        "CameraType", sol::property(
            [this](Camera&) { return EnumItem{ enumType("CameraType"), static_cast<int>(session.camera().type) }; },
            [this](Camera&, const EnumItem& e) { session.camera().type = static_cast<CameraType>(e.value); }),
        "CameraSubject", sol::property([this](Camera&) { return instanceOrNil(session.humanoid()); }, [](Camera&, sol::object) {}),
        "ViewportSize", sol::property([this](Camera&) { return Vector2L{ session.viewport().size }; }),
        "ScreenPointToRay", [screenRay](Camera&, float x, float y) { return screenRay(x, y); },
        "ViewportPointToRay", [screenRay](Camera&, float x, float y) { return screenRay(x, y); },
        "WorldToScreenPoint", [worldToScreen](Camera&, const Vector3L& p) { return worldToScreen(p); },
        "WorldToViewportPoint", [worldToScreen](Camera&, const Vector3L& p) { return worldToScreen(p); });

    // ------------------------------------------------------------ game / workspace
    auto serviceByName = [this](const std::string& name) -> sol::object {
        static const std::pair<const char*, ClassId> instanceServices[] = {
            { "Workspace", ClassId::Workspace }, { "Players", ClassId::Players }, { "Lighting", ClassId::Lighting },
            { "ReplicatedStorage", ClassId::ReplicatedStorage }, { "ScriptService", ClassId::ScriptService },
            { "ServerScriptService", ClassId::ScriptService }, { "ServerStorage", ClassId::ReplicatedStorage },
            { "StarterGui", ClassId::StarterGui }, { "StarterPlayer", ClassId::StarterPlayer }, { "SoundService", ClassId::SoundService },
        };
        for (const auto& [n, cls] : instanceServices) if (name == n) return instanceFor(scene.service(cls)->id);
        if (name == "RunService") return sol::make_object(lua, RunService{});
        if (name == "UserInputService") return sol::make_object(lua, UserInputService{});
        if (name == "TweenService") return sol::make_object(lua, TweenService{});
        if (name == "Debris") return sol::make_object(lua, Debris{});
        if (name == "CollectionService") return sol::make_object(lua, CollectionService{});
        if (name == "HttpService") return sol::make_object(lua, HttpService{});
        if (name == "DataStoreService") return sol::make_object(lua, DataStoreService{});
        return nil();
    };
    lua.new_usertype<DataModel>("DataModel", sol::no_constructor,
        "GetService", [serviceByName](DataModel&, const std::string& name) {
            sol::object s = serviceByName(name);
            if (s.get_type() == sol::type::lua_nil) fail("'" + name + "' is not a valid service name");
            return s;
        },
        "FindService", [serviceByName](DataModel&, const std::string& name) { return serviceByName(name); },
        "IsLoaded", [](DataModel&) { return true; },
        // Engine extras: on-screen message and win / lose.
        "Message", [this](DataModel&, const std::string& text, sol::optional<float> seconds) { session.showMessage(text, seconds.value_or(3.0f)); },
        "Win", [this](DataModel&, sol::optional<std::string> text) { session.finish(true, text.value_or("")); },
        "Lose", [this](DataModel&, sol::optional<std::string> text) { session.finish(false, text.value_or("")); },
        sol::meta_function::index, [serviceByName](DataModel&, const std::string& key) -> sol::object {
            sol::object s = serviceByName(key);
            if (s.get_type() == sol::type::lua_nil) fail("'" + key + "' is not a valid member of DataModel");
            return s;
        },
        sol::meta_function::to_string, [](DataModel&) { return std::string("Game"); });

    lua["game"] = DataModel{};
    lua["Game"] = lua["game"];
    lua["workspace"] = instanceFor(scene.workspace().id);
    lua["Workspace"] = lua["workspace"];
    // Shortcuts (RIFT extras, also used by older RIFT scripts): the common
    // services without game:GetService.
    lua["Players"] = instanceFor(scene.service(ClassId::Players)->id);
    lua["RunService"] = RunService{};
    lua["UserInputService"] = UserInputService{};
    lua["TweenService"] = TweenService{};
}

} // namespace rift
