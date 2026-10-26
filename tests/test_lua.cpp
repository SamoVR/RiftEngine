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
#include <cstdio>
#include <fstream>
#include <iterator>

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

    // A LocalScript in StarterPlayerScripts: runs on the client (copied into PlayerScripts).
    Instance* localScript(const std::string& source, const std::string& name = "") {
        Instance* folder = nullptr;
        for (Instance* c : scene.starterPlayer().children) if (c->cls == ClassId::StarterPlayerScripts) folder = c;
        return script(source, folder, ClassId::LocalScript, name);
    }

    GameSession& start(bool character = true, fs::path dataFolder = {}, int players = 1) {
        GameSession::Options options;
        options.spawnCharacter = character;
        options.dataStoreFolder = dataFolder;
        options.playerCount = players;
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
        local player = game.Players:GetPlayers()[1]
        assert(player and game.Players.LocalPlayer == nil) -- no LocalPlayer on the server
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

TEST_CASE("a sideways launch carries the character through the air") {
    Fixture f;
    Instance* spawn = f.scene.create(ClassId::SpawnLocation, &f.scene.workspace());
    spawn->position = { 0, 0.2f, 0 };
    f.script(R"(
        local character = game.Players:GetPlayers()[1].Character or game.Players:GetPlayers()[1].CharacterAdded:Wait()
        task.wait(0.3)
        character.HumanoidRootPart.AssemblyLinearVelocity = Vector3.new(20, 14, 0)
    )");
    GameSession& s = f.start(true);
    f.run(0.3f);
    REQUIRE(s.rootPart());
    const float startX = s.rootPart()->worldPosition().x;
    f.run(0.8f);
    // Without the carry the character would stop at once (nobody presses a key).
    CHECK_MESSAGE(s.rootPart()->worldPosition().x - startX > 8.0f, f.dump());
    // Back on the ground it stops.
    f.run(1.5f);
    const float landedX = s.rootPart()->worldPosition().x;
    f.run(0.5f);
    CHECK(std::abs(s.rootPart()->worldPosition().x - landedX) < 0.5f);
}

TEST_CASE("a launch pad throws a character that a LocalScript teleported onto it") {
    Fixture f;
    Instance* spawn = f.scene.create(ClassId::SpawnLocation, &f.scene.workspace());
    spawn->position = { 0, 0.2f, 0 };
    Instance* pad = f.scene.create(ClassId::Part, &f.scene.workspace());
    pad->name = "Pad";
    pad->position = { 20, 0.15f, 0 };
    pad->size = { 3, 0.3f, 3 };
    f.script(R"(
        workspace.Pad.Touched:Connect(function(hit)
            local root = hit.Parent:FindFirstChild("HumanoidRootPart")
            if root and not workspace.Pad:GetAttribute("Used") then
                workspace.Pad:SetAttribute("Used", true)
                root.AssemblyLinearVelocity = Vector3.new(15, 18, 0)
            end
        end)
    )");
    f.localScript(R"(
        local player = game.Players.LocalPlayer
        local character = player.Character or player.CharacterAdded:Wait()
        task.wait(0.5)
        character.HumanoidRootPart.CFrame = CFrame.new(workspace.Pad.Position + Vector3.new(0, 1.6, 0))
        for k = 1, 8 do
            task.wait(0.25)
            local root = character.HumanoidRootPart.CFrame.Position
            print("at", math.floor(root.X), "body", (character.Torso.CFrame.Position - root).Magnitude < 1)
        end
    )");
    GameSession& s = f.start(true);
    f.run(3.0f);
    REQUIRE(s.rootPart());
    // Thrown towards +X (from x = 20), never backwards, and the body stays with the root.
    CHECK_MESSAGE(s.rootPart()->worldPosition().x > 26.0f, f.dump());
    CHECK_MESSAGE(!f.logged("body false"), f.dump());
    CHECK(glm::length(s.rootPart()->position) < 0.5f); // still in its place inside the model
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
    // The player's own view of it: a LocalScript.
    f.localScript(R"(
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
    // GUI input is the client's: a LocalScript inside the ScreenGui (copied into PlayerGui).
    f.script("script.Parent.Play.Activated:Connect(function() print('clicked') end)", gui, ClassId::LocalScript);
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

TEST_CASE("a click on something inside a button activates the button") {
    Fixture f;
    Instance* gui = f.scene.create(ClassId::ScreenGui, f.scene.service(ClassId::StarterGui));
    Instance* button = f.scene.create(ClassId::TextButton, gui);
    button->name = "Play";
    button->text = "";
    button->guiPosition = { 0.5f, -100.0f, 0.5f, -30.0f };
    button->guiSize = { 0.0f, 200.0f, 0.0f, 60.0f };
    Instance* label = f.scene.create(ClassId::TextLabel, button); // covers the whole button, opaque
    label->guiSize = { 1.0f, 0.0f, 1.0f, 0.0f };
    label->backgroundTransparency = 0.0f;
    f.script("script.Parent.Play.Activated:Connect(function() print('clicked through the label') end)", gui, ClassId::LocalScript);
    GameSession& s = f.start(false);
    ViewportInput view;
    view.size = { 1000, 600 };
    view.mouse = { 500, 300 };
    for (bool down : { false, true, false }) {
        f.input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, down);
        f.input.newFrame();
        s.update(1.0f / 60.0f, view);
    }
    CHECK_MESSAGE(f.logged("clicked through the label"), f.dump());
}

TEST_CASE("GUI text styling, rotation and UIGradient from scripts") {
    Fixture f;
    f.script(R"(
        local label = Instance.new("TextLabel")
        label.Font = Enum.Font.GothamBlack
        label.TextScaled = true
        label.TextStrokeTransparency = 0
        label.TextStrokeColor3 = Color3.fromRGB(20, 10, 40)
        label.Rotation = -8
        local gradient = Instance.new("UIGradient")
        gradient.Color = ColorSequence.new(Color3.fromRGB(255, 220, 90), Color3.fromRGB(255, 120, 40))
        gradient.Transparency = NumberSequence.new(0, 0.5)
        gradient.Rotation = 90
        gradient.Offset = Vector2.new(0, 0.25)
        gradient.Parent = label
        assert(gradient:IsA("UIComponent") and not gradient:IsA("GuiObject"))
        print("style", label.Font.Name, label.TextScaled, label.TextStrokeTransparency, label.Rotation, gradient.Rotation, gradient.Offset.Y)
        local tween = game:GetService("TweenService"):Create(label, TweenInfo.new(0.2), { TextTransparency = 1, Rotation = 0 })
        tween:Play()
        tween.Completed:Wait()
        print("faded", label.TextTransparency, label.Rotation)
    )");
    f.start(false);
    f.run(0.5f);
    CHECK_MESSAGE(f.logged("style GothamBlack true 0 -8 90 0.25"), f.dump());
    CHECK_MESSAGE(f.logged("faded 1 0"), f.dump());
    CHECK(f.errors() == 0);
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

TEST_CASE("the Crystal Isles demo plays a whole round") {
    // A copy, so nothing is written into the example; with a short round.
    const fs::path source = Paths::engineData().parent_path() / "examples/CrystalIsles";
    const fs::path root = fs::temp_directory_path() / "rift_crystal_isles";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::copy(source, root, fs::copy_options::recursive, ec);
    REQUIRE_MESSAGE(!ec, ec.message());
    const fs::path scripts = root / "scenes/CrystalIsles/scripts";
    {
        std::ifstream in(scripts / "Config.lua");
        std::string config((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const std::string from = "Config.RoundTime = 120";
        REQUIRE(config.find(from) != std::string::npos);
        config.replace(config.find(from), from.size(), "Config.RoundTime = 6");
        std::ofstream(scripts / "Config.lua", std::ios::trunc) << config;
    }
    // The player presses Play, then walks into gems (teleporting onto them).
    std::ofstream(scripts / "TestPlayer.lua") << R"(
        local player = game.Players.LocalPlayer
        local remotes = game.ReplicatedStorage.Remotes
        local gui = player.PlayerGui
        task.wait(1)
        print("menu", gui.MenuGui.Enabled)
        remotes.StartRound:FireServer()
        remotes.RoundEvent.OnClientEvent:Connect(function(kind, data)
            if kind == "Results" then print("results", data.Score, data.Blue + data.Purple + data.Gold, gui.ResultsGui.Enabled) end
        end)
        while game.ReplicatedStorage.RoundState.Value ~= "Playing" do task.wait(0.1) end
        print("hud", gui.HudGui.Enabled)
        for _ = 1, 3 do
            task.wait(0.4)
            local gem = workspace.ActiveGems:GetChildren()[1]
            if gem then player.Character.HumanoidRootPart.CFrame = CFrame.new(gem:GetPivot().Position) end
        end
    )";

    const fs::path previousRoot = Paths::projectRoot();
    Paths::setProjectRoot(root);
    Scene scene;
    const fs::path file = root / "scenes/CrystalIsles/scene.json";
    const SceneIO::LoadResult loaded = SceneIO::loadScene(scene, file);
    REQUIRE(loaded.ok);
    std::string warnings;
    for (const std::string& w : loaded.warnings) warnings += w + "\n";
    CHECK_MESSAGE(loaded.warnings.empty(), warnings);
    CHECK_FALSE(scene.terrain().empty());
    for (Instance* c : scene.starterPlayer().children) {
        if (c->cls != ClassId::StarterPlayerScripts) continue;
        Instance* s = scene.create(ClassId::LocalScript, c);
        s->name = "TestPlayer";
        s->source = "scripts/TestPlayer.lua";
    }
    ScriptStore store;
    store.reset(Paths::toUtf8(file.parent_path()));
    Input input;
    GameSession::Options options;
    options.dataStoreFolder = root / "datastores";
    {
        GameSession session(scene, store, input, options);
        session.start();
        ViewportInput view;
        view.size = { 1280, 720 };
        for (int i = 0; i < 60 * 14; ++i) {
            input.newFrame();
            session.update(1.0f / 60.0f, view);
        }
        std::string out;
        int errors = 0;
        bool menu = false, hud = false, results = false;
        int score = 0, gems = 0;
        for (const auto& line : session.logLines()) {
            out += line.text + "\n";
            if (line.level == LogLevel::Error) ++errors;
            if (line.text.find("menu true") != std::string::npos) menu = true;
            if (line.text.find("hud true") != std::string::npos) hud = true;
            if (line.text.rfind("results ", 0) == 0) {
                results = line.text.find(" true") != std::string::npos;
                std::sscanf(line.text.c_str(), "results %d %d", &score, &gems);
            }
        }
        CHECK_MESSAGE(errors == 0, out);
        CHECK_MESSAGE(menu, out);
        CHECK_MESSAGE(hud, out);
        CHECK_MESSAGE(results, out);
        CHECK_MESSAGE(gems >= 1, out);
        CHECK_MESSAGE(score >= gems, out);
    }
    Paths::setProjectRoot(previousRoot);
    fs::remove_all(root, ec);
}

TEST_CASE("every Lua example in the docs compiles") {
    const fs::path root = Paths::engineData().parent_path();
    int blocks = 0;
    for (const char* doc : { "docs/SCRIPTING.md", "docs/GUIDE.md", "README.md" }) {
        std::ifstream in(root / doc);
        REQUIRE_MESSAGE(in.is_open(), doc);
        std::string line, code;
        bool inLua = false;
        int startLine = 0, lineNo = 0;
        while (std::getline(in, line)) {
            ++lineNo;
            const size_t fence = line.find("```");
            if (fence != std::string::npos) {
                if (!inLua && line.compare(fence, 6, "```lua") == 0) { inLua = true; code.clear(); startLine = lineNo; continue; }
                if (inLua) {
                    inLua = false;
                    ++blocks;
                    const std::string error = LuaRuntime::checkSyntax(code, std::string(doc) + ":" + std::to_string(startLine));
                    CHECK_MESSAGE(error.empty(), error);
                    continue;
                }
            }
            if (inLua) code += line + "\n";
        }
    }
    CHECK(blocks > 15);
}

TEST_CASE("every example scene loads and runs without script errors") {
    const fs::path examples = Paths::engineData().parent_path() / "examples";
    int scenes = 0;
    for (const auto& entry : fs::recursive_directory_iterator(examples)) {
        if (entry.path().filename() != "scene.json") continue;
        ++scenes;
        INFO(entry.path().string());
        Scene scene;
        const SceneIO::LoadResult r = SceneIO::loadScene(scene, entry.path());
        REQUIRE_MESSAGE(r.ok, r.error);
        ScriptStore store;
        store.reset(Paths::toUtf8(entry.path().parent_path()));
        Input input;
        // Assets resolve in the example's own project (examples/<name>/scenes/<scene>/scene.json).
        const fs::path previousRoot = Paths::projectRoot();
        Paths::setProjectRoot(entry.path().parent_path().parent_path().parent_path());
        GameSession::Options options;
        options.dataStoreFolder = fs::temp_directory_path() / "rift_example_datastores";
        {
            GameSession session(scene, store, input, options);
            session.start();
            for (int i = 0; i < 60; ++i) {
                input.newFrame();
                session.update(1.0f / 60.0f, ViewportInput{});
            }
            for (const auto& line : session.logLines()) CHECK_MESSAGE(line.level != LogLevel::Error, line.text);
        }
        Paths::setProjectRoot(previousRoot);
    }
    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "rift_example_datastores", ec);
    CHECK(scenes >= 2);
}

TEST_CASE("stopping after touches, deaths and destroyed objects is clean") {
    // Plays DemoObby, puts the character on every scripted part (coin, kill
    // brick, jump pad, finish), lets the handlers run, then tears the session
    // down the way Stop does. Run several times to catch order-dependent bugs.
    const fs::path file = Paths::engineData().parent_path() / "examples/DemoObby/scenes/DemoObby/scene.json";
    Scene scene;
    REQUIRE(SceneIO::loadScene(scene, file).ok);
    ScriptStore store;
    store.reset(Paths::toUtf8(file.parent_path()));
    for (int round = 0; round < 3; ++round) {
        Input input;
        auto session = std::make_unique<GameSession>(scene, store, input, GameSession::Options{});
        session->start();
        ViewportInput view;
        auto step = [&](int frames) {
            for (int i = 0; i < frames; ++i) {
                input.newFrame();
                session->update(1.0f / 60.0f, view);
            }
        };
        step(30);
        for (const char* target : { "Coin", "JumpPad", "KillBrick", "Finish" }) {
            Instance* part = session->scene().workspace().findFirstChild(target);
            Instance* root = session->rootPart();
            if (!part || !root) { step(200); continue; } // dead: wait for the respawn
            root->setWorldFrame(part->worldPosition() + glm::vec3(0.0f, 1.5f, 0.0f), glm::vec3(0.0f));
            if (const PropertyInfo* p = findProperty(root->cls, "Position")) session->scene().notifyPropertyChanged(*root, *p);
            step(20 + round * 15);
        }
        // Stop at different moments: mid-respawn, mid-tween, right after a touch.
        step(round * 7);
        session.reset();
    }
    CHECK(scene.workspace().findFirstChild("Coin")); // the edited scene is untouched
}

TEST_CASE("the client gets the server's world, the server never sees the client's") {
    Fixture f;
    Instance* secret = f.scene.create(ClassId::Folder, f.scene.service(ClassId::ServerStorage));
    secret->name = "Secret";
    f.script(R"(
        local p = Instance.new("Part")
        p.Name = "Built"
        p.Anchored = true
        p.Parent = workspace
        task.wait(0.2)
        p.Color = Color3.fromRGB(0, 255, 0)
        print("server sees client part:", workspace:FindFirstChild("ClientOnly") ~= nil)
    )");
    f.localScript(R"(
        local built = workspace:WaitForChild("Built")
        print("client sees", built.Name, #game:GetService("ServerStorage"):GetChildren())
        local mine = Instance.new("Part")
        mine.Name = "ClientOnly"
        mine.Parent = workspace
        built:GetPropertyChangedSignal("Color"):Wait()
        print("client color", built.Color.G == 1)
    )");
    GameSession& s = f.start(false);
    f.run(0.5f);
    CHECK_MESSAGE(f.logged("client sees Built 0"), f.dump());
    CHECK_MESSAGE(f.logged("client color true"), f.dump());
    CHECK_MESSAGE(f.logged("server sees client part: false"), f.dump());
    CHECK(s.scene(NetworkSide::Client).workspace().findFirstChild("ClientOnly"));
    CHECK_FALSE(s.scene(NetworkSide::Server).workspace().findFirstChild("ClientOnly"));
    CHECK(f.errors() == 0);
}

TEST_CASE("RemoteEvent and RemoteFunction carry values both ways") {
    Fixture f;
    Instance* ping = f.scene.create(ClassId::RemoteEvent, f.scene.service(ClassId::ReplicatedStorage));
    ping->name = "Ping";
    Instance* ask = f.scene.create(ClassId::RemoteFunction, f.scene.service(ClassId::ReplicatedStorage));
    ask->name = "Ask";
    f.script(R"(
        local rs = game:GetService("ReplicatedStorage")
        rs.Ping.OnServerEvent:Connect(function(player, n, t)
            print("server got", player:IsA("Player"), n, t.x, t.list[2], t.where == workspace)
            rs.Ping:FireClient(player, "pong", Vector3.new(1, 2, 3))
        end)
        rs.Ask.OnServerInvoke = function(player, n)
            task.wait(0.1) -- handlers may yield
            return n * 2, "ok"
        end
    )");
    f.localScript(R"(
        local rs = game:GetService("ReplicatedStorage")
        local ping = rs:WaitForChild("Ping")
        ping.OnClientEvent:Connect(function(msg, v) print("client got", msg, v.Y) end)
        ping:FireServer(5, { x = "hi", list = { 1, 2 }, where = workspace })
        local a, b = rs.Ask:InvokeServer(21)
        print("invoke", a, b)
        local ok, err = pcall(function() ping:FireClient(game.Players.LocalPlayer) end)
        print("client FireClient fails:", not ok)
    )");
    f.start(false);
    f.run(0.5f);
    CHECK_MESSAGE(f.logged("server got true 5 hi 2 true"), f.dump());
    CHECK_MESSAGE(f.logged("client got pong 2"), f.dump());
    CHECK_MESSAGE(f.logged("invoke 42 ok"), f.dump());
    CHECK_MESSAGE(f.logged("client FireClient fails: true"), f.dump());
    CHECK(f.errors() == 0);
}

TEST_CASE("client-only services fail clearly in a server Script") {
    Fixture f;
    f.script(R"(
        local ok, err = pcall(function() return game:GetService("RunService"):BindToRenderStep("x", 0, function() end) end)
        print("render step:", ok, err)
    )");
    f.localScript(R"(
        local ok = pcall(function() return game:GetService("DataStoreService") end)
        print("datastore on client:", ok)
    )");
    f.start(false);
    f.run(0.1f);
    CHECK_MESSAGE(f.logged("LocalScript"), f.dump());
    CHECK_MESSAGE(f.logged("datastore on client: false"), f.dump());
}

TEST_CASE("LocalScripts in StarterCharacterScripts run in each new character") {
    Fixture f;
    Instance* folder = nullptr;
    for (Instance* c : f.scene.starterPlayer().children) if (c->cls == ClassId::StarterCharacterScripts) folder = c;
    REQUIRE(folder);
    f.script("print('character script in', script.Parent.Name)", folder, ClassId::LocalScript);
    GameSession& s = f.start(true);
    f.run(0.2f);
    CHECK_MESSAGE(f.logged("character script in"), f.dump());
    // The character carries a copy, the template stays in StarterPlayer.
    Instance* character = s.scene(NetworkSide::Client).find(s.characterId());
    REQUIRE(character);
    bool carried = false;
    for (const Instance* c : character->children) carried |= c->cls == ClassId::LocalScript;
    CHECK(carried);
}
