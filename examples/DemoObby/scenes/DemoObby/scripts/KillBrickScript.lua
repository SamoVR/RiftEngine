-- Kills any character that touches it (the player respawns at the spawn).
local brick = script.Parent

brick.Touched:Connect(function(hit)
    local humanoid = hit.Parent:FindFirstChildOfClass("Humanoid")
    if humanoid and humanoid.Health > 0 then
        humanoid.Health = 0
        game:Message("Ouch! Back to spawn.", 2)
    end
end)
