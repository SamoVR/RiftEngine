-- Launches the player upwards.
local power = 28

script.Parent.Touched:Connect(function(other)
    if other == Players.LocalPlayer.Character then
        other.Velocity = Vector3.new(0, power, 0)
    end
end)
