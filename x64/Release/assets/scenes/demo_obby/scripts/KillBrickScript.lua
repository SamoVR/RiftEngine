-- Sends the player back to the spawn point when touched.
local brick = script.Parent
local player = Players.LocalPlayer

brick.Touched:Connect(function(other)
    if other == player.Character then
        game:Message("Ouch! Back to spawn.", 2)
        player:LoadCharacter()
    end
end)
