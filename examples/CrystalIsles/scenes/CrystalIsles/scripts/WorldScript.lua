-- Crystal Isles: the parts of the world that move things (server): launch
-- pads, speed pads and moving platforms. Found by their tags.
local Players = game:GetService("Players")
local CollectionService = game:GetService("CollectionService")
local TweenService = game:GetService("TweenService")
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local Config = require(ReplicatedStorage:WaitForChild("Config"))
local padUsed = ReplicatedStorage.Remotes.PadUsed

local function characterOf(hit)
	local model = hit.Parent
	local humanoid = model and model:FindFirstChildOfClass("Humanoid")
	if not humanoid or humanoid.Health <= 0 then return nil end
	return model, humanoid, Players:GetPlayerFromCharacter(model)
end

-- Launch pads throw the character to another island (each pad's "Launch"
-- attribute is the velocity).
local launched = {}
for _, pad in ipairs(CollectionService:GetTagged("LaunchPad")) do
	local velocity = pad:GetAttribute("Launch")
	pad.Touched:Connect(function(hit)
		local character, humanoid, player = characterOf(hit)
		if not character or humanoid.WalkSpeed <= 0 then return end
		if launched[character] and time() < launched[character] then return end
		launched[character] = time() + 1
		character.HumanoidRootPart.AssemblyLinearVelocity = velocity
		if player then padUsed:FireClient(player, "Launch") end
	end)
end

-- Speed pads: run faster for a few seconds, leaving a trail.
local boosted = {}
for _, pad in ipairs(CollectionService:GetTagged("SpeedPad")) do
	pad.Touched:Connect(function(hit)
		local character, humanoid, player = characterOf(hit)
		if not character or boosted[character] or humanoid.WalkSpeed <= 0 then return end
		boosted[character] = true
		humanoid.WalkSpeed = Config.BoostSpeed

		local torso = character:FindFirstChild("Torso")
		local trail
		if torso then
			local top = Instance.new("Attachment")
			top.Position = Vector3.new(0, 0.35, 0)
			top.Parent = torso
			local bottom = Instance.new("Attachment")
			bottom.Position = Vector3.new(0, -0.35, 0)
			bottom.Parent = torso
			trail = Instance.new("Trail")
			trail.Attachment0 = top
			trail.Attachment1 = bottom
			trail.Lifetime = 0.35
			trail.Color = ColorSequence.new(Color3.fromRGB(120, 255, 160), Color3.fromRGB(60, 220, 255))
			trail.Transparency = NumberSequence.new(0.2, 1)
			trail.LightEmission = 1
			trail.Parent = torso
		end
		if player then padUsed:FireClient(player, "Boost", Config.BoostTime) end

		task.delay(Config.BoostTime, function()
			boosted[character] = nil
			if humanoid.Parent and humanoid.WalkSpeed == Config.BoostSpeed then humanoid.WalkSpeed = Config.WalkSpeed end
			if trail then
				trail.Attachment0:Destroy()
				trail.Attachment1:Destroy()
				trail:Destroy()
			end
		end)
	end)
end

-- Moving platforms go back and forth along their "Travel" attribute.
for _, platform in ipairs(CollectionService:GetTagged("Mover")) do
	local travel = platform:GetAttribute("Travel")
	local duration = platform:GetAttribute("Duration") or 3
	local info = TweenInfo.new(duration, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true, 0.6)
	TweenService:Create(platform, info, { Position = platform.Position + travel }):Play()
end
