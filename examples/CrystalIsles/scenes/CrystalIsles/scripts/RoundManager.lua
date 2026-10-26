-- Crystal Isles: runs the rounds (server). Places the gems, counts the points,
-- keeps the clock and remembers each player's best score.
local Players = game:GetService("Players")
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local ServerStorage = game:GetService("ServerStorage")
local DataStoreService = game:GetService("DataStoreService")

local Config = require(ReplicatedStorage:WaitForChild("Config"))
local remotes = ReplicatedStorage.Remotes
local roundState = ReplicatedStorage.RoundState   -- Lobby, Countdown, Playing, Results
local timeLeft = ReplicatedStorage.TimeLeft
local templates = ServerStorage.GemTemplates
local activeGems = workspace.ActiveGems
local store = DataStoreService:GetDataStore("CrystalIsles")

-- Where gems can appear: "Common" spots everywhere, "Gold" ones in hard places.
local spawnPoints = { Common = {}, Gold = {} }
for _, point in ipairs(workspace.GemSpawns:GetChildren()) do
	local tier = point:GetAttribute("Tier") or "Common"
	table.insert(spawnPoints[tier] or spawnPoints.Common, point)
end

local taken = {}     -- spawn point -> the gem on it
local stats = {}     -- player -> this round's numbers
local best = {}      -- player -> best score so far
local running = false

local function newStats()
	return { Score = 0, Blue = 0, Purple = 0, Gold = 0, Combo = 0, BestCombo = 1, LastPickup = -100 }
end

local function setScore(player, value)
	local leaderstats = player:FindFirstChild("leaderstats")
	local score = leaderstats and leaderstats:FindFirstChild("Score")
	if score then score.Value = value end
end

-- ---- gems
local commonKinds = { "Blue", "Purple" }

local function pickKind()
	local total = 0
	for _, kind in ipairs(commonKinds) do total = total + Config.Gems[kind].Weight end
	local roll = math.random() * total
	for _, kind in ipairs(commonKinds) do
		roll = roll - Config.Gems[kind].Weight
		if roll <= 0 then return kind end
	end
	return "Blue"
end

local function freePoint(tier, avoid)
	local free = {}
	for _, point in ipairs(spawnPoints[tier]) do
		if not taken[point] and point ~= avoid then table.insert(free, point) end
	end
	if #free == 0 then return nil end
	return free[math.random(#free)]
end

local collect

local function spawnGem(tier, avoid, quiet)
	local point = freePoint(tier, avoid)
	if not point then return end
	local kind = tier == "Gold" and "Gold" or pickKind()
	local gem = templates[kind]:Clone()
	gem:PivotTo(CFrame.new(point.Position))
	gem.Parent = activeGems
	taken[point] = gem

	local done = false
	gem.Hitbox.Touched:Connect(function(hit)
		if done or not running then return end
		local player = Players:GetPlayerFromCharacter(hit.Parent)
		if not player then return end
		done = true
		taken[point] = nil
		collect(player, gem, kind, point)
	end)
	if kind == "Gold" and not quiet then
		remotes.Announce:FireAllClients("A Sun Gem appeared on " .. (point:GetAttribute("Area") or "an island") .. "!", "Gold")
	end
end

collect = function(player, gem, kind, point)
	local s = stats[player]
	if not s then
		s = newStats()
		stats[player] = s
	end
	-- Quick pickups in a row make a combo.
	local now = time()
	if now - s.LastPickup <= Config.ComboWindow then s.Combo = s.Combo + 1 else s.Combo = 1 end
	s.LastPickup = now
	local multiplier = Config.Multiplier(s.Combo)
	s.BestCombo = math.max(s.BestCombo, multiplier)
	local points = Config.Gems[kind].Points * multiplier
	s.Score = s.Score + points
	s[kind] = s[kind] + 1
	setScore(player, s.Score)

	local where = gem:GetPivot().Position
	gem:Destroy()
	remotes.GemCollected:FireAllClients(player.Name, kind, points, multiplier, s.Combo, where)

	-- Another one appears somewhere else.
	local gold = kind == "Gold"
	task.delay(gold and Config.SunGemDelay or Config.RespawnDelay, function()
		if running then spawnGem(gold and "Gold" or "Common", point) end
	end)
end

local function clearGems()
	for _, gem in ipairs(activeGems:GetChildren()) do gem:Destroy() end
	taken = {}
end

-- ---- characters
local function setMovement(walk, jump)
	for _, player in ipairs(Players:GetPlayers()) do
		local character = player.Character
		local humanoid = character and character:FindFirstChildOfClass("Humanoid")
		if humanoid then
			humanoid.WalkSpeed = walk
			humanoid.JumpPower = jump
		end
	end
end

local function gatherAtSpawn()
	local base = workspace.SpawnLocation.Position
	for i, player in ipairs(Players:GetPlayers()) do
		local root = player.Character and player.Character:FindFirstChild("HumanoidRootPart")
		if root then
			local angle = (i - 1) * 2.4
			local radius = i == 1 and 0 or 2
			local at = base + Vector3.new(math.sin(angle) * radius, 1.3, math.cos(angle) * radius)
			root.CFrame = CFrame.lookAt(at, at + Vector3.new(0, 0, -1))
		end
	end
end

-- ---- a round
local function runRound()
	roundState.Value = "Countdown"
	clearGems()
	stats = {}
	for _, player in ipairs(Players:GetPlayers()) do
		stats[player] = newStats()
		setScore(player, 0)
	end
	gatherAtSpawn()
	setMovement(0, 0)
	timeLeft.Value = Config.RoundTime
	remotes.RoundEvent:FireAllClients("Countdown", Config.Countdown)
	task.wait(Config.Countdown)

	running = true
	roundState.Value = "Playing"
	setMovement(Config.WalkSpeed, Config.JumpPower)
	for _ = 1, Config.ActiveGems do spawnGem("Common") end
	for _ = 1, Config.SunGems do spawnGem("Gold", nil, true) end
	remotes.RoundEvent:FireAllClients("Go")
	remotes.Announce:FireAllClients("Sun Gems are out there: follow the beams of light!", "Gold")

	local endsAt = time() + Config.RoundTime
	while time() < endsAt do
		timeLeft.Value = math.ceil(endsAt - time())
		task.wait(0.2)
	end
	timeLeft.Value = 0
	running = false
	roundState.Value = "Results"
	clearGems()

	for _, player in ipairs(Players:GetPlayers()) do
		local s = stats[player] or newStats()
		local previous = best[player] or 0
		local newBest = s.Score > previous
		if newBest then
			best[player] = s.Score
			pcall(function() store:SetAsync("best_" .. player.UserId, s.Score) end)
		end
		remotes.RoundEvent:FireClient(player, "Results", {
			Score = s.Score, Blue = s.Blue, Purple = s.Purple, Gold = s.Gold,
			BestCombo = s.BestCombo, Best = best[player] or 0, NewBest = newBest,
		})
	end
	pcall(function() store:IncrementAsync("rounds", 1) end)
end

remotes.StartRound.OnServerEvent:Connect(function(player)
	if roundState.Value == "Lobby" or roundState.Value == "Results" then
		task.spawn(runRound)
	end
end)

-- ---- players
local function loadBest(player)
	local ok, value = pcall(function() return store:GetAsync("best_" .. player.UserId) end)
	best[player] = ok and tonumber(value) or 0
end

local function setup(player)
	if player:FindFirstChild("leaderstats") then return end
	local leaderstats = Instance.new("Folder")
	leaderstats.Name = "leaderstats"
	local score = Instance.new("IntValue")
	score.Name = "Score"
	score.Parent = leaderstats
	leaderstats.Parent = player
	loadBest(player)
end

Players.PlayerAdded:Connect(setup)
for _, player in ipairs(Players:GetPlayers()) do setup(player) end

remotes.GetBest.OnServerInvoke = function(player)
	if best[player] == nil then loadBest(player) end
	return best[player]
end
