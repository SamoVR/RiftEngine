// InterfaceGame.cpp -- the game-engine side of the editor UI: Play/Stop,
// Insert templates, per-object game properties, the Script Editor and the
// Output console. (The scene-editing UI is in Interface.cpp.)
#include "Interface.h"
#include "misc/cpp/imgui_stdlib.h"
#include <algorithm>
#include <cfloat>

void Interface::togglePlay() {
    if (!runtime) return;
    if (runtime->isRunning()) {
        runtime->stop();
        showStatus("Stopped - scene restored to its state before Play");
        return;
    }
    clearSelection();
    playing = false; // timeline playback would fight the simulation
    showOutput = true;
    runtime->start();
}

namespace {
    // Scripts used by Insert > Scripted templates. They double as examples of
    // the Lua API for people writing their first script.
    const char* kKillBrickScript =
        "-- Sends the player back to the spawn point when touched.\n"
        "local brick = script.Parent\n"
        "local player = Players.LocalPlayer\n"
        "\n"
        "brick.Touched:Connect(function(other)\n"
        "    if other == player.Character then\n"
        "        game:Message(\"Ouch! Back to spawn.\", 2)\n"
        "        player:LoadCharacter()\n"
        "    end\n"
        "end)\n";
    const char* kSpinnerScript =
        "-- Spins the part around the Y axis.\n"
        "local part = script.Parent\n"
        "local speed = 90 -- degrees per second\n"
        "\n"
        "RunService.Heartbeat:Connect(function(dt)\n"
        "    part.Rotation = part.Rotation + Vector3.new(0, speed * dt, 0)\n"
        "end)\n";
    const char* kCoinScript =
        "-- A spinning, bobbing coin that disappears when collected.\n"
        "local coin = script.Parent\n"
        "local startY = coin.Position.Y\n"
        "local collected = false\n"
        "\n"
        "RunService.Heartbeat:Connect(function(dt)\n"
        "    if collected then return end\n"
        "    coin.Rotation = coin.Rotation + Vector3.new(0, 120 * dt, 0)\n"
        "    coin.Position = Vector3.new(coin.Position.X, startY + math.sin(time() * math.pi) * 0.25, coin.Position.Z)\n"
        "end)\n"
        "\n"
        "coin.Touched:Connect(function(other)\n"
        "    if other == Players.LocalPlayer.Character and not collected then\n"
        "        collected = true\n"
        "        print(\"Coin collected!\")\n"
        "        coin:Destroy()\n"
        "    end\n"
        "end)\n";
    const char* kJumpPadScript =
        "-- Launches the player upwards.\n"
        "local power = 28\n"
        "\n"
        "script.Parent.Touched:Connect(function(other)\n"
        "    if other == Players.LocalPlayer.Character then\n"
        "        other.Velocity = Vector3.new(0, power, 0)\n"
        "    end\n"
        "end)\n";
    const char* kFinishScript =
        "-- Ends the run when the player reaches this part.\n"
        "script.Parent.Touched:Connect(function(other)\n"
        "    if other == Players.LocalPlayer.Character then\n"
        "        game:Win(\"Finished in \" .. math.floor(time() + 0.5) .. \" s!\")\n"
        "    end\n"
        "end)\n";
    const char* kEmptyScript =
        "-- Lua script. It runs while the game is playing (Game > Play / F5).\n"
        "-- script.Parent is the part this script is attached to (or workspace).\n"
        "\n"
        "print(\"Hello from \" .. script.Name)\n"
        "\n"
        "RunService.Heartbeat:Connect(function(dt)\n"
        "    -- runs every frame, dt = seconds since the last frame\n"
        "end)\n";

    const char* kLuaReference =
        "Globals    workspace  game  script  Players  RunService  UserInputService\n"
        "Instance   Name  ClassName  Parent  Position  Rotation  Size  Color\n"
        "           Anchored  CanCollide  Visible  Bounciness  Velocity  WorldPosition\n"
        "           :GetChildren()  :GetDescendants()  :FindFirstChild(name, recursive)\n"
        "           :IsA(class)  :Clone()  :Destroy()  :GetFullName()  :IsDescendantOf(x)\n"
        "Events     .Touched(other)  .ChildAdded(child)  .Destroying()\n"
        "           :Connect(fn)  :Once(fn)  :Wait()   (Connect returns a connection: :Disconnect())\n"
        "Create     Instance.new(\"Part\" | \"Sphere\" | \"Wedge\")  then set .Parent = workspace\n"
        "Values     Vector3.new(x,y,z)  Color3.new / fromRGB / fromHSV  (+ - * / == :Dot :Cross :Lerp .Magnitude .Unit)\n"
        "Time       task.wait(s)  task.spawn(f, ...)  task.delay(s, f, ...)  time()  RunService.Heartbeat:Connect(function(dt) end)\n"
        "Input      UserInputService:IsKeyDown(Enum.KeyCode.E)  .InputBegan / .InputEnded (input, processed)\n"
        "Player     Players.LocalPlayer.Character (the player's part)   :LoadCharacter() respawns\n"
        "Game       game:Message(text, seconds)  game:Win(text)  game:Lose(text)   workspace.Gravity\n"
        "Modules    require(\"name\") loads scripts/name.lua   (dots = folders: \"lib.util\")\n"
        "Notes      Lua 5.4. No io / os.execute / load. A script that never yields is stopped after ~1 s.";
}

std::unique_ptr<Object> Interface::makeScriptObject(Object* parent, const std::string& name, const std::string& source) {
    const std::string path = scriptStore.uniquePath(name);
    scriptStore.create(path, source); // lives in the store (dirty) until the scene is saved
    auto script = std::make_unique<Object>(name, std::vector<Vertex>());
    script->kind = Object::Kind::Script;
    script->scriptPath = path;
    if (parent) script->setParent(parent);
    return script;
}

void Interface::addScriptTo(Object* part) {
    if (!part || part->kind != Object::Kind::Part || isPlaying()) return;
    auto script = makeScriptObject(part, "Script", kEmptyScript);
    Object* raw = script.get();
    history.execute(std::make_unique<AddObjectCommand>(scene, std::move(script), "Add Script to " + part->name));
    openScriptEditor(raw);
    showStatus("Added a script to " + part->name);
}

void Interface::insertTemplate(const std::string& kind) {
    if (kind == "Script") {
        // Attached to the selected part, or scene-level if nothing (or several things) is selected.
        Object* parent = (selectedObjects.size() == 1 && primaryObject && primaryObject->kind == Object::Kind::Part) ? primaryObject : nullptr;
        auto script = makeScriptObject(parent, "Script", kEmptyScript);
        Object* raw = script.get();
        history.execute(std::make_unique<AddObjectCommand>(scene, std::move(script), "Insert Script"));
        selectObject(raw, false);
        openScriptEditor(raw);
        showStatus(parent ? "Inserted a script attached to " + parent->name : "Inserted a scene-level script");
        return;
    }

    std::vector<Vertex> mesh = createCubeVertices();
    if (kind == "Sphere" || kind == "Coin") mesh = createSphereVertices();
    else if (kind == "Wedge") mesh = createTriangle3DVertices();

    Texture* texture = defaultTexture->isValid() ? defaultTexture.get() : nullptr;
    const bool plain = kind == "Part" || kind == "Sphere" || kind == "Wedge";
    auto object = std::make_unique<Object>(makeUniqueName(kind), mesh, plain ? texture : nullptr);
    object->position = camera->target;

    const char* scriptSource = nullptr;
    auto& g = object->game;
    if (kind == "Baseplate") {
        object->position = glm::vec3(0.0f, -0.5f, 0.0f);
        object->scale = glm::vec3(60.0f, 1.0f, 60.0f);
        object->color = glm::vec3(0.36f, 0.38f, 0.40f);
    } else if (kind == "SpawnPoint") {
        object->scale = glm::vec3(3.0f, 0.4f, 3.0f);
        object->color = glm::vec3(0.30f, 0.80f, 0.45f);
    } else if (kind == "KillBrick") {
        object->scale = glm::vec3(4.0f, 0.4f, 4.0f);
        object->color = glm::vec3(0.90f, 0.15f, 0.15f);
        scriptSource = kKillBrickScript;
    } else if (kind == "Spinner") {
        object->scale = glm::vec3(8.0f, 0.5f, 0.8f);
        object->color = glm::vec3(0.95f, 0.55f, 0.10f);
        scriptSource = kSpinnerScript;
    } else if (kind == "Coin") {
        object->scale = glm::vec3(0.7f);
        object->color = glm::vec3(1.0f, 0.85f, 0.10f);
        g.canCollide = false;
        scriptSource = kCoinScript;
    } else if (kind == "JumpPad") {
        object->scale = glm::vec3(2.0f, 0.3f, 2.0f);
        object->color = glm::vec3(0.20f, 0.85f, 0.95f);
        scriptSource = kJumpPadScript;
    } else if (kind == "FallingBlock") {
        object->position += glm::vec3(0.0f, 5.0f, 0.0f);
        object->color = glm::vec3(0.60f, 0.35f, 0.90f);
        g.anchored = false;
        g.bounciness = 0.3f;
    } else if (kind == "Finish") {
        object->scale = glm::vec3(4.0f, 0.3f, 4.0f);
        object->color = glm::vec3(1.0f, 0.80f, 0.20f);
        scriptSource = kFinishScript;
    }

    // Like Roblox Studio: a new part lands ON whatever is under the camera
    // pivot instead of inside it (all inserts used to pile up at one spot).
    if (kind != "Baseplate" && kind != "FallingBlock") {
        GameRuntime::Box box = GameRuntime::worldBox(*object);
        float floorY = -1e30f;
        for (Object* other : scene.getObjects()) {
            if (other->kind != Object::Kind::Part) continue;
            const GameRuntime::Box ob = GameRuntime::worldBox(*other);
            const bool underneath = box.lo.x < ob.hi.x && box.hi.x > ob.lo.x && box.lo.z < ob.hi.z && box.hi.z > ob.lo.z;
            if (underneath) floorY = std::max(floorY, ob.hi.y);
        }
        if (floorY < -1e29f) floorY = 0.0f; // nothing below: stand on the ground plane
        object->position.y += floorY - box.lo.y;
    }

    Object* raw = object.get();
    if (scriptSource) {
        // One undo step adds the part and the Lua script attached to it.
        auto script = makeScriptObject(raw, raw->name + "Script", scriptSource);
        std::vector<std::unique_ptr<Command>> commands;
        commands.push_back(std::make_unique<AddObjectCommand>(scene, std::move(object), "Insert " + kind));
        commands.push_back(std::make_unique<AddObjectCommand>(scene, std::move(script), "Add script"));
        history.execute(std::make_unique<CompositeCommand>("Insert " + kind, std::move(commands)));
    } else {
        history.execute(std::make_unique<AddObjectCommand>(scene, std::move(object), "Insert " + kind));
    }
    selectObject(raw, false);
    showStatus("Inserted " + raw->name);
}

void Interface::gamePropertiesUI(Object* object) {
    pushHeading("Game");
    auto& g = object->game;
    ImGui::Checkbox("Anchored", &g.anchored);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Anchored parts are never moved by physics.\nTurn off to make the part fall and collide.");
    ImGui::SameLine();
    ImGui::Checkbox("Can Collide", &g.canCollide);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the player and other parts pass through (touch events still fire).");
    ImGui::SameLine();
    ImGui::Checkbox("Visible", &g.visible);
    if (!g.anchored) ImGui::SliderFloat("Bounciness", &g.bounciness, 0.0f, 1.0f, "%.2f");

    ImGui::Spacing();
    pushHeading("Scripts");
    int attached = 0;
    for (Object* child : object->children) {
        if (child->kind != Object::Kind::Script) continue;
        ImGui::PushID(child);
        ImGui::Text(ICON_FA_CODE " %s", child->name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Edit")) openScriptEditor(child);
        ImGui::PopID();
        ++attached;
    }
    if (attached == 0) ImGui::TextDisabled("No scripts attached.");
    if (ImGui::Button(ICON_FA_CODE " Add Script")) addScriptTo(object);
}

void Interface::scriptPropertiesUI(Object* script) {
    pushHeading("Script");
    if (script->scriptPath.empty()) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "No source file");
    else ImGui::TextDisabled("%s", script->scriptPath.c_str());
    ImGui::TextDisabled(script->parent ? "Runs attached to the parent part (script.Parent)" : "Scene-level script (script.Parent is workspace)");

    ImGui::Checkbox("Enabled", &script->scriptEnabled);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("A disabled script does not run when the game starts.");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CODE " Edit")) openScriptEditor(script);

    if (!script->scriptPath.empty() && !scriptStore.has(script->scriptPath)) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "File not found next to the scene.");
    }
}

void Interface::openScriptEditor(Object* script) {
    if (!script || script->kind != Object::Kind::Script) return;
    scriptTarget = script;
    showScriptEditor = true;
    if (script->scriptPath.empty()) {
        script->scriptPath = scriptStore.uniquePath(script->name);
    }
    if (!scriptStore.has(script->scriptPath) && !scriptStore.load(script->scriptPath)) {
        scriptStore.create(script->scriptPath, kEmptyScript); // missing file: start a fresh one
    }
    const ScriptStore::Entry* entry = scriptStore.find(script->scriptPath);
    scriptCheckResult = entry ? GameRuntime::checkSyntax(entry->text, script->scriptPath) : std::string();
    ImGui::SetWindowFocus("Script Editor");
}

void Interface::scriptEditorUI() {
    if (!showScriptEditor) return;
    ImGui::SetNextWindowSize(ImVec2(660.0f, 540.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Editor", &showScriptEditor)) { ImGui::End(); return; }

    if (!scriptTarget || scriptTarget->kind != Object::Kind::Script) {
        ImGui::TextDisabled("No script open.");
        ImGui::TextDisabled("Double-click a script in the Explorer, or use Insert > Script (Lua).");
        ImGui::End();
        return;
    }

    const std::string path = scriptTarget->scriptPath;
    pushHeading(scriptTarget->name.c_str());
    ImGui::SameLine();
    if (scriptCheckResult.empty()) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.5f, 1.0f), ICON_FA_CHECK " no syntax errors");
    else ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), ICON_FA_EXCLAMATION_TRIANGLE " %s", scriptCheckResult.c_str());
    ImGui::TextDisabled("%s", path.c_str());

    std::string* text = scriptStore.editableText(path);
    if (!text) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "This script's file is missing.");
        if (ImGui::Button("Create file")) { scriptStore.create(path, kEmptyScript); scriptCheckResult.clear(); }
        ImGui::End();
        return;
    }

    const ScriptStore::Entry* entry = scriptStore.find(path);
    const bool dirty = entry && entry->dirty;
    ImGui::SameLine();
    if (dirty) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "(unsaved)");
    if (ImGui::SmallButton(ICON_FA_SAVE " Save file")) {
        if (scriptStore.root().empty()) showStatus("Save the scene first (File > Save): it needs a folder for its scripts", true);
        else if (scriptStore.save(path)) showStatus("Saved " + path);
        else showStatus("Could not save " + path + " (see RenderEngine_log.txt)", true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("File > Save also saves every script.");
    if (isPlaying()) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "The game is running: edits apply the next time you press Play.");

    if (ImGui::CollapsingHeader("Lua API reference")) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.75f, 0.82f, 0.95f, 1.0f));
        ImGui::TextUnformatted(kLuaReference);
        ImGui::PopStyleColor();
    }

    const ImVec2 size(-FLT_MIN, std::max(120.0f, ImGui::GetContentRegionAvail().y));
    if (ImGui::InputTextMultiline("##source", text, size, ImGuiInputTextFlags_AllowTabInput)) {
        scriptStore.touch(path);
        scriptCheckResult = GameRuntime::checkSyntax(*text, path); // live: compiling is cheap
    }
    ImGui::End();
}

void Interface::outputUI() {
    if (!showOutput || !runtime) return;
    if (!ImGui::Begin("Output", &showOutput)) { ImGui::End(); return; }

    if (ImGui::SmallButton("Clear")) runtime->clearLog();
    ImGui::Separator();

    ImGui::BeginChild("##log", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (const auto& line : runtime->log()) {
        ImGui::TextDisabled("%6.2f", line.time);
        ImGui::SameLine();
        if (line.isError) ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.40f, 1.0f), "%s", line.text.c_str());
        else ImGui::TextUnformatted(line.text.c_str());
    }
    if (runtime->log().size() != lastLogSize) { // follow new output
        lastLogSize = runtime->log().size();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

void Interface::playOverlay() {
    if (!isPlaying()) return;
    const ViewArea* vp = &viewArea;
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Coloured frame around the window = "you are in play mode".
    draw->AddRect(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x, vp->WorkPos.y + vp->WorkSize.y),
                  IM_COL32(60, 200, 110, 200), 0.0f, 0, 3.0f);

    const std::string& message = runtime->message();
    if (!message.empty()) {
        ImFont* font = headingFont ? headingFont : ImGui::GetFont();
        const float fontSize = font->FontSize * 2.0f * settings.uiScale;
        const ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, message.c_str());
        const ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.22f);
        const ImVec2 pos(center.x - textSize.x * 0.5f, center.y - textSize.y * 0.5f);
        draw->AddRectFilled(ImVec2(pos.x - 18, pos.y - 10), ImVec2(pos.x + textSize.x + 18, pos.y + textSize.y + 10), IM_COL32(10, 12, 20, 190), 8.0f);
        ImU32 color = IM_COL32(255, 255, 255, 255);
        if (runtime->outcome() == GameRuntime::Outcome::Won) color = IM_COL32(255, 215, 70, 255);
        if (runtime->outcome() == GameRuntime::Outcome::Lost) color = IM_COL32(255, 95, 85, 255);
        draw->AddText(font, fontSize, pos, color, message.c_str());
    }

    const char* hint = runtime->outcome() != GameRuntime::Outcome::None ? "Game over - press F5 to stop and return to the editor"
                     : runtime->isPaused() ? "PAUSED"
                     : "WASD move  |  Space jump  |  Shift sprint  |  Right-drag look  |  F5 stop";
    const ImVec2 hintSize = ImGui::CalcTextSize(hint);
    const ImVec2 hintPos(vp->WorkPos.x + (vp->WorkSize.x - hintSize.x) * 0.5f, vp->WorkPos.y + vp->WorkSize.y - hintSize.y - 14.0f);
    draw->AddRectFilled(ImVec2(hintPos.x - 10, hintPos.y - 5), ImVec2(hintPos.x + hintSize.x + 10, hintPos.y + hintSize.y + 5), IM_COL32(10, 12, 20, 160), 6.0f);
    draw->AddText(hintPos, IM_COL32(220, 230, 245, 255), hint);
}

// Roblox-Studio-like arrangement: toolbar on top, Explorer over Properties on
// the right, Script Editor / Settings on the left, Output / Timeline below,
// the 3D view in the middle.
void Interface::buildDefaultLayout(ImGuiID dockspaceId) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspaceId);
    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace | ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

    ImGuiID center = dockspaceId;
    const ImGuiID top = ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.08f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.22f, nullptr, &center);
    const ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.55f, nullptr, &right);
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.26f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26f, nullptr, &center);

    ImGui::DockBuilderDockWindow("Toolbar###Scene Controls", top);
    ImGui::DockBuilderDockWindow("Explorer###Object List", right);
    ImGui::DockBuilderDockWindow("Properties", rightBottom);
    ImGui::DockBuilderDockWindow("Script Editor", left);
    ImGui::DockBuilderDockWindow("Settings", left);
    ImGui::DockBuilderDockWindow("Output", bottom);
    ImGui::DockBuilderDockWindow("Timeline", bottom);
    if (ImGuiDockNode* toolbarNode = ImGui::DockBuilderGetNode(top)) {
        toolbarNode->LocalFlags |= ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoResize;
    }
    ImGui::DockBuilderFinish(dockspaceId);
}
