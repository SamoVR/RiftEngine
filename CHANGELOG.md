# Changelog

Every release of RIFT. The newest is at the top; the editor's updater and the
GitHub release page show the section of the version being installed.

RIFT is in **beta**: versions are 0.x until the engine is complete.

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

