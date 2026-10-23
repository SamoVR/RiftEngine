# RIFT Engine

A Roblox-Studio-style 3D game engine and editor: build scenes from parts, script
them in Lua, press Play to test, and ship the result as a standalone game.

## Building

Requirements: Windows 10/11, Visual Studio 2022 or 2026 with "Desktop development with C++".

1. Open `RiftEngine.sln`.
2. Pick `Debug` or `Release` (x64) and build. `RiftEditor` is the startup project.
3. Run. The editor finds its `data/` folder by itself, so any working directory works.

Command line: `msbuild RiftEngine.sln -p:Configuration=Release -p:Platform=x64`

Output goes to `build/bin/<Configuration>/`.

## Repository layout

| Folder | Contents |
|---|---|
| `src/` | Engine (`RiftCore` library), `editor/` (RiftEditor.exe) |
| `data/` | Files the engine ships with: shaders, fonts, icons, default textures |
| `examples/` | Example projects (open a `scenes/*/scene.json` in the editor) |
| `third_party/` | Vendored libraries (built as one static library) |
| `docs/` | Documentation (`SCRIPTING.md`: the Lua API) |
| `tests/` | Unit tests |

Per-user files (settings, window layout, logs) live in `%APPDATA%\RIFT`.
