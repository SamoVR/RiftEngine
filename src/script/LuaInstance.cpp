// LuaInstance.cpp -- the Instance type as scripts see it: every reflected
// property by name (part.Color, part.Anchored, ...), a few computed ones
// (Parent, CFrame, ClassName, ...), methods (Clone, Destroy, WaitForChild,
// ...) and events (Touched, Changed, ChildAdded, ...).
#include "script/LuaInternal.h"
#include "script/LuaApi.h"
#include "scene/BrickColor.h"
#include "physics/PhysicsWorld.h"
#include "core/TransformUtil.h"

#include <algorithm>
#include <unordered_set>
#include <cmath>

namespace rift {

using namespace rift::lua;

namespace {

using luaapi::hasEvent;
using luaapi::isA;
using luaapi::methodApplies;

std::string memberError(const std::string& member, const Instance& i) {
    return "'" + member + "' is not a valid member of " + classInfo(i.cls).name + " \"" + i.fullName() + "\"";
}

} // namespace

void LuaRuntime::Impl::registerInstance() {
    // ------------------------------------------------------------ signals
    lua.new_usertype<Connection>("RBXScriptConnection", sol::no_constructor,
        "Disconnect", [](Connection& c) { if (auto s = c.s.lock()) s->disconnect(c.id); },
        "Connected", sol::property([](Connection& c) {
            auto s = c.s.lock();
            return s && std::any_of(s->conns.begin(), s->conns.end(), [&](const auto& x) { return x->id == c.id; });
        }));

    auto connect = [this](Signal& sig, sol::main_protected_function fn, bool once) {
        if (!sig.s) fail("this event no longer exists");
        auto c = std::make_shared<SignalState::Conn>();
        c->id = sig.s->nextId++;
        c->fn = std::move(fn);
        c->once = once;
        c->script = currentScript;
        sig.s->conns.push_back(c);
        return Connection{ sig.s, c->id };
    };
    lua.new_usertype<Signal>("RBXScriptSignal", sol::no_constructor,
        "Connect", [connect](Signal& sig, sol::main_protected_function fn) { return connect(sig, std::move(fn), false); },
        "connect", [connect](Signal& sig, sol::main_protected_function fn) { return connect(sig, std::move(fn), false); },
        "Once", [connect](Signal& sig, sol::main_protected_function fn) { return connect(sig, std::move(fn), true); },
        "Wait", sol::yielding([](Signal& sig, sol::this_state ts) {
            if (!sig.s) fail("this event no longer exists");
            sig.s->waiters.push_back(ts.L);
        }),
        sol::meta_function::to_string, [](const Signal&) { return std::string("Signal"); });

    // ------------------------------------------------------------ AnimationTrack
    auto anim = [this]() -> AnimationSystem& { return session.animation(); };
    auto trackEvent = [this](const AnimTrack& t, const char* name) { return Signal{ trackSignal(t.id, name) }; };
    lua.new_usertype<AnimTrack>("AnimationTrack", sol::no_constructor,
        "Play", [anim](const AnimTrack& t, sol::optional<double> fade, sol::optional<double> weight, sol::optional<double> speed) {
            anim().play(t.id, static_cast<float>(fade.value_or(0.1)), static_cast<float>(weight.value_or(1.0)), static_cast<float>(speed.value_or(1.0)));
        },
        "Stop", [anim](const AnimTrack& t, sol::optional<double> fade) { anim().stop(t.id, static_cast<float>(fade.value_or(0.1))); },
        "AdjustSpeed", [anim](const AnimTrack& t, sol::optional<double> speed) { anim().adjustSpeed(t.id, static_cast<float>(speed.value_or(1.0))); },
        "AdjustWeight", [anim](const AnimTrack& t, sol::optional<double> weight, sol::optional<double> fade) {
            anim().adjustWeight(t.id, static_cast<float>(weight.value_or(1.0)), static_cast<float>(fade.value_or(0.1)));
        },
        "GetTimeOfKeyframe", [anim](const AnimTrack& t, const std::string& name) {
            const float time = anim().timeOfKeyframe(t.id, name);
            if (time < 0.0f) fail("GetTimeOfKeyframe: no keyframe named \"" + name + "\"");
            return time;
        },
        "GetMarkerReachedSignal", [this](const AnimTrack& t, const std::string& name) { return Signal{ trackSignal(t.id, "Marker:" + name) }; },
        "Destroy", [anim](const AnimTrack& t) { anim().stop(t.id, 0.0f); },
        "Animation", sol::property([this, anim](const AnimTrack& t) { return instanceOrNil(scene.find(anim().info(t.id).animation)); }),
        "Name", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).name; }),
        "IsPlaying", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).playing; }),
        "Length", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).length; }),
        "Speed", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).speed; }),
        "WeightCurrent", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).weightCurrent; }),
        "WeightTarget", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).weightTarget; }),
        "Looped", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).looped; },
                                [anim](const AnimTrack& t, bool looped) { anim().setLooped(t.id, looped); }),
        "TimePosition", sol::property([anim](const AnimTrack& t) { return anim().info(t.id).timePosition; },
                                      [anim](const AnimTrack& t, double time) { anim().setTimePosition(t.id, static_cast<float>(time)); }),
        "Priority", sol::property(
            [anim](const AnimTrack& t) { return EnumItem{ enumType("AnimationPriority"), static_cast<int>(anim().info(t.id).priority) }; },
            [anim](const AnimTrack& t, const sol::object& v) {
                if (!v.is<EnumItem>() || !v.as<EnumItem>().type || v.as<EnumItem>().type->name != "AnimationPriority") fail("Priority expects an Enum.AnimationPriority");
                anim().setPriority(t.id, static_cast<AnimationPriority>(v.as<EnumItem>().value));
            }),
        "Stopped", sol::property([trackEvent](const AnimTrack& t) { return trackEvent(t, "Stopped"); }),
        "Ended", sol::property([trackEvent](const AnimTrack& t) { return trackEvent(t, "Ended"); }),
        "DidLoop", sol::property([trackEvent](const AnimTrack& t) { return trackEvent(t, "DidLoop"); }),
        "KeyframeReached", sol::property([trackEvent](const AnimTrack& t) { return trackEvent(t, "KeyframeReached"); }),
        "ClassName", sol::property([](const AnimTrack&) { return std::string("AnimationTrack"); }),
        sol::meta_function::equal_to, [](const AnimTrack& a, const AnimTrack& b) { return a.id == b.id; },
        sol::meta_function::to_string, [anim](const AnimTrack& t) { return anim().info(t.id).name; });

    // ------------------------------------------------------------ methods
    sol::table m = lua.create_table();

    m["Clone"] = [this](const Inst& h) -> sol::object {
        Instance* i = need(h);
        if (i->isService() || i->cls == ClassId::Player) fail(classInfo(i->cls).name + " cannot be cloned");
        size_t count = 1;
        Scene::forEachDescendant(*i, [&](Instance&) { ++count; });
        countCreated(count);
        Instance* copy = scene.cloneSubtree(*i);
        return copy ? instanceFor(copy->id) : nil(); // nil when Archivable is false (Roblox)
    };
    m["Destroy"] = [this](const Inst& h) {
        Instance* i = scene.find(h.id);
        if (!i) return; // destroying twice is fine
        if (i->isService() || i->cls == ClassId::Player) fail(classInfo(i->cls).name + " cannot be destroyed");
        scene.destroy(i);
    };
    m["Remove"] = m["Destroy"];
    m["ClearAllChildren"] = [this](const Inst& h) {
        Instance* i = need(h);
        for (Instance* c : std::vector<Instance*>(i->children)) {
            if (c->cls != ClassId::Player) scene.destroy(c);
        }
    };
    m["GetChildren"] = [this](const Inst& h) { return toTable(need(h)->children); };
    m["GetDescendants"] = [this](const Inst& h) {
        std::vector<Instance*> all;
        Scene::forEachDescendant(*need(h), [&](Instance& d) { all.push_back(&d); });
        return toTable(all);
    };
    m["FindFirstChild"] = [this](const Inst& h, const std::string& name, sol::optional<bool> recursive) -> sol::object {
        Instance* i = need(h);
        if (!recursive.value_or(false)) return instanceOrNil(i->findFirstChild(name));
        Instance* found = nullptr;
        Scene::forEachDescendant(*i, [&](Instance& d) { if (!found && d.name == name) found = &d; });
        return instanceOrNil(found);
    };
    m["FindFirstDescendant"] = [this](const Inst& h, const std::string& name) -> sol::object {
        Instance* found = nullptr;
        Scene::forEachDescendant(*need(h), [&](Instance& d) { if (!found && d.name == name) found = &d; });
        return instanceOrNil(found);
    };
    m["FindFirstChildOfClass"] = [this](const Inst& h, const std::string& cls) -> sol::object {
        for (Instance* c : need(h)->children) if (classInfo(c->cls).name == cls) return instanceFor(c->id);
        return nil();
    };
    m["FindFirstChildWhichIsA"] = [this](const Inst& h, const std::string& cls, sol::optional<bool> recursive) -> sol::object {
        Instance* i = need(h);
        if (!recursive.value_or(false)) {
            for (Instance* c : i->children) if (isA(*c, cls)) return instanceFor(c->id);
            return nil();
        }
        Instance* found = nullptr;
        Scene::forEachDescendant(*i, [&](Instance& d) { if (!found && isA(d, cls)) found = &d; });
        return instanceOrNil(found);
    };
    m["FindFirstAncestor"] = [this](const Inst& h, const std::string& name) -> sol::object {
        for (Instance* a = need(h)->parent; a; a = a->parent) if (a->name == name) return instanceFor(a->id);
        return nil();
    };
    m["FindFirstAncestorOfClass"] = [this](const Inst& h, const std::string& cls) -> sol::object {
        for (Instance* a = need(h)->parent; a; a = a->parent) if (classInfo(a->cls).name == cls) return instanceFor(a->id);
        return nil();
    };
    m["FindFirstAncestorWhichIsA"] = [this](const Inst& h, const std::string& cls) -> sol::object {
        for (Instance* a = need(h)->parent; a; a = a->parent) if (isA(*a, cls)) return instanceFor(a->id);
        return nil();
    };
    m["WaitForChild"] = sol::yielding([this](const Inst& h, const std::string& name, sol::optional<double> timeout, sol::this_state ts) {
        Instance* i = need(h);
        if (Instance* c = i->findFirstChild(name)) {
            // Already there: resume on the next event flush with the child.
            lua_State* T = ts.L;
            const uint64_t childId = c->id;
            pending.push_back([this, T, childId]() { if (threads.count(T)) { pushInstance(T, childId); resume(T, 1); } });
            return;
        }
        childWaits.push_back({ ts.L, i->id, name, timeout.has_value(), timeout ? now + *timeout : 0.0, now, false });
    });
    m["IsA"] = [this](const Inst& h, const std::string& cls) { return isA(*need(h), cls); };
    m["IsDescendantOf"] = [this](const Inst& h, const Inst& other) { return need(h)->isDescendantOf(need(other)); };
    m["IsAncestorOf"] = [this](const Inst& h, const Inst& other) { return need(other)->isDescendantOf(need(h)); };
    m["GetFullName"] = [this](const Inst& h) { return need(h)->fullName(); };

    // ---- attributes and tags
    m["GetAttribute"] = [this](const Inst& h, const std::string& name) -> sol::object {
        const Value* v = need(h)->attribute(name);
        return v ? attributeToLua(*v) : nil();
    };
    m["SetAttribute"] = [this](const Inst& h, const std::string& name, sol::object value) {
        Instance* i = need(h);
        std::string error;
        const Value v = attributeFromLua(value, error);
        if (!error.empty()) fail(error);
        scene.setAttribute(*i, name, v);
    };
    m["GetAttributes"] = [this](const Inst& h) {
        sol::table t = lua.create_table();
        for (const Attribute& a : need(h)->attributes) t[a.name] = attributeToLua(a.value);
        return t;
    };
    m["GetAttributeChangedSignal"] = [this](const Inst& h, const std::string& name) { return Signal{ signalFor(need(h)->id, "attr:" + name) }; };
    m["GetPropertyChangedSignal"] = [this](const Inst& h, const std::string& prop) {
        Instance* i = need(h);
        if (!findProperty(i->cls, prop) && prop != "Parent" && prop != "CFrame") fail(memberError(prop, *i));
        return Signal{ signalFor(i->id, "prop:" + (prop == "CFrame" ? std::string("Position") : prop)) };
    };
    m["AddTag"] = [this](const Inst& h, const std::string& tag) {
        Instance* i = need(h);
        if (i->hasTag(tag)) return;
        i->addTag(tag);
        scene.notifyTagsChanged(*i);
        auto it = tagAdded.find(tag);
        const uint64_t id = i->id;
        if (it != tagAdded.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
    };
    m["RemoveTag"] = [this](const Inst& h, const std::string& tag) {
        Instance* i = need(h);
        if (!i->hasTag(tag)) return;
        i->removeTag(tag);
        scene.notifyTagsChanged(*i);
        auto it = tagRemoved.find(tag);
        const uint64_t id = i->id;
        if (it != tagRemoved.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
    };
    m["HasTag"] = [this](const Inst& h, const std::string& tag) { return need(h)->hasTag(tag); };
    m["GetTags"] = [this](const Inst& h) {
        sol::table t = lua.create_table();
        int n = 0;
        for (const std::string& tag : need(h)->tags) t[++n] = tag;
        return t;
    };

    // ---- parts and models
    m["GetTouchingParts"] = [this](const Inst& h) {
        Instance* i = needPart(h, "GetTouchingParts");
        std::vector<Instance*> parts;
        for (uint64_t id : session.physics().touchingParts(i->id)) if (Instance* p = scene.find(id)) parts.push_back(p);
        return toTable(parts);
    };
    m["ApplyImpulse"] = [this](const Inst& h, const Vector3L& impulse) { session.physics().applyImpulse(needPart(h, "ApplyImpulse")->id, impulse.v); };
    m["ApplyAngularImpulse"] = [this](const Inst& h, const Vector3L& impulse) { session.physics().applyAngularImpulse(needPart(h, "ApplyAngularImpulse")->id, impulse.v); };
    m["GetMass"] = [this](const Inst& h) {
        Instance* i = needPart(h, "GetMass");
        const float mass = session.physics().massOf(i->id);
        return mass > 0.0f ? mass : i->density * 100.0f * i->size.x * i->size.y * i->size.z;
    };
    auto setFrame = [this](Instance& i, const CFrame& cf) {
        glm::vec3 p, r, s;
        TransformUtil::decompose(cf.toMat4(), p, r, s);
        i.setWorldFrame(cf.p, r);
        if (const PropertyInfo* pos = findProperty(i.cls, "Position")) scene.notifyPropertyChanged(i, *pos);
        if (const PropertyInfo* rot = findProperty(i.cls, "Rotation")) scene.notifyPropertyChanged(i, *rot);
    };
    m["GetPivot"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (!i->hasTransform()) fail(memberError("GetPivot", *i));
        return CFrame::fromMat4(i->worldFrame());
    };
    m["PivotTo"] = [this, setFrame](const Inst& h, const CFrame& cf) {
        Instance* i = need(h);
        if (!i->hasTransform()) fail(memberError("PivotTo", *i));
        setFrame(*i, cf);
    };
    m["SetPrimaryPartCFrame"] = m["PivotTo"];
    m["MoveTo"] = [this, setFrame](const Inst& h, const Vector3L& position) {
        Instance* i = need(h);
        if (i->cls == ClassId::Humanoid) { session.moveCharacterTo(i->id, position.v); return; }
        if (!i->hasTransform()) fail(memberError("MoveTo", *i));
        CFrame cf = CFrame::fromMat4(i->worldFrame());
        cf.p = position.v;
        setFrame(*i, cf);
    };
    m["GetBoundingBox"] = [this](const Inst& h) {
        Instance* i = need(h);
        glm::vec3 lo(1e30f), hi(-1e30f);
        auto addPart = [&](const Instance& p) {
            const glm::mat4 w = p.worldTransform();
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 corner((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f);
                const glm::vec3 wp = glm::vec3(w * glm::vec4(corner, 1.0f));
                lo = glm::min(lo, wp);
                hi = glm::max(hi, wp);
            }
        };
        if (i->isBasePart()) addPart(*i);
        Scene::forEachDescendant(*i, [&](Instance& d) { if (d.isBasePart()) addPart(d); });
        if (lo.x > hi.x) { lo = hi = i->worldPosition(); }
        CFrame cf;
        cf.p = (lo + hi) * 0.5f;
        return std::make_tuple(cf, Vector3L{ hi - lo });
    };
    m["GetExtentsSize"] = [this](const Inst& h) {
        Instance* i = need(h);
        glm::vec3 lo(1e30f), hi(-1e30f);
        Scene::forEachDescendant(*i, [&](Instance& d) {
            if (!d.isBasePart()) return;
            const glm::vec3 c = d.worldPosition(), e = d.size * 0.5f;
            lo = glm::min(lo, c - e);
            hi = glm::max(hi, c + e);
        });
        return Vector3L{ lo.x > hi.x ? glm::vec3(0.0f) : hi - lo };
    };

    // ---- humanoid, sound, player, players, workspace
    // ---- the built-in GUI (StarterGui:SetCoreGuiEnabled)
    auto coreGuiType = [](const sol::object& o) -> int {
        if (o.is<EnumItem>()) return o.as<EnumItem>().value;
        if (o.get_type() == sol::type::string) {
            const EnumType* t = enumType("CoreGuiType");
            if (auto item = t ? t->byName(o.as<std::string>()) : nullptr) return item->second;
        }
        fail("expected Enum.CoreGuiType");
    };
    m["SetCoreGuiEnabled"] = [this, coreGuiType](const Inst& h, sol::object type, bool enabled) {
        Instance* i = need(h);
        if (i->cls != ClassId::StarterGui) fail(memberError("SetCoreGuiEnabled", *i));
        requireSide(NetworkSide::Client, "SetCoreGuiEnabled");
        const int t = coreGuiType(type); // PlayerList, Health, Backpack, Chat, All, EmotesMenu
        if (t == 0 || t == 4) me().playerList = enabled;
        if (t == 2 || t == 4) me().backpackBar = enabled;
    };
    m["GetCoreGuiEnabled"] = [this, coreGuiType](const Inst& h, sol::object type) {
        Instance* i = need(h);
        if (i->cls != ClassId::StarterGui) fail(memberError("GetCoreGuiEnabled", *i));
        requireSide(NetworkSide::Client, "GetCoreGuiEnabled");
        const int t = coreGuiType(type);
        if (t == 0) return me().playerList;
        if (t == 2) return me().backpackBar;
        if (t == 4) return me().playerList && me().backpackBar;
        return true;
    };

    // ---- animation
    // A Humanoid's / AnimationController's Animator, made on demand (Roblox does too).
    auto animatorOf = [this](Instance& i, bool create) -> Instance* {
        if (i.cls == ClassId::Animator) return &i;
        for (Instance* c : i.children) if (c->cls == ClassId::Animator) return c;
        return create ? scene.create(ClassId::Animator, &i) : nullptr;
    };
    m["LoadAnimation"] = [this, animatorOf](const Inst& h, const Inst& animation) {
        Instance* i = need(h);
        if (i->cls != ClassId::Animator && i->cls != ClassId::Humanoid && i->cls != ClassId::AnimationController) fail(memberError("LoadAnimation", *i));
        Instance* anim = need(animation);
        if (anim->cls != ClassId::Animation) fail("LoadAnimation expects an Animation, got a " + classInfo(anim->cls).name);
        Instance* animator = animatorOf(*i, true);
        if (!animator->isDescendantOf(&scene.workspace())) fail("LoadAnimation: the model must be in the Workspace");
        std::string error;
        const uint64_t id = session.animation().load(isClient() ? client : -1, scene, *animator, *anim, error);
        if (!id) fail("LoadAnimation: " + error);
        return AnimTrack{ id };
    };
    m["GetPlayingAnimationTracks"] = [this, animatorOf](const Inst& h) {
        Instance* animator = animatorOf(*need(h), false);
        sol::table out = lua.create_table();
        if (!animator) return out;
        int n = 0;
        for (uint64_t id : session.animation().playingTracks(animator->id)) out[++n] = AnimTrack{ id };
        return out;
    };
    m["GetState"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Humanoid) fail(memberError("GetState", *i));
        return EnumItem{ enumType("HumanoidStateType"), static_cast<int>(session.humanoidState(i->id)) };
    };

    // ---- particles
    m["Emit"] = [this](const Inst& h, sol::optional<double> count) {
        Instance* i = need(h);
        if (i->cls != ClassId::ParticleEmitter) fail(memberError("Emit", *i));
        const int n = std::clamp(static_cast<int>(count.value_or(16.0)), 0, 10000);
        i->emitRequests += n;
        // Particles are drawn by the clients: the server's Emit reaches them too.
        if (!isClient()) {
            for (int k = 0; k < session.clientCount(); ++k)
                if (Instance* copy = session.client(k).scene->find(i->id)) copy->emitRequests += n;
        }
    };
    m["Clear"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls == ClassId::Terrain) { scene.terrain().clear(); return; }
        if (i->cls != ClassId::ParticleEmitter) fail(memberError("Clear", *i));
        ++i->clearRequests;
        if (!isClient()) {
            for (int k = 0; k < session.clientCount(); ++k)
                if (Instance* copy = session.client(k).scene->find(i->id)) ++copy->clearRequests;
        }
    };

    // ---- tools
    m["Activate"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Tool) fail(memberError("Activate", *i));
        session.activateTool(i->id, true);
    };
    m["Deactivate"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Tool) fail(memberError("Deactivate", *i));
        session.activateTool(i->id, false);
    };
    m["EquipTool"] = [this](const Inst& h, const Inst& tool) {
        Instance* i = need(h);
        if (i->cls != ClassId::Humanoid) fail(memberError("EquipTool", *i));
        if (need(tool)->cls != ClassId::Tool) fail("EquipTool expects a Tool");
        GameSession::Client* c = session.clientOfHumanoid(i->id);
        if (!c) fail("EquipTool only works on a player's Humanoid");
        if (isClient() && c->index != client) fail("a LocalScript can only equip tools on its own character");
        session.equipTool(c->index, tool.id);
    };
    m["UnequipTools"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Humanoid) fail(memberError("UnequipTools", *i));
        GameSession::Client* c = session.clientOfHumanoid(i->id);
        if (!c) return;
        if (isClient() && c->index != client) fail("a LocalScript can only unequip tools on its own character");
        session.unequipTools(c->index);
    };
    m["TakeDamage"] = [this](const Inst& h, float amount) {
        Instance* i = need(h);
        if (i->cls != ClassId::Humanoid) fail(memberError("TakeDamage", *i));
        scene.setProperty(*i, "Health", std::max(0.0f, i->health - std::max(0.0f, amount)));
    };
    m["Play"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Play", *i)); session.playSound(*i); };
    m["Stop"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Stop", *i)); session.stopSound(*i); };
    m["Pause"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Pause", *i)); session.pauseSound(*i, true); };
    m["Resume"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Resume", *i)); session.pauseSound(*i, false); };
    m["LoadCharacter"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Player) fail(memberError("LoadCharacter", *i));
        requireSide(NetworkSide::Server, "LoadCharacter");
        session.loadCharacter(i->id);
    };
    m["Kick"] = [this](const Inst& h, sol::optional<std::string> message) {
        Instance* i = need(h);
        if (i->cls != ClassId::Player) fail(memberError("Kick", *i));
        session.finish(false, message.value_or("You were kicked from the game"));
    };
    m["GetMouse"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Player) fail(memberError("GetMouse", *i));
        requireSide(NetworkSide::Client, "GetMouse");
        return Mouse{};
    };
    m["GetPlayers"] = [this](const Inst& h) {
        Instance* i = need(h);
        if (i->cls != ClassId::Players) fail(memberError("GetPlayers", *i));
        std::vector<Instance*> players;
        for (Instance* c : i->children) if (c->cls == ClassId::Player) players.push_back(c);
        return toTable(players);
    };
    m["GetPlayerFromCharacter"] = [this](const Inst& h, sol::object character) -> sol::object {
        need(h);
        if (!character.is<Inst>()) return nil();
        const GameSession::Client* c = session.clientOfCharacter(character.as<Inst>().id);
        return c ? instanceOrNil(scene.find(c->playerId)) : nil();
    };
    m["Raycast"] = [this](const Inst& h, const Vector3L& origin, const Vector3L& direction, sol::optional<RaycastParams> params) -> sol::object {
        Instance* i = need(h);
        if (i->cls != ClassId::Workspace) fail(memberError("Raycast", *i));
        RaycastFilter filter;
        if (params) {
            filter.instances = params->filter;
            filter.include = params->include;
            filter.respectCanCollide = params->respectCanCollide;
        }
        const auto hit = session.physics().raycast(origin.v, direction.v, filter);
        if (!hit) return nil();
        Instance* part = scene.find(hit->instance);
        sol::table result = lua.create_table();
        result["Instance"] = instanceOrNil(part);
        result["Position"] = Vector3L{ hit->position };
        result["Normal"] = Vector3L{ hit->normal };
        result["Distance"] = hit->distance;
        if (part && part->isBasePart()) result["Material"] = EnumItem{ enumType("Material"), static_cast<int>(part->material) };
        if (part && part->cls == ClassId::Terrain) {
            const Material m = scene.terrain().materialAt(hit->position - hit->normal * 0.3f);
            result["Material"] = EnumItem{ enumType("Material"), static_cast<int>(m) };
        }
        return result;
    };
    // ---- Terrain (workspace.Terrain)
    auto terrainOf = [this](const Inst& h, const char* member) -> TerrainData& {
        Instance* i = need(h);
        if (i->cls != ClassId::Terrain) fail(memberError(member, *i));
        return scene.terrain();
    };
    auto materialOf = [](const sol::object& o) -> Material {
        if (o.is<EnumItem>()) {
            const EnumItem e = o.as<EnumItem>();
            if (e.type && e.type->name == "Material" && e.value >= 0 && e.value < kMaterialCount) return static_cast<Material>(e.value);
        }
        fail("expected Enum.Material");
    };
    auto terrainMaterial = [materialOf](const sol::object& o) {
        const Material m = materialOf(o);
        if (m != Material::Air && !TerrainData::isTerrainMaterial(m)) fail("that material cannot be used on terrain (use Grass, Rock, Sand, Water, ...)");
        return m;
    };
    m["FillBlock"] = [terrainOf, terrainMaterial](const Inst& h, const CFrame& cf, const Vector3L& size, sol::object material) {
        terrainOf(h, "FillBlock").fillBlock(cf.toMat4(), size.v, terrainMaterial(material));
    };
    m["FillBall"] = [terrainOf, terrainMaterial](const Inst& h, const Vector3L& center, float radius, sol::object material) {
        terrainOf(h, "FillBall").fillBall(center.v, radius, terrainMaterial(material));
    };
    m["FillCylinder"] = [terrainOf, terrainMaterial](const Inst& h, const CFrame& cf, float height, float radius, sol::object material) {
        terrainOf(h, "FillCylinder").fillCylinder(cf.toMat4(), height, radius, terrainMaterial(material));
    };
    m["GetMaterialColor"] = [terrainOf, materialOf](const Inst& h, sol::object material) {
        return terrainOf(h, "GetMaterialColor").materialColor(materialOf(material));
    };
    m["SetMaterialColor"] = [terrainOf, materialOf](const Inst& h, sol::object material, const Color3& color) {
        terrainOf(h, "SetMaterialColor").setMaterialColor(materialOf(material), color);
    };
    m["GetPartBoundsInBox"] = [this](const Inst& h, const CFrame& cf, const Vector3L& size) {
        Instance* i = need(h);
        if (i->cls != ClassId::Workspace) fail(memberError("GetPartBoundsInBox", *i));
        glm::vec3 p, r, s;
        TransformUtil::decompose(cf.toMat4(), p, r, s);
        std::vector<Instance*> parts;
        for (uint64_t id : session.physics().partsInBox(cf.p, size.v, r)) if (Instance* part = scene.find(id)) parts.push_back(part);
        return toTable(parts);
    };
    m["GetPartsInPart"] = [this](const Inst& h, const Inst& partHandle) {
        need(h);
        Instance* part = needPart(partHandle, "GetPartsInPart");
        std::vector<Instance*> parts;
        for (uint64_t id : session.physics().partsInBox(part->worldPosition(), part->size, part->worldRotation())) {
            if (id == part->id) continue;
            if (Instance* p = scene.find(id)) parts.push_back(p);
        }
        return toTable(parts);
    };
    // ---- remotes and bindables
    // Arguments are copied (RemoteValue) like Roblox: tables become new
    // tables, Instances travel by id, functions are dropped.
    auto argsFrom = [this](sol::variadic_args va, size_t skip) {
        std::vector<RemoteValue> args;
        size_t k = 0;
        for (auto a : va) {
            if (k++ < skip) continue;
            args.push_back(toRemote(sol::object(a)));
        }
        return args;
    };
    m["FireServer"] = [this, argsFrom](const Inst& h, sol::variadic_args va) {
        Instance* i = need(h);
        requireSide(NetworkSide::Client, "FireServer");
        RemoteMessage msg;
        msg.kind = RemoteMessage::Kind::Event;
        msg.to = NetworkSide::Server;
        msg.client = client;
        msg.remote = i->id;
        msg.args = argsFrom(va, 0);
        session.postRemote(std::move(msg));
    };
    m["FireClient"] = [this, argsFrom](const Inst& h, const Inst& player, sol::variadic_args va) {
        Instance* i = need(h);
        requireSide(NetworkSide::Server, "FireClient");
        if (need(player)->cls != ClassId::Player) fail("FireClient expects a Player as its first argument");
        const GameSession::Client* c = session.clientOfPlayer(player.id);
        if (!c) return; // the player left
        RemoteMessage msg;
        msg.kind = RemoteMessage::Kind::Event;
        msg.to = NetworkSide::Client;
        msg.client = c->index;
        msg.remote = i->id;
        msg.args = argsFrom(va, 0);
        session.postRemote(std::move(msg));
    };
    m["FireAllClients"] = [this, argsFrom](const Inst& h, sol::variadic_args va) {
        Instance* i = need(h);
        requireSide(NetworkSide::Server, "FireAllClients");
        const std::vector<RemoteValue> args = argsFrom(va, 0);
        for (int k = 0; k < session.clientCount(); ++k) {
            RemoteMessage msg;
            msg.kind = RemoteMessage::Kind::Event;
            msg.to = NetworkSide::Client;
            msg.client = k;
            msg.remote = i->id;
            msg.args = args;
            session.postRemote(std::move(msg));
        }
    };
    m["Fire"] = [this, argsFrom](const Inst& h, sol::variadic_args va) {
        Instance* i = need(h);
        auto args = std::make_shared<std::vector<RemoteValue>>(argsFrom(va, 0));
        queueInstanceEvent(i->id, "Event", [this, args](lua_State* T) { return pushRemoteArgs(T, *args); });
    };
    // Invocations yield until the other side answers; they are Lua wrappers
    // around a yielding C++ call that returns (ok, results...).
    auto startInvoke = sol::yielding([this, argsFrom](const std::string& method, const Inst& h, sol::variadic_args va, sol::this_state ts) {
        Instance* i = need(h);
        requireSide(method == "InvokeServer" ? NetworkSide::Client : NetworkSide::Server, method.c_str());
        RemoteMessage msg;
        msg.kind = RemoteMessage::Kind::Invoke;
        msg.remote = i->id;
        if (isClient()) {
            msg.to = NetworkSide::Server;
            msg.client = client;
            msg.args = argsFrom(va, 0);
        } else {
            sol::object player = va.size() > 0 ? sol::object(va[0]) : sol::object();
            if (!player.is<Inst>() || need(player.as<Inst>())->cls != ClassId::Player) fail("InvokeClient expects a Player as its first argument");
            const GameSession::Client* c = session.clientOfPlayer(player.as<Inst>().id);
            if (!c) fail("InvokeClient: that player has left");
            msg.to = NetworkSide::Client;
            msg.client = c->index;
            msg.args = argsFrom(va, 1);
        }
        msg.call = nextCall++;
        pendingCalls[msg.call] = ts.L;
        session.postRemote(std::move(msg));
    });
    lua["__rift_invoke"] = startInvoke;
    sol::protected_function makeInvoke = lua.safe_script(R"LUA(
        local start = __rift_invoke
        return function(method)
            return function(self, ...)
                local r = table.pack(start(method, self, ...))
                if not r[1] then error(r[2], 2) end
                return table.unpack(r, 2, r.n)
            end
        end
    )LUA");
    lua["__rift_invoke"] = sol::lua_nil;
    m["InvokeServer"] = sol::function(makeInvoke("InvokeServer"));
    m["InvokeClient"] = sol::function(makeInvoke("InvokeClient"));
    sol::protected_function makeBindableInvoke = lua.safe_script(R"LUA(
        return function(callback)
            return function(self, ...)
                local fn = callback(self)
                if not fn then error("OnInvoke is not set for " .. self:GetFullName(), 2) end
                return fn(...)
            end
        end
    )LUA");
    // Through set_function: a lambda passed straight to a Lua call would be bound like a member function.
    lua.set_function("__rift_onInvoke", [this](const Inst& h) -> sol::object {
        auto it = callbacks.find(SignalKey{ need(h)->id, "OnInvoke" });
        if (it == callbacks.end()) return nil();
        return sol::make_object(lua, it->second);
    });
    sol::function onInvoke = lua["__rift_onInvoke"];
    lua["__rift_onInvoke"] = sol::lua_nil;
    m["Invoke"] = sol::function(makeBindableInvoke(onInvoke));

    sol::table methods = m;

    // ------------------------------------------------------------ the type
    lua.new_usertype<Inst>("Instance", sol::no_constructor,
        "new", sol::factories([this](const std::string& cls, sol::optional<sol::object> parent) -> sol::object {
            const ClassInfo* info = classByName(cls);
            if (!info || !info->creatable) fail("Unable to create an Instance of type \"" + cls + "\"");
            if (info->id == ClassId::Script || info->id == ClassId::LocalScript || info->id == ClassId::ModuleScript) {
                fail("Scripts are created in the editor (Insert > Script); clone one from ReplicatedStorage instead");
            }
            countCreated(1);
            Instance* i = scene.create(info->id);
            if (i->isBasePart()) i->anchored = false; // new parts fall, like Roblox
            if (parent && parent->is<Inst>()) {
                std::string error;
                if (!scene.setParent(i, need(parent->as<Inst>()), Scene::npos, &error)) fail(error);
                countFalling(*i);
            }
            return instanceFor(i->id);
        }),
        sol::meta_function::equal_to, [](const Inst& a, const Inst& b) { return a.id == b.id; },
        sol::meta_function::to_string, [this](const Inst& h) {
            if (const Instance* i = scene.find(h.id)) return i->name;
            auto t = tombstones.find(h.id);
            return t != tombstones.end() ? t->second->name : std::string("<destroyed>");
        },
        sol::meta_function::garbage_collect, [this](const Inst& h) { tombstones.erase(h.id); },
        sol::meta_function::index, [this, methods](const Inst& h, sol::stack_object key) -> sol::object {
            if (key.get_type() != sol::type::string) fail("Instance members are accessed by name");
            const std::string name = key.as<std::string>();
            Instance* i = need(h);
            sol::object method = methods[name];
            if (method.valid() && method.get_type() == sol::type::function && methodApplies(name, *i)) return method;

            // Computed members.
            if (name == "ClassName") return sol::make_object(lua, classInfo(i->cls).name);
            if (name == "Parent") {
                if (i->cls == ClassId::Player || !i->parent) return instanceOrNil(i->parent);
                return instanceFor(i->parent->id);
            }
            if (name == "CFrame" && (i->hasTransform())) return sol::make_object(lua, CFrame::fromMat4(i->worldFrame()));
            if (name == "Orientation" && i->hasTransform()) return sol::make_object(lua, Vector3L{ i->rotation });
            if (name == "WorldPosition" && i->hasTransform()) return sol::make_object(lua, Vector3L{ i->worldPosition() });
            if (name == "AssemblyLinearVelocity" && i->isBasePart()) return sol::make_object(lua, Vector3L{ i->velocity });
            if (name == "AssemblyAngularVelocity" && i->isBasePart()) return sol::make_object(lua, Vector3L{ glm::radians(i->angularVelocity) });
            if (name == "PrimaryPart" && i->cls == ClassId::Model) {
                auto it = primaryParts.find(i->id);
                if (it != primaryParts.end() && scene.find(it->second)) return instanceFor(it->second);
                Instance* root = i->findFirstChild("HumanoidRootPart");
                return instanceOrNil(root && root->isBasePart() ? root : nullptr);
            }
            if (i->cls == ClassId::Humanoid && name == "RootPart") {
                Instance* model = i->parent;
                Instance* root = model ? model->findFirstChild("HumanoidRootPart") : nullptr;
                return instanceOrNil(root);
            }
            if (i->isBasePart() && name == "BrickColor") return sol::make_object(lua, BrickColorL{ nearestBrickColor(i->color) });
            if (i->cls == ClassId::Tool && name == "Grip") {
                return sol::make_object(lua, CFrame::fromMat4(TransformUtil::compose(i->gripPos, i->gripRotation, glm::vec3(1.0f))));
            }
            if (i->cls == ClassId::Player) {
                if (name == "Character") {
                    const GameSession::Client* c = session.clientOfPlayer(i->id);
                    return c && c->characterId ? instanceOrNil(scene.find(c->characterId)) : nil();
                }
                if (name == "UserId") return sol::make_object(lua, 1);
                if (name == "DisplayName") return sol::make_object(lua, i->name);
            }
            // Players.LocalPlayer is nil on the server, like Roblox.
            if (i->cls == ClassId::Players && name == "LocalPlayer") return instanceOrNil(localPlayer());
            if ((i->cls == ClassId::RemoteFunction && (name == "OnServerInvoke" || name == "OnClientInvoke"))
                || (i->cls == ClassId::BindableFunction && name == "OnInvoke")) {
                fail(name + " is a callback member of " + classInfo(i->cls).name + "; you can only set it");
            }
            if (i->cls == ClassId::Workspace && name == "CurrentCamera") {
                // The client's Workspace.Camera; the server has no camera object.
                if (isClient()) {
                    if (Instance* c = scene.find(me().cameraId)) return instanceFor(c->id);
                }
                return sol::make_object(lua, Camera{});
            }
            if (i->cls == ClassId::Camera) {
                // The live camera behind the object (same as the Camera API).
                static const std::unordered_set<std::string> live = { "CFrame", "Focus", "FieldOfView", "CameraType", "CameraSubject", "ViewportSize",
                                                                      "ScreenPointToRay", "ViewportPointToRay", "WorldToScreenPoint", "WorldToViewportPoint" };
                if (live.count(name)) {
                    sol::userdata camera = sol::make_object(lua, Camera{});
                    return camera[name].get<sol::object>();
                }
            }
            if (i->cls == ClassId::Sound) {
                if (name == "IsPlaying") return sol::make_object(lua, session.soundPlaying(*i));
                if (name == "TimePosition") return sol::make_object(lua, session.soundTime(*i));
                if (name == "TimeLength") return sol::make_object(lua, session.soundLength(*i));
            }
            if (i->cls == ClassId::Players && name == "PlayerAdded") return sol::make_object(lua, Signal{ playerAdded });
            // Reflected properties.
            if (const PropertyInfo* p = findProperty(i->cls, name)) {
                if (p->has(PF_ScriptRead)) return toLua(p->get(*i), p);
            }
            // Events.
            if (hasEvent(*i, name)) return sol::make_object(lua, Signal{ signalFor(i->id, name) });
            // A child with that name (part.Handle).
            if (Instance* child = i->findFirstChild(name)) return instanceFor(child->id);
            fail(memberError(name, *i));
        },
        sol::meta_function::new_index, [this, setFrame](const Inst& h, sol::stack_object key, sol::stack_object value) {
            if (key.get_type() != sol::type::string) fail("Instance members are set by name");
            const std::string name = key.as<std::string>();
            Instance* i = need(h);
            if (i->cls == ClassId::Camera && (name == "CFrame" || name == "FieldOfView" || name == "CameraType")) {
                sol::userdata camera = sol::make_object(lua, Camera{});
                camera[name] = sol::object(value); // (value lives on the calling coroutine's stack)
                return;
            }
            if (name == "Parent") {
                if (!scene.find(i->id)) fail("The Parent property of " + i->name + " is locked (it was destroyed)");
                Instance* newParent = nullptr;
                if (value.is<Inst>()) newParent = need(value.as<Inst>());
                else if (value.get_type() != sol::type::lua_nil) fail("Parent must be an Instance or nil");
                if (i->cls == ClassId::Player) fail("the Player's Parent cannot be changed");
                std::string error;
                if (!scene.setParent(i, newParent, Scene::npos, &error)) fail(error);
                countFalling(*i);
                return;
            }
            if (name == "CFrame" && i->hasTransform()) {
                if (!value.is<CFrame>()) fail("CFrame must be a CFrame");
                setFrame(*i, value.as<CFrame>());
                return;
            }
            if (name == "BrickColor" && i->isBasePart()) {
                if (!value.is<BrickColorL>()) fail("BrickColor must be a BrickColor");
                scene.setProperty(*i, "Color", brickColors()[static_cast<size_t>(value.as<BrickColorL>().index)].color);
                return;
            }
            if (name == "Grip" && i->cls == ClassId::Tool) {
                if (!value.is<CFrame>()) fail("Grip must be a CFrame");
                glm::vec3 p, r, s;
                TransformUtil::decompose(value.as<CFrame>().toMat4(), p, r, s);
                scene.setProperty(*i, "GripPos", p);
                scene.setProperty(*i, "GripRotation", r);
                return;
            }
            std::string target = name;
            if (name == "Orientation") target = "Rotation";
            if (name == "AssemblyLinearVelocity") target = "Velocity";
            if (name == "AssemblyAngularVelocity" && i->isBasePart()) {
                if (!value.is<Vector3L>()) fail("AssemblyAngularVelocity must be a Vector3");
                scene.setProperty(*i, "AngularVelocity", glm::degrees(value.as<Vector3L>().v));
                return;
            }
            const bool remoteCallback = (i->cls == ClassId::RemoteFunction && (name == "OnServerInvoke" || name == "OnClientInvoke"))
                || (i->cls == ClassId::BindableFunction && name == "OnInvoke");
            if (remoteCallback) {
                if (name == "OnServerInvoke") requireSide(NetworkSide::Server, "OnServerInvoke");
                if (name == "OnClientInvoke") requireSide(NetworkSide::Client, "OnClientInvoke");
                if (value.get_type() == sol::type::lua_nil) callbacks.erase(SignalKey{ i->id, name });
                else if (value.get_type() == sol::type::function) callbacks[SignalKey{ i->id, name }] = sol::main_protected_function(value);
                else fail(name + " must be a function");
                return;
            }
            if (name == "PrimaryPart" && i->cls == ClassId::Model) {
                if (value.get_type() == sol::type::lua_nil) primaryParts.erase(i->id);
                else primaryParts[i->id] = need(value.as<Inst>())->id;
                return;
            }
            const PropertyInfo* p = findProperty(i->cls, target);
            if (!p) fail(memberError(name, *i));
            if (!p->has(PF_ScriptWrite)) fail(classInfo(i->cls).name + "." + name + " is read-only");
            std::string error;
            const Value v = fromLua(value, *p, error);
            if (!error.empty()) fail(error);
            if (!scene.setProperty(*i, *p, v, &error)) fail(error);
        });
}

} // namespace rift
