# Changelog

Every release of RIFT. The newest is at the top; the editor's updater and the
GitHub release page show the section of the version being installed.

RIFT is in **beta**: versions are 0.x until the engine is complete.

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

