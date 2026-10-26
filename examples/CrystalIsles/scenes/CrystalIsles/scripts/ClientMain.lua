-- Crystal Isles: the player's side of the game. The menu, the countdown, the
-- HUD, the results, the camera, music and sounds. The server (RoundManager)
-- decides everything that counts; this script only shows it.
local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService = game:GetService("RunService")
local TweenService = game:GetService("TweenService")
local SoundService = game:GetService("SoundService")
local StarterGui = game:GetService("StarterGui")
local Debris = game:GetService("Debris")

local Config = require(ReplicatedStorage:WaitForChild("Config"))
local remotes = ReplicatedStorage:WaitForChild("Remotes")
local roundState = ReplicatedStorage:WaitForChild("RoundState")
local timeLeft = ReplicatedStorage:WaitForChild("TimeLeft")
local player = Players.LocalPlayer
local playerGui = player:WaitForChild("PlayerGui")
local camera = workspace.CurrentCamera

local menu = playerGui:WaitForChild("MenuGui")
local hud = playerGui:WaitForChild("HudGui")
local results = playerGui:WaitForChild("ResultsGui")

local WHITE = Color3.new(1, 1, 1)
local GOLD = Color3.fromRGB(255, 210, 90)
local GREEN = Color3.fromRGB(130, 255, 160)

-- Our own HUD shows the score; the built-in leaderboard only with several players.
StarterGui:SetCoreGuiEnabled(Enum.CoreGuiType.Backpack, false)
local function updatePlayerList()
	StarterGui:SetCoreGuiEnabled(Enum.CoreGuiType.PlayerList, #Players:GetPlayers() > 1)
end
Players.PlayerAdded:Connect(updatePlayerList)
updatePlayerList()

-- ============================================================================
--  Helpers
-- ============================================================================
local function tween(object, seconds, goals, style, direction)
	local t = TweenService:Create(object, TweenInfo.new(seconds, style or Enum.EasingStyle.Quad, direction or Enum.EasingDirection.Out), goals)
	t:Play()
	return t
end

local function play(name, speed)
	local template = SoundService:FindFirstChild(name)
	if not template then return end
	local sound = template:Clone()
	if speed then sound.PlaybackSpeed = speed end
	sound.Parent = SoundService
	sound:Play()
	Debris:AddItem(sound, 4)
end

local function humanoid()
	local character = player.Character
	return character and character:FindFirstChildOfClass("Humanoid")
end

local function setFrozen(frozen)
	local h = humanoid()
	if h then
		h.WalkSpeed = frozen and 0 or Config.WalkSpeed
		h.JumpPower = frozen and 0 or Config.JumpPower
	end
end

-- A little wiggle when something changes (back to where it rests).
local function wiggle(object, angle, rest)
	rest = rest or 0
	object.Rotation = rest + (angle or 8)
	tween(object, 0.35, { Rotation = rest }, Enum.EasingStyle.Back)
end

-- Buttons grow a little under the mouse (around their centre).
local function hoverable(button)
	local size, position = button.Size, button.Position
	local anchor = button.AnchorPoint
	local growX, growY = 12, 4
	local bigger = UDim2.new(size.X.Scale, size.X.Offset + growX, size.Y.Scale, size.Y.Offset + growY)
	local shifted = UDim2.new(position.X.Scale, position.X.Offset - growX * (0.5 - anchor.X), position.Y.Scale, position.Y.Offset - growY * (0.5 - anchor.Y))
	button.MouseEnter:Connect(function()
		play("Hover")
		tween(button, 0.12, { Size = bigger, Position = shifted })
	end)
	button.MouseLeave:Connect(function()
		tween(button, 0.12, { Size = size, Position = position })
	end)
end

-- ============================================================================
--  Toasts and popups
-- ============================================================================
local toasts = hud:WaitForChild("Toasts")
local popups = hud:WaitForChild("Popups")

local function toast(text, color)
	if not hud.Enabled then return end
	for _, other in ipairs(toasts:GetChildren()) do
		tween(other, 0.25, { Position = other.Position + UDim2.fromOffset(0, 48) })
	end
	local pill = Instance.new("TextLabel")
	pill.Size = UDim2.fromOffset(math.max(220, #text * 9 + 56), 40)
	pill.AnchorPoint = Vector2.new(0.5, 0)
	pill.Position = UDim2.new(0.5, 0, 0, -12)
	pill.BackgroundColor3 = Color3.fromRGB(14, 16, 42)
	pill.BackgroundTransparency = 1
	pill.CornerRadius = 20
	pill.Text = text
	pill.Font = Enum.Font.GothamBold
	pill.TextSize = 17
	pill.TextColor3 = color or WHITE
	pill.TextTransparency = 1
	pill.Parent = toasts
	tween(pill, 0.3, { Position = UDim2.new(0.5, 0, 0, 0), TextTransparency = 0, BackgroundTransparency = 0.15 })
	task.delay(3.6, function()
		tween(pill, 0.4, { TextTransparency = 1, BackgroundTransparency = 1 })
		Debris:AddItem(pill, 0.5)
	end)
end

-- "+3" floating up from where the gem was.
local function popup(text, color, worldPosition, big)
	local screen, onScreen = camera:WorldToViewportPoint(worldPosition)
	if not onScreen then
		local size = camera.ViewportSize
		screen = Vector3.new(size.X / 2, size.Y * 0.45, 0)
	end
	local label = Instance.new("TextLabel")
	label.BackgroundTransparency = 1
	label.Size = UDim2.fromOffset(220, 60)
	label.AnchorPoint = Vector2.new(0.5, 0.5)
	label.Position = UDim2.fromOffset(screen.X, screen.Y)
	label.Text = text
	label.Font = Enum.Font.GothamBlack
	label.TextSize = big and 52 or 38
	label.TextColor3 = color
	label.TextStrokeColor3 = Color3.fromRGB(20, 12, 50)
	label.TextStrokeTransparency = 0.15
	label.Rotation = math.random(-10, 10)
	label.Parent = popups
	tween(label, 1.2, { Position = UDim2.fromOffset(screen.X, screen.Y - 90), TextTransparency = 1, TextStrokeTransparency = 1, Rotation = 0 }, Enum.EasingStyle.Quad, Enum.EasingDirection.In)
	Debris:AddItem(label, 1.3)
end

-- ============================================================================
--  Menu
-- ============================================================================
local content = menu:WaitForChild("Content")
local backdrop = menu:WaitForChild("Backdrop")
local howPanel = menu:WaitForChild("HowPanel")
local contentHome = content.Position
local inMenu = false
local orbit = 0

local function offscreen(position, by)
	return UDim2.new(position.X.Scale + by, position.X.Offset, position.Y.Scale, position.Y.Offset)
end

local function showMenu()
	inMenu = true
	results.Enabled = false
	hud.Enabled = false
	menu.Enabled = true
	howPanel.Visible = false
	content.Position = offscreen(contentHome, -0.45)
	tween(content, 0.7, { Position = contentHome }, Enum.EasingStyle.Back)
	backdrop.BackgroundTransparency = 1
	tween(backdrop, 0.6, { BackgroundTransparency = 0 })
	camera.CameraType = Enum.CameraType.Scriptable
	setFrozen(true)
	task.spawn(function()
		local ok, best = pcall(function() return remotes.GetBest:InvokeServer() end)
		if ok and best then content.BestCard.BestValue.Text = tostring(best) end
	end)
end

local function hideMenu()
	inMenu = false
	tween(content, 0.35, { Position = offscreen(contentHome, -0.5) }, Enum.EasingStyle.Quad, Enum.EasingDirection.In)
	tween(backdrop, 0.4, { BackgroundTransparency = 1 })
	task.delay(0.45, function()
		if not inMenu then menu.Enabled = false end
	end)
	camera.CameraType = Enum.CameraType.Custom
end

-- ============================================================================
--  HUD
-- ============================================================================
local timerPill = hud:WaitForChild("TimerPill")
local timerText = timerPill.TimeText
local timeBar = timerPill.BarBack.BarFill
local scoreCard = hud:WaitForChild("ScoreCard")
local scoreText = scoreCard.ScoreText
local comboBadge = hud:WaitForChild("ComboBadge")
local boostBar = hud:WaitForChild("BoostBar")
local countdown = hud:WaitForChild("Countdown")
local counts = {
	Blue = hud.GemCounts.Blue.Count,
	Purple = hud.GemCounts.Purple.Count,
	Gold = hud.GemCounts.Gold.Count,
}
local mine = { Score = 0, Blue = 0, Purple = 0, Gold = 0 }
local shownScore = 0
local lastPickup = -100

local function resetHud()
	mine = { Score = 0, Blue = 0, Purple = 0, Gold = 0 }
	shownScore = 0
	scoreText.Text = "0"
	for _, label in pairs(counts) do label.Text = "0" end
	timerText.Text = Config.FormatTime(Config.RoundTime)
	timerText.TextColor3 = WHITE
	timeBar.Size = UDim2.fromScale(1, 1)
	comboBadge.Visible = false
	boostBar.Visible = false
	countdown.Visible = false
	for _, child in ipairs(toasts:GetChildren()) do child:Destroy() end
end

local function joinRound()
	if inMenu then hideMenu() end
	results.Enabled = false
	resetHud()
	hud.Enabled = true
end

timeLeft.Changed:Connect(function(value)
	if not hud.Enabled then return end
	timerText.Text = Config.FormatTime(value)
	tween(timeBar, 0.3, { Size = UDim2.fromScale(math.clamp(value / Config.RoundTime, 0, 1), 1) }, Enum.EasingStyle.Linear)
	if value <= 10 and value > 0 and roundState.Value == "Playing" then
		timerText.TextColor3 = Color3.fromRGB(255, 120, 120)
		timerText.TextSize = 46
		tween(timerText, 0.45, { TextSize = 36 })
		play("Tick", 1.25)
	end
end)

RunService.RenderStepped:Connect(function(dt)
	-- The menu camera circles the islands.
	if inMenu then
		orbit = orbit + dt * 0.05
		local center = Vector3.new(4, 5, -12)
		local eye = center + Vector3.new(math.sin(orbit) * 80, 32, math.cos(orbit) * 80)
		camera.CFrame = CFrame.lookAt(eye, center)
	end
	-- The score counts up to the real one.
	if shownScore ~= mine.Score then
		shownScore = math.min(mine.Score, shownScore + math.max(1, math.floor((mine.Score - shownScore) * 0.2)))
		scoreText.Text = tostring(shownScore)
	end
	-- The combo badge goes away when the combo ends.
	if comboBadge.Visible and time() - lastPickup > Config.ComboWindow then
		comboBadge.Visible = false
	end
end)

-- ============================================================================
--  Results
-- ============================================================================
local panel = results:WaitForChild("Panel")
local dim = results:WaitForChild("Dim")

local function confetti()
	local root = player.Character and player.Character:FindFirstChild("HumanoidRootPart")
	if not root then return end
	local holder = Instance.new("Part")
	holder.Anchored = true
	holder.CanCollide = false
	holder.CanTouch = false
	holder.CastShadow = false
	holder.Transparency = 1
	holder.Size = Vector3.new(1, 0.2, 1)
	holder.Position = root.Position + Vector3.new(0, 2.5, 0)
	holder.Parent = workspace
	for _, color in ipairs({ Color3.fromRGB(255, 205, 80), Color3.fromRGB(236, 72, 153), Color3.fromRGB(34, 211, 238), Color3.fromRGB(160, 110, 255) }) do
		local emitter = Instance.new("ParticleEmitter")
		emitter.Enabled = false
		emitter.Color = ColorSequence.new(color)
		emitter.Size = NumberSequence.new(0.28, 0.2)
		emitter.Lifetime = NumberRange.new(1.4, 2.4)
		emitter.Speed = NumberRange.new(7, 13)
		emitter.SpreadAngle = Vector2.new(55, 55)
		emitter.Acceleration = Vector3.new(0, -12, 0)
		emitter.Drag = 1.2
		emitter.EmissionDirection = Enum.NormalId.Top
		emitter.Parent = holder
		emitter:Emit(22)
	end
	Debris:AddItem(holder, 4)
end

local function cheer()
	local h = humanoid()
	local animator = h and h:FindFirstChildOfClass("Animator")
	if animator then
		animator:LoadAnimation(ReplicatedStorage.Animations.Cheer):Play(0.15)
	end
end

local function showResults(data)
	hud.Enabled = false
	results.Enabled = true
	dim.BackgroundTransparency = 1
	tween(dim, 0.4, { BackgroundTransparency = 0.45 })
	panel.Size = UDim2.fromOffset(420, 360)
	tween(panel, 0.5, { Size = UDim2.fromOffset(540, 486) }, Enum.EasingStyle.Back)
	panel.ScoreText.Text = "0"
	panel.Gems.Blue.Count.Text = tostring(data.Blue)
	panel.Gems.Purple.Count.Text = tostring(data.Purple)
	panel.Gems.Gold.Count.Text = tostring(data.Gold)
	panel.BestText.Text = "Best: " .. tostring(data.Best)
	panel.ComboText.Text = "Best combo: x" .. tostring(data.BestCombo)
	panel.NewBest.Visible = false
	play("Fanfare")
	cheer()
	confetti()
	task.spawn(function()
		task.wait(0.35)
		local steps = 32
		for k = 1, steps do
			panel.ScoreText.Text = tostring(math.floor(data.Score * k / steps + 0.5))
			task.wait(0.025)
		end
		if data.NewBest and data.Score > 0 then
			local badge = panel.NewBest
			badge.Visible = true
			badge.Size = UDim2.fromOffset(40, 12)
			tween(badge, 0.45, { Size = UDim2.fromOffset(132, 38) }, Enum.EasingStyle.Back)
			play("GemGold")
		end
	end)
end

-- ============================================================================
--  Events from the server
-- ============================================================================
remotes.RoundEvent.OnClientEvent:Connect(function(kind, data)
	if kind == "Countdown" then
		joinRound()
		task.spawn(function()
			countdown.Visible = true
			for n = data, 1, -1 do
				if roundState.Value ~= "Countdown" then break end
				countdown.Text = tostring(n)
				countdown.TextTransparency = 0
				countdown.TextStrokeTransparency = 0.1
				countdown.TextSize = 260
				tween(countdown, 0.5, { TextSize = 190 }, Enum.EasingStyle.Back)
				play("Tick")
				task.wait(1)
			end
		end)
	elseif kind == "Go" then
		countdown.Visible = true
		countdown.Text = "GO!"
		countdown.TextSize = 250
		countdown.TextTransparency = 0
		play("Go")
		tween(countdown, 0.8, { TextSize = 150, TextTransparency = 1, TextStrokeTransparency = 1 })
		task.delay(0.8, function() countdown.Visible = false end)
	elseif kind == "Results" then
		showResults(data)
	end
end)

remotes.GemCollected.OnClientEvent:Connect(function(who, kind, points, multiplier, combo, position)
	if who ~= player.Name then return end
	local gem = Config.Gems[kind]
	play(gem.Sound, 1 + math.min(combo - 1, 10) * 0.03)
	mine.Score = mine.Score + points
	mine[kind] = mine[kind] + 1
	counts[kind].Text = tostring(mine[kind])
	wiggle(counts[kind].Parent, 10)
	wiggle(scoreCard, -4)
	popup("+" .. points, gem.Color, position + Vector3.new(0, 1.5, 0), kind == "Gold")
	lastPickup = time()
	if multiplier > 1 then
		local wasVisible = comboBadge.Visible
		local text = comboBadge.Label
		text.Text = "x" .. multiplier .. " COMBO"
		comboBadge.Visible = true
		if not wasVisible or combo == 4 or combo == 8 then
			text.TextSize = 30
			tween(text, 0.35, { TextSize = 20 }, Enum.EasingStyle.Back)
			wiggle(comboBadge, -12, -6)
		end
	end
	if kind == "Gold" then toast("Sun Gem!  +" .. points, GOLD) end
end)

remotes.Announce.OnClientEvent:Connect(function(text, kind)
	toast(text, kind == "Gold" and GOLD or WHITE)
end)

remotes.PadUsed.OnClientEvent:Connect(function(kind, seconds)
	if kind == "Launch" then
		play("Whoosh")
	elseif kind == "Boost" then
		play("Boost")
		boostBar.Visible = hud.Enabled
		local fill = boostBar.BarBack.BarFill
		fill.Size = UDim2.fromScale(1, 1)
		tween(fill, seconds, { Size = UDim2.fromScale(0, 1) }, Enum.EasingStyle.Linear)
		task.delay(seconds, function() boostBar.Visible = false end)
		toast("Speed boost!", GREEN)
	end
end)

-- ============================================================================
--  Buttons
-- ============================================================================
for _, button in ipairs({ content.PlayButton, content.HowButton, howPanel.CloseButton, panel.AgainButton, panel.MenuButton }) do
	hoverable(button)
end

content.PlayButton.Activated:Connect(function()
	if not inMenu then return end
	play("Click")
	if roundState.Value == "Playing" then
		-- A round is already on (another player started it): jump in.
		joinRound()
		setFrozen(false)
	else
		hideMenu()
		remotes.StartRound:FireServer()
	end
end)

content.HowButton.Activated:Connect(function()
	play("Click")
	howPanel.Visible = true
	howPanel.Size = UDim2.fromOffset(500, 390)
	tween(howPanel, 0.4, { Size = UDim2.fromOffset(580, 452) }, Enum.EasingStyle.Back)
end)

howPanel.CloseButton.Activated:Connect(function()
	play("Click")
	howPanel.Visible = false
end)

panel.AgainButton.Activated:Connect(function()
	play("Click")
	results.Enabled = false
	remotes.StartRound:FireServer()
end)

panel.MenuButton.Activated:Connect(function()
	play("Click")
	showMenu()
end)

-- ============================================================================
--  Start
-- ============================================================================
player.CharacterAdded:Connect(function(character)
	character:WaitForChild("Humanoid")
	if inMenu then
		setFrozen(true)
	elseif hud.Enabled then
		toast("Whoops! Back to the plaza.", WHITE)
	end
end)

SoundService:WaitForChild("Music"):Play()
if not player.Character then player.CharacterAdded:Wait() end
showMenu()
