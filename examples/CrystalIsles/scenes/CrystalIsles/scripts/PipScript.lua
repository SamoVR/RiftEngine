-- Pip, the guide at the plaza: stands idle and waves now and then.
local ReplicatedStorage = game:GetService("ReplicatedStorage")

local animator = script.Parent.Humanoid.Animator

local idle = Instance.new("Animation")
idle.AnimationId = "engine:animations/idle.anim"
local idleTrack = animator:LoadAnimation(idle)
idleTrack.Looped = true
idleTrack:Play()

local wave = animator:LoadAnimation(ReplicatedStorage.Animations.Wave)
task.wait(1.5)
while true do
	wave:Play(0.2)
	wave.Stopped:Wait()
	task.wait(3 + math.random() * 3)
end
