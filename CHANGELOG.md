# Changelog

Every release of RIFT. The newest is at the top; the editor's updater and the
GitHub release page show the section of the version being installed.

RIFT is in **beta**: versions are 0.x until the engine is complete.

## 0.2.0

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

