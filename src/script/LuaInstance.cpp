// LuaInstance.cpp -- the Instance type as scripts see it: every reflected
// property by name (part.Color, part.Anchored, ...), a few computed ones
// (Parent, CFrame, ClassName, ...), methods (Clone, Destroy, WaitForChild,
// ...) and events (Touched, Changed, ChildAdded, ...).
#include "script/LuaInternal.h"
#include "physics/PhysicsWorld.h"
#include "core/TransformUtil.h"

#include <algorithm>
#include <cmath>

namespace rift {

using namespace rift::lua;

namespace {

// Roblox's abstract base classes, for IsA / FindFirstChildWhichIsA.
bool isA(const Instance& i, const std::string& cls) {
    if (cls == "Instance") return true;
    if (cls == classInfo(i.cls).name) return true;
    if (cls == "BasePart") return i.isBasePart();
    if (cls == "PVInstance") return i.hasTransform() || i.cls == ClassId::Workspace;
    if (cls == "Model") return i.cls == ClassId::Model || i.cls == ClassId::Workspace;
    if (cls == "LuaSourceContainer" || cls == "BaseScript") return i.isScript() && (cls == "LuaSourceContainer" || i.cls == ClassId::Script);
    if (cls == "GuiObject") return i.isGuiObject();
    if (cls == "GuiButton") return i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton;
    if (cls == "GuiBase2d") return i.isGui();
    if (cls == "LayerCollector") return i.cls == ClassId::ScreenGui;
    if (cls == "Light") return i.isLight();
    if (cls == "ValueBase") return i.isValueObject();
    return false;
}

bool hasEvent(const Instance& i, const std::string& name) {
    static const char* common[] = { "Changed", "ChildAdded", "ChildRemoved", "DescendantAdded", "DescendantRemoving",
                                    "AncestryChanged", "Destroying", "AttributeChanged" };
    for (const char* e : common) if (name == e) return true;
    if (i.isBasePart()) return name == "Touched" || name == "TouchEnded";
    if (i.cls == ClassId::Humanoid) return name == "Died" || name == "HealthChanged" || name == "Jumping" || name == "Running";
    if (i.cls == ClassId::Sound) return name == "Ended";
    if (i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton) {
        return name == "Activated" || name == "MouseButton1Click" || name == "MouseButton1Down" || name == "MouseButton1Up"
            || name == "MouseEnter" || name == "MouseLeave";
    }
    if (i.isGuiObject()) return name == "MouseEnter" || name == "MouseLeave";
    if (i.cls == ClassId::Player) return name == "CharacterAdded" || name == "CharacterRemoving";
    return false;
}

// Methods that only exist on some classes. Anything else is shared by all
// instances. On other classes the name falls through to child lookup, so a
// button named "Play" is found even though Sound has a :Play() method.
bool methodApplies(const std::string& name, const Instance& i) {
    static const std::unordered_map<std::string, bool (*)(const Instance&)> only = {
        { "Play", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Stop", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Pause", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "Resume", [](const Instance& x) { return x.cls == ClassId::Sound; } },
        { "LoadCharacter", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "Kick", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "GetMouse", [](const Instance& x) { return x.cls == ClassId::Player; } },
        { "GetPlayers", [](const Instance& x) { return x.cls == ClassId::Players; } },
        { "GetPlayerFromCharacter", [](const Instance& x) { return x.cls == ClassId::Players; } },
        { "Raycast", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "GetPartBoundsInBox", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "GetPartsInPart", [](const Instance& x) { return x.cls == ClassId::Workspace; } },
        { "TakeDamage", [](const Instance& x) { return x.cls == ClassId::Humanoid; } },
        { "GetTouchingParts", [](const Instance& x) { return x.isBasePart(); } },
        { "ApplyImpulse", [](const Instance& x) { return x.isBasePart(); } },
        { "ApplyAngularImpulse", [](const Instance& x) { return x.isBasePart(); } },
        { "GetMass", [](const Instance& x) { return x.isBasePart(); } },
        { "GetPivot", [](const Instance& x) { return x.hasTransform(); } },
        { "PivotTo", [](const Instance& x) { return x.hasTransform(); } },
        { "SetPrimaryPartCFrame", [](const Instance& x) { return x.cls == ClassId::Model; } },
        { "MoveTo", [](const Instance& x) { return x.hasTransform() || x.cls == ClassId::Humanoid; } },
        { "GetBoundingBox", [](const Instance& x) { return x.cls == ClassId::Model || x.isBasePart(); } },
        { "GetExtentsSize", [](const Instance& x) { return x.cls == ClassId::Model; } },
    };
    auto it = only.find(name);
    return it == only.end() || it->second(i);
}

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

    // ------------------------------------------------------------ methods
    sol::table m = lua.create_table();

    m["Clone"] = [this](const Inst& h) -> sol::object {
        Instance* i = need(h);
        if (i->isService() || i->cls == ClassId::Player) fail(classInfo(i->cls).name + " cannot be cloned");
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
        auto it = tagAdded.find(tag);
        const uint64_t id = i->id;
        if (it != tagAdded.end()) queue(it->second, [this, id](lua_State* T) { return pushInstance(T, id); });
    };
    m["RemoveTag"] = [this](const Inst& h, const std::string& tag) {
        Instance* i = need(h);
        if (!i->hasTag(tag)) return;
        i->removeTag(tag);
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
        if (i->cls == ClassId::Humanoid) { session.moveCharacterTo(position.v); return; }
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
    m["TakeDamage"] = [this](const Inst& h, float amount) {
        Instance* i = need(h);
        if (i->cls != ClassId::Humanoid) fail(memberError("TakeDamage", *i));
        scene.setProperty(*i, "Health", std::max(0.0f, i->health - std::max(0.0f, amount)));
    };
    m["Play"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Play", *i)); session.playSound(*i); };
    m["Stop"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Stop", *i)); session.stopSound(*i); };
    m["Pause"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Pause", *i)); session.pauseSound(*i, true); };
    m["Resume"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Sound) fail(memberError("Resume", *i)); session.pauseSound(*i, false); };
    m["LoadCharacter"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Player) fail(memberError("LoadCharacter", *i)); session.loadCharacter(); };
    m["Kick"] = [this](const Inst& h, sol::optional<std::string> message) {
        Instance* i = need(h);
        if (i->cls != ClassId::Player) fail(memberError("Kick", *i));
        session.finish(false, message.value_or("You were kicked from the game"));
    };
    m["GetMouse"] = [this](const Inst& h) { Instance* i = need(h); if (i->cls != ClassId::Player) fail(memberError("GetMouse", *i)); return Mouse{}; };
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
        Instance* c = session.character();
        return c && c->id == character.as<Inst>().id ? instanceOrNil(session.player()) : nil();
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
        return result;
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
    sol::table methods = m;

    // ------------------------------------------------------------ the type
    lua.new_usertype<Inst>("Instance", sol::no_constructor,
        "new", sol::factories([this](const std::string& cls, sol::optional<sol::object> parent) -> sol::object {
            const ClassInfo* info = classByName(cls);
            if (!info || !info->creatable) fail("Unable to create an Instance of type \"" + cls + "\"");
            if (info->id == ClassId::Script || info->id == ClassId::ModuleScript) {
                fail("Scripts are created in the editor (Insert > Script); clone one from ReplicatedStorage instead");
            }
            Instance* i = scene.create(info->id);
            if (i->isBasePart()) i->anchored = false; // new parts fall, like Roblox
            if (parent && parent->is<Inst>()) {
                std::string error;
                if (!scene.setParent(i, need(parent->as<Inst>()), Scene::npos, &error)) fail(error);
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
            if (i->cls == ClassId::Player) {
                if (name == "Character") return instanceOrNil(session.character());
                if (name == "UserId") return sol::make_object(lua, 1);
                if (name == "DisplayName") return sol::make_object(lua, i->name);
                if (name == "PlayerGui") return instanceOrNil(scene.service(ClassId::StarterGui));
            }
            if (i->cls == ClassId::Players && name == "LocalPlayer") return instanceOrNil(session.player());
            if (i->cls == ClassId::Workspace && name == "CurrentCamera") return sol::make_object(lua, Camera{});
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
            if (name == "Parent") {
                if (!scene.find(i->id)) fail("The Parent property of " + i->name + " is locked (it was destroyed)");
                Instance* newParent = nullptr;
                if (value.is<Inst>()) newParent = need(value.as<Inst>());
                else if (value.get_type() != sol::type::lua_nil) fail("Parent must be an Instance or nil");
                if (i->cls == ClassId::Player) fail("the Player's Parent cannot be changed");
                std::string error;
                if (!scene.setParent(i, newParent, Scene::npos, &error)) fail(error);
                return;
            }
            if (name == "CFrame" && i->hasTransform()) {
                if (!value.is<CFrame>()) fail("CFrame must be a CFrame");
                setFrame(*i, value.as<CFrame>());
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
