-- Spins the part around the Y axis.
local part = script.Parent
local speed = 90 -- degrees per second

RunService.Heartbeat:Connect(function(dt)
    part.Rotation = part.Rotation + Vector3.new(0, speed * dt, 0)
end)
