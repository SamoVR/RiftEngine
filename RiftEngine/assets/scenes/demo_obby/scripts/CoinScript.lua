-- A spinning, bobbing coin that disappears when collected.
local coin = script.Parent
local startY = coin.Position.Y
local collected = false

RunService.Heartbeat:Connect(function(dt)
    if collected then return end
    coin.Rotation = coin.Rotation + Vector3.new(0, 120 * dt, 0)
    coin.Position = Vector3.new(coin.Position.X, startY + math.sin(time() * math.pi) * 0.25, coin.Position.Z)
end)

coin.Touched:Connect(function(other)
    if other == Players.LocalPlayer.Character and not collected then
        collected = true
        print("Coin collected!")
        coin:Destroy()
    end
end)
