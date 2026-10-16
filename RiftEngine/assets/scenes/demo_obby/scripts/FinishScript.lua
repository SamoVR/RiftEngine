-- Ends the run when the player reaches this part.
script.Parent.Touched:Connect(function(other)
    if other == Players.LocalPlayer.Character then
        game:Win("Finished in " .. math.floor(time() + 0.5) .. " s!")
    end
end)
