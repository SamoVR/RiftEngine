// Several players in one session (Studio's local server test) and tools.
#include "doctest.h"
#include "game/GameSession.h"
#include "script/LuaRuntime.h"
#include "script/ScriptStore.h"
#include "physics/PhysicsWorld.h"
#include "platform/Input.h"
#include "core/Paths.h"
#include "editor/Templates.h"

#include <GLFW/glfw3.h>
#include <filesystem>

using namespace rift;
namespace fs = std::filesystem;

namespace {

struct Game {
    Scene scene;
    ScriptStore store;
    Input input;
    fs::path folder;
    std::unique_ptr<GameSession> session;
    int scriptCount = 0;

    Game() {
        folder = fs::temp_directory_path() / ("rift_players_test_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::remove_all(folder);
        fs::create_directories(folder);
        store.reset(Paths::toUtf8(folder));
        Instance* floor = scene.create(ClassId::Part, &scene.workspace());
        floor->name = "Floor";
        floor->size = { 100, 1, 100 };
        floor->position = { 0, -0.5f, 0 };
    }
    ~Game() {
        session.reset();
        std::error_code ec;
        fs::remove_all(folder, ec);
    }

    Instance* script(const std::string& source, Instance* parent, ClassId cls = ClassId::Script) {
        const std::string path = "scripts/S" + std::to_string(++scriptCount) + ".lua";
        store.create(path, source);
        Instance* s = scene.create(cls, parent);
        s->source = path;
        return s;
    }
    Instance* starterPlayerScripts() {
        for (Instance* c : scene.starterPlayer().children) if (c->cls == ClassId::StarterPlayerScripts) return c;
        return nullptr;
    }
    // A tool in StarterPack: a Handle and a script per side that report its events.
    Instance* tool(const std::string& name) {
        Instance* t = scene.create(ClassId::Tool, scene.service(ClassId::StarterPack));
        t->name = name;
        Instance* handle = scene.create(ClassId::Part, t);
        handle->name = "Handle";
        handle->size = { 0.3f, 0.3f, 1.0f };
        handle->anchored = false;
        return t;
    }
    GameSession& start(int players, bool characters = true) {
        GameSession::Options options;
        options.playerCount = players;
        options.spawnCharacter = characters;
        session = std::make_unique<GameSession>(scene, store, input, options);
        session->start();
        return *session;
    }
    void run(float seconds) {
        ViewportInput view;
        for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) {
            input.newFrame();
            session->update(1.0f / 60.0f, view);
        }
    }
    // A key or mouse button pressed and released (pressed during a frame, like GLFW's callbacks).
    void tapKey(int key) {
        ViewportInput view;
        input.newFrame();
        input.setKey(key, true);
        session->update(1.0f / 60.0f, view);
        input.newFrame();
        input.setKey(key, false);
        session->update(1.0f / 60.0f, view);
    }
    void click() {
        ViewportInput view;
        view.mouse = { 640.0f, 300.0f };
        input.newFrame();
        input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, true);
        session->update(1.0f / 60.0f, view);
        input.newFrame();
        input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, false);
        session->update(1.0f / 60.0f, view);
    }
    int count(const std::string& text) const {
        int n = 0;
        for (const auto& line : session->logLines()) if (line.text.find(text) != std::string::npos) ++n;
        return n;
    }
    std::string dump() const {
        std::string out;
        for (const auto& line : session->logLines()) out += line.text + "\n";
        return out;
    }
    int errors() const {
        int n = 0;
        for (const auto& line : session->logLines()) if (line.level == LogLevel::Error) ++n;
        return n;
    }
};

} // namespace

TEST_CASE("several test players: each has a client, a character and its own LocalPlayer") {
    Game g;
    g.script(R"(
        game.Players.PlayerAdded:Connect(function(p) print("joined", p.Name) end)
    )", g.scene.service(ClassId::ScriptService));
    g.script(R"(
        local me = game.Players.LocalPlayer
        print("I am", me.Name, "of", #game.Players:GetPlayers())
    )", g.starterPlayerScripts(), ClassId::LocalScript);
    GameSession& s = g.start(3);
    g.run(1.2f); // they land
    REQUIRE(s.clientCount() == 3);
    CHECK_MESSAGE(g.count("joined Player1") == 1, g.dump());
    CHECK(g.count("joined Player3") == 1);
    CHECK_MESSAGE(g.count("I am Player1 of 3") == 1, g.dump());
    CHECK(g.count("I am Player2 of 3") == 1);
    CHECK(g.count("I am Player3 of 3") == 1);
    for (int k = 0; k < 3; ++k) {
        CHECK(s.client(k).characterId != 0);
        // Every client sees every character.
        for (int j = 0; j < 3; ++j) CHECK(s.client(j).scene->find(s.client(k).characterId));
    }
    // Only the player in focus moves.
    const glm::vec3 before0 = s.scene().find(s.client(0).rootPartId)->worldPosition();
    const glm::vec3 before1 = s.scene().find(s.client(1).rootPartId)->worldPosition();
    s.setFocus(1);
    g.input.setKey(GLFW_KEY_W, true);
    g.run(0.6f);
    g.input.setKey(GLFW_KEY_W, false);
    CHECK(glm::length(s.scene().find(s.client(1).rootPartId)->worldPosition() - before1) > 2.0f);
    CHECK(glm::length(s.scene().find(s.client(0).rootPartId)->worldPosition() - before0) < 0.5f);
    CHECK(g.errors() == 0);
}

TEST_CASE("remotes reach the right player") {
    Game g;
    Instance* remote = g.scene.create(ClassId::RemoteEvent, g.scene.service(ClassId::ReplicatedStorage));
    remote->name = "Ping";
    g.script(R"(
        local ping = game.ReplicatedStorage.Ping
        ping.OnServerEvent:Connect(function(player, text)
            print("server got", text, "from", player.Name)
            ping:FireClient(player, "just you")
            ping:FireAllClients("everyone")
        end)
    )", g.scene.service(ClassId::ScriptService));
    g.script(R"(
        local me = game.Players.LocalPlayer
        local ping = game.ReplicatedStorage:WaitForChild("Ping")
        ping.OnClientEvent:Connect(function(text) print(me.Name, "heard", text) end)
        if me.Name == "Player2" then ping:FireServer("hello") end
    )", g.starterPlayerScripts(), ClassId::LocalScript);
    g.start(2, false);
    g.run(0.3f);
    CHECK_MESSAGE(g.count("server got hello from Player2") == 1, g.dump());
    CHECK(g.count("Player2 heard just you") == 1);
    CHECK(g.count("Player1 heard just you") == 0);
    CHECK(g.count("Player1 heard everyone") == 1);
    CHECK(g.count("Player2 heard everyone") == 1);
    CHECK(g.errors() == 0);
}

TEST_CASE("tools: StarterPack fills the Backpack, equip holds it, activation reaches both sides") {
    Game g;
    Instance* t = g.tool("Wand");
    g.script(R"(
        local tool = script.Parent
        tool.Equipped:Connect(function() print("server equipped") end)
        tool.Activated:Connect(function() print("server activated") end)
        tool.Unequipped:Connect(function() print("server unequipped") end)
    )", t);
    g.script(R"(
        local tool = script.Parent
        tool.Equipped:Connect(function(mouse) print("client equipped", mouse ~= nil) end)
        tool.Activated:Connect(function() print("client activated") end)
    )", t, ClassId::LocalScript);
    GameSession& s = g.start(1);
    g.run(0.1f);
    GameSession::Client& c = s.client(0);
    Instance* backpack = s.scene().find(c.backpackId);
    REQUIRE(backpack);
    REQUIRE(backpack->children.size() == 1);
    const uint64_t toolId = backpack->children[0]->id;
    CHECK(s.hotbar(0).size() == 1);

    // Key 1 equips it (the Backpack bar).
    g.tapKey(GLFW_KEY_1);
    g.run(0.1f);
    Instance* tool = s.scene().find(toolId);
    REQUIRE(tool);
    CHECK(tool->parent->id == c.characterId);
    CHECK(s.hotbar(0)[0].equipped);
    // The Handle is in the right hand.
    Instance* arm = s.scene().find(c.characterId)->findFirstChild("Right Arm");
    Instance* handle = tool->findFirstChild("Handle");
    CHECK(glm::length(handle->worldPosition() - arm->worldPosition()) < 1.5f);
    CHECK_MESSAGE(g.count("server equipped") == 1, g.dump());
    CHECK(g.count("client equipped true") == 1);

    // A click activates it on both sides.
    g.click();
    g.run(0.1f);
    CHECK_MESSAGE(g.count("server activated") == 1, g.dump());
    CHECK(g.count("client activated") == 1);

    // Unequip: back in the Backpack.
    s.unequipTools(0);
    g.run(0.05f);
    CHECK(tool->parent->id == c.backpackId);
    CHECK(g.count("server unequipped") == 1);

    // Drop it, then walk onto it: picked up again.
    s.equipTool(0, toolId);
    s.dropTool(0);
    g.run(0.5f);
    CHECK(tool->parent == &s.scene().workspace());
    g.run(2.0f);
    Instance* root = s.scene().find(c.rootPartId);
    root->setWorldFrame(handle->worldPosition() + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f));
    if (const PropertyInfo* p = findProperty(root->cls, "Position")) s.scene().notifyPropertyChanged(*root, *p);
    g.run(0.3f);
    CHECK(tool->parent->id == c.characterId);
    CHECK(g.errors() == 0);
}

TEST_CASE("a fresh Backpack every spawn, and scripts can equip") {
    Game g;
    g.tool("Pick");
    g.script(R"(
        local Players = game:GetService("Players")
        Players.PlayerAdded:Connect(function(player)
            player.CharacterAdded:Connect(function(character)
                local tool = player.Backpack:WaitForChild("Pick")
                character:WaitForChild("Humanoid"):EquipTool(tool)
                print("holding", tool.Parent == character)
            end)
        end)
    )", g.scene.service(ClassId::ScriptService));
    GameSession& s = g.start(1);
    g.run(0.2f);
    s.loadCharacter(s.client(0).playerId); // respawn: CharacterAdded again, a new Pick
    g.run(0.2f);
    CHECK_MESSAGE(g.count("holding true") == 1, g.dump()); // the first spawn happened before scripts connected
    Instance* backpack = s.scene().find(s.client(0).backpackId);
    CHECK(backpack->children.empty()); // the new Pick is in the hand
    CHECK(g.errors() == 0);
}

TEST_CASE("the Sword template hurts another player it hits") {
    Game g;
    const PartTemplate* sword = findTemplate("Sword");
    REQUIRE(sword);
    CHECK(LuaRuntime::checkSyntax(sword->scriptSource, "SwordScript").empty());
    Instance* tool = g.scene.create(ClassId::Tool, g.scene.service(ClassId::StarterPack));
    tool->name = "Sword";
    tool->gripPos = sword->gripPos;
    Instance* handle = g.scene.create(ClassId::Part, tool);
    handle->name = "Handle";
    handle->size = sword->size;
    handle->anchored = false;
    sword->build(g.scene, *handle);
    g.script(sword->scriptSource, tool);
    GameSession& s = g.start(2);
    g.run(1.0f);
    s.equipTool(0, s.scene().find(s.client(0).backpackId)->children[0]->id);
    // Player2 stands right in front of Player1's sword arm.
    Instance* attacker = s.scene().find(s.client(0).rootPartId);
    Instance* victim = s.scene().find(s.client(1).rootPartId);
    victim->setWorldFrame(attacker->worldPosition() + glm::vec3(0.66f, 0.0f, -1.4f), glm::vec3(0.0f));
    if (const PropertyInfo* p = findProperty(victim->cls, "Position")) s.scene().notifyPropertyChanged(*victim, *p);
    g.run(0.2f);
    g.click();
    g.run(0.5f);
    Instance* hum = s.scene().find(s.client(1).humanoidId);
    REQUIRE(hum);
    CHECK_MESSAGE(hum->health < 100.0f, g.dump());
    CHECK(s.scene().find(s.client(0).humanoidId)->health == doctest::Approx(100.0f)); // not itself
    CHECK(g.errors() == 0);
}
