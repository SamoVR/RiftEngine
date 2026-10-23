-- Launches characters upwards.
local pad = script.Parent
local power = 28

pad.Touched:Connect(function(hit)
    local character = hit.Parent
    local root = character:FindFirstChild("HumanoidRootPart")
    if root and character:FindFirstChildOfClass("Humanoid") then
        root.AssemblyLinearVelocity = Vector3.new(0, power, 0)
    end
end)
