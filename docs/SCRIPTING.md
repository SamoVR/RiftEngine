# RIFT scripting reference

RIFT games are scripted in **Lua 5.4** with an API modelled on Roblox. If you
know Roblox Studio, most of it works the way you expect: `workspace`,
`game:GetService`, `Instance.new`, `part.Touched:Connect`, `task.wait`,
`TweenService`, `CFrame`, `UDim2`, `Enum`. The [differences](#differences-from-roblox)
are listed at the end.

- [Scripts and modules](#scripts-and-modules)
- [Client and server](#client-and-server)
- [Globals](#globals)
- [Instances](#instances)
- [Classes and properties](#classes-and-properties)
- [Events](#events)
- [Players and characters](#players-and-characters)
- [Physics](#physics)
- [Time and tasks](#time-and-tasks)
- [Input](#input)
- [Camera](#camera)
- [GUI](#gui)
- [Sound](#sound)
- [Services](#services)
- [Data types](#data-types)
- [Enums](#enums)
- [Lua libraries and limits](#lua-libraries-and-limits)
- [Differences from Roblox](#differences-from-roblox)
- [Examples](#examples)

## Scripts and modules

Code lives in `.lua` files in the scene's `scripts/` folder; the Explorer holds
objects that point at them. There are three kinds, like in Roblox:

| | Runs on | Typical place | Used for |
|---|---|---|---|
| **Script** | the server | ScriptService, inside parts | game rules, damage, coins, saving data |
| **LocalScript** | the client (the player's computer) | StarterPlayerScripts, StarterGui, StarterCharacterScripts | input, camera, GUI, effects only this player sees |
| **ModuleScript** | whoever `require`s it | ReplicatedStorage | shared code |

Insert one with Insert > Script / LocalScript / ModuleScript: it goes inside the
selected object, or (nothing selected) into ScriptService, StarterPlayerScripts
or ReplicatedStorage. See [Client and server](#client-and-server) for how the
two sides talk.

```lua
-- Inside a part: script.Parent is the part.
local part = script.Parent

part.Touched:Connect(function(hit)
    local humanoid = hit.Parent:FindFirstChildOfClass("Humanoid")
    if humanoid then
        humanoid.Health = 0
    end
end)
```

When the game starts, every enabled **Script** in Workspace, ScriptService or
under a player runs; ReplicatedStorage, ServerStorage, StarterGui and
StarterPlayer hold templates and nothing in them runs. **LocalScripts** run in
what belongs to the local player: its PlayerGui, PlayerScripts, Backpack and
character, which get copies of StarterGui, StarterPlayerScripts and
StarterCharacterScripts. A script starts when it enters such a place (a clone
parented to Workspace, for example) and stops when it is destroyed or disabled
(`script.Enabled = false`).

Each Script has its own global variables. To share code, use a **ModuleScript**:

```lua
-- ModuleScript "Util" in ReplicatedStorage
local Util = {}
function Util.round(x, step) return math.floor(x / step + 0.5) * step end
return Util
```

```lua
local Util = require(game:GetService("ReplicatedStorage").Util)
-- or by name:
local Util = require("Util")
```

`require(name)` loads `scripts/Name.lua` of the scene, or else `scripts/Name.lua`
of the project (modules shared by every scene). Dots are folders:
`require("ai.Patrol")` loads `scripts/ai/Patrol.lua`. A module runs once; every
`require` gets the same value. A module must return a value and cannot yield
while loading.

`print(...)` and `warn(...)` write to the Output panel. Errors show the script
and line (`Workspace.Door.Script:12: attempt to index nil`); clicking the line in
the Output opens the script there.

## Client and server

A RIFT game runs as two worlds, exactly like Roblox:

- The **server** owns the game. It runs the Scripts, simulates physics and
  decides what is true: health, coins, which doors are open.
- The **client** is the player's computer. It gets a copy of everything in
  Workspace, ReplicatedStorage, Lighting, StarterGui, StarterPlayer, Players and
  SoundService, kept up to date as the server changes it (**replication**). It
  runs the LocalScripts and handles input, the camera, the GUI and sounds.

ServerStorage and ScriptService never reach the client, so code and secrets in
them stay on the server. What a LocalScript creates or changes exists only on
that client: the server never sees it. The one exception is the player's own
character, which the client moves and the server follows (that is how walking
works without lag).

While playing in the editor, the **Client / Server** switch in the toolbar (and
at the top of the Explorer) picks which world you look at. The Client view is the
game as the player sees it. The Server view shows the server's world with the
editor camera: select and move things to see the change replicate. The Output
tags every line with **Client** or **Server**.

`game.Players.LocalPlayer` is the player on the client and `nil` on the server;
on the server use `Players.PlayerAdded` or `Players:GetPlayers()`.
`RunService:IsClient()` / `:IsServer()` tell where code runs. Client-only:
`UserInputService` input, `player:GetMouse()`, `RunService.RenderStepped` and
`:BindToRenderStep`, `FireServer` / `InvokeServer`. Server-only:
`DataStoreService`, `player:LoadCharacter()`, `FireClient` / `FireAllClients` /
`InvokeClient`. Calling one from the wrong side is an error that says so.

### RemoteEvent and RemoteFunction

Put remotes in ReplicatedStorage, where both sides can see them.

```lua
-- Script (server), ScriptService
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local buy = ReplicatedStorage.BuyItem          -- a RemoteEvent
local getPrice = ReplicatedStorage.GetPrice    -- a RemoteFunction

buy.OnServerEvent:Connect(function(player, item)
    -- Never trust the client: check everything it sends.
    if item == "Sword" and player.leaderstats.Coins.Value >= 10 then
        player.leaderstats.Coins.Value = player.leaderstats.Coins.Value - 10
        buy:FireClient(player, "You bought a sword!")
    end
end)

getPrice.OnServerInvoke = function(player, item)
    return item == "Sword" and 10 or nil
end
```

```lua
-- LocalScript (client), StarterPlayerScripts
local ReplicatedStorage = game:GetService("ReplicatedStorage")
local buy = ReplicatedStorage:WaitForChild("BuyItem")

buy.OnClientEvent:Connect(function(message)
    print(message)
end)

print("A sword costs", ReplicatedStorage.GetPrice:InvokeServer("Sword"))
buy:FireServer("Sword")
```

| | Client | Server |
|---|---|---|
| **RemoteEvent** | `:FireServer(...)`, `OnClientEvent` | `:FireClient(player, ...)`, `:FireAllClients(...)`, `OnServerEvent(player, ...)` |
| **RemoteFunction** | `:InvokeServer(...)` (waits for the result), `OnClientInvoke` | `:InvokeClient(player, ...)`, `OnServerInvoke = function(player, ...)` |

The server's handlers get the sending `player` first. Arguments are copied:
numbers, strings, booleans, tables (without functions or metatables),
Instances (`nil` if the other side does not have them), Vector3, Vector2,
CFrame, Color3, UDim, UDim2 and EnumItems. Messages arrive on the next frame.
An error in an `OnServerInvoke` handler is raised in the caller.

**BindableEvent** (`:Fire(...)`, `Event`) and **BindableFunction**
(`:Invoke(...)`, `OnInvoke`) do the same between scripts on the **same** side;
their arguments are passed as they are, not copied.

## Globals

| Name | |
|---|---|
| `game` | The DataModel: `game:GetService("Name")`, `game.Workspace`, `game.Players`, ... and the RIFT extras `game:Message`, `game:Win`, `game:Lose` |
| `workspace` | The Workspace service |
| `script` | The Script (or ModuleScript) that is running |
| `Players`, `RunService`, `UserInputService`, `TweenService` | Shortcuts for the services (RIFT extra) |
| `Instance.new(className [, parent])` | Creates an instance |
| `Vector3`, `Vector2`, `CFrame`, `Color3`, `UDim`, `UDim2`, `Enum`, `RaycastParams`, `TweenInfo` | Data types |
| `task` | `task.wait`, `task.spawn`, `task.defer`, `task.delay`, `task.cancel` |
| `wait(s)`, `spawn(f)`, `delay(s, f)` | Old names of the task functions |
| `time()`, `elapsedTime()` | Seconds since the game started (stops while paused) |
| `tick()` | Seconds since 1970 (wall clock) |
| `typeof(v)` | `"Instance"`, `"Vector3"`, `"CFrame"`, `"EnumItem"`, ... or the Lua type |
| `require(module)` | Loads a ModuleScript |
| `shared` | A table shared by all scripts |
| `print`, `warn` | Output |

`game:GetService` knows `Workspace`, `Players`, `Lighting`, `ReplicatedStorage`,
`ServerStorage` (server only), `ScriptService` / `ServerScriptService`,
`StarterGui`, `StarterPlayer`, `SoundService`, `RunService`, `UserInputService`,
`TweenService`, `Debris`, `CollectionService`, `HttpService` and `DataStoreService`.

`game:Message(text [, seconds])` shows a banner (3 s by default).
`game:Win([text])` and `game:Lose([text])` end the run with a win or lose screen.

## Instances

Everything in the Explorer is an Instance. Members are accessed by name:
properties (`part.Color`), methods (`part:Clone()`), events (`part.Touched`) and,
when nothing else matches, children (`workspace.Map.Door`). Reading or writing a
name that does not exist is an error (`'Colour' is not a valid member of Part
"Workspace.Brick"`), so typos never silently give `nil`.

### Members every instance has

| | |
|---|---|
| `Name` | string |
| `ClassName` | read-only |
| `Parent` | Instance or nil |
| `Archivable` | false: not saved, not cloned |
| `:Clone()` | Copy with all descendants, not parented (nil when Archivable is false) |
| `:Destroy()` | Removes it and its descendants for the rest of the run. `Parent` is locked afterwards |
| `:ClearAllChildren()` | Destroys the children |
| `:GetChildren()`, `:GetDescendants()` | Arrays |
| `:FindFirstChild(name [, recursive])` | Instance or nil |
| `:FindFirstChildOfClass(class)`, `:FindFirstChildWhichIsA(class [, recursive])` | |
| `:FindFirstAncestor(name)`, `:FindFirstAncestorOfClass(class)`, `:FindFirstAncestorWhichIsA(class)` | |
| `:FindFirstDescendant(name)` | |
| `:WaitForChild(name [, timeout])` | Yields until the child exists (nil after the timeout) |
| `:IsA(class)` | Includes the base classes `BasePart`, `GuiObject`, `GuiButton`, `Light`, `ValueBase`, `LuaSourceContainer`, `PVInstance` |
| `:IsDescendantOf(x)`, `:IsAncestorOf(x)` | |
| `:GetFullName()` | `"Workspace.Map.Door"` |
| `:GetAttribute(name)`, `:SetAttribute(name, value)`, `:GetAttributes()` | Custom values (nil, boolean, number, string, Vector3, Vector2, Color3, UDim2) |
| `:GetAttributeChangedSignal(name)`, `:GetPropertyChangedSignal(prop)` | Signals |
| `:AddTag(tag)`, `:RemoveTag(tag)`, `:HasTag(tag)`, `:GetTags()` | Tags (see CollectionService) |

Attributes and tags set in the editor's Properties panel are visible to scripts.

### Creating and destroying

```lua
local crate = Instance.new("Part")
crate.Name = "Crate"
crate.Size = Vector3.new(2, 2, 2)
crate.Position = Vector3.new(0, 20, 0)
crate.Parent = workspace          -- nothing is in the world until it has a Parent

local copy = crate:Clone()        -- deep copy; scripts inside start when it enters the world
copy.Parent = workspace
game:GetService("Debris"):AddItem(copy, 5)   -- destroyed after 5 seconds

crate:Destroy()
```

Parts made with `Instance.new` are **unanchored** (they fall), like Roblox;
parts placed in the editor are anchored. Scripts cannot be created with
`Instance.new` (their code is a file): put a Script in ReplicatedStorage and
`Clone()` it.

A destroyed Instance is gone from the world for good. Scripts that still hold it
can read its last properties (handy for messages: `part.Name`), but setting its
`Parent` is an error.

Everything a game changes, creates or destroys exists only while it runs: Stop
returns the editor's scene exactly as it was.

## Classes and properties

Positions are in **studs**, rotations in **degrees** (`Rotation` / `Orientation`)
or radians (`CFrame` functions, like Roblox). `Position` and `Rotation` are
relative to the parent part or model; `CFrame` and `WorldPosition` are in world
space. A child's `Size` is never scaled by its parent.

**Part, SpawnLocation, MeshPart** (`BasePart`)

| Property | |
|---|---|
| `Position`, `Rotation` (= `Orientation`), `Size` | Vector3 |
| `CFrame` | World position and rotation |
| `WorldPosition` | read-only Vector3 |
| `Shape` | `Enum.PartType` Block, Ball, Cylinder, Wedge, CornerWedge (Part only) |
| `Color` | Color3 |
| `Material` | `Enum.Material` Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete |
| `Transparency`, `Reflectance` | 0..1 |
| `Texture`, `TextureScale` | Image file drawn on the faces, repeats per stud (0 = stretch) |
| `CastShadow`, `Visible` | |
| `Anchored`, `CanCollide`, `CanTouch` | |
| `Density`, `Friction`, `Elasticity` | |
| `Velocity` (= `AssemblyLinearVelocity`), `AngularVelocity` (degrees/s), `AssemblyAngularVelocity` (radians/s) | Runtime |
| `Enabled` | SpawnLocation: can players spawn here |
| `MeshId` | MeshPart: the model file (.obj, .gltf, .glb) |

Methods: `:GetTouchingParts()`, `:ApplyImpulse(v)`, `:ApplyAngularImpulse(v)`,
`:GetMass()`, `:GetPivot()`, `:PivotTo(cframe)`, `:GetBoundingBox()`. Events:
`Touched(otherPart)`, `TouchEnded(otherPart)`.

**Model**: `Position`, `Rotation`, `CFrame`, `PrimaryPart`; `:GetPivot()`,
`:PivotTo(cf)`, `:SetPrimaryPartCFrame(cf)`, `:MoveTo(position)`,
`:GetBoundingBox()` (CFrame, size), `:GetExtentsSize()`. Moving a model moves
everything inside it.

**Folder**: groups instances; has no position.

**PointLight / SpotLight**: `Enabled`, `Color`, `Brightness`, `Range`, `Shadows`,
`Position`; SpotLight also `Rotation` and `Angle` (degrees; it shines along its
-Z axis). Inside a part, a light moves with it.

**Sound**: see [Sound](#sound).

**Humanoid**: see [Players and characters](#players-and-characters).

**Value objects** (`IntValue`, `NumberValue`, `StringValue`, `BoolValue`,
`Vector3Value`, `Color3Value`): `Value`. Their `Changed` event passes the new value.

**Workspace**: `Gravity` (studs/s², default 30), `FallenPartsDestroyHeight`
(parts below it are destroyed; default -60), `CurrentCamera`; methods
`:Raycast`, `:GetPartBoundsInBox`, `:GetPartsInPart` (see [Physics](#physics)).

**Lighting**: `ClockTime` (0-24, moves the sun), `Brightness`, `SunColor`,
`GlobalShadows`, `Ambient`, `SkyColor`, `HorizonColor`, `ExposureCompensation`,
`FogColor`, `FogStart`, `FogEnd`.

**StarterPlayer**: `WalkSpeed`, `JumpPower`, `CharacterColor`, `CameraMode`
(`Enum.CameraMode` ThirdPerson / FirstPerson), `CameraMaxZoomDistance`.

**SoundService**: `Volume` (master volume).

**Script**, **LocalScript**: `Enabled`. **ModuleScript**: (none).

**RemoteEvent**, **RemoteFunction**, **BindableEvent**, **BindableFunction**:
see [Client and server](#client-and-server).

GUI classes are under [GUI](#gui).

## Events

Events are signals:

```lua
local connection = part.Touched:Connect(function(hit) print(hit.Name) end)
part.Touched:Once(function(hit) print("first touch") end) -- disconnects itself after one call
local hit = part.Touched:Wait()              -- yields until the next time it fires
connection:Disconnect()
print(connection.Connected)
```

Handlers are **deferred**: they run right after the code that caused the event
yields or finishes, never in the middle of it. Each handler runs in its own
thread, so it may call `task.wait`.

| Event | On | Arguments |
|---|---|---|
| `Changed` | every instance | property name (value objects: the new value) |
| `ChildAdded`, `ChildRemoved` | every instance | child |
| `DescendantAdded`, `DescendantRemoving` | every instance | descendant |
| `AncestryChanged` | every instance | instance, new parent |
| `AttributeChanged` | every instance | attribute name |
| `Destroying` | every instance | |
| `Touched`, `TouchEnded` | parts | the other part |
| `Died`, `HealthChanged(health)`, `Jumping`, `Running(speed)` | Humanoid | |
| `CharacterAdded`, `CharacterRemoving` | Player | character |
| `PlayerAdded` | Players | player |
| `Ended` | Sound | |
| `Activated`, `MouseButton1Click`, `MouseButton1Down`, `MouseButton1Up` | TextButton, ImageButton | |
| `MouseEnter`, `MouseLeave` | GUI elements | |

`:GetPropertyChangedSignal("Position")` fires when that one property changes.

## Players and characters

A published game has one player. When testing in the editor you can start
several (the players button next to Play): each gets a character, a client with
its own LocalScripts and GUI, and a slot in `Players:GetPlayers()` on the server.
`game.Players.LocalPlayer` is the client's own player. `Players.PlayerAdded`
fires for every player after every script had the chance to connect, so the
usual pattern works:

```lua
local Players = game:GetService("Players")
Players.PlayerAdded:Connect(function(player)
    player.CharacterAdded:Connect(function(character)
        print(player.Name .. " spawned")
    end)
end)
```

**Player**: `Name`, `DisplayName`, `UserId` (1), `Character`, and the folders
`PlayerGui` (copies of StarterGui), `Backpack` and `PlayerScripts` (copies of
StarterPlayerScripts, client only); `:LoadCharacter()` (respawn now, server),
`:GetMouse()` (client), `:Kick([message])` (ends the game).

**Players**: `LocalPlayer`, `:GetPlayers()`, `:GetPlayerFromCharacter(model)`.

`RemoteEvent:FireClient(player, ...)` reaches only that player's client,
`:FireAllClients(...)` every client, and `OnServerEvent` tells the server which
player fired.

The **character** is a Model named after the player, in Workspace, with a
`Humanoid`, a `HumanoidRootPart` (the collision capsule) and `Head`, `Torso`,
`Left Arm`, `Right Arm`, `Left Leg`, `Right Leg`. It appears on an enabled
SpawnLocation (a random one if there are several). Put your own Model named
**StarterCharacter** (with a Humanoid and a HumanoidRootPart) in StarterPlayer to
replace it. Scripts and LocalScripts in StarterPlayer.StarterCharacterScripts are
copied into every new character.

Touch handlers usually find the character from the part that touched:

```lua
part.Touched:Connect(function(hit)
    local humanoid = hit.Parent:FindFirstChildOfClass("Humanoid")
    local player = game.Players:GetPlayerFromCharacter(hit.Parent)
end)
```

**Humanoid**: `Health`, `MaxHealth`, `WalkSpeed`, `JumpPower`, `MoveDirection`
(read-only), `Jump` (set to true to jump), `RootPart`; `:TakeDamage(amount)`,
`:MoveTo(position)` (walks there). When `Health` reaches 0 the character falls
apart, `Died` fires and the player respawns 3 seconds later.

Moving the character: set `character.HumanoidRootPart.CFrame` (teleport) or
`AssemblyLinearVelocity` (a push or jump pad).

**Leaderboard**: a Folder named `leaderstats` inside the Player, with IntValue /
NumberValue / StringValue / BoolValue children, is shown in the top right corner.

## Tools and the Backpack

A **Tool** is something a character can hold: a sword, a flashlight, a
building tool. Tools in **StarterPack** are copied into every player's
`Backpack` when the character spawns; the backpack bar at the bottom of the
screen shows them, and the number keys (or a click) equip them. A tool needs a
part named **Handle** (unless `RequiresHandle` is false): the character's right
hand holds it, placed by `GripPos` and `GripRotation`.

```lua
-- A Script inside the Tool (Insert > Gameplay > Sword has a full example)
local tool = script.Parent
tool.Activated:Connect(function()        -- clicked while equipped
    local character = tool.Parent
    print(character.Name .. " swings")
end)
tool.Equipped:Connect(function() print("out") end)
tool.Unequipped:Connect(function() print("away") end)
```

**Tool**: `Enabled`, `RequiresHandle`, `CanBeDropped` (Backspace drops it),
`ManualActivationOnly` (clicks do not activate; call `:Activate()`), `ToolTip`,
`TextureId` (the icon in the backpack bar), `GripPos`, `GripRotation`;
events `Equipped`, `Unequipped`, `Activated`, `Deactivated`. An equipped tool is
a child of the character; an unequipped one lives in the player's Backpack.
`Humanoid:EquipTool(tool)` and `Humanoid:UnequipTools()` do the same from code.
A dropped tool lies in Workspace until a character touches it.

**Built-in GUI**: `StarterGui:SetCoreGuiEnabled(Enum.CoreGuiType.Backpack, false)`
hides the backpack bar (`PlayerList` the leaderboard, `All` both);
`:GetCoreGuiEnabled(type)` reads it. Call it from a LocalScript.

## Terrain

`workspace.Terrain` is smooth voxel terrain, like Roblox's: a grid of 2-stud
voxels, each with a material and how full it is. Make it with the **Terrain
Editor** (View > Terrain Editor) or from scripts:

```lua
local terrain = workspace.Terrain
terrain:FillBlock(CFrame.new(0, -4, 0), Vector3.new(200, 8, 200), Enum.Material.Grass)
terrain:FillBall(Vector3.new(20, 6, 0), 10, Enum.Material.Rock)
terrain:FillCylinder(CFrame.new(-30, 0, 0), 4, 12, Enum.Material.Water) -- height, radius
terrain:FillBall(Vector3.new(0, 0, 0), 6, Enum.Material.Air)             -- Air carves
terrain:SetMaterialColor(Enum.Material.Grass, Color3.fromRGB(110, 160, 70))
terrain:Clear()
```

Terrain materials: Grass, LeafyGrass, Ground, Mud, Sand, Sandstone, Rock,
Slate, Basalt, Limestone, CrackedLava, Salt, Snow, Glacier, Ice, Asphalt,
Pavement, Concrete, Brick, Cobblestone, WoodPlanks and Water (others raise an
error). Characters stand on terrain and swim in terrain water (Space swims
up). `workspace:Raycast` hits terrain: `result.Instance` is the Terrain and
`result.Material` the voxel's material. The Terrain's `WaterColor` and
`WaterTransparency` properties colour the water. Changes made by the server
reach every client.

## Effects

An **Attachment** is a point on a part (`Position` and `Orientation` in the
part's own space, `WorldPosition` in the world). Effects use them:

- **ParticleEmitter**: in a part, particles come out of the face named by `EmissionDirection` (anywhere on it); in an Attachment, out of its point. `Rate` per second, `Lifetime` and `Speed` (NumberRange), `SpreadAngle`, `Acceleration`, `Drag`, `Rotation` / `RotSpeed`, and over each particle's life `Color` (ColorSequence), `Size` and `Transparency` (NumberSequence). `LightEmission` 1 makes them add light (fire, magic), `LockedToPart` moves them with the part, `Texture` is an image (without one: a soft round dot). `:Emit(n)` sends out n at once (also with `Enabled` off), `:Clear()` removes them all.
- **Beam**: a band from `Attachment0` to `Attachment1`, bent along each attachment's X axis by `CurveSize0` / `CurveSize1`, `Width0` / `Width1`, `Segments`, `FaceCamera`, and along its length `Color` and `Transparency`. A `Texture` repeats with `TextureMode` / `TextureLength` and scrolls with `TextureSpeed`.
- **Trail**: a ribbon left behind as its two attachments move (put them at the top and bottom of a sword's blade). `Lifetime`, `MinLength`, `MaxLength`, `WidthScale`, and `Color` / `Transparency` from the newest end (0) to the oldest (1).

```lua
local emitter = Instance.new("ParticleEmitter")
emitter.Color = ColorSequence.new(Color3.fromRGB(255, 200, 80), Color3.fromRGB(255, 60, 20))
emitter.Size = NumberSequence.new(1, 0)
emitter.Lifetime = NumberRange.new(0.5, 1)
emitter.Speed = NumberRange.new(8)
emitter.SpreadAngle = Vector2.new(180, 180)
emitter.LightEmission = 1
emitter.Enabled = false
emitter.Parent = part
emitter:Emit(40)   -- a burst
```

Effects are only drawn, never simulated by physics: every client draws its own
particles, and `:Emit` on the server reaches all of them.

## Physics

Anchored parts never move by themselves (scripts can still move them).
Unanchored parts fall, collide and
bounce; unanchored parts inside an unanchored part or model are welded to it and
move as one body. `CanCollide = false` makes a part pass-through (Touched still
fires unless `CanTouch` is false).

```lua
part:ApplyImpulse(Vector3.new(0, 500, 0))   -- mass x velocity change
print(part:GetMass())
part.AssemblyLinearVelocity = Vector3.new(0, 30, 0)
```

**Touched** fires when another part starts touching this one (resting contact
counts; parts welded into the same body do not touch each other), **TouchEnded**
when they separate. `part:GetTouchingParts()` lists what touches it now.

**Raycasts**:

```lua
local params = RaycastParams.new()
params.FilterType = Enum.RaycastFilterType.Exclude
params.FilterDescendantsInstances = { character }
local result = workspace:Raycast(origin, direction * 100, params)
if result then
    print(result.Instance, result.Position, result.Normal, result.Distance, result.Material)
end
```

`RaycastParams`: `FilterDescendantsInstances`, `FilterType` (Exclude / Include),
`RespectCanCollide`, `:AddToFilter(instances)`.

`workspace:GetPartBoundsInBox(cframe, size)` and `workspace:GetPartsInPart(part)`
return the parts in a box.

## Time and tasks

```lua
task.wait(0.5)                 -- yields; returns the time actually waited
task.spawn(fn, 1, 2)           -- runs fn(1, 2) now in a new thread
task.defer(fn)                 -- runs fn at the end of this step
task.delay(2, fn)              -- runs fn in 2 seconds
local thread = task.delay(5, explode)
task.cancel(thread)
```

`RunService` events (all receive `dt`, the frame time in seconds):

| | |
|---|---|
| `RenderStepped(dt)` | Before physics, every frame |
| `Stepped(time, dt)` | Before physics |
| `Heartbeat(dt)` | After physics: the usual place for per-frame code |

`RunService:BindToRenderStep(name, priority, fn)` / `:UnbindFromRenderStep(name)`,
`:IsStudio()` (true in the editor), `:IsRunning()`, `:IsClient()`, `:IsServer()`.

Game time stops while the game is paused.

## Input

Input is the client's: use it from a LocalScript. On the server nothing is ever
pressed and the input events never fire.

```lua
local UserInputService = game:GetService("UserInputService")

UserInputService.InputBegan:Connect(function(input, processedByGui)
    if processedByGui then return end
    if input.KeyCode == Enum.KeyCode.E then
        print("E pressed")
    elseif input.UserInputType == Enum.UserInputType.MouseButton1 then
        print("click at", input.Position)
    end
end)

if UserInputService:IsKeyDown(Enum.KeyCode.LeftShift) then
    print("sprinting")
end
```

**UserInputService**: `InputBegan`, `InputEnded`, `InputChanged` (mouse movement
and wheel) with `(input, processedByGui)`; `:IsKeyDown(key)`,
`:IsMouseButtonPressed(Enum.UserInputType.MouseButton1)`, `:GetMouseLocation()`,
`:GetMouseDelta()`, `:GetKeysPressed()`, `MouseBehavior`
(`Enum.MouseBehavior.LockCenter` locks the mouse for first-person controls).

**InputObject**: `KeyCode`, `UserInputType`, `UserInputState`, `Position`, `Delta`.

**Mouse** (`player:GetMouse()`): `Hit` (CFrame of the point under the mouse),
`Target` (the part under it), `X`, `Y`, `ViewSizeX`, `ViewSizeY`, `UnitRay`;
events `Button1Down`, `Button1Up`, `Button2Down`, `Button2Up`, `Move`,
`WheelForward`, `WheelBackward`.

Key names: `A`-`Z`, `Zero`-`Nine`, `F1`-`F12`, `KeypadZero`-`KeypadNine`,
`Space`, `Return`, `Tab`, `Escape`, `Backspace`, `Delete`, `Insert`, `Home`,
`End`, `PageUp`, `PageDown`, `Up`, `Down`, `Left`, `Right`, `LeftShift`,
`RightShift`, `LeftControl`, `RightControl`, `LeftAlt`, `RightAlt`, `CapsLock`,
`Minus`, `Equals`, `LeftBracket`, `RightBracket`, `Semicolon`, `Quote`, `Comma`,
`Period`, `Slash`, `BackSlash`, `Backquote`. `IsKeyDown` also accepts the name
as a string (`IsKeyDown("E")`).

Keys only reach the game while its view has the keyboard (in the editor: after
clicking the viewport).

## Camera

`workspace.CurrentCamera` is the player's camera; control it from a LocalScript.

| | |
|---|---|
| `CFrame` | Where the camera is and looks |
| `CameraType` | `Enum.CameraType.Custom` (follows the character), `Scriptable` (you set CFrame), `Fixed` |
| `FieldOfView` | Vertical, degrees |
| `Focus`, `CameraSubject`, `ViewportSize` | read-only |
| `:ScreenPointToRay(x, y)`, `:ViewportPointToRay(x, y)` | `{ Origin, Direction }` |
| `:WorldToScreenPoint(v)`, `:WorldToViewportPoint(v)` | Vector3 (x, y in pixels, z depth) and whether it is on screen |

```lua
local camera = workspace.CurrentCamera
camera.CameraType = Enum.CameraType.Scriptable
camera.CFrame = CFrame.lookAt(Vector3.new(0, 30, 40), Vector3.new(0, 0, 0))
```

In Custom mode the player turns the camera with the right mouse button and zooms
with the wheel; walls pull the camera in. StarterPlayer.CameraMode =
FirstPerson gives a first-person camera.

## GUI

ScreenGuis in StarterGui are templates: when the character spawns, they are
copied into `player.PlayerGui`, and those copies are what the player sees (a
ScreenGui with `ResetOnSpawn = false` keeps its copy across respawns). Change the
GUI from a LocalScript inside the ScreenGui (`script.Parent`) or through
`player.PlayerGui`; the server can change PlayerGui too and it replicates.

| Class | |
|---|---|
| `ScreenGui` | `Enabled`, `DisplayOrder`, `ResetOnSpawn` |
| `Frame` | Rectangle |
| `TextLabel`, `TextButton` | + `Text`, `TextColor3`, `TextSize`, `TextXAlignment`, `TextYAlignment`, `TextWrapped` |
| `ImageLabel`, `ImageButton` | + `Image`, `ImageColor3`, `ImageTransparency`, `ScaleType` (Stretch, Fit, Crop) |

Every element has `Position` and `Size` (UDim2), `AnchorPoint` (Vector2),
`ZIndex`, `Visible`, `BackgroundColor3`, `BackgroundTransparency`,
`CornerRadius`, `BorderSizePixel`, `BorderColor3`.

```lua
-- LocalScript in StarterPlayerScripts
local player = game.Players.LocalPlayer
local gui = Instance.new("ScreenGui")
gui.Parent = player.PlayerGui

local button = Instance.new("TextButton")
button.Size = UDim2.fromOffset(200, 50)
button.Position = UDim2.fromScale(0.5, 0.9)
button.AnchorPoint = Vector2.new(0.5, 0.5)
button.Text = "Jump!"
button.Parent = gui

button.Activated:Connect(function()
    local humanoid = player.Character.Humanoid
    humanoid.Jump = true
end)
```

Clicks on GUI elements are reported to `InputBegan` with `processedByGui = true`.

## Sound

**Sound**: `SoundId` (an audio file: .wav, .mp3, .ogg, .flac), `Volume`,
`PlaybackSpeed`, `Looped`, `Playing` (set in the editor: plays when the game
starts), `RollOffMaxDistance`; read-only `IsPlaying`, `TimePosition`,
`TimeLength`. Methods `:Play()`, `:Stop()`, `:Pause()`, `:Resume()`; event `Ended`.

A Sound inside a part is positional: it gets quieter with distance and comes from
the part's direction. Anywhere else (SoundService, a GUI) it plays flat.

## Services

### TweenService

```lua
local TweenService = game:GetService("TweenService")
local info = TweenInfo.new(2, Enum.EasingStyle.Quad, Enum.EasingDirection.Out, -1, true)
local tween = TweenService:Create(part, info, { Position = part.Position + Vector3.new(0, 10, 0), Color = Color3.new(1, 0, 0) })
tween:Play()
tween.Completed:Connect(function(state) print(state) end)
```

`TweenInfo.new(time, easingStyle, easingDirection, repeatCount, reverses, delayTime)`;
repeatCount -1 repeats forever. Any number, Vector3, Vector2, Color3, UDim2 or
CFrame property can be tweened. `Tween`: `:Play()`, `:Pause()`, `:Cancel()`,
`PlaybackState`, `Completed`. A new tween of the same property replaces the old
one. `TweenService:GetValue(alpha, style, direction)` gives the eased value.

### Debris

`Debris:AddItem(instance, seconds)` destroys the instance later (10 s by default).

### CollectionService

`GetTagged(tag)`, `HasTag`, `AddTag`, `RemoveTag`, `GetTags`,
`GetInstanceAddedSignal(tag)`, `GetInstanceRemovedSignal(tag)`. Tags added in the
editor work too:

```lua
local CollectionService = game:GetService("CollectionService")
for _, lava in ipairs(CollectionService:GetTagged("Lava")) do
    lava.Touched:Connect(function(hit)
        local h = hit.Parent:FindFirstChildOfClass("Humanoid")
        if h then h:TakeDamage(10) end
    end)
end
```

### DataStoreService

Saves data between runs, on the player's computer:

```lua
local store = game:GetService("DataStoreService"):GetDataStore("Progress")
local best = store:GetAsync("BestTime") or math.huge
store:SetAsync("BestTime", math.min(best, time()))
store:IncrementAsync("Runs", 1)
store:UpdateAsync("Coins", function(old) return (old or 0) + 5 end)
store:RemoveAsync("OldKey")
```

Values can be nil, booleans, numbers, strings and tables of them. Each data store
is a JSON file: `%APPDATA%\RIFT\datastores\<game>\` when playing in the editor,
`%APPDATA%\RIFT Games\<game>\datastores\` in a built game.

### HttpService

`JSONEncode(value)`, `JSONDecode(text)`, `GenerateGUID([wrapInBraces])`. There
is no network access.

## Data types

All data types are immutable values: `v.X = 1` is an error, make a new one.

**Vector3**: `Vector3.new(x, y, z)`, `Vector3.zero`, `.one`, `.xAxis`, `.yAxis`,
`.zAxis`; `X`, `Y`, `Z`, `Magnitude`, `Unit`; `:Dot(v)`, `:Cross(v)`,
`:Lerp(v, t)`, `:Angle(v)`, `:Abs()`, `:Floor()`, `:Ceil()`, `:Sign()`,
`:Min(v)`, `:Max(v)`, `:FuzzyEq(v [, eps])`; `+ - * /` with vectors and numbers,
unary `-`, `==`.

**Vector2**: `Vector2.new(x, y)`, `.zero`, `.one`; `X`, `Y`, `Magnitude`, `Unit`;
`:Dot`, `:Lerp`; arithmetic.

**CFrame** (position + rotation):

| | |
|---|---|
| `CFrame.new()`, `CFrame.new(pos)`, `CFrame.new(x, y, z)`, `CFrame.new(pos, lookAt)`, `CFrame.new(x, y, z, r00 ... r22)` | |
| `CFrame.lookAt(pos, target [, up])` | |
| `CFrame.Angles(rx, ry, rz)` = `fromEulerAnglesXYZ`, `fromEulerAnglesYXZ`, `fromOrientation`, `fromAxisAngle(axis, angle)` | radians |
| `CFrame.identity` | |
| `Position` (`p`), `X`, `Y`, `Z`, `LookVector`, `RightVector`, `UpVector`, `XVector`, `YVector`, `ZVector`, `Rotation` | |
| `:Inverse()`, `:Lerp(cf, t)`, `:ToWorldSpace(cf)`, `:ToObjectSpace(cf)`, `:PointToWorldSpace(v)`, `:PointToObjectSpace(v)`, `:VectorToWorldSpace(v)`, `:VectorToObjectSpace(v)`, `:ToEulerAnglesXYZ()`, `:ToOrientation()`, `:GetComponents()` | |
| `cf * cf`, `cf * v`, `cf + v`, `cf - v` | |

```lua
part.CFrame = part.CFrame * CFrame.Angles(0, math.rad(90), 0)   -- turn 90 degrees
```

**Color3**: `Color3.new(r, g, b)` (0..1), `Color3.fromRGB(255, 128, 0)`,
`Color3.fromHSV(h, s, v)`, `Color3.fromHex("#FF8000")`; `R`, `G`, `B`;
`:Lerp(c, t)`, `:ToHSV()`, `:ToHex()`.

**UDim**: `UDim.new(scale, offset)`; `Scale`, `Offset`.
**UDim2**: `UDim2.new(xScale, xOffset, yScale, yOffset)`, `UDim2.new(udimX, udimY)`,
`UDim2.fromScale(x, y)`, `UDim2.fromOffset(x, y)`; `X`, `Y`, `Width`, `Height`;
`:Lerp(u, t)`; `+`, `-`.

**NumberRange**: `NumberRange.new(min [, max])`; `Min`, `Max` (particle
lifetimes and speeds are picked at random between them).

**NumberSequence**: a number that changes over 0..1 (a particle's life, along a
beam). `NumberSequence.new(n)`, `NumberSequence.new(from, to)`,
`NumberSequence.new({ NumberSequenceKeypoint.new(time, value [, envelope]), ... })`
(2 to 20 points, the first at time 0 and the last at 1); `Keypoints`. A keypoint
has `Time`, `Value`, `Envelope` (random spread around the value).

**ColorSequence**: the same with colours. `ColorSequence.new(color)`,
`ColorSequence.new(from, to)`, `ColorSequence.new({ ColorSequenceKeypoint.new(time, color), ... })`;
`Keypoints` (each has `Time` and `Value`). Properties of these types also accept
a plain number or Color3 (`emitter.Transparency = 0.5`).

**EnumItem**: `Name`, `Value`, `EnumType`, `:IsA(typeName)`. Properties also
accept the item's name as a string (`part.Material = "Neon"`).

## Enums

`Enum.<Type>.<Item>`, and `Enum.<Type>:GetEnumItems()`:

| Type | Items |
|---|---|
| `PartType` | Block, Ball, Cylinder, Wedge, CornerWedge |
| `Material` | Plastic, SmoothPlastic, Metal, Neon, Glass, Wood, Concrete |
| `KeyCode` | see [Input](#input) |
| `UserInputType` | None, Keyboard, MouseButton1, MouseButton2, MouseButton3, MouseMovement, MouseWheel |
| `UserInputState` | Begin, Change, End, Cancel, None |
| `EasingStyle` | Linear, Sine, Quad, Cubic, Quart, Quint, Exponential, Circular, Back, Bounce, Elastic |
| `EasingDirection` | In, Out, InOut |
| `PlaybackState` | Begin, Playing, Paused, Completed, Cancelled, Delayed |
| `RaycastFilterType` | Exclude, Include (also Blacklist, Whitelist) |
| `MouseBehavior` | Default, LockCenter, LockCurrentPosition |
| `CameraType` | Custom, Scriptable, Fixed |
| `CameraMode` | ThirdPerson, FirstPerson |
| `TextXAlignment` / `TextYAlignment` | Left, Center, Right / Top, Center, Bottom |
| `ScaleType` | Stretch, Fit, Crop |

## Lua libraries and limits

Available: `math`, `string`, `table`, `utf8`, `coroutine`, and from `os` only the
time functions (`os.time`, `os.clock`, `os.date`, `os.difftime`). Not available:
`io`, `package`, `debug`, `load`, `loadstring`, `dofile`, `loadfile`,
`collectgarbage`, `string.dump` and the `raw*` functions. Scripts cannot reach
files, the network or the engine's internals.

RIFT adds the Luau helpers people use most: `math.clamp`, `math.sign`,
`math.round`, `math.lerp`, `math.map`, `table.find`, `table.clear`,
`table.clone`, `string.split`.

- A script that runs for about a second without yielding (`while true do end`) is stopped with an error, so the game never freezes. Put `task.wait()` in long loops.
- All scripts together may use 512 MB of memory.
- An error stops only the thread it happened in; the rest of the game keeps running.

## Differences from Roblox

- **Lua 5.4, not Luau.** No type annotations, no `continue`, no compound assignment (write `x = x + 1`, not `x += 1`), no string interpolation with backticks. Integer division is `//`.
- **One player.** The client/server split works like Roblox (Scripts, LocalScripts, remotes, replication), but there is always exactly one player and both sides run in the same program, so remotes are never lost and arrive one frame later.
- **Units**: gravity is 30 studs/s² (Roblox: 196.2) and the default character walks at 8 and jumps with 12, which feel similar at RIFT's scale.
- `Instance.new("Script")` is not supported (code lives in files): clone a Script instead.
- Reading or writing a member that does not exist is an error.
- Only the classes listed above exist (no constraints, ...). MeshParts collide as their bounding box.
- DataStores save to local files and never fail or throttle; `HttpService` has no network functions.
- `game:Message`, `game:Win`, `game:Lose` and the global service shortcuts are RIFT additions.

## Examples

**Kill brick**

```lua
script.Parent.Touched:Connect(function(hit)
    local humanoid = hit.Parent:FindFirstChildOfClass("Humanoid")
    if humanoid then humanoid.Health = 0 end
end)
```

**Spinning part**

```lua
local part = script.Parent
game:GetService("RunService").Heartbeat:Connect(function(dt)
    part.CFrame = part.CFrame * CFrame.Angles(0, math.rad(90 * dt), 0)
end)
```

**Moving platform**

```lua
local TweenService = game:GetService("TweenService")
local platform = script.Parent
local info = TweenInfo.new(3, Enum.EasingStyle.Sine, Enum.EasingDirection.InOut, -1, true)
TweenService:Create(platform, info, { Position = platform.Position + Vector3.new(16, 0, 0) }):Play()
```

**Coins and a leaderboard**

```lua
local Players = game:GetService("Players")

Players.PlayerAdded:Connect(function(player)
    local stats = Instance.new("Folder")
    stats.Name = "leaderstats"
    stats.Parent = player
    local coins = Instance.new("IntValue")
    coins.Name = "Coins"
    coins.Parent = stats
end)

for _, coin in ipairs(workspace.Coins:GetChildren()) do
    coin.Touched:Connect(function(hit)
        local player = Players:GetPlayerFromCharacter(hit.Parent)
        if not player or not coin.Parent then return end
        player.leaderstats.Coins.Value = player.leaderstats.Coins.Value + 1
        coin:Destroy()
    end)
end
```

**Spawner**

```lua
local template = game.ReplicatedStorage.Crate
while true do
    local crate = template:Clone()
    crate.Position = Vector3.new(math.random(-10, 10), 30, math.random(-10, 10))
    crate.Parent = workspace
    game:GetService("Debris"):AddItem(crate, 8)
    task.wait(1.5)
end
```

**Best time with a DataStore**

```lua
local store = game:GetService("DataStoreService"):GetDataStore("Records")

script.Parent.Touched:Connect(function(hit)
    if not hit.Parent:FindFirstChildOfClass("Humanoid") then return end
    local t = math.floor(time() * 10) / 10
    local best = store:GetAsync("Best")
    if not best or t < best then
        store:SetAsync("Best", t)
        game:Win("New record: " .. t .. " s!")
    else
        game:Win("Finished in " .. t .. " s (record " .. best .. " s)")
    end
end)
```
