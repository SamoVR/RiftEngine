-- Ends the run when a character reaches this part.
local finish = script.Parent

finish.Touched:Connect(function(hit)
    if hit.Parent:FindFirstChildOfClass("Humanoid") then
        game:Win("Finished in " .. math.floor(time() + 0.5) .. " s!")
    end
end)
