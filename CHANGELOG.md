# Changelog

Every release of RIFT. The newest is at the top; the editor's updater and the
GitHub release page show the section of the version being installed.

RIFT is in **beta**: versions are 0.x until the engine is complete.

## 0.6.2

### Much faster
- **RIFT now asks for the real graphics card.** On laptops with two it was running on the weak built-in one: the Canyon went from 26 to about 200 frames per second in the editor on an RTX 4060 laptop, with nothing turned down.

### Fixed
- **Clouds and Weather work**: Insert now puts them in Lighting, where they do something, and ones already left in the Workspace work too. So do Atmosphere and the post effects when inserted.
- **Water's edge**: no more hard rectangular steps along shores.
- A changed copy of a built game is now refused when it starts, whichever file was changed.
- `UserInputService.GamepadEnabled` says when a gamepad is connected.
- Brighter bushes in the Canyon. Key names in Settings are capitals.

- Esc closes an open menu, Preferences and Project Settings. A project always opens on the Explorer, not Git Changes.
- The whole floor no longer changes colour when the mouse passes over it; it keeps just its outline.
- The title bar says a name once when the project and scene share it, and Ctrl+P shows where each result is, so two scripts with one name can be told apart.

### New
- **Character sounds**: footsteps that change with the ground, jumping, landing, splashing, swimming, climbing, wind while falling and dying, for every character. All RIFT's own. `StarterPlayer.CharacterSoundsEnabled` turns them off.
- **Loading is a small box** over the editor or the game, like Roblox Studio. Only starting the program fills the window.
- **Ctrl+P**: one box to find objects and scripts, insert anything and run commands.
- **Edit > Align and Distribute** for selected parts and whole models.
- **Script editor**: F12 goes to where a name is declared, Ctrl+Shift+R renames a variable in the script (not in strings or comments), and a button lists the script's functions.
- **Game > Scale Old Scene to Full Size**: an old project with a small character becomes Roblox-sized in one undoable step.

## 0.6.1

### Fixed: what 0.6.0 broke
- **Built games have their terrain again.**
- **Discord Rich Presence works again.**
- **Toolbox and home screen pictures load again**, with no more errors in the Output.
- Links ending in `.com` open again.
- A changed copy of a built game now stops with a message as soon as a changed file is read, instead of carrying on without it.

### Fixed: in play
- **Falling stops getting faster**: `Workspace.TerminalVelocity`, 250 studs per second.
- **The underwater view and swimming hold all the way down** to a river's bed.
- **Walking and running no longer bounce** up and down.

### Sizes
- A new Part is 4 by 1 by 2, as on Roblox, and so is `Instance.new("Part")` (4 by 1.2 by 2).
- In a game with a smaller character (old projects have `CharacterScale` 0.4), new parts, seats, templates and Toolbox models are sized to match it, and the Output says so.

### Loading screens
- The window is never blank or frozen: starting RIFT, opening a project or scene, saving, building, importing, copying an example, pressing Play, joining and starting a built game all show what is going on, once they take more than a moment.

### New
- **TextBox**, **ScrollingFrame** and **UIListLayout** (with `LayoutOrder` on every GUI object).
- Discord shows when you are hosting, shaping terrain or animating.

### Safer
- The updater takes downloads only from RIFT's own releases, and checks the size and SHA-256 before starting one.
- Only the player who was asked can answer an `InvokeClient`, and players' calls can no longer get each other's answers.
- The host ignores a character moving impossibly far, and drops a player's remote calls above 500 a second.
- A Lua pattern that would backtrack for ever now stops with an error.

### Faster
- The ground's collision is built on every core.

## 0.6.0

### Built games are protected
- **Build Game now makes one `.riftgame` file** instead of a folder of your files: encrypted, with a key made for that build, and with every file checked, so a changed copy refuses to run. Scripts are in it as stripped bytecode: no source, comments or local names. A script with an error stops the build and says where.
- This stops casual copying and tampering. The key has to be on the player's computer for the game to run, so a skilled and determined person could still dig things out: true of every game engine.

### Safer
- Bad data from another machine can no longer crash a host or a joiner, and joiners can only equip tools they carry or that lie in the world.
- Scenes, scripts and other machines can no longer point at network paths or climb out of the game's folder.
- Opening a project someone sent you no longer lets its `.git/config` or hooks run programs, and double-clicking a file in a project no longer runs programs.

### The world
- **Clouds** and **Weather** (rain, snow) objects in Lighting. `Lighting.DayLengthMinutes` makes the day go by.

### Characters
- **TrussPart** and `Climbable` parts to climb. **Shift** sprints and **C** crouches (`StarterPlayer.SprintEnabled`, `SprintSpeed`, `CrouchEnabled`, `CrouchSpeed`; `Humanoid.Crouching`).

### Models
- **.fbx** models can be imported.

### Plugins
- **Plugins menu**: small Lua programs in your own plugins folder add buttons that change the scene, and can be undone. See the guide. Projects cannot bring their own, so opening one runs nothing.

### Sound and controls
- `SoundService.AmbientReverb`. **Gamepads** work in built games. Players can **change the keys** in Settings. **ContextActionService** in scripts.

### Faster
- **The insides of meshes are no longer drawn**, like Roblox. Set the new `MeshPart.DoubleSided` for thin things seen from both sides; glTF models that ask for it get it by themselves. A model made inside out will now look hollow: turn DoubleSided on, or fix it in your modelling program.
- **A world's terrain loads on every core, before the first frame**: the whole Canyon in under 4 seconds. Shadows of far walls no longer appear as you walk around, which is what made them seem to follow the character.

### Collisions
- `MeshPart.CollisionFidelity` is a choice now, as in Roblox: **Box**, **Hull**, **PreciseConvexDecomposition**. Games saved with the old numbers keep what they had.

### A new Esc menu
- **Redesigned**: a panel at the side of the screen with the game's name, buttons that respond smoothly, and Settings and Play online on a page beside it, over the game blurred and darkened. Restart and Quit ask to be pressed twice.
- **Settings**: graphics presets (Low to Ultra) as well as every switch, a volume slider, showing frames per second, and the controls.
- **Your game decides how it looks**: the new **MainMenu** object in the Explorer sets the title, subtitle, logo, fonts, colours, which side it is on, its backdrop and blur, what each button says, and which buttons there are. Selecting it shows the menu in the 3D view while you change it.
- **Turn it off** with `MainMenu.Enabled`, or **replace it** with `MainMenu.CustomMenu`: Esc then shows a ScreenGui of your own, and the game keeps running.
- **Scripts**: in a LocalScript, `MainMenu:OpenMenu()`, `:CloseMenu()`, `:QuitGame()`, `:RestartGame()`, and `MainMenu.IsOpen`.

### Animations
- **Every default animation is new**, for R6 and R15: idle, walk, run, jump, fall, sit, holding a tool, swimming. Limbs move smoothly instead of stopping at each keyframe, knees and elbows bend as they should, arms swing against the legs, and the body rises and falls with each step.
- `tools/make_animations.py` writes them, if you want to change how they move. Your own animations and Animate scripts keep working.

### More properties
All of these are in the Properties panel and in scripts, are saved with the game, and reach everyone online.
- **TextChatService**: the chat window's font, text size and colour, background, size and place on screen; the input bar's placeholder text and colours; speech bubbles on or off, their font, colours, text size, how long they stay and how far they show. Scripts also reach the same things by Roblox's names, through `ChatWindowConfiguration` and `BubbleChatConfiguration`.
- **Players**: `MaxPlayers`, `RespawnTime`, `CharacterAutoLoads`.
- **Humanoid**: `DisplayName`, `AutoRotate`, `MaxSlopeAngle`, `BreakJointsOnDeath`, `CameraOffset`, `NameDisplayDistance`, `HealthDisplayDistance`. **StarterPlayer** gives every new character the last ones.
- **Lighting**: `TimeOfDay` ("14:30:00"), `GeographicLatitude`, `ShadowSoftness`, `OutdoorAmbient`.
- **Parts**: `CanQuery` (raycasts pass through), `Locked` (clicking in the 3D view passes over it). **Model**: `PrimaryPart`.
- **Sound**: `RollOffMinDistance`. **SoundService**: `RolloffScale`.
- **GUI**: `ClipsDescendants`, and `AutoButtonColor` on buttons.

### Names over heads
- Other characters show their name above their head, and a health bar when hurt, as far away as `NameDisplayDistance` and `HealthDisplayDistance` say. Set them to 0 to hide them.

### Swimming
- **Characters really swim** in terrain water: they float at the surface with their head out, swim where the camera looks (look down and press W to dive), rise again when left alone, and Space leaves the water, onto a bank for example. Speed changes softly, as in water.
- New **swimming and treading water animations**, for R6 and R15. `swimidle` joins the ids a custom Animate script can use.
- Non-player characters float and swim as well.

### Under water
- **With the camera in water you see you are in it**: everything fades into the water's colour with distance, the water's surface is seen from below, with the sky in a bright circle overhead, sunlight moves over the bottom, and the picture sways a little. It follows `Terrain.WaterColor` and `WaterTransparency`, in the editor and in games.

### Online
- **Whoever joins is animated**: their Animate script (and any other LocalScript in their character) did not start, so they stood stiff on every machine. Now it starts, after respawning too.
- Scripts on the host hear `Running`, `Jumping`, `FreeFalling`, `Swimming` and `StateChanged` for players who joined.
- A machine that goes silent is noticed: after 45 seconds it is dropped, or the player is told the host is gone.
- The host ignores impossible numbers from a player, cleans up player names, and is not tied up by connections that never say who they are.

### Shadows
- Shadows no longer move along with the character. Walls far away towards a low sun now shadow the ground near you as well as far from you, and simplified far terrain no longer shadows the real ground up close.

### Characters
- No more sliding down slopes you can walk on.
- Bumping into small cubes no longer lifts a character, and loose parts no longer launch it or speed it up, from under the feet or from the side.
- Small bumps in the ground no longer count as falling, so the fall animation stops flickering on rough terrain.

## 0.5.5

### Photo-real graphics
All of it optional: a game turns it on in Lighting, and each computer can turn each part off (Preferences, a built game's Esc menu). Older games look as they did.
- **Canyon**, a new example: walk along a river through a desert canyon, made from free photo-scanned rock, sand and plants.
- **SurfaceAppearance**: texture sets (colour, normal, roughness, ambient occlusion, height) on parts, MeshParts and terrain. glTF models bring their own.
- **Lighting.RealisticLighting**: physically based shading, lit by the sky itself.
- **Atmosphere**: a real sky worked out from the sun, air and dust in the distance, and light shafts through the air.
- **Terrain** takes four texture sets, projected so cliffs do not stretch, blended by their height maps, with real relief up close, and knows how much sky each place can see.
- **Scatter**: thousands of copies of a model growing on the terrain, with wind.
- **Real water**: you see into it, it mirrors its surroundings, the sun glitters on it, it foams at the banks, and `Terrain.WaterFlow` says which way it runs.
- **Lighting.AutoExposure**, and frames blended over time, for sharp edges and no shimmer.
- **Render scale** (Preferences, a built game's Esc menu): draw the 3D view smaller and stretch it, for slower computers.
- Terrain far away is drawn in simpler versions, nearest first, on every game.

### Faster at high resolutions
- **Automatic resolution**, on by default in realistic scenes: draws as many pixels as your graphics card keeps up with, and puts the picture back together at full size from several frames. Your window or screen stays at its full size, and the interface stays sharp. It can be turned off, with a Render scale slider to set it yourself.

### Explorer
- **ReplicatedFirst**: LocalScripts in it run before all others. **TextChatService** is now in the Explorer, and its **ChatEnabled** turns the chat off.
- Services are in Roblox Studio's order, objects inside them too (Camera and Terrain first, then by name), with its icon colours.
- Every object has a read-only **ClassName** property.

### Camera
- `Camera.ViewOffset`: moves the view alone, not where the camera aims. Canyon's **HandheldCamera** script uses it for a camera held in the hand.

### Fixes
- Shadows no longer move when you turn the camera.

### Models
- `MeshId` can name one mesh in a file: `rocks.gltf#2`.
- Models get simpler versions for far away, made when they load.
- `MeshPart.CollisionFidelity`: its box, a hull around it, or its exact shape.

### Small things
- A turned SpawnLocation says which way the player starts out facing.

### Examples
- The **Obby** and **Playground** examples and the Obby template are gone.

## 0.5.0

### Online multiplayer
- **Host and join by address**, over the internet or a local network. Test tab > **Online** in the editor; **Online** in a built game's Esc menu; or `--host`, `--join 1.2.3.4` and `--server` on the command line.
- **Dedicated servers**: `--server` runs a game with no window and no player of its own.
- Each player's machine runs its LocalScripts, GUI and character, and the host runs the Scripts and the world, so games made for the Test players button work online as they are. RemoteEvents, RemoteFunctions, tools, prompts, chat, teams, respawning and terrain all go over the connection.
- `Players.PlayerRemoving` is new, `Players.PlayerAdded` also fires for those who join later, and `player.Character` works for every player, not just yours.

### Faster
Measured on the same machine, with no script doing anything unusual:
- A world of **30,000 parts** costs **5 times less** to keep running when nothing moves (16.7 ms a frame to 3.3); 100,000 parts 4 times less.
- The renderer takes **half the time** to go through a big world.
- **Scripts run twice as fast**: stopping endless loops no longer slows every instruction.
- `Vector3.new` is 3 times faster, reading `.Parent` 6 times, and setting a part's properties about twice. Values like Vector3 no longer make work for the garbage collector.
- Finding prompts, billboards, portals and effects no longer means searching the whole world every frame.

### Physics
- **Constraints**: Hinge (with motors and servos), Ball Socket, Prismatic, Rod, Rope, Spring, Weld, No Collision.
- **VehicleSeat**: sit down and drive the hinges in its model. Try **Go Kart** in the Toolbox.

### Characters
- **Non-player characters**: a Model with a Humanoid and an unanchored HumanoidRootPart walks, jumps, animates and dies. `Humanoid.MoveToFinished` is new.
- **PathfindingService**: `CreatePath`, `ComputeAsync`, `GetWaypoints`, around walls, up ledges and over gaps, with nothing to bake.

### First person
- `StarterPlayer.CameraMinZoomDistance` and `CameraMaxZoomDistance`: both at 0.5 is first person only.
- **ViewportFrame**: GUI that shows parts of its own through its own Camera, for first-person weapons that never poke through walls and for item previews.
- Setting `workspace.CurrentCamera.CFrame` now also turns a following camera, for recoil.
- **FPS Range**, a new example: rifle, pistol and shotgun with recoil, reloading and a real viewmodel, moving targets, a timed round and a best score, through a portal, with the new graphics on.

### Scripts
- `` `interpolated strings` `` are coloured as strings in the script editor.

### Tools
- **Scale** and **Transform** now work on Models and on several things at once. A Model's Scale keeps its shape; Transform can stretch it.

### Releases
- Source code moved to a private repository. Downloads, the website and the docs stay public at the same addresses.

## 0.4.0

### Top bar
- A ribbon like Roblox Studio's: tabs Home, Model, Test and View, with big buttons that have their names on them, in named groups. Play stays on every tab. If the window gets narrow the ribbon scrolls; buttons never overlap.
- New drawn icons for the tools and part shapes, and icons are centred in their buttons.

### Tools
- **Scale** is now the classic one: a ball on each side of a part. Ctrl resizes both sides, Shift keeps proportions.
- The old box handles are the new **Transform** tool (Ctrl+5).

### Characters
- **R15** is the new default body, with elbows, knees, hands and feet and its own animations. **R6** is an option: `StarterPlayer.CharacterRigType`. Games from before this version stay R6.
- Rounded head and a real face, on both bodies.
- New **Decal** class: an image on a face of a part.

### Graphics
Each can be switched off by the game (Lighting) and on each computer (ribbon's View tab, Preferences, a built game's Esc menu). Older games keep their look until you turn them on.
- HDR with tone mapping, ambient occlusion, screen-space reflections, and shadows out to 600 studs with soft edges.
- Post effect objects scripts can control: Bloom, Sun Rays (god rays), Color Correction, Blur, Depth Of Field.

### Portals
- **Insert > Portal Pair**: seamless doors. See through them, and characters, loose parts and the camera pass through, with views inside views.

### Scripting
- **Luau syntax**: `+=` and friends, `continue`, `for k, v in t do`, type annotations, `if` expressions, `` `interpolated {strings}` ``, `1_000` and `0b1010`. Also `bit32`, `math.noise`, `table.create`.
- **Teams** service: Team objects, team spawns, teams on the leaderboard and in the chat.
- **Changed:** a part's `Position` and `Orientation` in scripts are now in the world, as in Roblox, and a part stays where it is when its `Parent` changes. Scripts that relied on them being relative to the parent need `CFrame` maths.

### Faster
- Parts are drawn instanced: thousands of parts take a handful of draw calls. Round parts use fewer triangles when small or far away, and specks under a pixel are skipped. Crystal Isles went from about 345,000 triangles to 102,000.

## 0.3.1

### Fixes
- Endless loops no longer stop or crash a game. A script that runs without waiting (`while true do end`) is paused and continues a little each frame, with no error, and the game keeps running. Objects made per frame are limited the same way, and the limit shrinks when the game slows down. The Output says which script it is.
- Terrain is no longer drawn from the inside: from inside the ground the camera sees out, as in Roblox.
- Water meets its banks: no more gaps or water disappearing around corners, and it shows from the sides too. Crystal Isles' pond sits below its rim now.
- New terrain joins terrain next to it (fills, and the Add brush) instead of leaving a crack.
- Replace All in Find in Scripts can't run twice when the new text still matches the search, so words no longer grow with every click.

### Roblox sizes
- The character is 5 studs tall with Roblox's proportions, so a new part (4, 1, 2) looks the right size next to it. Gravity 196.2, WalkSpeed 16 and JumpPower 50, as in Roblox. Spawns, seats, the sword and the Toolbox's models fit.
- Your existing games play exactly as before: they load with the new `StarterPlayer.CharacterScale` at 0.4 and their old values. Set it to 1 to use the full size.

### Updates
- A new version now opens a proper window when RIFT starts (not during a game), with readable release notes and big buttons. Later and Skip version are there too.
- When an update is available, Help turns green with a filled "Update to RIFT x.y.z!" row at the top, and the status bar has a filled badge.

## 0.3.0

### Source control, like Visual Studio
- **Git Changes** (the old Source Control panel): stage and unstage files one by one or all at once, see any file's diff, Commit Staged or Commit All (and push or sync in the same click), amend the last commit, stash changes and bring them back.
- Merge conflicts are listed with **Keep Mine**, **Take Theirs** and **Mark Resolved**, and a stopped merge, revert or cherry-pick can be aborted.
- **Git Repository** (Git > Git Repository, a tab beside the viewport): your branches, the server's and the tags, next to the history of every branch as a graph with coloured lanes, where branches and tags point, and which commits are outgoing or incoming. Pick a commit to see its files and their diffs; right-click it to branch, tag, check out, revert, cherry-pick or reset.
- A **Git** menu: fetch, pull, push, sync, new branch, switch, merge, connect to GitHub.
- Fixed: git's warnings (line endings) broke the panel, discarding a change in a project inside a bigger repository touched the wrong file, and a background refresh disabled every button for a moment.

### Playing
- **Backpack**: the <kbd>`</kbd> key (left of 1) or the new backpack button opens the inventory: every tool in a grid with a search box. Drag tools onto the ten numbered slots of the bar (or back into the inventory).
- **Chat**: <kbd>/</kbd> opens the chat box (top left); what players say appears for everyone, with a speech bubble over the speaker's head.
- **Seats**: characters sit down when they touch one; Space gets them up.
- **ProximityPrompts**: "E Interact" appears near the part it is in; press, hold or click it.
- Lighting is matte like Roblox: plastic no longer shines; only Metal, Glass, Ice, Foil and Reflectance do. The night sky has a moon.
- A script that loops without waiting (making parts, or dropping thousands of unanchored ones) is stopped with a clear error instead of freezing or closing the game.

### Editor
- **Toolbox** (View > Toolbox): models that come with RIFT (a door, a bench, a sign, a street lamp, a tree, a treasure chest) and the ones you save (Save Selection, or right-click > Save to Toolbox), with their scripts and images. Click to insert, or drag into the viewport.
- **BillboardGui** and **SurfaceGui** (Insert > GUI): GUI floating over a part, facing the camera, and GUI painted on a face of a part. Both show while editing.
- Insert > Parts > **Seat**, Insert > Objects > **Proximity Prompt**.
- Copy, paste and duplicate give LocalScripts their own file too.

### Scripting
- `Player.Chatted`, and **TextChatService**: `MessageReceived`, `SendingMessage`, `TextChannels.RBXGeneral:SendAsync` / `:DisplaySystemMessage`, `:DisplayBubble`, `ChatWindowConfiguration`, `BubbleChatConfiguration`; the older `Chat:Chat`. `CoreGuiType.Chat` hides the chat window. Each player has its own `UserId`.
- **Seat** (`Disabled`, `Occupant`, `:Sit(humanoid)`), `Humanoid.Sit`, `SeatPart`, `Seated`, the Seated state, and a sitting pose in the Animate script.
- **ProximityPrompt**: `ActionText`, `ObjectText`, `KeyboardKeyCode`, `HoldDuration`, `MaxActivationDistance`, `RequiresLineOfSight`, `ClickablePrompt`; `Triggered`, `TriggerEnded`, `PromptButtonHoldBegan`, `PromptButtonHoldEnded`, `PromptShown`, `PromptHidden`.
- **BillboardGui** (`Adornee`, `Size` in studs and pixels, `StudsOffset`, `StudsOffsetWorldSpace`, `AlwaysOnTop`, `MaxDistance`) and **SurfaceGui** (`Adornee`, `Face`, `SizingMode`, `CanvasSize`, `PixelsPerStud`, `AlwaysOnTop`), with clickable buttons.

## 0.2.1

### Fixes
- Games built with 0.2.0 could stop at start with "Could not load font file": the build now includes every font the player uses, and a missing font no longer stops a game.
- Accepting an autocomplete suggestion (Tab, Enter or a click) changed the script editor's text but not the script the game ran, until the next key press.
- Ctrl+Backspace and Ctrl+Delete delete a whole word in the script editor.
- An anchored part in a character (the HumanoidRootPart, a held handle) holds it in place, like Roblox: no walking, no falling.
- When the character dies, the camera follows the head as the body falls apart instead of dropping through the map inside the head.
- In first person the player's own character is not drawn, so the camera no longer looks out from inside the head.

### Faster
- Parts are drawn without their insides (back faces are skipped) and only when they are in view; the shadow pass skips what is outside the shadow map. A busy scene draws about a third fewer triangles.

### Scripting
- `workspace.CurrentCamera` is a Camera object in the client's Workspace, shown in the Explorer with its CFrame, FieldOfView and CameraType.
- Autocomplete says what each suggestion is (property and its type, method, event, the class of a child, service, ...) and shows a short description of the selected one.

## 0.2.0

### Crystal Isles, a demo game
- A complete little game in `examples/` (the home screen opens a copy): a menu with a best score, a countdown, a two-minute gem race over floating voxel islands, combos, launch pads, speed pads, a moving platform, a waving guide, and a results screen. Made only with what the editor and the Lua API offer, so every part of it is something to open and learn from.

### Editor
- A polish pass over the whole interface: aligned rows and headings in Preferences and Project Settings, one style of section header in Properties and Source Control, tidier home screen, About and New project dialogs, roomier menus, a proper dropdown arrow on the material picker, softer shadows.
- New project suggests a name that is free and warns when the folder exists.
- The version is 0.x while RIFT is in beta.

### GUI
- Text: `Font` (Roblox's Enum.Font names), `TextScaled`, `TextTransparency`, an outline with `TextStrokeColor3` / `TextStrokeTransparency`.
- `Rotation` for every GUI element; its children turn with it.
- **UIGradient** (Insert > GUI): shades an element's background, text and image.
- Clicks on anything inside a button (a label, an icon) reach the button.

### Terrain
- Smooth voxel terrain like Roblox's (`workspace.Terrain`): 2-stud voxels in 22 natural materials plus water, meshed smooth, with shadows and materials that blend into each other.
- The **Terrain Editor**: generate land (hills, mountains, lakes, beaches, caves) and sculpt it with Add, Subtract, Grow, Erode, Smooth, Flatten and Paint brushes (sphere or cube, size, strength, Ctrl to reverse, Shift to smooth). Material and water colours. Every stroke is one undo step.
- Characters walk on terrain and swim in its water. Ray casts hit it and report its material.
- Scripts: `Terrain:FillBlock`, `FillBall`, `FillCylinder`, `Clear`, `GetMaterialColor` / `SetMaterialColor`; server changes reach every client.
- Saved next to the scene as `terrain.bin`.

### Animations
- Roblox's animation system: Animator, AnimationController, Animation, KeyframeSequence / Keyframe / Pose / KeyframeMarker and Motor6D joints. Tracks fade, loop, blend by priority and weight, and fire Stopped, Ended, DidLoop, KeyframeReached and marker events.
- Characters are jointed now and animate with a default Animate script: idle, walk, run, jump, fall, swim, and the arm raised while holding a tool. Replace it with your own Animate in StarterCharacterScripts.
- The **Animation Editor**: pose a rig on a timeline with the Move and Rotate tools (limbs turn around their joints, changes are keyed automatically), drag keys, set easing, name keyframes, preview, and save `.anim` files to use as AnimationIds. Insert > Animation > Character Rig to practise on.
- Humanoid states: `GetState()`, `StateChanged`, `FreeFalling`, `Swimming`. KeyframeSequenceProvider.
- The CFrame property type (Motor6D.C0 / C1, Pose.CFrame) with position and rotation fields in Properties.

### Colours and materials
- A colour palette with Roblox's 64 BrickColors, recent colours, a picker and a hex field, from the toolbar's paint bucket (for the selected parts) and next to every colour in Properties.
- A material picker with a preview of every material, grouped and searchable, from the toolbar and the Material property.
- `BrickColor` in scripts, and `part.BrickColor`.

### Scripting
- Autocomplete in the script editor: keywords, the API, your own names, and the members of what is before a `.` or `:` (objects in the scene, services, Enum, Vector3 and friends, events, simple locals).
- **Find in Scripts** (Ctrl+Shift+F): search every script of the scene, jump to a result, replace all.
- Built-in scripts (the default Animate) open read-only.

### Fixes
- `game:GetService("ServerStorage")` returned ReplicatedStorage.
- Setting `HumanoidRootPart.CFrame` moved only the root part, leaving the body behind; it moves the whole character now.
- A push faster than walking (launch pads, knock-back) now carries the character through the air instead of stopping at once; letting go of the keys in the air slows down smoothly.

### Effects
- Attachments, ParticleEmitters, Beams and Trails, drawn in the editor and in the game, with Roblox's properties (colour and size over life, spread, acceleration, drag, light emission, curved beams, scrolling textures, fading trails).
- NumberRange, NumberSequence and ColorSequence for scripts, and editors for them: a curve with draggable points and a gradient with colour stops.
- Ready-made Fire, Smoke, Sparkles, Magic Beam and Speed Trail in Insert > Effects; beams and trails get their attachments made and linked.
- `ParticleEmitter:Emit(n)` and `:Clear()`.

### Materials
- Parts get the full set of Roblox materials (Wood, Brick, Cobblestone, Granite, Marble, Slate, Fabric, Foil, Diamond Plate, Corroded Metal, Wood Planks, Pebble, Ice, Force Field, and the terrain materials), each with its own procedural look.

### Players and tools
- Test with several players at once (1 to 8): each gets a character and a client; pick whose view you see while playing.
- Tools and the Backpack: StarterPack, equipping with the number keys, Activated / Equipped events, dropping and picking up, grip position, `Humanoid:EquipTool`. A ready-made Sword.
- `StarterGui:SetCoreGuiEnabled` hides the backpack bar or the leaderboard.

## 0.1.0

The first beta.

### The engine
- The editor: dockable Viewport, Explorer, Properties, Output, Assets and script tabs; Roblox Studio camera and selection; move, rotate and resize tools with snapping; undo for everything; copy, paste, group.
- Lua 5.4 scripting with a Roblox-style API, Jolt physics, Humanoid characters, GUI, sounds, lighting and shadows.
- 3D models (OBJ, glTF, GLB) as MeshParts.
- The standalone player and File > Build Game.

### Client and server, like Roblox
- Games now run as a **server** and a **client**. Scripts run on the server, the new **LocalScript** runs on the player's side (input, camera, GUI).
- **RemoteEvent** and **RemoteFunction** for talking between the two, **BindableEvent** and **BindableFunction** within one side.
- **Replication**: the client gets a live copy of Workspace, ReplicatedStorage, Lighting and the rest; ServerStorage and ScriptService stay on the server.
- PlayerGui (copies of StarterGui, with ResetOnSpawn), PlayerScripts, Backpack, StarterPlayerScripts and StarterCharacterScripts.
- While playing, a **Client / Server** switch in the toolbar and the Explorer shows either world. The Server view has the editor camera, selection and move tools; the Output tags every line Client or Server.

### Version control
- A **Source Control** panel (Git): commit, history, branches, push, pull, connect to GitHub, discard changes.
- New projects can start as git repositories, with a .gitignore for RIFT projects.

### Updates and installing
- **RIFT-Setup.exe** installs RIFT for the current user (no administrator rights), with Start menu and desktop shortcuts and an entry in Apps & features to uninstall it.
- RIFT checks for new versions and updates itself in place (Help > Check for Updates).
- RIFT and the games it builds no longer need the Visual C++ runtime to be installed.

### Editor
- A new home screen: project cards with a picture of the scene, search, templates and examples, a Learn page.
- Preferences with categories: projects folder, accent colour, anti-aliasing, frame rate limit, background throttling, camera field of view and inverted mouse, script editor options (tab width, whitespace, mini map, closing brackets, external editor), play options (clear output, save before playing, pause on errors), output options.
- Project Settings can move the project's scenes to another folder (for example Levels).
- Opening an example asks where to put the copy and what to call it.
- Optional confirmation before deleting objects.

