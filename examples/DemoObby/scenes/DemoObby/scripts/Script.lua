-- Prints what touches this part (at most once per second).
local part = script.Parent
local last = -1

part.Touched:Connect(function(hit)
    if time() - last < 1 then return end
    last = time()
    print(hit:GetFullName() .. " touched " .. part.Name)
end)
