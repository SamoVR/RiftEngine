# Third-party libraries

Everything RIFT builds from source, trimmed to the files that are compiled
(licences kept next to each library). Built as the `ThirdParty` static library.

| Library | Version | Licence | Used for |
|---|---|---|---|
| Dear ImGui (docking) | 1.92.9b | MIT | Editor UI, game GUI drawing |
| ImGuizmo | master (2025) | MIT | Move / rotate / scale gizmo, timeline |
| GLFW | 3.4 | zlib | Window, input |
| GLEW | 2.2.0 | BSD / MIT | OpenGL function loading (static) |
| GLM | 1.0 | MIT | Math |
| nlohmann/json | 3.x | MIT | Scene files, settings, JSON for scripts |
| stb_image / stb_image_write | 2.x | MIT / public domain | Textures, screenshots |
| Lua | 5.4.7 | MIT | Scripting (compiled as C++) |
| sol2 | 3.2.3 | MIT | Lua bindings |
| Jolt Physics | 5.6.0 | MIT | Physics (CPU only; Hair, compute and shader folders removed) |
| doctest | 2.5.3 | MIT | Unit tests |
| Font Awesome (icons header + fonts) | 5 | SIL OFL 1.1 / MIT | Editor icons |
| Discord Game SDK | 3.x | Discord SDK licence | Optional Rich Presence (delay-loaded DLL) |

## Local modifications

* `lua/lobject.c` (`tostringbuff`): floats that hold whole numbers are
  printed without a trailing `.0` (`7`, not `7.0`), matching Luau / Roblox,
  so `"Coins: " .. coins` reads naturally in game text.
