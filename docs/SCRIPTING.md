# Scripting in RIFT Engine

Game scripts are **Lua 5.4** files. The API is modelled on Roblox (`workspace`, `Instance.new`, `part.Touched:Connect(...)`, `task.wait`, `RunService.Heartbeat`), so if you know Roblox Studio most of it will feel familiar. The differences are listed [at the end](#differences-from-roblox); read them once.

## Where scripts live

A scene is a **folder**:

```
assets/scenes/demo_obby/
    scene.json                 the objects (format 3)
    scripts/
        CoinScript.lua         plain Lua files: edit them in the engine or in any editor
        FinishScript.lua
```

* A **Script** is an object in the Explorer (code icon). Its `source` is a path relative to the scene folder (`scripts/CoinScript.lua`).
* A Script whose parent is a part is **attached** to that part: `script.Parent` is the part.
  A Script with no parent is **scene-level**: `script.Parent` is `workspace`.
* Insert one with **Insert > Script (Lua)** (attached to the selected part, or scene-level), with **Add Script** in a part's Properties, or by inserting a scripted template (Coin, Kill Brick, Spinner, Jump Pad, Finish Line).
* **Double-click** a Script in the Explorer to open it in the **Script Editor**.
* **File > Save** writes `scene.json` *and* every changed script. **Save As** creates a new folder named after the file you type (`Level2` becomes `Level2/scene.json` + `Level2/scripts/`).
* Duplicating a Script gives another Script that uses the **same file** (like a prefab). Duplicating a part duplicates the scripts attached to it.
* Edit the `.lua` files in VS Code (or anything else) while the engine is open: changes are picked up within a second (hot reload). If you also have unsaved edits to the same script in the engine, yours are kept and you are told.
* Press **Play** (F5) to run. The game runs the editor's current text, saved or not. **Stop** puts the scene back exactly as it was.

## First script

```lua
-- Attached to a part: make it spin and turn red when the player touches it.
local part = script.Parent

RunService.Heartbeat:Connect(function(dt)
    part.Rotation = part.Rotation + Vector3.new(0, 90 * dt, 0)
end)

part.Touched:Connect(function(other)
    if other == Players.LocalPlayer.Character then
        part.Color = Color3.fromRGB(255, 0, 0)
        game:Message("Ouch!", 2)
    end
end)
```

`print(...)` writes to the **Output** panel, prefixed with the script's name. Errors show the file and line (`scripts/CoinScript.lua:7: ...`).

## Globals

| Name | What it is |
|---|---|
| `workspace` | Root of the world. `workspace:GetChildren()`, `workspace.Coin` (finds a child by name), `workspace.Gravity` (default `-30`). |
| `game` | `game:GetService(name)`, `game.Workspace`, `game.Players`, `game.RunService`, `game.UserInputService`, plus the engine extras `game:Message(text, seconds)`, `game:Win(text)`, `game:Lose(text)`. |
| `script` | The Script object that is running. |
| `Players` | `Players.LocalPlayer` (the only player). |
| `RunService` | `RunService.Heartbeat` fires every frame with `dt`. |
| `UserInputService` | `IsKeyDown(key)`, `InputBegan`, `InputEnded`. |
| `Instance` | `Instance.new(className [, parent])` |
| `Vector3`, `Color3`, `Enum` | value types (below). |
| `task` | `task.wait`, `task.spawn`, `task.delay`, `task.defer`. |
| `time()` | seconds since Play started. |
| `typeof(v)` | `"Instance"`, `"Vector3"`, `"Color3"`, or the Lua type name. |
| `require(name)` | loads `scripts/name.lua` from the scene folder (dots are folders: `require("lib.util")` loads `scripts/lib/util.lua`). The module runs once; its return value is cached. A module cannot yield. |
| `print`, `warn` | log to Output (`warn` is shown in red). |

Standard Lua libraries available: `math` (plus `clamp`, `sign`, `round`, `lerp`), `string`, `table`, `utf8`, `coroutine`, and `os` limited to the time functions (`os.time`, `os.clock`, `os.date`, `os.difftime`). There is **no** `io`, `package`, `debug`, `load`, `dofile` or `loadfile`.

## Instances

Everything in the Explorer is an Instance. Class names: `Part` (any mesh), `Script`, and `Workspace`.

### Properties

| Property | Type | Notes |
|---|---|---|
| `Name` | string | names may repeat |
| `ClassName` | string | read-only |
| `Parent` | Instance or nil | `workspace` for top-level parts. See [creating and destroying](#creating-and-destroying). |
| `Position` | Vector3 | **local** to the parent (same as world position for top-level parts) |
| `WorldPosition` | Vector3 | read-only |
| `Rotation` | Vector3 | Euler angles in **degrees** |
| `Size` | Vector3 | the part's scale (the built-in cube/sphere are 1 unit across) |
| `Color` | Color3 | ignored while the part has a texture |
| `Anchored` | bool | anchored parts are never moved by physics |
| `CanCollide` | bool | off: things pass through, `Touched` still fires |
| `Visible` | bool | |
| `Bounciness` | number | 0 to 1, unanchored parts only |
| `Velocity` | Vector3 | unanchored parts and the player's Character |
| `Enabled` | bool | Scripts only, read-only while playing |

Parts only: a Script has no `Position`, `Size`, ... and reading them is an error. Reading an unknown name is an error too (`'Typo' is not a valid member of Part "Coin"`), so typos are not silently `nil`.

### Methods

`:GetChildren()` `:GetDescendants()` `:FindFirstChild(name [, recursive])` `:FindFirstChildOfClass(class)` `:IsA(class)` `:IsDescendantOf(ancestor)` `:GetFullName()` `:Clone()` `:Destroy()`

`IsA` accepts `"Instance"`, `"Part"`, `"BasePart"`, `"Script"`, `"Workspace"`.

### Events

`.Touched(other)`, `.ChildAdded(child)`, `.Destroying()`

Every event is a signal:

```lua
local connection = part.Touched:Connect(function(other) ... end)
part.Touched:Once(function(other) ... end)   -- fires one time
local other = part.Touched:Wait()            -- yields until it fires
connection:Disconnect()
print(connection.Connected)
```

Each handler runs in its own coroutine, so a handler may call `task.wait`.

`Touched` fires when a contact **begins**, with the other part (or the player's Character). Only parts somebody is listening to are tested. A part and its own descendants do not touch each other.

### Creating and destroying

```lua
local p = Instance.new("Part")          -- also "Sphere", "Wedge"
p.Name = "Crate"
p.Position = Vector3.new(0, 10, 0)
p.Size = Vector3.new(2, 2, 2)
p.Parent = workspace                    -- nothing exists in the world until it has a Parent

local copy = p:Clone()                  -- deep copy, includes scripts (they start when it enters the world)
copy.Parent = workspace

copy:Destroy()
```

* A new part starts **unanchored** (it falls) and not in the world until you set `Parent`. `Parent = nil` takes a script-created part back out of the world; parts that come from the scene can only be removed with `:Destroy()`.
* `:Destroy()` on a part from the scene **hides** it and disables its collision for the rest of the run; **Stop brings it back**. Parts created by scripts are really freed.
* Using an Instance after it was destroyed raises `this Instance was destroyed`; catch it with `pcall` if you need to.
* Everything the game changes (properties, hierarchy, destroyed or created parts) is undone on Stop.

## The player

`Players.LocalPlayer.Character` is an Instance for the player's box (name `"Player"`). It is what other parts receive in `Touched`:

```lua
if other == Players.LocalPlayer.Character then ... end
```

| | |
|---|---|
| `Character.Position`, `Character.Velocity` | teleport / launch (`Velocity = Vector3.new(0, 28, 0)` is a jump pad) |
| `Players.LocalPlayer:LoadCharacter()` | respawn at the SpawnPoint |
| `game:Message(text, seconds)` | big text in the middle of the screen |
| `game:Win(text)` / `game:Lose(text)` | ends the run and shows the text |

A part named `SpawnPoint` decides where the player appears.

## Time

```lua
task.wait(0.5)                    -- yields; returns the time actually waited
task.spawn(fn, ...)               -- runs fn(...) now, in a new coroutine
task.delay(2, fn, ...)            -- runs fn(...) after 2 seconds
task.defer(fn, ...)               -- runs fn(...) on the next frame
RunService.Heartbeat:Connect(function(dt) end)
local t = time()
```

All of these use game time, which stops while the game is paused. `wait(n)` is an alias of `task.wait(n)`.

## Input

```lua
if UserInputService:IsKeyDown(Enum.KeyCode.E) then ... end   -- also IsKeyDown("E")

UserInputService.InputBegan:Connect(function(input, processed)
    if input.KeyCode == Enum.KeyCode.Space then ... end
end)
```

`Enum.KeyCode` has `A`-`Z`, `Zero`-`Nine`, `Space`, `LeftShift`, `RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `Return`, `Tab`, `Escape`, `Backspace`, `Up`, `Down`, `Left`, `Right`, `F1`-`F12`. While you type in an editor text field, key presses are not delivered.

## Value types

```lua
local v = Vector3.new(1, 2, 3)           -- X Y Z, Magnitude, Unit
v + v, v - v, v * 2, 2 * v, v / 2, v * v, -v, v == v
v:Dot(w)  v:Cross(w)  v:Lerp(w, 0.5)
Vector3.zero  Vector3.one  Vector3.xAxis  Vector3.yAxis  Vector3.zAxis

local c = Color3.new(1, 0.5, 0)          -- 0..1
Color3.fromRGB(255, 128, 0)  Color3.fromHSV(0.1, 1, 1)
c.R  c.G  c.B  c:Lerp(other, 0.5)
```

Vectors and colors are immutable: `v.X = 5` is an error, make a new one.

## Limits and safety

* **Runaway scripts are stopped.** A script that runs about a second without yielding (for example `while true do end`) is stopped with an error, so the editor never freezes. Use `task.wait()` in loops.
* Each script has its **own globals**; they are not shared. Use a module (`require`) to share code.
* A script error stops that coroutine only; the rest of the game keeps running.
* Scripts cannot touch the file system, the network or the engine internals.

## Differences from Roblox

You will hit these first:

* **Lua 5.4, not Luau.** No type annotations, no `continue`, no compound assignment (`x = x + 1`, not `x += 1`), no `task.cancel`, and numbers print as `3.0` when they are floats (`print(part.Position.X)` shows `3.0`).
* **No CFrame yet.** Use `Position` and `Rotation` (degrees). `Position` is local to the parent; `WorldPosition` is read-only.
* **`Size` is the scale** and **`Visible`** replaces `Transparency`.
* **No Models, Folders, BindableEvents, RemoteEvents, TweenService, Humanoid, or physics joints.** Single player only (`Players.LocalPlayer`).
* Parts you place in the editor are **anchored by default** (Roblox's are not); `Instance.new("Part")` is unanchored like in Roblox.
* `Instance.new("Script")` is not supported; scripts are created in the editor.
* Reading or writing an unknown member is an error.
* Scene objects destroyed by a script come back on Stop.
* `game:Message`, `game:Win`, `game:Lose` and `workspace.Gravity` (negative = down) are engine additions.

## Examples

**Door that opens when the player is near**

```lua
local door = script.Parent
local closedY = door.Position.Y

RunService.Heartbeat:Connect(function(dt)
    local player = Players.LocalPlayer.Character
    local near = (player.Position - door.Position).Magnitude < 6
    local target = near and closedY + 4 or closedY
    door.Position = door.Position:Lerp(Vector3.new(door.Position.X, target, door.Position.Z), math.min(1, dt * 5))
end)
```

**Spawner**

```lua
local template = workspace.Crate           -- a part in the scene, anchored = false
while true do
    task.wait(2)
    local c = template:Clone()
    c.Position = Vector3.new(math.random(-5, 5), 12, math.random(-5, 5))
    c.Parent = workspace
    task.delay(6, function() c:Destroy() end)
end
```

**Collect everything to win**

```lua
local coins = 0
for _, coin in ipairs(workspace:GetChildren()) do
    if coin.Name == "Coin" then
        coin.Touched:Once(function(other)
            coins = coins + 1
            coin:Destroy()
            if coins == 5 then game:Win("All coins!") end
        end)
    end
end
```
