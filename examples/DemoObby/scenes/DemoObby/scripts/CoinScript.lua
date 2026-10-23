-- A spinning, bobbing coin. Touching it adds 1 to the player's Coins.
local coin = script.Parent
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local startY = coin.Position.Y
local collected = false

RunService.Heartbeat:Connect(function(dt)
    if collected then return end
    coin.Rotation = coin.Rotation + Vector3.new(0, 120 * dt, 0)
    coin.Position = Vector3.new(coin.Position.X, startY + math.sin(time() * math.pi) * 0.25, coin.Position.Z)
end)

coin.Touched:Connect(function(hit)
    if collected then return end
    local player = Players:GetPlayerFromCharacter(hit.Parent)
    if not player then return end
    collected = true

    -- "leaderstats" values show on the leaderboard (top right).
    local stats = player:FindFirstChild("leaderstats")
    if not stats then
        stats = Instance.new("Folder")
        stats.Name = "leaderstats"
        stats.Parent = player
    end
    local coins = stats:FindFirstChild("Coins")
    if not coins then
        coins = Instance.new("IntValue")
        coins.Name = "Coins"
        coins.Parent = stats
    end
    coins.Value = coins.Value + 1
    print("Coin collected!")
    coin:Destroy()
end)
