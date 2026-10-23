# RIFT Engine – z editora na game engine

Prvý krok prestavby „mini Blendera“ na engine v štýle Roblox Studia. Doterajší editor (vkladanie tvarov, gizmo, textúry, hierarchia, undo, ukladanie/načítanie) zostáva **nástrojom na stavanie scény**. Pribudol herný režim, fyzika, skriptovanie a Discord Rich Presence.

## Čo pribudlo

| Funkcia | Kde | Use case |
|---|---|---|
| **Play / Stop** (F5, Esc ukončí hru) – scéna sa po Stop vráti presne do stavu pred Play | Toolbar, menu *Game* | UC-017 |
| **Hráčska postava** – WASD/šípky, Space skok, Shift šprint, pravé tlačidlo myši = rozhliadanie, kamera sleduje hráča | `GameRuntime` | UC-018 |
| **Fyzika** – *Anchored* (nehýbe sa), *Can Collide*, *Bounciness*, gravitácia, kolízie AABB, „kill plane“ | Properties → *Game* | UC-016 |
| **Skripty (Lua 5.4)** – API v štýle Roblox (`workspace`, `Instance.new`, `part.Touched:Connect`, `task.wait`, `RunService.Heartbeat`); skript je objekt *Script* v Exploreri a `.lua` súbor na disku | *Insert → Script (Lua)*, Properties → *Add Script* | UC-014, UC-015 |
| **Script Editor** – kontrola syntaxe počas písania, referencia API, ukladanie súboru, hot reload zmien z externého editora | dvojklik na Script v Exploreri | UC-015 |
| **Output** – výpisy `print` a chyby skriptov s číslom riadku | menu *Game → Output* | – |
| **Insert menu** – Part, Sphere, Wedge, Baseplate, Spawn Point a hotové skriptované šablóny: Kill Brick, Spinner, Coin, Jump Pad, Falling Block, Finish Line | menu *Insert* | UC-001 |
| **Discord Rich Presence** – „Building house.json · 12 objects“ / „Playtesting …“ + čas | `RichPresence` | – |

Panely *Object List* a *Scene Controls* sa teraz volajú **Explorer** a **Toolbar** (rozloženie okien v `imgui.ini` zostalo).
Nové vlastnosti objektov sa ukladajú do scény pod kľúčom `"game"`.

## Súbory

- `LuaRuntime.h/.cpp` – Lua 5.4 cez sol2: sandbox, API (`Instance`, `Vector3`, `Color3`, signály, `task`, služby), plánovač coroutines, ochrana proti nekonečným slučkám. sol2 je skrytý za `pimpl`, aby sa nekompiloval všade.
- `GameRuntime.h/.cpp` – herný režim: snapshot/obnova scény (aj hierarchie), hráč, fyzika, detekcia dotykov, register objektov podľa id pre skripty (vytváranie, klonovanie, mazanie).
- `ScriptStore.h/.cpp` – `.lua` súbory otvorenej scény v pamäti: dirty stav, uloženie, Save As, hot reload, validácia ciest (žiadne `..`).
- `InterfaceGame.cpp` – UI pre herné časti (Play, Insert, Script objekty, Script Editor, Output, overlay správ).
- `RichPresence.h/.cpp` – Discord Game SDK (už bol v projekte v `libraries/discord-rpc`).
- `libraries/lua` (Lua 5.4.7, MIT) a `libraries/sol2` (sol2 3.3.0, MIT) – **Lua sa kompiluje ako C++** a definuje sa `SOL_USING_CXX_LUA=1`, aby chyba v skripte bola C++ výnimka a destruktory sa vykonali (v C by `longjmp` preskočil destruktory). V `.vcxproj` majú Lua súbory `CompileAs = CompileAsCpp`.
- `SCRIPTING.md` – úplný popis Lua API.
- `assets/branding/` – návrhy loga (SVG) + `preview.html`.

## Formát scény v3 – scéna je priečinok

```
assets/scenes/demo_obby/
    scene.json
    scripts/CoinScript.lua ...
```

- Každý objekt má stabilné `"id"` (16 hex číslic) a `"class"` (`Part` / `Script`). Rodič sa ukladá ako **id**, takže mená sa môžu opakovať.
- *Script* nemá sieť ani transformáciu, len `"source"` (cesta k `.lua` relatívne k priečinku scény) a `"enabled"`. Script, ktorého rodič je objekt, je k nemu pripojený (`script.Parent`); bez rodiča je to skript celej scény.
- Základné tvary sa ukladajú menom (`"mesh": "sphere"`), ostatné siete ako jedno ploché pole `"v"`; len hodnoty odlišné od predvolených, jeden objekt = jeden riadok.
- Načíta sa len formát 3 (`"format": 3`). Staršie súbory sa odmietnu s jasnou chybou; scény v `assets/scenes` boli prevedené jednorazovo.
- *Save As* vytvorí priečinok podľa zadaného mena; *Save* uloží `scene.json` aj zmenené skripty. Scénu možno otvoriť z príkazového riadku: `RenderEngine.exe assets/scenes/demo_obby/scene.json`.
- `assets/scenes/demo_obby` – ukážková hra (mince, spinner, kill brick, jump pad, cieľ), skripty v Lua.

## Lua – príklad

```lua
local part = script.Parent

RunService.Heartbeat:Connect(function(dt)
    part.Rotation = part.Rotation + Vector3.new(0, 90 * dt, 0)
end)

part.Touched:Connect(function(other)
    if other == Players.LocalPlayer.Character then
        part.Color = Color3.fromRGB(255, 0, 0)
        game:Message("Dotkol si sa ma!", 2)
    end
end)
```

Úplný zoznam je v `SCRIPTING.md` a v Script Editore (*Lua API reference*).
Ochrana: skript, ktorý beží približne sekundu bez `task.wait()`, sa zastaví s chybou, takže nekonečná slučka editor nezamrazí. Skripty nemajú prístup k súborom ani sieti (žiadne `io`, `load`, `os.execute`).

## Discord Rich Presence – nastavenie (jednorazovo)

1. <https://discord.com/developers/applications> → *New Application* → názov **RIFT Engine** (to Discord zobrazí ako „Playing RIFT Engine“).
2. *Rich Presence → Art Assets* → nahraj logo s kľúčom **`rift_logo`** (min. 512×512 PNG).
3. *Application ID* skopíruj do `RichPresence::kClientId` v `RichPresence.h`, alebo do `settings.json` ako `"discordClientId"`.
4. Vypnutie: `"discordRichPresence": false` v `settings.json`.

Bez ID, alebo keď Discord nebeží, sa nič nedeje (engine beží normálne; Discord sa skúša pripojiť každých 15 s).

## Obmedzenia / ďalšie kroky

- Fyzika je zámerne jednoduchá: statické časti sa testujú ako orientované kvádre (SAT), pohyblivé telesá a hráč ako osovo zarovnané boxy, simulujú sa len objekty bez rodiča a otáčajúca sa plošina hráča neunáša. Pre plnú fyziku → Jolt/Bullet.
- Skriptovanie zatiaľ nemá `CFrame`, `TweenService`, `BindableEvent`, UI/HUD zo skriptov ani zvuk; hráč je jeden box (`Players.LocalPlayer.Character`).
- Chýba ešte z use case-ov: UC-006 import modelu (.obj), UC-008 svetlá, UC-011 editor shaderov, UC-019 export hry (samostatný „player“ .exe, ktorý načíta scénu a hneď spustí Play).
- Undo nepokrýva zmeny Game vlastností a text skriptu (skripty majú vlastné ukladanie do súboru).

## Build

Projekt má nastavený toolset **v145** (VS 2026). Vo VS 2022 buď *Retarget solution*, alebo:

```
MSBuild RenderEngine.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143
```
