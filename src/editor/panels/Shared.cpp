// Shared bits used by several panels: the Insert menu, asset kinds, and what
// happens when an asset is dropped onto the scene.
#include "editor/panels/Panels.h"
#include "editor/Templates.h"
#include "editor/Theme.h"
#include "core/Paths.h"
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

    ImGui::SeparatorText("Objects");
    classItem(editor, ClassId::Model, "Model");
    classItem(editor, ClassId::Folder, "Folder");
    classItem(editor, ClassId::PointLight, "Point Light");
    classItem(editor, ClassId::SpotLight, "Spot Light");
    classItem(editor, ClassId::Sound, "Sound");
    if (ImGui::BeginMenu(ICON_FA_HASHTAG "  Values")) {
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
    classItem(editor, ClassId::ModuleScript, "ModuleScript");

    ImGui::SeparatorText("More");
    if (ImGui::BeginMenu(ICON_FA_DESKTOP "  GUI")) {
        classItem(editor, ClassId::ScreenGui, "ScreenGui");
        classItem(editor, ClassId::Frame, "Frame");
        classItem(editor, ClassId::TextLabel, "TextLabel");
        classItem(editor, ClassId::TextButton, "TextButton");
        classItem(editor, ClassId::ImageLabel, "ImageLabel");
        classItem(editor, ClassId::ImageButton, "ImageButton");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ICON_FA_WAND_MAGIC_SPARKLES "  Gameplay")) {
        for (const PartTemplate& t : partTemplates()) {
            const char* icon = t.sceneScript ? ICON_FA_TROPHY : t.cls == ClassId::SpawnLocation ? ICON_FA_FLAG : ICON_FA_PUZZLE_PIECE;
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
                Instance* parent = target->isGuiObject() || target->cls == ClassId::ScreenGui ? target : nullptr;
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
