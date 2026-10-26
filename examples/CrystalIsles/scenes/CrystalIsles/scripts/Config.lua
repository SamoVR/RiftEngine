-- Crystal Isles: the numbers both sides use (the server and every client
-- require this module from ReplicatedStorage).
local Config = {}

Config.RoundTime = 120        -- seconds
Config.Countdown = 3          -- 3, 2, 1, GO
Config.ActiveGems = 18        -- common gems on the map at once
Config.SunGems = 2            -- Sun Gems on the map at once
Config.RespawnDelay = 1.2     -- a new gem appears this long after one is taken
Config.SunGemDelay = 8
Config.ComboWindow = 2.5      -- pick the next gem within this many seconds to keep the combo
Config.WalkSpeed = 10
Config.JumpPower = 13
Config.BoostSpeed = 18
Config.BoostTime = 5

-- Weight: how often a common spot gets this gem.
Config.Gems = {
	Blue = { Name = "Sky Shard", Points = 1, Weight = 72, Color = Color3.fromRGB(110, 195, 255), Image = "assets/images/gem_blue.png", Sound = "GemBlue" },
	Purple = { Name = "Amethyst", Points = 3, Weight = 28, Color = Color3.fromRGB(196, 150, 255), Image = "assets/images/gem_purple.png", Sound = "GemPurple" },
	Gold = { Name = "Sun Gem", Points = 10, Weight = 0, Color = Color3.fromRGB(255, 210, 90), Image = "assets/images/gem_gold.png", Sound = "GemGold" },
}

-- The combo multiplier for the n-th gem in a row.
function Config.Multiplier(combo)
	if combo >= 8 then return 3 end
	if combo >= 4 then return 2 end
	return 1
end

function Config.FormatTime(seconds)
	seconds = math.max(0, math.floor(seconds))
	return string.format("%d:%02d", seconds // 60, seconds % 60)
end

return Config
