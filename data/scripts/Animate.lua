-- Animate: RIFT's default character animations (like Roblox's Animate script).
--
-- Every character gets this LocalScript unless StarterPlayer >
-- StarterCharacterScripts has its own LocalScript named "Animate". To change
-- the animations, copy this file into your game, put a LocalScript named
-- Animate in StarterCharacterScripts with it, and edit the ids below (make
-- new animations with the Animation Editor).

local character = script.Parent
local humanoid = character:WaitForChild("Humanoid")
local animator = humanoid:WaitForChild("Animator")

local ids = {
	idle = "engine:animations/idle.anim",
	walk = "engine:animations/walk.anim",
	run = "engine:animations/run.anim",
	jump = "engine:animations/jump.anim",
	fall = "engine:animations/fall.anim",
	swim = "engine:animations/swim.anim",
	tool = "engine:animations/toolnone.anim",
}

local tracks = {}
for name, id in pairs(ids) do
	local animation = Instance.new("Animation")
	animation.Name = name
	animation.AnimationId = id
	tracks[name] = animator:LoadAnimation(animation)
end

-- One movement animation at a time, cross-faded.
local current = nil
local function play(name, fade, speed)
	local track = tracks[name]
	if current ~= track then
		if current then current:Stop(fade) end
		current = track
		track:Play(fade)
	end
	track:AdjustSpeed(speed or 1)
end

humanoid.Running:Connect(function(speed)
	if speed > 0.5 then
		local walkSpeed = math.max(humanoid.WalkSpeed, 1)
		if speed > walkSpeed * 1.5 then
			play("run", 0.2, speed / (walkSpeed * 2))
		else
			play("walk", 0.2, speed / walkSpeed)
		end
	else
		play("idle", 0.25)
	end
end)

humanoid.Jumping:Connect(function()
	play("jump", 0.1)
end)

humanoid.FreeFalling:Connect(function(falling)
	if falling then play("fall", 0.3) end
end)

humanoid.Swimming:Connect(function(speed)
	play("swim", 0.35, math.clamp(speed / 8, 0.5, 2))
end)

humanoid.Died:Connect(function()
	if current then current:Stop(0.2) end
	tracks.tool:Stop(0.2)
end)

-- Holding a tool: the right arm reaches forward, over whatever the legs do.
local function updateTool()
	if character:FindFirstChildOfClass("Tool") then
		if not tracks.tool.IsPlaying then tracks.tool:Play(0.1) end
	elseif tracks.tool.IsPlaying then
		tracks.tool:Stop(0.1)
	end
end
character.ChildAdded:Connect(updateTool)
character.ChildRemoved:Connect(updateTool)

play("idle", 0)
updateTool()
