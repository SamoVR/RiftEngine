# RIFT engine architecture

How the engine is put together, for people working on RIFT itself. For using
RIFT, see the [user guide](GUIDE.md) and the [scripting reference](SCRIPTING.md).

## Projects in the solution

| Project | Output | Contents |
|---|---|---|
| `ThirdParty` | static library | Every vendored library in `third_party/`, warnings off, optimised even in Debug |
| `RiftCore` | static library | The engine: everything in `src/` except `editor/` and `player/` |
| `RiftEditor` | `RiftEditor.exe` | The editor (`src/editor`) |
| `RiftPlayer` | `RiftPlayer.exe` | Runs finished games (`src/player`) |
| `RiftTests` | `RiftTests.exe` | doctest unit tests; also compiles the editor's non-UI code |

Shared settings (C++20, `/utf-8`, include paths, output folders `build/bin` and
`build/obj`) are in `Rift.props`. Source files are picked up with folder globs,
so adding a `.cpp` needs no project edit. vcpkg is disabled so a system-wide
vcpkg cannot pull in a second GLFW.

## Source layout

```
src/core       Log, Paths (data / user / project folders), TransformUtil, Version
src/scene      Value, Instance, Reflection, Scene, SceneIO
src/render     Renderer, Framebuffer, Shader, Texture(Cache), GpuMesh, Primitives,
               ModelLoader (OBJ / glTF), Picking, ImageWrite
src/physics    PhysicsWorld (Jolt): bodies, character controller, queries, touches
src/script     LuaRuntime (sol2): scheduler, Instance API, data types, services; ScriptStore
src/game       GameSession (one run of a game), GameCamera, GuiSystem
src/audio      AudioSystem (miniaudio)
src/project    Project (project.json, templates), Exporter (Build Game)
src/platform   Input, Platform (dialogs, shell, clipboard), RichPresence (Discord)
src/ui         GameOverlay: draws game GUI / HUD with ImGui draw lists
src/editor     Editor (logic), Commands (undo), EditorApp (window, menus), Theme, panels/
src/player     PlayerApp
```

Nothing in `core`, `scene`, `physics`, `script`, `game` or `project` uses OpenGL
or ImGui, so all of it runs in the tests without a window.

## The scene model

Every object is an `Instance` (`scene/Instance.h`): one plain struct with the
fields of every class (position, size, colour, text, health, ...), tagged with a
`ClassId`. Classes reuse fields where the meaning matches. This keeps instances
trivially copyable (Play mode clones the whole scene; undo snapshots instances)
and avoids virtual dispatch.

The **reflection tables** (`scene/Reflection.cpp`) declare, per class, which
fields are exposed and under which property names, with type, category, range,
enum names, tooltip and flags (`PF_Serialize`, `PF_Editor`, `PF_ScriptRead`,
`PF_ScriptWrite`, `PF_ReadOnlyUI`, `PF_Runtime`). One table drives the
Properties panel, scene files, undo and the Lua API: adding a property is one
line there. Property values travel as `Value` (a `std::variant`).

A `Scene` owns the instances. Its root is a fixed set of services (Workspace,
Players, Lighting, ReplicatedStorage, ScriptService, StarterGui, StarterPlayer,
SoundService). Every change made through the Scene API notifies
`SceneObserver`s; physics, the Lua runtime and the editor listen.

Instances are referred to by **id** (random 64-bit, stable across save / load,
undo and Play) everywhere outside the scene layer, never by pointer.

**Transforms**: only position and rotation are inherited (the "frame"); `Size`
scales the part itself and never its children. Rotations are Euler degrees
applied X, then Y, then Z (`M = T * Rx * Ry * Rz * S`).

## Files

**Scene files** (`SceneIO`, format 4): JSON with one instance per line, parents
before children, and only the properties that differ from the class default,
stored by name. Files stay small and diff well, and new properties never break
old files. Format 3 (RIFT 0.8) is converted when loaded. Saving writes a
temporary file and renames it, so a crash never leaves half a scene.

The same encoding is used for undo snapshots, copy / paste and duplicating.

**Paths** (`core/Paths`): engine data is found by walking up from the executable
to a `data/` folder (shipped games have it next to the exe; development builds
find the repository's). User data goes to `%APPDATA%\RIFT` (games:
`%APPDATA%\RIFT Games\<game>`). Asset paths in scenes are relative to the
project (`assets/textures/wood.png`) or point into the engine data
(`engine:textures/checker.png`), so projects can be moved.

Scripts are files. `ScriptStore` keeps the text of the open scene's scripts in
memory (what the editor edits and the game runs), tracks unsaved changes, and
reloads files changed by other programs. Paths are validated (relative, no `..`);
the `project:` prefix reaches the project's shared `scripts/` folder.

## Rendering

`Renderer::render(scene, camera, settings, framebuffer)` draws into an offscreen
multisampled framebuffer that the editor shows in its Viewport panel and the
player draws full-window. One frame:

1. Sun shadow map: an orthographic depth pass around the camera, snapped to texels; sampled with hardware PCF.
2. Opaque parts: one draw per part, sharing one mesh per shape (MeshParts use their model, cached per file). Lighting is computed in linear space: sun, ambient sky / ground, up to 16 point and spot lights, materials (Neon glows, Metal and Glass reflect the sky), fog, ACES tone mapping.
3. Sky: a gradient from `Lighting` colours; the sun's direction and colour come from `Lighting.ClockTime`.
4. Editor grid (editor only).
5. Transparent parts, back to front.
6. Overlay lines: selection and hover boxes, light gizmos.

`Picking` finds the part under the mouse on the CPU by testing the ray against
each part's real shape in its own space; no GPU readback is needed.

## Physics

`PhysicsWorld` wraps **Jolt Physics** (CPU only).

- Anchored parts are static bodies (kinematic once a script or tween moves them).
- Unanchored parts are dynamic; unanchored parts below an unanchored part or model are merged into one compound body (welded).
- `CanCollide = false` parts become sensors: they do not collide but can still be touched and hit by raycasts.
- Fast bodies use linear-cast motion quality so they do not tunnel.
- The character is a Jolt `CharacterVirtual` capsule: walking, jumping, slopes and steps.
- Touches are found by shape queries for parts that scripts listen to (`Touched`), so only interested parts cost anything.
- The physics world follows the scene through the observer interface: property changes rebuild or move bodies lazily before the next step or query.

## Scripting

`LuaRuntime` runs Lua 5.4 through sol2. Lua is compiled as C++
(`SOL_USING_CXX_LUA`), so a Lua error unwinds C++ frames properly. One runtime
lives for one play session, so every Play starts from a fresh state.

- **Sandbox**: no `io`, `package`, `debug`, `load`; `os` trimmed to time functions; a 512 MB allocator cap; an instruction-count hook stops a script that runs more than about a second without yielding.
- **Instances** are userdata holding an id. Member access goes through the reflection tables, then computed members (CFrame, Parent, ...), events, and finally children by name. Unknown members are errors. Destroyed instances keep a tombstone copy so stale handles can still be read.
- **Scheduler**: every script, handler and `task.spawn` runs in its own coroutine. `task.wait`, `WaitForChild` and `Signal:Wait` park the coroutine; the runtime resumes it by time or event. Handlers of engine events are **deferred** (queued and run after the current code yields), like Roblox's deferred signal mode.
- **Services** (`LuaServices.cpp`): RunService, UserInputService, Mouse, Camera, TweenService, Debris, CollectionService, HttpService (JSON), DataStoreService (JSON files per game).

## A game session

`GameSession` is one run of a game, used by the editor's Play and by the player.
It clones the scene, creates the player, builds physics and the Lua runtime, and
then each `update(dt)`:

1. Input to Lua (InputBegan, ...), `RenderStepped`, `Stepped`
2. Character movement and physics step
3. Touches, deaths and respawning, fallen parts
4. `Heartbeat`, timers, tweens, Debris
5. Camera (follow, collision, first person)
6. GUI layout and clicks (`GuiSystem`), sounds

The host (editor viewport or player window) passes the view size, mouse and
focus in `ViewportInput` and draws the result: the 3D scene with `Renderer`, the
GUI and HUD with `GameOverlay`.

## The editor

- `Editor` holds the open project and scene, selection, clipboard and play session, and implements every operation (insert, delete, duplicate, group, reparent, property edits). It draws nothing, so it is unit-tested.
- Every change is a `Command` (`Commands.h`) recorded in `CommandHistory`. Commands store ids and JSON snapshots, never pointers, so undo works no matter what happened in between. Dragging a widget previews the value live and records one step when the drag ends.
- `EditorApp` owns the window, ImGui (docking), menus, toolbar, shortcuts and dialogs; `panels/` has one class per panel.
- The UI uses Dear ImGui 1.92 with dynamic fonts (Inter, JetBrains Mono, Font Awesome 6), ImGuizmo for the gizmos and ImGuiColorTextEdit for scripts.

## The player and Build Game

`RiftPlayer.exe` takes a project folder on the command line, or runs
`game/project.json` next to itself. **Build Game** (`project/Exporter`) copies
the player as `<Game>.exe`, replaces its icon resource with the project's icon,
copies the engine data it needs and the project's scenes, scripts and assets,
and writes `rift-build.json`, which marks the folder as a build that may be
replaced next time.

## Tests

`RiftTests.exe` covers transforms and scene files, the undo commands and editor
operations, ScriptStore, physics, the Lua API (scheduler, events, tweens, data
stores, sandbox limits), primitives and model loading, exporting, and rendering
(a hidden window renders the demo scene to PNGs in `build/test-output`). Every
Lua example in the documentation is compiled by the tests as well.

Run all tests with `build\bin\Debug\RiftTests.exe`; `--test-case="*physics*"`
picks some.

## Adding things

- **A property**: one line in the class's table in `Reflection.cpp` (and a field in `Instance` if no existing one fits). The editor, files and Lua pick it up.
- **A class**: a `ClassId`, a `def(...)` block in `Reflection.cpp`, an icon and colour in `editor/Theme.cpp`, and behaviour where it lives (renderer, physics, session).
- **A Lua method or service**: `LuaInstance.cpp` (methods on instances) or `LuaServices.cpp`, plus a test in `tests/test_lua.cpp` and a line in `docs/SCRIPTING.md`.
- **A panel**: a class in `editor/panels/Panels.h`, drawn from `EditorApp::frame`.
