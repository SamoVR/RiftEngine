-- Crystal Isles: what only has to look nice, done by every client on its own:
-- gems and the shrine crystal spin and bob, and pickups burst into sparkles.
local CollectionService = game:GetService("CollectionService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local TweenService = game:GetService("TweenService")
local Debris = game:GetService("Debris")

local Config = require(ReplicatedStorage:WaitForChild("Config"))
local remotes = ReplicatedStorage:WaitForChild("Remotes")
local activeGems = workspace:WaitForChild("ActiveGems")

-- Spinning things remember where they started (the server never moves them,
-- so this client's changes stay).
local spinning = {}

local function track(object, speed, bob)
	spinning[object] = {
		base = object.Position,
		phase = math.random() * math.pi * 2,
		speed = object:GetAttribute("SpinSpeed") or speed,
		bob = object:GetAttribute("Bob") or bob,
	}
end

for _, object in ipairs(CollectionService:GetTagged("Spin")) do track(object, 40, 0.3) end
for _, gem in ipairs(activeGems:GetChildren()) do track(gem, 100, 0.25) end
activeGems.ChildAdded:Connect(function(gem) track(gem, 100, 0.25) end)
activeGems.ChildRemoved:Connect(function(gem) spinning[gem] = nil end)

RunService.RenderStepped:Connect(function()
	local t = time()
	for object, s in pairs(spinning) do
		if object.Parent then
			object.Rotation = Vector3.new(0, (t * s.speed + math.deg(s.phase)) % 360, 0)
			object.Position = s.base + Vector3.new(0, math.sin(t * 2.2 + s.phase) * s.bob, 0)
		else
			spinning[object] = nil
		end
	end
end)

-- A burst of sparkles and a flash of light where a gem was taken (for every player's pickups).
remotes.GemCollected.OnClientEvent:Connect(function(who, kind, points, multiplier, combo, position)
	local gem = Config.Gems[kind]
	local holder = Instance.new("Part")
	holder.Anchored = true
	holder.CanCollide = false
	holder.CanTouch = false
	holder.CastShadow = false
	holder.Transparency = 1
	holder.Size = Vector3.new(0.2, 0.2, 0.2)
	holder.Position = position
	holder.Parent = workspace

	local attachment = Instance.new("Attachment")
	attachment.Parent = holder
	local burst = Instance.new("ParticleEmitter")
	burst.Enabled = false
	burst.Texture = "assets/images/sparkle.png"
	burst.Color = ColorSequence.new(gem.Color, Color3.new(1, 1, 1))
	burst.Size = NumberSequence.new(kind == "Gold" and 1.0 or 0.65, 0)
	burst.Transparency = NumberSequence.new(0, 1)
	burst.Lifetime = NumberRange.new(0.4, 0.9)
	burst.Speed = NumberRange.new(4, 10)
	burst.SpreadAngle = Vector2.new(180, 180)
	burst.Drag = 3
	burst.LightEmission = 1
	burst.RotSpeed = NumberRange.new(-240, 240)
	burst.Parent = attachment
	burst:Emit(kind == "Gold" and 60 or 26)

	local light = Instance.new("PointLight")
	light.Color = gem.Color
	light.Range = kind == "Gold" and 16 or 10
	light.Brightness = 3
	light.Parent = holder
	TweenService:Create(light, TweenInfo.new(0.6), { Brightness = 0 }):Play()
	Debris:AddItem(holder, 1.5)
end)
