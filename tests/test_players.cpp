// Several players in one session (Studio's local server test) and tools.
#include "doctest.h"
#include "game/GameSession.h"
#include "script/LuaRuntime.h"
#include "script/ScriptStore.h"
#include "physics/PhysicsWorld.h"
#include "platform/Input.h"
#include <algorithm>
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

TEST_CASE("the backpack: ` opens the inventory, search, drag tools between it and the bar") {
    Game g;
    for (int k = 1; k <= 10; ++k) g.tool("Tool" + std::to_string(k));
    g.tool("Sword");
    g.tool("Shield");
    GameSession& s = g.start(1);
    g.run(0.1f);
    GameSession::Client& c = s.client(0);
    // Ten fit on the bar; all twelve are in the inventory.
    CHECK(s.hotbar(0).size() == 10);
    CHECK(c.inventory.size() == 12);
    const uint64_t sword = c.inventory[10];
    CHECK(std::find(c.hotbarTools.begin(), c.hotbarTools.end(), sword) == c.hotbarTools.end());
    CHECK_FALSE(s.inventory(0).open);
    CHECK(s.inventory(0).button);

    g.tapKey(GLFW_KEY_GRAVE_ACCENT);
    GameSession::InventoryView inv = s.inventory(0);
    REQUIRE(inv.open);
    CHECK(inv.cells.size() == 12);
    CHECK(s.hotbar(0).size() == 10);

    // Frames with the mouse at a place, the left button down or up.
    ViewportInput view;
    auto frame = [&](glm::vec2 mouse, bool down) {
        view.mouse = mouse;
        g.input.newFrame();
        g.input.setMouseButton(GLFW_MOUSE_BUTTON_LEFT, down);
        s.update(1.0f / 60.0f, view);
    };
    auto center = [](glm::vec2 pos, glm::vec2 size) { return pos + size * 0.5f; };
    auto drag = [&](glm::vec2 from, glm::vec2 to) {
        frame(from, true);
        frame((from + to) * 0.5f, true);
        frame(to, true);
        frame(to, false);
    };

    // Typing in the search box filters the tools, and number keys type instead of equipping.
    frame(center(inv.searchPos, inv.searchSize), true);
    frame(center(inv.searchPos, inv.searchSize), false);
    CHECK(c.searchFocused);
    g.input.newFrame();
    for (char ch : std::string("sw")) g.input.addText(static_cast<unsigned char>(ch));
    g.input.setKey(GLFW_KEY_1, true);
    s.update(1.0f / 60.0f, view);
    g.input.newFrame();
    g.input.setKey(GLFW_KEY_1, false);
    s.update(1.0f / 60.0f, view);
    CHECK(c.inventorySearch == "sw");
    CHECK(c.equipped == 0);
    inv = s.inventory(0);
    REQUIRE(inv.cells.size() == 1);
    CHECK(inv.cells[0].tool == sword);

    // Drag the Sword onto slot 3: Tool3 leaves the bar (still in the inventory).
    const uint64_t tool3 = c.hotbarTools[2];
    std::vector<GameSession::HotbarSlot> bar = s.hotbar(0);
    drag(center(inv.cells[0].pos, inv.cells[0].size), center(bar[2].pos, bar[2].size));
    CHECK(c.hotbarTools[2] == sword);
    CHECK(std::find(c.hotbarTools.begin(), c.hotbarTools.end(), tool3) == c.hotbarTools.end());
    CHECK(c.inventory.size() == 12);
    CHECK_FALSE(c.searchFocused); // clicked away from the box

    // Slots swap when dragged onto each other.
    const uint64_t tool1 = c.hotbarTools[0], tool6 = c.hotbarTools[5];
    bar = s.hotbar(0);
    drag(center(bar[0].pos, bar[0].size), center(bar[5].pos, bar[5].size));
    CHECK(c.hotbarTools[0] == tool6);
    CHECK(c.hotbarTools[5] == tool1);

    // Dragged into the inventory: off the bar, so key 4 does nothing.
    const uint64_t tool4 = c.hotbarTools[3];
    bar = s.hotbar(0);
    inv = s.inventory(0);
    drag(center(bar[3].pos, bar[3].size), center(inv.gridPos, inv.gridSize));
    CHECK(c.hotbarTools[3] == 0);
    CHECK(std::find(c.inventory.begin(), c.inventory.end(), tool4) != c.inventory.end());

    // A click (no drag) on a tool in the inventory equips it.
    inv = s.inventory(0);
    REQUIRE_FALSE(inv.cells.empty());
    frame(center(inv.cells[0].pos, inv.cells[0].size), true);
    frame(center(inv.cells[0].pos, inv.cells[0].size), false);
    CHECK(c.equipped == inv.cells[0].tool);

    // Closed again: only the filled slots show.
    g.tapKey(GLFW_KEY_GRAVE_ACCENT);
    CHECK_FALSE(s.inventory(0).open);
    CHECK(s.hotbar(0).size() == 9);
    g.tapKey(GLFW_KEY_4);
    CHECK(c.equipped == inv.cells[0].tool);
    CHECK(g.errors() == 0);
}

TEST_CASE("chat: / opens the box, Enter says it to everyone, Player.Chatted and TextChatService hear it") {
    Game g;
    g.script(R"(
        local Players = game:GetService("Players")
        local function watch(p) p.Chatted:Connect(function(m) print("server heard", p.Name, m) end) end
        Players.PlayerAdded:Connect(watch)
        task.wait(0.6)
        game:GetService("TextChatService").TextChannels.RBXGeneral:DisplaySystemMessage("Welcome!")
    )", g.scene.service(ClassId::ScriptService));
    g.script(R"(
        local TextChatService = game:GetService("TextChatService")
        local me = game.Players.LocalPlayer
        TextChatService.MessageReceived:Connect(function(m)
            print(me.Name, "got", m.Text, m.TextSource and m.TextSource.Name or "system")
        end)
        TextChatService.SendingMessage:Connect(function(m) print(me.Name, "sending", m.Text) end)
        if me.Name == "Player2" then
            task.wait(0.5)
            TextChatService:WaitForChild("TextChannels"):WaitForChild("RBXGeneral"):SendAsync("from code")
        end
    )", g.starterPlayerScripts(), ClassId::LocalScript);
    GameSession& s = g.start(2);
    g.run(0.1f);

    // Type "/" (any layout: the character counts), then the message, then Enter.
    ViewportInput view;
    auto type = [&](const std::string& text, int key = 0) {
        g.input.newFrame();
        for (char ch : text) g.input.addText(static_cast<unsigned char>(ch));
        if (key) g.input.setKey(key, true);
        s.update(1.0f / 60.0f, view);
        g.input.newFrame();
        if (key) g.input.setKey(key, false);
        s.update(1.0f / 60.0f, view);
    };
    type("/");
    CHECK(s.client(0).chatOpen);
    CHECK(s.typing());
    CHECK(s.client(0).chatDraft.empty()); // the / only opens it
    type("hi there");
    CHECK(s.client(0).chatDraft == "hi there");
    type("", GLFW_KEY_ENTER);
    CHECK_FALSE(s.client(0).chatOpen);
    g.run(0.1f);
    CHECK_MESSAGE(g.count("server heard Player1 hi there") == 1, g.dump());
    CHECK(g.count("Player1 sending hi there") == 1);
    CHECK(g.count("Player1 got hi there Player1") == 1);
    CHECK(g.count("Player2 got hi there Player1") == 1);
    REQUIRE(s.client(1).chat.size() == 1);
    CHECK(s.client(1).chat[0].speaker == "Player1");
    // A bubble over Player1's head.
    REQUIRE(s.bubbles().size() == 1);
    CHECK(s.scene().find(s.bubbles()[0].part)->name == "Head");

    // Esc closes the box without saying anything; blank messages say nothing.
    type("/");
    type("never mind");
    type("", GLFW_KEY_ESCAPE);
    CHECK_FALSE(s.client(0).chatOpen);
    type("/");
    type("   ", GLFW_KEY_ENTER);

    // From code, and a system line from the server.
    g.run(0.6f);
    CHECK_MESSAGE(g.count("server heard Player2 from code") == 1, g.dump());
    CHECK(g.count("Player1 got from code Player2") == 1);
    CHECK(g.count("Player1 got Welcome! system") == 1);
    CHECK(g.count("Player2 got Welcome! system") == 1);
    CHECK(g.count("never mind") == 0);
    CHECK(s.client(0).chat.size() == 3);
    // Bubbles pop after a while.
    g.run(16.0f);
    CHECK(s.bubbles().empty());
    CHECK(g.errors() == 0);
}

TEST_CASE("seats: touching one sits the character down, Space gets it up, Seat:Sit from a script") {
    Game g;
    Instance* seat = g.scene.create(ClassId::Seat, &g.scene.workspace());
    seat->name = "Chair";
    seat->size = { 2.0f, 0.4f, 2.0f };
    seat->position = { 0.0f, 0.2f, 0.0f }; // where the character spawns: it drops onto it
    Instance* bench = g.scene.create(ClassId::Seat, &g.scene.workspace());
    bench->name = "Bench";
    bench->size = { 2.0f, 0.4f, 2.0f };
    bench->position = { 10.0f, 0.2f, 0.0f };
    g.script(R"(
        local Players = game:GetService("Players")
        Players.PlayerAdded:Connect(function(player)
            local character = player.Character or player.CharacterAdded:Wait()
            character:WaitForChild("Humanoid").Seated:Connect(function(active, seat)
                print("seated", active, seat and seat.Name or "none")
            end)
        end)
        task.wait(2.5)
        workspace.Bench:Sit(game.Players:GetPlayers()[1].Character.Humanoid)
    )", g.scene.service(ClassId::ScriptService));
    GameSession& s = g.start(1);
    g.run(1.5f);
    GameSession::Client& c = s.client(0);
    Instance* hum = s.scene().find(c.humanoidId);
    Instance* chair = s.scene().find(seat->id);
    REQUIRE(hum);
    REQUIRE(chair);
    CHECK_MESSAGE(hum->sit, g.dump());
    CHECK(chair->occupant.id == hum->id);
    CHECK(hum->seatPart.id == chair->id);
    CHECK(c.state == HumanoidState::Seated);
    CHECK(g.count("seated true Chair") == 1);
    // Sitting on it, not standing on it: the root is just above the seat's top.
    Instance* root = s.scene().find(c.rootPartId);
    CHECK(root->worldPosition().y == doctest::Approx(0.4f + c.seatHip + 0.05f).epsilon(0.05));

    // Space: up and off.
    g.tapKey(GLFW_KEY_SPACE);
    g.run(0.2f);
    CHECK_FALSE(hum->sit);
    CHECK(chair->occupant.id == 0);
    CHECK(g.count("seated false none") == 1);

    // A script sits it on the bench.
    g.run(1.0f);
    CHECK_MESSAGE(s.scene().find(bench->id)->occupant.id == hum->id, g.dump());
    CHECK(g.count("seated true Bench") == 1);
    CHECK(g.errors() == 0);
}

TEST_CASE("proximity prompts: shown in reach, E triggers it, holding fills it up") {
    Game g;
    Instance* door = g.scene.create(ClassId::Part, &g.scene.workspace());
    door->name = "Door";
    door->position = { 0.0f, 1.0f, -4.0f };
    door->size = { 2.0f, 2.0f, 0.4f };
    Instance* prompt = g.scene.create(ClassId::ProximityPrompt, door);
    prompt->actionText = "Open";
    prompt->objectText = "Door";
    g.script(R"(
        local prompt = workspace.Door.ProximityPrompt
        prompt.Triggered:Connect(function(player) print("triggered by", player.Name) end)
        prompt.TriggerEnded:Connect(function(player) print("trigger ended") end)
        prompt.PromptButtonHoldBegan:Connect(function(player) print("hold began") end)
        prompt.PromptButtonHoldEnded:Connect(function(player) print("hold ended") end)
    )", g.scene.service(ClassId::ScriptService));
    g.script(R"(
        local prompt = workspace:WaitForChild("Door"):WaitForChild("ProximityPrompt")
        prompt.PromptShown:Connect(function() print("client shown") end)
        prompt.Triggered:Connect(function(player) print("client triggered", player.Name) end)
    )", g.starterPlayerScripts(), ClassId::LocalScript);
    GameSession& s = g.start(1);
    g.run(1.2f);
    const GameSession::PromptView v = s.promptView(0);
    REQUIRE_MESSAGE(v.visible, g.dump());
    CHECK(v.key == "E");
    CHECK(v.action == "Open");
    CHECK(v.object == "Door");
    CHECK(g.count("client shown") == 1);

    g.tapKey(GLFW_KEY_E);
    g.run(0.05f);
    CHECK_MESSAGE(g.count("triggered by Player") == 1, g.dump());
    CHECK(g.count("client triggered Player") == 1);
    CHECK(g.count("trigger ended") == 1);

    // Hold for half a second; letting go early does nothing.
    Instance* live = s.scene().find(prompt->id);
    live->holdDuration = 0.5f;
    live->keyboardKeyCode = "F";
    ViewportInput view;
    g.input.newFrame();
    g.input.setKey(GLFW_KEY_F, true);
    s.update(1.0f / 60.0f, view);
    g.run(0.2f);
    CHECK(s.promptView(0).progress > 0.2f);
    g.input.newFrame();
    g.input.setKey(GLFW_KEY_F, false);
    s.update(1.0f / 60.0f, view);
    CHECK(g.count("hold began") == 1);
    CHECK(g.count("hold ended") == 1);
    CHECK(g.count("triggered by Player") == 1);
    g.input.newFrame();
    g.input.setKey(GLFW_KEY_F, true);
    s.update(1.0f / 60.0f, view);
    g.run(0.6f);
    CHECK(g.count("triggered by Player") == 2);
    g.input.newFrame();
    g.input.setKey(GLFW_KEY_F, false);
    s.update(1.0f / 60.0f, view);

    // Out of reach: hidden.
    live->maxActivationDistance = 1.0f;
    g.run(0.05f);
    CHECK_FALSE(s.promptView(0).visible);
    CHECK(g.errors() == 0);
}
