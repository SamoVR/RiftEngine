// Shared bits used by several panels: the Insert menu, asset kinds, and what
// happens when an asset is dropped onto the scene.
#include "editor/panels/Panels.h"
#include "editor/Templates.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "render/ModelLoader.h"

#include "IconsFontAwesome6.h"
#include <algorithm>
#include <cctype>

namespace rift {

namespace fs = std::filesystem;

AssetKind assetKind(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return AssetKind::Folder;
    std::string ext = Paths::toUtf8(path.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tga") return AssetKind::Image;
    if (ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac") return AssetKind::Sound;
    if (ext == ".obj" || ext == ".gltf" || ext == ".glb") return AssetKind::Model;
    if (ext == ".lua") return AssetKind::Script;
    if (ext == ".json" && path.filename() == "scene.json") return AssetKind::Scene;
    return AssetKind::Other;
}

const char* assetIcon(AssetKind kind) {
    switch (kind) {
        case AssetKind::Folder: return ICON_FA_FOLDER;
        case AssetKind::Image: return ICON_FA_IMAGE;
        case AssetKind::Sound: return ICON_FA_MUSIC;
        case AssetKind::Model: return ICON_FA_SHAPES;
        case AssetKind::Script: return ICON_FA_FILE_CODE;
        case AssetKind::Scene: return ICON_FA_MAP;
        default: return ICON_FA_FILE;
    }
}

namespace {
    bool classItem(Editor& editor, ClassId cls, const char* label, PartShape shape = PartShape::Block, const char* icon = nullptr) {
        if (!theme::menuItem(icon ? icon : theme::classIcon(cls), theme::classColor(cls), label)) return false;
        editor.insert(cls, shape);
        return true;
    }
}

void openInExternalEditor(Editor& editor, const std::string& scriptPath) {
    ScriptStore& store = editor.scripts();
    if (const ScriptStore::Entry* entry = store.find(scriptPath); entry && entry->dirty) store.save(scriptPath);
    const std::string file = Paths::toUtf8(store.diskPath(scriptPath));
    const std::string& program = editor.settings().externalEditor;
    bool ok = false;
    if (!program.empty()) ok = Platform::launch(program, { file });
    else {
        // Windows' choice for .lua; without one, Notepad.
        ok = Platform::launch(file, {});
        if (!ok) ok = Platform::launch("notepad.exe", { file });
    }
    if (!ok) editor.notify("Could not open " + scriptPath + " in the external editor (Preferences > Scripts)", NoticeLevel::Error);
}

void drawSideSwitch(Editor& editor, float height) {
    if (!editor.playing()) return;
    const float h = height > 0.0f ? height : ImGui::GetFrameHeight();
    struct Option { NetworkSide side; const char* label; const char* tip; ImVec4 color; };
    const Option options[2] = {
        { NetworkSide::Client, ICON_FA_LAPTOP "  Client", "The player's view: you control the game, LocalScripts and the GUI run here.", { 0.20f, 0.48f, 0.85f, 1.0f } },
        { NetworkSide::Server, ICON_FA_SERVER "  Server", "The server's world: fly around with the editor camera, select and move things.\nChanges replicate to the client. Scripts run here.", { 0.16f, 0.58f, 0.36f, 1.0f } },
    };
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    for (int k = 0; k < 2; ++k) {
        const Option& o = options[k];
        const bool active = editor.viewSide == o.side;
        if (k) ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, active ? o.color : ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active ? o.color : ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        if (ImGui::Button(o.label, ImVec2(0.0f, h)) && !active) editor.setViewSide(o.side);
        ImGui::PopStyleColor(2);
        theme::tooltip(o.tip);
    }
    ImGui::PopStyleVar();

    // Several test players: which one you are.
    GameSession* s = editor.session();
    if (s && s->clientCount() > 1) {
        ImGui::SameLine();
        const Instance* current = s->player();
        const std::string label = std::string(ICON_FA_USER "  ") + (current ? current->name : std::string("Player"));
        ImGui::SetNextItemWidth(ImGui::CalcTextSize(label.c_str()).x + ImGui::GetFrameHeight() * 1.6f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, (h - ImGui::GetFontSize()) * 0.5f));
        if (ImGui::BeginCombo("##player", label.c_str())) {
            for (int k = 0; k < s->clientCount(); ++k) {
                const Instance* p = s->scene().find(s->client(k).playerId);
                const std::string name = p ? p->name : "Player" + std::to_string(k + 1);
                if (ImGui::Selectable(name.c_str(), k == s->focus())) editor.setViewPlayer(k);
            }
            ImGui::EndCombo();
        }
        ImGui::PopStyleVar();
        theme::tooltip("Which test player you control and see (the others stand still).");
    }
}

void drawInsertMenuItems(Editor& editor) {
    if (!editor.hasScene()) {
        ImGui::TextDisabled("Open a scene first");
        return;
    }
    ImGui::SeparatorText("Parts");
    classItem(editor, ClassId::Part, "Block");
    classItem(editor, ClassId::Part, "Sphere", PartShape::Ball, ICON_FA_CIRCLE);
    classItem(editor, ClassId::Part, "Cylinder", PartShape::Cylinder, ICON_FA_DATABASE);
    classItem(editor, ClassId::Part, "Wedge", PartShape::Wedge, ICON_FA_STAIRS);
    classItem(editor, ClassId::Part, "Corner Wedge", PartShape::CornerWedge, ICON_FA_MOUNTAIN);
    classItem(editor, ClassId::SpawnLocation, "Spawn Location");
    classItem(editor, ClassId::Seat, "Seat");
    theme::tooltip("Characters sit down when they touch it; Space gets them up.");

    ImGui::SeparatorText("Objects");
    classItem(editor, ClassId::Model, "Model");
    classItem(editor, ClassId::Folder, "Folder");
    classItem(editor, ClassId::ProximityPrompt, "Proximity Prompt");
    theme::tooltip("Goes into the selected part: \"E Interact\" shows up near it, and Triggered fires when a player presses E.");
    classItem(editor, ClassId::PointLight, "Point Light");
    classItem(editor, ClassId::SpotLight, "Spot Light");
    classItem(editor, ClassId::Sound, "Sound");
    classItem(editor, ClassId::Tool, "Tool");
    theme::tooltip("Something players hold: goes into StarterPack with a Handle part. Click activates it.");
    if (theme::beginMenu(ICON_FA_HASHTAG, theme::classColor(ClassId::IntValue), "Values")) {
        classItem(editor, ClassId::IntValue, "IntValue");
        classItem(editor, ClassId::NumberValue, "NumberValue");
        classItem(editor, ClassId::StringValue, "StringValue");
        classItem(editor, ClassId::BoolValue, "BoolValue");
        classItem(editor, ClassId::Vector3Value, "Vector3Value");
        classItem(editor, ClassId::Color3Value, "Color3Value");
        ImGui::EndMenu();
    }

    ImGui::SeparatorText("Code");
    classItem(editor, ClassId::Script, "Script");
    theme::tooltip("Runs on the server: game rules, data, anything every player shares.");
    classItem(editor, ClassId::LocalScript, "LocalScript");
    theme::tooltip("Runs on the client: input, camera and GUI. Lives in StarterPlayerScripts, StarterGui or StarterCharacterScripts.");
    classItem(editor, ClassId::ModuleScript, "ModuleScript");
    if (theme::beginMenu(ICON_FA_TOWER_BROADCAST, theme::classColor(ClassId::RemoteEvent), "Communication")) {
        classItem(editor, ClassId::RemoteEvent, "RemoteEvent");
        theme::tooltip("Client <-> server messages: FireServer / OnServerEvent, FireClient / OnClientEvent.");
        classItem(editor, ClassId::RemoteFunction, "RemoteFunction");
        theme::tooltip("Client <-> server calls with a result: InvokeServer / OnServerInvoke.");
        classItem(editor, ClassId::BindableEvent, "BindableEvent");
        theme::tooltip("Messages between scripts on the same side.");
        classItem(editor, ClassId::BindableFunction, "BindableFunction");
        ImGui::EndMenu();
    }

    ImGui::SeparatorText("More");
    if (theme::beginMenu(ICON_FA_FIRE, theme::classColor(ClassId::ParticleEmitter), "Effects")) {
        if (theme::menuItem(theme::classIcon(ClassId::Attachment), theme::classColor(ClassId::Attachment), "Attachment")) editor.insertEffect("Attachment");
        theme::tooltip("A point on the selected part. Beams and trails go between two; particles can come out of one.");
        if (theme::menuItem(theme::classIcon(ClassId::ParticleEmitter), theme::classColor(ClassId::ParticleEmitter), "ParticleEmitter")) editor.insertEffect("ParticleEmitter");
        theme::tooltip("Particles from the selected part (or a new invisible one).");
        if (theme::menuItem(theme::classIcon(ClassId::Beam), theme::classColor(ClassId::Beam), "Beam")) editor.insertEffect("Beam");
        theme::tooltip("Select two parts: a beam between them. One part: across it.");
        if (theme::menuItem(theme::classIcon(ClassId::Trail), theme::classColor(ClassId::Trail), "Trail")) editor.insertEffect("Trail");
        theme::tooltip("A fading ribbon behind the selected part as it moves.");
        ImGui::SeparatorText("Ready-made");
        static const char* const icons[] = { ICON_FA_FIRE, ICON_FA_SMOG, ICON_FA_WAND_MAGIC_SPARKLES, ICON_FA_BOLT, ICON_FA_WIND };
        static const ImVec4 colors[] = { { 1.0f, 0.55f, 0.2f, 1.0f }, { 0.7f, 0.7f, 0.75f, 1.0f }, { 1.0f, 0.85f, 0.35f, 1.0f }, { 0.65f, 0.45f, 1.0f, 1.0f }, { 0.55f, 0.8f, 1.0f, 1.0f } };
        const auto& presets = Editor::effectPresets();
        for (size_t k = 0; k < presets.size(); ++k) {
            if (theme::menuItem(icons[k % 5], colors[k % 5], presets[k].second.c_str())) editor.insertEffect(presets[k].first);
        }
        ImGui::EndMenu();
    }
    if (theme::beginMenu(ICON_FA_PERSON_RUNNING, theme::classColor(ClassId::Animation), "Animation")) {
        if (theme::menuItem(ICON_FA_PERSON, theme::classColor(ClassId::Humanoid), "Character Rig")) editor.insertRig();
        theme::tooltip("A body to animate, like the players' characters: open the Animation Editor with it selected.");
        classItem(editor, ClassId::Animation, "Animation");
        theme::tooltip("Points at an animation (AnimationId); scripts load it with animator:LoadAnimation(animation).");
        classItem(editor, ClassId::AnimationController, "AnimationController");
        theme::tooltip("Lets a model without a Humanoid play animations.");
        classItem(editor, ClassId::Animator, "Animator");
        classItem(editor, ClassId::Motor6D, "Motor6D");
        theme::tooltip("A joint: animations turn Part1 around it.");
        classItem(editor, ClassId::KeyframeSequence, "KeyframeSequence");
        ImGui::EndMenu();
    }
    if (theme::beginMenu(ICON_FA_DESKTOP, theme::classColor(ClassId::ScreenGui), "GUI")) {
        classItem(editor, ClassId::ScreenGui, "ScreenGui");
        classItem(editor, ClassId::Frame, "Frame");
        classItem(editor, ClassId::TextLabel, "TextLabel");
        classItem(editor, ClassId::TextButton, "TextButton");
        classItem(editor, ClassId::ImageLabel, "ImageLabel");
        classItem(editor, ClassId::ImageButton, "ImageButton");
        ImGui::Separator();
        classItem(editor, ClassId::BillboardGui, "BillboardGui");
        theme::tooltip("Floats over the selected part, always facing the camera: name tags, health bars.");
        classItem(editor, ClassId::SurfaceGui, "SurfaceGui");
        theme::tooltip("Covers a face of the selected part: signs, screens, buttons on walls.");
        ImGui::Separator();
        classItem(editor, ClassId::UIGradient, "UIGradient");
        theme::tooltip("Goes inside the selected GUI element and shades it with a gradient.");
        ImGui::EndMenu();
    }
    if (theme::beginMenu(ICON_FA_WAND_MAGIC_SPARKLES, theme::color::warning, "Gameplay")) {
        for (const PartTemplate& t : partTemplates()) {
            const char* icon = t.sceneScript ? ICON_FA_TROPHY : t.tool ? theme::classIcon(ClassId::Tool) : t.cls == ClassId::SpawnLocation ? ICON_FA_FLAG : ICON_FA_PUZZLE_PIECE;
            const ImVec4 color = t.sceneScript ? theme::color::warning : ImVec4(t.color.r, t.color.g, t.color.b, 1.0f);
            if (theme::menuItem(icon, color, t.title.c_str())) editor.insertTemplate(t.id);
            theme::tooltip(t.description.c_str());
        }
        ImGui::EndMenu();
    }
}

void dropAsset(Editor& editor, const std::string& stored, Instance* target) {
    const fs::path file = Paths::resolveAsset(stored);
    std::string name = Paths::toUtf8(file.stem());
    switch (assetKind(file)) {
        case AssetKind::Image:
            if (target && target->isBasePart()) editor.setProperty({ target->id }, "Texture", AssetPath{ stored });
            else if (target && (target->cls == ClassId::ImageLabel || target->cls == ClassId::ImageButton)) editor.setProperty({ target->id }, "Image", AssetPath{ stored });
            else if (target && target->isGui()) {
                Instance* parent = target->isGuiObject() || target->isLayerCollector() ? target : nullptr;
                if (parent) editor.insertWith(ClassId::ImageLabel, parent, [&](Instance& i) {
                    i.name = name;
                    i.texture.path = stored;
                    i.guiSize = { 0.0f, 160.0f, 0.0f, 160.0f };
                    i.backgroundTransparency = 1.0f;
                });
            } else editor.notify("Drop an image onto a part or a GUI element", NoticeLevel::Info);
            break;
        case AssetKind::Sound: {
            Instance* parent = target && target->isBasePart() ? target : editor.scene().service(ClassId::SoundService);
            editor.insertWith(ClassId::Sound, parent, [&](Instance& i) {
                i.name = name;
                i.soundId.path = stored;
            });
            break;
        }
        case AssetKind::Model: {
            // The model's own size (1 unit = 1 stud), colour and texture.
            ModelData model;
            std::string error;
            if (!loadModel(file, model, &error)) {
                editor.notify("Cannot import " + Paths::toUtf8(file.filename()) + ": " + error, NoticeLevel::Error);
                break;
            }
            glm::vec3 size = model.nativeSize;
            // Tiny or huge models (other units) get a sensible size; the shape is kept.
            const float largest = std::max({ size.x, size.y, size.z });
            if (largest < 0.5f || largest > 200.0f) size *= 8.0f / largest;
            Instance& workspace = editor.scene().workspace();
            const glm::vec3 at = editor.placementPoint(size);
            std::error_code ec;
            const bool texture = !model.texture.empty() && std::filesystem::exists(model.texture, ec);
            editor.insertWith(ClassId::MeshPart, &workspace, [&](Instance& i) {
                i.name = name;
                i.mesh.path = stored;
                i.size = size;
                i.position = at;
                if (model.hasColor) i.color = Color3::from(model.color);
                else i.color = Color3(0.85f, 0.85f, 0.85f);
                if (texture) {
                    i.texture.path = Paths::toStored(model.texture);
                    i.textureScale = glm::vec2(0.0f); // the model's own UVs
                    if (!model.hasColor) i.color = Color3(1.0f, 1.0f, 1.0f);
                }
            });
            break;
        }
        case AssetKind::Scene:
            editor.requestDiscard([&editor, file] { editor.openScene(file); });
            break;
        case AssetKind::Script:
            editor.notify("Scripts are added with Insert > Script (then paste the code)", NoticeLevel::Info);
            break;
        default:
            editor.notify("RIFT does not know what to do with " + Paths::toUtf8(file.filename()), NoticeLevel::Info);
            break;
    }
}

} // namespace rift
