// Scripting runtime and game session, headless.
#include "doctest.h"
#include "game/GameSession.h"
#include "script/LuaRuntime.h"
#include "script/ScriptStore.h"
#include "physics/PhysicsWorld.h"
#include "platform/Input.h"
#include "scene/SceneIO.h"
#include "core/Paths.h"

#include <GLFW/glfw3.h>
#include <filesystem>
#include <fstream>

using namespace rift;
namespace fs = std::filesystem;

namespace {

// A scene folder in %TEMP% with scripts kept in memory (like unsaved editor edits).
struct Fixture {
    Scene scene;
    ScriptStore store;
    Input input;
    fs::path folder;
    std::unique_ptr<GameSession> session;
    int scriptCount = 0;

    Fixture() {
        folder = fs::temp_directory_path() / ("rift_lua_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(folder);
        fs::create_directories(folder);
        store.reset(Paths::toUtf8(folder));
        // A floor so the character has somewhere to stand.
        Instance* floor = scene.create(ClassId::Part, &scene.workspace());
        floor->name = "Floor";
        floor->size = { 100, 1, 100 };
        floor->position = { 0, -0.5f, 0 };
    }
    ~Fixture() {
        session.reset();
        std::error_code ec;
        fs::remove_all(folder, ec);
    }

    Instance* script(const std::string& source, Instance* parent = nullptr, ClassId cls = ClassId::Script, const std::string& name = "") {
        const std::string path = "scripts/S" + std::to_string(++scriptCount) + ".lua";
        store.create(path, source);
        Instance* s = scene.create(cls, parent ? parent : scene.service(ClassId::ScriptService));
        s->source = path;
        if (!name.empty()) s->name = name;
        return s;
    }

    GameSession& start(bool character = true, fs::path dataFolder = {}) {
        GameSession::Options options;
        options.spawnCharacter = character;
        options.dataStoreFolder = dataFolder;
        session = std::make_unique<GameSession>(scene, store, input, options);
        session->start();
        return *session;
    }

    void run(float seconds, float dt = 1.0f / 60.0f) {
        ViewportInput view;
        for (float t = 0.0f; t < seconds; t += dt) {
            input.newFrame();
            session->update(dt, view);
        }
    }

    bool logged(const std::string& text) const {
        for (const auto& line : session->logLines()) if (line.text.find(text) != std::string::npos) return true;
        return false;
    }
    int errors() const {
        int n = 0;
        for (const auto& line : session->logLines()) if (line.level == LogLevel::Error) ++n;
        return n;
    }
    std::string dump() const {
        std::string out;
        for (const auto& line : session->logLines()) out += line.text + "\n";
        return out;
    }
    Instance* find(const std::string& name) const {
        Instance* found = nullptr;
        session->scene().forEachInTree([&](Instance& i) { if (!found && i.name == name) found = &i; });
        return found;
    }
};

} // namespace

TEST_CASE("print, warn and runtime errors reach the output with the script's name") {
    Fixture f;
    f.script("print('hello', 1 + 2, Vector3.new(1, 2, 3))\nwarn('careful')\nlocal x = nil\nprint(x.y)", nullptr, ClassId::Script, "Greeter");
    f.start(false);
    CHECK(f.logged("hello 3 1, 2, 3"));
    CHECK(f.logged("careful"));
    CHECK(f.logged("ScriptService.Greeter:4"));
    CHECK(f.errors() == 1);
}

TEST_CASE("task.wait, spawn, defer, delay and cancel") {
    Fixture f;
    f.script(R"(
        local order = {}
        task.spawn(function() table.insert(order, "spawn") end)
        task.defer(function() table.insert(order, "defer") end)
        table.insert(order, "main")
        task.delay(0.5, function(a, b) table.insert(order, "delay " .. a .. b) end, "x", "y")
        local cancelled = task.delay(0.2, function() table.insert(order, "never") end)
        task.cancel(cancelled)
        local waited = task.wait(1)
        print(table.concat(order, ","), waited >= 0.99)
    )");
    f.start(false);
    f.run(1.2f);
    CHECK_MESSAGE(f.logged("spawn,main,defer,delay xy true"), f.dump());
    CHECK(f.errors() == 0);
}

TEST_CASE("instances: create, properties, clone, destroy, errors") {
    Fixture f;
    f.script(R"(
        local p = Instance.new("Part")
        p.Name = "Made"
        p.Size = Vector3.new(2, 3, 4)
        p.Color = Color3.fromRGB(255, 0, 0)
        p.Material = Enum.Material.Neon
        p.Anchored = true
        p.Parent = workspace
        assert(workspace:FindFirstChild("Made") == p)
        assert(p.Material == Enum.Material.Neon and p.Material.Name == "Neon")
        assert(p.Size.Y == 3 and p.ClassName == "Part" and p:IsA("BasePart"))
        local c = p:Clone()
        assert(c.Parent == nil and c.Name == "Made")
        c.Parent = workspace
        assert(#workspace:GetChildren() >= 3)
        p:SetAttribute("Health", 50)
        assert(p:GetAttribute("Health") == 50)
        local ok, err = pcall(function() p.NoSuchThing = 1 end)
        assert(not ok and string.find(err, "NoSuchThing"))
        ok, err = pcall(function() p.Size = "big" end)
        assert(not ok and string.find(err, "Vector3"))
        p:Destroy()
        assert(p.Name == "Made" and p.Parent == nil) -- destroyed instances stay readable (Roblox)
        ok, err = pcall(function() p.Parent = workspace end)
        assert(not ok and string.find(err, "locked"))
        print("instances ok")
    )");
    f.start(false);
    CHECK_MESSAGE(f.logged("instances ok"), f.dump());
    CHECK(f.errors() == 0);
}

TEST_CASE("events: Changed, ChildAdded, attributes, Once, Wait, Disconnect") {
    Fixture f;
    f.script(R"(
        local folder = Instance.new("Folder")
        folder.Parent = workspace
        local log = {}
        folder.ChildAdded:Connect(function(c) table.insert(log, "added " .. c.Name) end)
        local conn = folder.Changed:Connect(function(prop) table.insert(log, "changed " .. prop) end)
        folder:GetAttributeChangedSignal("Score"):Connect(function() table.insert(log, "score " .. folder:GetAttribute("Score")) end)
        folder.ChildRemoved:Once(function(c) table.insert(log, "removed " .. c.Name) end)
        local a = Instance.new("Part") a.Name = "A" a.Parent = folder
        folder.Name = "Box"
        folder:SetAttribute("Score", 7)
        a:Destroy()
        task.wait()
        conn:Disconnect()
        folder.Name = "Ignored"
        task.spawn(function()
            local child = folder.ChildAdded:Wait()
            table.insert(log, "waited " .. child.Name)
            print(table.concat(log, ";"))
        end)
        local b = Instance.new("IntValue") b.Name = "B" b.Parent = folder
    )");
    f.start(false);
    f.run(0.2f);
    CHECK_MESSAGE(f.logged("added A;changed Name;score 7;removed A;waited B"), f.dump());
    CHECK(f.errors() == 0);
}

TEST_CASE("value objects pass their new value to Changed (leaderstats)") {
    Fixture f;
    f.script(R"(
        local player = game.Players.LocalPlayer
        local stats = Instance.new("Folder") stats.Name = "leaderstats" stats.Parent = player
        local coins = Instance.new("IntValue") coins.Name = "Coins" coins.Parent = stats
        coins.Changed:Connect(function(v) print("coins now", v) end)
        coins.Value = coins.Value + 5
    )");
    f.start(false);
    f.run(0.1f);
    CHECK_MESSAGE(f.logged("coins now 5"), f.dump());
}

TEST_CASE("WaitForChild and require") {
    Fixture f;
    Instance* module = f.script("local M = {} function M.double(x) return x * 2 end return M", f.scene.service(ClassId::ReplicatedStorage), ClassId::ModuleScript, "MathUtil");
    (void)module;
    f.script(R"(
        local util = require(game.ReplicatedStorage:WaitForChild("MathUtil"))
        task.delay(0.3, function() local p = Instance.new("Part") p.Name = "Late" p.Parent = workspace end)
        local late = workspace:WaitForChild("Late")
        print("double", util.double(21), late.Name)
        print("timeout", workspace:WaitForChild("Never", 0.2))
    )");
    f.start(false);
    f.run(1.0f);
    CHECK_MESSAGE(f.logged("double 42 Late"), f.dump());
    CHECK(f.logged("timeout nil"));
}

TEST_CASE("runaway loops are stopped and memory is capped; the game keeps going") {
    Fixture f;
    f.script("while true do end");
    f.script("local t = {} for i = 1, 2000 do t[i] = string.rep('x', 1000000) .. i end"); // > 512 MB
    f.script("task.wait(0.5) print('still alive')");
    f.start(false);
    f.run(1.0f);
    CHECK(f.logged("more than 1 second"));
    CHECK(f.logged("not enough memory"));
    CHECK(f.logged("still alive"));
}

TEST_CASE("sandbox: no file or process access") {
    Fixture f;
    f.script("print('sandbox', io, os.execute, load, dofile, debug, package, os.time() > 0)");
    f.start(false);
    CHECK_MESSAGE(f.logged("sandbox nil nil nil nil nil nil true"), f.dump());
}

TEST_CASE("scripts that stop or destroy themselves") {
    Fixture f;
    f.script("task.wait(0.1) print('before') script.Enabled = false print('after disable')");
    f.script("task.wait(0.1) script:Destroy() print('after destroy')");
    f.script("RunService.Heartbeat:Connect(function() end) task.wait(0.1) script.Parent = nil");
    f.start(false);
    f.run(0.5f);
    CHECK(f.logged("before"));
    CHECK(f.errors() == 0);
}

TEST_CASE("value types: CFrame, Vector3, Color3, UDim2") {
    Fixture f;
    f.script(R"(
        local cf = CFrame.new(1, 2, 3) * CFrame.Angles(0, math.pi / 2, 0)
        local p = cf * Vector3.new(1, 0, 0)
        assert(math.abs(p.X - 1) < 1e-4 and math.abs(p.Z - 2) < 1e-4, tostring(p))
        local back = cf:Inverse() * cf
        assert((back.Position).Magnitude < 1e-5)
        local look = CFrame.lookAt(Vector3.new(0, 0, 0), Vector3.new(0, 0, -10)).LookVector
        assert(math.abs(look.Z + 1) < 1e-5)
        local c = Color3.fromHSV(0, 1, 1)
        assert(c.R == 1 and c.G == 0)
        local u = UDim2.new(0.5, 10, 1, -20) + UDim2.fromOffset(5, 5)
        assert(u.X.Offset == 15 and u.Y.Scale == 1)
        assert(typeof(cf) == "CFrame" and typeof(u) == "UDim2" and typeof(workspace) == "Instance")
        assert(Vector3.new(3, 4, 0).Magnitude == 5)
        print("types ok")
    )");
    f.start(false);
    CHECK_MESSAGE(f.logged("types ok"), f.dump());
}

TEST_CASE("TweenService moves a part and Completed fires") {
    Fixture f;
    Instance* part = f.scene.create(ClassId::Part, &f.scene.workspace());
    part->name = "Door";
    part->position = { 0, 5, 0 };
    f.script(R"(
        local door = workspace.Door
        local tween = game:GetService("TweenService"):Create(door, TweenInfo.new(0.5, Enum.EasingStyle.Linear), { Position = Vector3.new(10, 5, 0), Transparency = 0.5 })
        tween:Play()
        local state = tween.Completed:Wait()
        print("tween", state.Name, door.Position.X, door.Transparency)
    )");
    f.start(false);
    f.run(0.25f);
    Instance* door = f.find("Door");
    REQUIRE(door);
    CHECK(door->position.x == doctest::Approx(5.0f).epsilon(0.1));
    f.run(0.5f);
    CHECK_MESSAGE(f.logged("tween Completed 10 0.5"), f.dump());
}

TEST_CASE("HttpService JSON and DataStores that persist between sessions") {
    const fs::path data = fs::temp_directory_path() / "rift_datastore_test";
    fs::remove_all(data);
    {
        Fixture f;
        f.script(R"(
            local http = game:GetService("HttpService")
            local text = http:JSONEncode({ name = "Ann", scores = { 1, 2, 3 } })
            local back = http:JSONDecode(text)
            assert(back.name == "Ann" and back.scores[3] == 3)
            local store = game:GetService("DataStoreService"):GetDataStore("Saves")
            store:SetAsync("best", 120)
            store:IncrementAsync("plays")
            store:UpdateAsync("best", function(old) return math.max(old, 150) end)
            print("saved")
        )");
        f.start(false, data);
        CHECK_MESSAGE(f.logged("saved"), f.dump());
    }
    {
        Fixture f;
        f.script(R"(
            local store = game:GetService("DataStoreService"):GetDataStore("Saves")
            print("loaded", store:GetAsync("best"), store:GetAsync("plays"), store:GetAsync("missing"))
        )");
        f.start(false, data);
        CHECK_MESSAGE(f.logged("loaded 150 1 nil"), f.dump());
    }
    fs::remove_all(data);
}

TEST_CASE("the character spawns on a SpawnLocation and walks with WASD") {
    Fixture f;
    Instance* spawn = f.scene.create(ClassId::SpawnLocation, &f.scene.workspace());
    spawn->position = { 10, 0.2f, 0 };
    GameSession& s = f.start(true);
    f.run(0.5f);
    Instance* character = s.character();
    REQUIRE(character);
    CHECK(character->cls == ClassId::Model);
    REQUIRE(s.rootPart());
    CHECK(s.rootPart()->worldPosition().x == doctest::Approx(10.0f).epsilon(0.05));
    CHECK(s.rootPart()->worldPosition().y == doctest::Approx(1.4f).epsilon(0.1)); // feet on the spawn (top 0.4) + 1
    const float startZ = s.rootPart()->worldPosition().z;
    f.input.setKey(GLFW_KEY_W, true);
    f.run(1.0f);
    f.input.setKey(GLFW_KEY_W, false);
    CHECK(std::abs(s.rootPart()->worldPosition().z - startZ) > 4.0f);
}

TEST_CASE("kill bricks work with the Roblox Humanoid pattern, and the player respawns") {
    Fixture f;
    Instance* brick = f.scene.create(ClassId::Part, &f.scene.workspace());
    brick->name = "Lava";
    brick->position = { 0, 0.25f, 0 };
    brick->size = { 6, 0.5f, 6 };
    f.script(R"(
        script.Parent.Touched:Connect(function(hit)
            local humanoid = hit.Parent:FindFirstChildOfClass("Humanoid")
            if humanoid then humanoid.Health = 0 end
        end)
    )", brick);
    f.script(R"(
        game.Players.LocalPlayer.CharacterAdded:Connect(function(char) print("character added", char.Name) end)
        local humanoid = game.Players.LocalPlayer.Character:WaitForChild("Humanoid")
        humanoid.Died:Connect(function() print("died") end)
    )");
    GameSession& s = f.start(true);
    f.run(1.0f);
    CHECK_MESSAGE(f.logged("died"), f.dump());
    f.run(3.5f);
    CHECK(f.logged("character added"));
    CHECK(s.humanoid());
    CHECK(f.errors() == 0);
}

TEST_CASE("GUI buttons fire Activated") {
    Fixture f;
    Instance* gui = f.scene.create(ClassId::ScreenGui, f.scene.service(ClassId::StarterGui));
    Instance* button = f.scene.create(ClassId::TextButton, gui);
    button->name = "Play";
    button->guiPosition = { 0.5f, -50.0f, 0.5f, -20.0f };
    button->guiSize = { 0.0f, 100.0f, 0.0f, 40.0f };
    f.script("game.StarterGui.ScreenGui.Play.Activated:Connect(function() print('clicked') end)");
    GameSession& s = f.start(false);
    ViewportInput view;
    view.size = { 1000, 600 };
    view.mouse = { 500, 300 };
    f.input.newFrame();
    s.update(1.0f / 60.0f, view);
    f.input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, true);
    f.input.newFrame();
    s.update(1.0f / 60.0f, view);
    f.input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, false);
    f.input.newFrame();
    s.update(1.0f / 60.0f, view);
    CHECK(s.gui().mouseOverGui());
    CHECK_MESSAGE(f.logged("clicked"), f.dump());
}

TEST_CASE("workspace:Raycast from a script") {
    Fixture f;
    f.script(R"(
        local result = workspace:Raycast(Vector3.new(0, 10, 0), Vector3.new(0, -50, 0))
        print("ray", result.Instance.Name, math.floor(result.Position.Y + 0.5), result.Normal.Y)
    )");
    f.start(false);
    f.run(0.1f);
    CHECK_MESSAGE(f.logged("ray Floor 0 1"), f.dump());
}

TEST_CASE("the edited scene is untouched by play") {
    Fixture f;
    f.script("for _, c in ipairs(workspace:GetChildren()) do c:Destroy() end workspace.Gravity = 5");
    const size_t before = f.scene.workspace().children.size();
    f.start(false);
    f.run(0.2f);
    CHECK(f.session->scene().workspace().children.empty());
    CHECK(f.scene.workspace().children.size() == before);
    CHECK(f.scene.workspace().gravity == doctest::Approx(30.0f));
}

TEST_CASE("the demo obby runs its scripts without errors") {
    Scene scene;
    const fs::path file = Paths::engineData().parent_path() / "examples/DemoObby/scenes/DemoObby/scene.json";
    REQUIRE(SceneIO::loadScene(scene, file).ok);
    ScriptStore store;
    store.reset(Paths::toUtf8(file.parent_path()));
    Input input;
    GameSession::Options options;
    GameSession session(scene, store, input, options);
    session.start();
    ViewportInput view;
    for (int i = 0; i < 240; ++i) {
        input.newFrame();
        session.update(1.0f / 60.0f, view);
    }
    std::string out;
    int errors = 0;
    for (const auto& line : session.logLines()) {
        out += line.text + "\n";
        if (line.level == LogLevel::Error) ++errors;
    }
    CHECK_MESSAGE(errors == 0, out);
    CHECK(session.character());
}
