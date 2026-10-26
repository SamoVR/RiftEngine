// Script editor autocomplete, and the API catalogue it uses checked against
// what the runtime really offers.
#include "doctest.h"
#include "editor/LuaCompletion.h"
#include "game/GameSession.h"
#include "platform/Input.h"
#include "scene/Reflection.h"
#include "scene/Scene.h"
#include "script/LuaApi.h"
#include "script/ScriptStore.h"
#include "core/Paths.h"

#include <algorithm>
#include <filesystem>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    bool has(const std::vector<std::string>& list, const std::string& s) { return std::find(list.begin(), list.end(), s) != list.end(); }

    std::vector<std::string> complete(const Scene& scene, const std::string& line, const std::string& doc = {}, const Instance* script = nullptr) {
        // The word being typed is whatever identifier ends the line.
        size_t start = line.size();
        while (start > 0 && (std::isalnum(static_cast<unsigned char>(line[start - 1])) || line[start - 1] == '_')) --start;
        return luaCompletions(line, start, line.substr(start), scene, script, doc.empty() ? line : doc);
    }
}

TEST_CASE("completion: members of scene objects, services, types, enums and locals") {
    Scene scene;
    Instance* map = scene.create(ClassId::Model, &scene.workspace());
    map->name = "Map";
    Instance* door = scene.create(ClassId::Part, map);
    door->name = "Door";
    Instance* sign = scene.create(ClassId::Part, map);
    sign->name = "Sign Post"; // not an identifier: not offered after "."
    Instance* script = scene.create(ClassId::Script, door);

    auto r = complete(scene, "workspace.Map.");
    CHECK(has(r, "Door"));
    CHECK_FALSE(has(r, "Sign Post"));
    CHECK(has(r, "ChildAdded"));
    CHECK(r.front() == "Door"); // children come first

    r = complete(scene, "workspace.Map.Door.Tr");
    CHECK(has(r, "Transparency"));
    CHECK_FALSE(has(r, "Anchored"));
    r = complete(scene, "workspace.Map.Door.");
    CHECK(has(r, "Anchored"));
    CHECK(has(r, "Touched"));
    CHECK(has(r, "CFrame"));

    r = complete(scene, "workspace.Map.Door:");
    CHECK(has(r, "Destroy"));
    CHECK(has(r, "ApplyImpulse"));
    CHECK_FALSE(has(r, "GetPlayers"));

    r = complete(scene, "script.Parent.Anch", {}, script);
    REQUIRE_FALSE(r.empty());
    CHECK(r.front() == "Anchored");

    r = complete(scene, "game:GetService(\"Players\").");
    CHECK(has(r, "LocalPlayer"));
    CHECK(has(r, "PlayerAdded"));
    r = complete(scene, "game:GetService(\"RunService\").");
    CHECK(has(r, "Heartbeat"));
    r = complete(scene, "game.");
    CHECK(has(r, "ReplicatedStorage"));

    r = complete(scene, "Vector3.");
    CHECK(has(r, "new"));
    CHECK(has(r, "zero"));
    r = complete(scene, "Vector3.new(1, 2, 3).");
    CHECK(has(r, "Magnitude"));
    r = complete(scene, "math.cl");
    CHECK(r.front() == "clamp");
    r = complete(scene, "Enum.");
    CHECK(has(r, "Material"));
    CHECK(has(r, "KeyCode"));
    r = complete(scene, "Enum.Material.Gr");
    CHECK(has(r, "Grass"));
    CHECK(has(r, "Granite"));

    // Locals: what they were set to.
    const std::string doc = "local door = workspace.Map.Door\n"
                            "local players = game:GetService(\"Players\")\n"
                            "local part = Instance.new(\"Part\")\n"
                            "local conn = door.Touched:Connect(function() end)\n"
                            "door.";
    r = complete(scene, "door.", doc);
    CHECK(has(r, "Anchored"));
    r = complete(scene, "players.", doc);
    CHECK(has(r, "LocalPlayer"));
    r = complete(scene, "part:", doc);
    CHECK(has(r, "GetMass"));
    r = complete(scene, "door.Touched:", doc);
    CHECK(has(r, "Connect"));
    r = complete(scene, "conn:", doc);
    CHECK(has(r, "Disconnect"));

    // A word on its own: keywords, globals, the file's own names.
    r = complete(scene, "fu", doc);
    CHECK(has(r, "function"));
    r = complete(scene, "pl", doc);
    CHECK(has(r, "players"));
    r = complete(scene, "work", doc);
    CHECK(r.front() == "workspace");
    // Nothing to offer: no list.
    CHECK(complete(scene, "local x = 12.").empty());
}

TEST_CASE("completion: every listed method and event exists in the Lua API") {
    // Instances the script can reach for each class with a method or event in the catalogue.
    Scene scene;
    ScriptStore store;
    Input input;
    const fs::path folder = fs::temp_directory_path() / "rift_completion_api";
    fs::remove_all(folder);
    fs::create_directories(folder);
    store.reset(Paths::toUtf8(folder));

    std::string lua = "local missing = {}\n"
                      "local function check(obj, name, kind)\n"
                      "  local ok, v = pcall(function() return obj[name] end)\n"
                      "  local t = ok and typeof(v) or 'error'\n"
                      "  if (kind == 'method' and t ~= 'function') or (kind == 'event' and t ~= 'RBXScriptSignal') then table.insert(missing, name .. ':' .. t) end\n"
                      "end\n"
                      "local function make(cls)\n"
                      "  if cls == 'Workspace' then return workspace end\n"
                      "  if cls == 'Players' or cls == 'StarterGui' then return game:GetService(cls) end\n"
                      "  if cls == 'Terrain' then return workspace.Terrain end\n"
                      "  local ok, obj = pcall(Instance.new, cls)\n"
                      "  return ok and obj or nil\n"
                      "end\n";
    int checks = 0;
    auto reachable = [](ClassId cls) {
        const ClassInfo& info = classInfo(cls);
        return info.creatable || cls == ClassId::Workspace || cls == ClassId::Players || cls == ClassId::StarterGui || cls == ClassId::Terrain;
    };
    for (const std::string& m : luaapi::methodNames()) {
        for (int c = 0; c < kClassCount; ++c) {
            Instance probe;
            probe.cls = static_cast<ClassId>(c);
            if (!luaapi::methodApplies(m, probe) || !reachable(probe.cls)) continue;
            lua += "do local o = make('" + classInfo(probe.cls).name + "') if o then check(o, '" + m + "', 'method') end end\n";
            ++checks;
            break;
        }
    }
    for (const std::string& e : luaapi::eventNames()) {
        for (int c = 0; c < kClassCount; ++c) {
            Instance probe;
            probe.cls = static_cast<ClassId>(c);
            if (!luaapi::hasEvent(probe, e) || !reachable(probe.cls)) continue;
            lua += "do local o = make('" + classInfo(probe.cls).name + "') if o then check(o, '" + e + "', 'event') end end\n";
            ++checks;
            break;
        }
    }
    lua += "print('missing: ' .. table.concat(missing, ', '))\n";
    CHECK(checks > 60);
    store.create("scripts/Api.lua", lua);
    Instance* s = scene.create(ClassId::Script, scene.service(ClassId::ScriptService));
    s->source = "scripts/Api.lua";
    {
        GameSession session(scene, store, input, GameSession::Options{});
        session.start();
        ViewportInput view;
        session.update(1.0f / 60.0f, view);
        bool found = false;
        std::string all;
        for (const auto& line : session.logLines()) {
            all += line.text + "\n";
            if (line.text.rfind("missing: ", 0) == 0) {
                found = true;
                CHECK_MESSAGE(line.text == "missing: ", line.text);
            }
        }
        CHECK_MESSAGE(found, all);
    }
    std::error_code ec;
    fs::remove_all(folder, ec);
}
