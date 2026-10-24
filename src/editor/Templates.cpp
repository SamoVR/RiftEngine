#include "editor/Templates.h"

namespace rift {

const char* newScriptSource() {
    return
        "-- This script runs when the game starts (Play / F5).\n"
        "-- script.Parent is the object this script is inside.\n"
        "\n"
        "print(\"Hello from \" .. script:GetFullName())\n";
}

const char* newLocalScriptSource() {
    return
        "-- This LocalScript runs on the player's computer (the client): input,\n"
        "-- the camera and the GUI. Talk to Scripts on the server with RemoteEvents.\n"
        "local Players = game:GetService(\"Players\")\n"
        "local player = Players.LocalPlayer\n"
        "\n"
        "print(\"Hello from the client, \" .. player.Name)\n";
}

const char* newModuleSource() {
    return
        "-- A ModuleScript returns a value (usually a table of functions).\n"
        "-- Load it from another script with:  local Module = require(path.to.ThisModule)\n"
        "\n"
        "local Module = {}\n"
        "\n"
        "function Module.greet(name)\n"
        "    return \"Hello, \" .. name\n"
        "end\n"
        "\n"
        "return Module\n";
}

const std::vector<PartTemplate>& partTemplates() {
    static const std::vector<PartTemplate> list = [] {
        std::vector<PartTemplate> t;

        PartTemplate kill;
        kill.id = "KillBrick";
        kill.title = "Kill Brick";
        kill.description = "Touching it kills the player.";
        kill.size = { 6, 0.6f, 6 };
        kill.color = { 1.0f, 0.18f, 0.12f };
        kill.material = Material::Neon;
        kill.scriptName = "KillScript";
        kill.scriptSource =
            "-- Kills any character that touches this part.\n"
            "local brick = script.Parent\n"
            "\n"
            "brick.Touched:Connect(function(hit)\n"
            "    local humanoid = hit.Parent:FindFirstChildOfClass(\"Humanoid\")\n"
            "    if humanoid then\n"
            "        humanoid.Health = 0\n"
            "    end\n"
            "end)\n";
        t.push_back(kill);

        PartTemplate spinner;
        spinner.id = "Spinner";
        spinner.title = "Spinner";
        spinner.description = "A bar that keeps rotating and knocks players off.";
        spinner.size = { 12, 0.8f, 1 };
        spinner.color = { 0.98f, 0.62f, 0.15f };
        spinner.material = Material::SmoothPlastic;
        spinner.scriptName = "SpinScript";
        spinner.scriptSource =
            "-- Spins the part around its vertical axis.\n"
            "local part = script.Parent\n"
            "local degreesPerSecond = 90\n"
            "\n"
            "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n"
            "    part.CFrame = part.CFrame * CFrame.Angles(0, math.rad(degreesPerSecond * dt), 0)\n"
            "end)\n";
        t.push_back(spinner);

        PartTemplate coin;
        coin.id = "Coin";
        coin.title = "Coin";
        coin.description = "Spins, adds 1 to the player's Coins on the leaderboard when collected.";
        coin.shape = PartShape::Cylinder;
        coin.size = { 1.6f, 0.25f, 1.6f };
        coin.color = { 1.0f, 0.82f, 0.18f };
        coin.material = Material::Metal;
        coin.canCollide = false;
        coin.scriptName = "CoinScript";
        coin.scriptSource =
            "-- A coin: spins, and is collected by touching it.\n"
            "local coin = script.Parent\n"
            "local Players = game:GetService(\"Players\")\n"
            "local collected = false\n"
            "\n"
            "-- Stand the coin up and spin it.\n"
            "coin.CFrame = coin.CFrame * CFrame.Angles(math.rad(90), 0, 0)\n"
            "game:GetService(\"RunService\").Heartbeat:Connect(function(dt)\n"
            "    if not collected then\n"
            "        coin.CFrame = coin.CFrame * CFrame.Angles(0, 0, math.rad(120 * dt))\n"
            "    end\n"
            "end)\n"
            "\n"
            "coin.Touched:Connect(function(hit)\n"
            "    if collected then return end\n"
            "    local player = Players:GetPlayerFromCharacter(hit.Parent)\n"
            "    if not player then return end\n"
            "    collected = true\n"
            "\n"
            "    -- leaderstats: a folder of values shown on the leaderboard.\n"
            "    local stats = player:FindFirstChild(\"leaderstats\")\n"
            "    if not stats then\n"
            "        stats = Instance.new(\"Folder\")\n"
            "        stats.Name = \"leaderstats\"\n"
            "        stats.Parent = player\n"
            "    end\n"
            "    local coins = stats:FindFirstChild(\"Coins\")\n"
            "    if not coins then\n"
            "        coins = Instance.new(\"IntValue\")\n"
            "        coins.Name = \"Coins\"\n"
            "        coins.Parent = stats\n"
            "    end\n"
            "    coins.Value = coins.Value + 1\n"
            "    coin:Destroy()\n"
            "end)\n";
        t.push_back(coin);

        PartTemplate pad;
        pad.id = "JumpPad";
        pad.title = "Jump Pad";
        pad.description = "Launches players into the air.";
        pad.size = { 4, 0.4f, 4 };
        pad.color = { 0.18f, 0.85f, 0.95f };
        pad.material = Material::Neon;
        pad.scriptName = "JumpPadScript";
        pad.scriptSource =
            "-- Launches characters upwards.\n"
            "local pad = script.Parent\n"
            "local power = 30\n"
            "\n"
            "pad.Touched:Connect(function(hit)\n"
            "    local character = hit.Parent\n"
            "    local humanoid = character:FindFirstChildOfClass(\"Humanoid\")\n"
            "    local root = character:FindFirstChild(\"HumanoidRootPart\")\n"
            "    if humanoid and root then\n"
            "        root.AssemblyLinearVelocity = Vector3.new(0, power, 0)\n"
            "    end\n"
            "end)\n";
        t.push_back(pad);

        PartTemplate moving;
        moving.id = "MovingPlatform";
        moving.title = "Moving Platform";
        moving.description = "Glides back and forth; players ride on it.";
        moving.size = { 6, 1, 6 };
        moving.color = { 0.45f, 0.55f, 0.95f };
        moving.material = Material::SmoothPlastic;
        moving.scriptName = "MoveScript";
        moving.scriptSource =
            "-- Moves the platform back and forth with TweenService.\n"
            "local TweenService = game:GetService(\"TweenService\")\n"
            "local platform = script.Parent\n"
            "local distance = Vector3.new(16, 0, 0) -- how far it travels\n"
            "local seconds = 3\n"
            "\n"
            "local info = TweenInfo.new(seconds, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)\n"
            "TweenService:Create(platform, info, { Position = platform.Position + distance }):Play()\n";
        t.push_back(moving);

        PartTemplate checkpoint;
        checkpoint.id = "Checkpoint";
        checkpoint.title = "Checkpoint";
        checkpoint.description = "A SpawnLocation; touching it makes it the place the player respawns.";
        checkpoint.cls = ClassId::SpawnLocation;
        checkpoint.size = { 6, 0.6f, 6 };
        checkpoint.color = { 0.3f, 0.85f, 0.45f };
        checkpoint.scriptName = "CheckpointScript";
        checkpoint.scriptSource =
            "-- Touching this checkpoint makes it the only enabled spawn,\n"
            "-- so the player respawns here.\n"
            "local checkpoint = script.Parent\n"
            "\n"
            "checkpoint.Touched:Connect(function(hit)\n"
            "    if not hit.Parent:FindFirstChildOfClass(\"Humanoid\") then return end\n"
            "    if checkpoint.Enabled and checkpoint.Color == Color3.fromRGB(255, 220, 60) then return end\n"
            "    for _, spawn in ipairs(workspace:GetDescendants()) do\n"
            "        if spawn:IsA(\"SpawnLocation\") then\n"
            "            spawn.Enabled = (spawn == checkpoint)\n"
            "        end\n"
            "    end\n"
            "    checkpoint.Color = Color3.fromRGB(255, 220, 60)\n"
            "    game:Message(\"Checkpoint!\", 1.5)\n"
            "end)\n";
        t.push_back(checkpoint);

        PartTemplate door;
        door.id = "Door";
        door.title = "Door";
        door.description = "Opens (fades and lets players through) when touched, closes after 3 seconds.";
        door.size = { 5, 8, 1 };
        door.color = { 0.55f, 0.36f, 0.2f };
        door.material = Material::Wood;
        door.scriptName = "DoorScript";
        door.scriptSource =
            "-- A door that opens when a character touches it.\n"
            "local door = script.Parent\n"
            "local open = false\n"
            "\n"
            "door.Touched:Connect(function(hit)\n"
            "    if open or not hit.Parent:FindFirstChildOfClass(\"Humanoid\") then return end\n"
            "    open = true\n"
            "    door.Transparency = 0.7\n"
            "    door.CanCollide = false\n"
            "    task.wait(3)\n"
            "    door.Transparency = 0\n"
            "    door.CanCollide = true\n"
            "    open = false\n"
            "end)\n";
        t.push_back(door);

        PartTemplate finish;
        finish.id = "Finish";
        finish.title = "Finish Line";
        finish.description = "Touching it wins the game.";
        finish.size = { 8, 0.6f, 8 };
        finish.color = { 1.0f, 0.84f, 0.2f };
        finish.material = Material::Neon;
        finish.scriptName = "FinishScript";
        finish.scriptSource =
            "-- Touching the finish wins the game.\n"
            "local finish = script.Parent\n"
            "\n"
            "finish.Touched:Connect(function(hit)\n"
            "    if hit.Parent:FindFirstChildOfClass(\"Humanoid\") then\n"
            "        game:Win(\"Finished in \" .. math.floor(time()) .. \" seconds!\")\n"
            "    end\n"
            "end)\n";
        t.push_back(finish);

        PartTemplate leaderboard;
        leaderboard.id = "Leaderboard";
        leaderboard.title = "Leaderboard Script";
        leaderboard.description = "Adds a leaderstats folder with Coins to every player (shown top right).";
        leaderboard.sceneScript = true;
        leaderboard.scriptName = "Leaderboard";
        leaderboard.scriptSource =
            "-- Gives every player a leaderboard entry (the \"leaderstats\" folder).\n"
            "local Players = game:GetService(\"Players\")\n"
            "\n"
            "local function setup(player)\n"
            "    local stats = Instance.new(\"Folder\")\n"
            "    stats.Name = \"leaderstats\"\n"
            "    stats.Parent = player\n"
            "\n"
            "    local coins = Instance.new(\"IntValue\")\n"
            "    coins.Name = \"Coins\"\n"
            "    coins.Parent = stats\n"
            "end\n"
            "\n"
            "Players.PlayerAdded:Connect(setup)\n"
            "for _, player in ipairs(Players:GetPlayers()) do\n"
            "    if not player:FindFirstChild(\"leaderstats\") then setup(player) end\n"
            "end\n";
        t.push_back(leaderboard);
        return t;
    }();
    return list;
}

const PartTemplate* findTemplate(const std::string& id) {
    for (const PartTemplate& t : partTemplates()) if (t.id == id) return &t;
    return nullptr;
}

} // namespace rift
