#include "editor/panels/Panels.h"
#include "editor/panels/SequenceEditors.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "render/Renderer.h"

#include "IconsFontAwesome6.h"
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace rift {

namespace fs = std::filesystem;

namespace {
    const ImVec4 kAxisColors[4] = {
        { 0.93f, 0.33f, 0.36f, 1.0f }, // X
        { 0.45f, 0.82f, 0.36f, 1.0f }, // Y
        { 0.33f, 0.56f, 0.96f, 1.0f }, // Z
        { 0.70f, 0.70f, 0.75f, 1.0f },
    };

    bool containsNoCase(const std::string& text, const char* needle) {
        const size_t n = std::strlen(needle);
        if (n == 0) return true;
        for (size_t p = 0; p + n <= text.size(); ++p) {
            size_t k = 0;
            while (k < n && std::tolower(static_cast<unsigned char>(text[p + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
            if (k == n) return true;
        }
        return false;
    }

    // Just enough decimals for the value ("128", "0.5", "1.25").
    const char* formatFor(float v) {
        // The fewest decimals (up to 3) that show the value as it would round to 3.
        static const char* const formats[] = { "%.0f", "%.1f", "%.2f", "%.3f" };
        const double a = std::abs(static_cast<double>(v));
        double scale = 1.0;
        for (int d = 0; d < 3; ++d, scale *= 10.0)
            if (std::abs(a - std::round(a * scale) / scale) < 0.0005) return formats[d];
        return formats[3];
    }

    float dragSpeed(const PropertyInfo& p) {
        if (p.step > 0.0f) return p.step;
        if (p.bounded()) return std::max((p.maxValue - p.minValue) / 200.0f, 0.001f);
        return 0.1f;
    }

    // N float fields side by side with a coloured bar per axis. Returns the
    // index of the field that changed (-1 = none). `mixed[i]`: show a dash.
    int multiFloat(const char* id, float* v, int count, float speed, float lo, float hi, const bool* mixed, const char* const* labels = nullptr) {
        int changed = -1;
        ImGui::PushID(id);
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        const float width = (ImGui::GetContentRegionAvail().x - spacing * (count - 1)) / count;
        for (int i = 0; i < count; ++i) {
            if (i) ImGui::SameLine(0.0f, spacing);
            ImGui::PushID(i);
            ImGui::SetNextItemWidth(width);
            if (v[i] == 0.0f) v[i] = 0.0f; // never show "-0"
            const char* fmt = mixed && mixed[i] ? "--" : formatFor(v[i]);
            std::string format = labels ? std::string(labels[i]) + " " + fmt : fmt;
            if (ImGui::DragFloat("##c", &v[i], speed, lo, hi, format.c_str(), ImGuiSliderFlags_NoRoundToFormat)) changed = i;
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            if (!labels) {
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(a.x, a.y + 3.0f), ImVec2(a.x + 2.5f, b.y - 3.0f), theme::u32(kAxisColors[std::min(i, 3)]), 1.0f);
            }
            ImGui::PopID();
        }
        ImGui::PopID();
        return changed;
    }

    std::vector<uint64_t> idsOf(const std::vector<Instance*>& list) {
        std::vector<uint64_t> ids;
        for (const Instance* i : list) ids.push_back(i->id);
        return ids;
    }

    // Copies `file` into the project's assets folder unless it is already in the project.
    std::string importAsset(Editor& editor, const std::string& file, const char* subfolder) {
        const fs::path path = Paths::fromUtf8(file);
        if (!editor.hasProject()) return Paths::toStored(path);
        const fs::path root = editor.project().root();
        std::error_code ec;
        const fs::path rel = fs::relative(path, root, ec);
        if (!ec && !rel.empty() && rel.native()[0] != L'.') return Paths::toStored(path);
        const fs::path dir = editor.project().assetsFolder() / subfolder;
        fs::create_directories(dir, ec);
        fs::path target = dir / path.filename();
        for (int n = 2; fs::exists(target, ec); ++n)
            target = dir / (path.stem().wstring() + L"_" + std::to_wstring(n) + path.extension().wstring());
        fs::copy_file(path, target, ec);
        if (ec) {
            editor.notify("Could not copy " + file + " into the project: " + ec.message(), NoticeLevel::Error);
            return Paths::toStored(path);
        }
        editor.notify("Imported " + Paths::toUtf8(target.filename()) + " into assets/" + subfolder, NoticeLevel::Success);
        return Paths::toStored(target);
    }

    // Property names that hold each kind of asset.
    AssetKind assetKindOf(const PropertyInfo& p) {
        if (p.name == "SoundId") return AssetKind::Sound;
        if (p.name == "MeshId") return AssetKind::Model;
        return AssetKind::Image;
    }
}

namespace {
    // Category headers in graphite rather than the selection colour.
    bool categoryHeader(const char* label, ImGuiTreeNodeFlags flags = 0) {
        ImGui::PushStyleColor(ImGuiCol_Header, theme::color::frame);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::color::frameHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::color::frameHover);
        ImGui::PushFont(theme::fonts().medium, 0.0f);
        const bool open = ImGui::CollapsingHeader(label, flags);
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        return open;
    }
}

void PropertiesPanel::draw(UiContext& ui, bool* open) {
    if (!ImGui::Begin(ICON_FA_SLIDERS "  Properties###Properties", open)) {
        ImGui::End();
        return;
    }
    Editor& editor = ui.editor;
    std::vector<Instance*> list = editor.selectedInstances();
    if (list.empty()) {
        const char* text = "Select an object to see its properties";
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2(std::max(8.0f, (ImGui::GetWindowWidth() - size.x) * 0.5f), ImGui::GetWindowHeight() * 0.4f));
        ImGui::TextDisabled("%s", text);
        ImGui::End();
        return;
    }
    Instance& primary = *list.back();

    // ---- header
    theme::pushHeading(1.1f);
    ImGui::TextColored(theme::classColor(primary.cls), "%s", theme::classIcon(primary.cls));
    ImGui::SameLine();
    if (list.size() == 1) ImGui::TextUnformatted(primary.name.c_str());
    else ImGui::Text("%d objects", static_cast<int>(list.size()));
    theme::pop();
    ImGui::SameLine();
    ImGui::TextDisabled("%s", className(primary.cls));
    if (primary.isScript() && list.size() == 1) {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Open Script").x - ImGui::GetFrameHeight() - 8.0f);
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE " Open Script")) editor.openScript(primary.id);
    }
    if (!classInfo(primary.cls).description.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::textDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(classInfo(primary.cls).description.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter properties", filter_, sizeof(filter_));
    ImGui::Spacing();

    ImGui::BeginChild("##props", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    // Properties every selected class has (multi-selection edits them all).
    std::vector<const PropertyInfo*> props;
    for (const PropertyInfo& p : classInfo(primary.cls).props) {
        if (!p.has(PF_Editor)) continue;
        if (!containsNoCase(p.name, filter_)) continue;
        bool common = true;
        for (const Instance* i : list) {
            const PropertyInfo* other = findProperty(i->cls, p.name);
            if (!other || other->type != p.type || !other->has(PF_Editor)) { common = false; break; }
        }
        if (common) props.push_back(&p);
    }
    std::vector<std::string> categories;
    for (const PropertyInfo* p : props) if (std::find(categories.begin(), categories.end(), p->category) == categories.end()) categories.push_back(p->category);

    for (const std::string& category : categories) {
        const bool openCategory = categoryHeader(category.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        if (!openCategory) continue;
        if (ImGui::BeginTable(category.c_str(), 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.38f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
            for (const PropertyInfo* p : props) {
                if (p->category != category) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                if (p->has(PF_ReadOnlyUI) && p->name != "Source") ImGui::TextDisabled("%s", p->name.c_str());
                else ImGui::TextUnformatted(p->name.c_str());
                theme::tooltip(p->tooltip.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::PushID(p->name.c_str());
                drawProperty(ui, list, *p);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (!filter_[0]) {
        drawAttributes(ui, list);
        if (list.size() == 1) drawTags(ui, primary);
    }
    ImGui::EndChild();
    ImGui::End();
}

void PropertiesPanel::drawProperty(UiContext& ui, const std::vector<Instance*>& list, const PropertyInfo& prop) {
    Editor& editor = ui.editor;
    Instance& primary = *list.back();
    const Value value = getProperty(primary, prop);
    const std::vector<uint64_t> ids = idsOf(list);
    bool mixed = false;
    for (const Instance* i : list) if (!valuesEqual(getProperty(*i, prop), value)) mixed = true;
    const bool readOnly = prop.has(PF_ReadOnlyUI);
    const float lo = prop.bounded() ? prop.minValue : 0.0f, hi = prop.bounded() ? prop.maxValue : 0.0f;
    ImGui::SetNextItemWidth(-FLT_MIN);

    // Script source: the file, with a button to open it.
    if (prop.name == "Source") {
        const std::string& path = std::get<std::string>(value);
        if (ImGui::Button(ICON_FA_PEN_TO_SQUARE)) editor.openScript(primary.id);
        theme::tooltip("Open in the script editor");
        ImGui::SameLine();
        ImGui::TextDisabled("%s", path.empty() ? "(no file yet)" : path.c_str());
        return;
    }
    if (readOnly) ImGui::BeginDisabled();

    switch (prop.type) {
        case ValueType::Bool: {
            bool v = std::get<bool>(value);
            if (mixed) ImGui::PushItemFlag(ImGuiItemFlags_MixedValue, true);
            if (ImGui::Checkbox("##v", &v)) editor.setProperty(ids, prop.name, mixed ? true : v);
            if (mixed) ImGui::PopItemFlag();
            break;
        }
        case ValueType::Int: {
            int v = std::get<int>(value);
            if (ImGui::DragInt("##v", &v, std::max(dragSpeed(prop), 0.1f), static_cast<int>(lo), static_cast<int>(hi), mixed ? "--" : "%d"))
                editor.previewProperty(ids, prop.name, v);
            break;
        }
        case ValueType::Float: {
            float v = std::get<float>(value);
            if (ImGui::DragFloat("##v", &v, dragSpeed(prop), lo, hi, mixed ? "--" : formatFor(v), ImGuiSliderFlags_NoRoundToFormat | (prop.bounded() ? ImGuiSliderFlags_AlwaysClamp : 0)))
                editor.previewProperty(ids, prop.name, v);
            break;
        }
        case ValueType::String: {
            std::string v = mixed ? std::string() : std::get<std::string>(value);
            const bool multiline = prop.name == "Text";
            if (multiline) ImGui::InputTextMultiline("##v", &v, ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 3.2f));
            else ImGui::InputTextWithHint("##v", mixed ? "(different values)" : "", &v);
            if (ImGui::IsItemDeactivatedAfterEdit()) editor.setProperty(ids, prop.name, v);
            break;
        }
        case ValueType::Vector3: {
            const glm::vec3 v = std::get<glm::vec3>(value);
            float f[3] = { v.x, v.y, v.z };
            bool mixedAxis[3] = { false, false, false };
            for (const Instance* i : list) {
                const glm::vec3 o = std::get<glm::vec3>(getProperty(*i, prop));
                for (int k = 0; k < 3; ++k) if (o[k] != v[k]) mixedAxis[k] = true;
            }
            const int changed = multiFloat("##v", f, 3, dragSpeed(prop), lo, hi, mixedAxis);
            if (changed >= 0) {
                // Only the edited axis changes on every selected object.
                std::vector<std::pair<uint64_t, Value>> values;
                for (Instance* i : list) {
                    glm::vec3 o = std::get<glm::vec3>(getProperty(*i, prop));
                    o[changed] = f[changed];
                    values.emplace_back(i->id, o);
                }
                editor.previewValues(prop.name, values);
            }
            break;
        }
        case ValueType::Vector2: {
            const glm::vec2 v = std::get<glm::vec2>(value);
            float f[2] = { v.x, v.y };
            const bool mixed2[2] = { mixed, mixed };
            const int changed = multiFloat("##v", f, 2, dragSpeed(prop), lo, hi, mixed2);
            if (changed >= 0) {
                std::vector<std::pair<uint64_t, Value>> values;
                for (Instance* i : list) {
                    glm::vec2 o = std::get<glm::vec2>(getProperty(*i, prop));
                    o[changed] = f[changed];
                    values.emplace_back(i->id, o);
                }
                editor.previewValues(prop.name, values);
            }
            break;
        }
        case ValueType::Color3: {
            const Color3 c = std::get<Color3>(value);
            float f[3] = { c.r, c.g, c.b };
            if (ImGui::ColorEdit3("##v", f, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoOptions))
                editor.previewProperty(ids, prop.name, Color3(f[0], f[1], f[2]));
            break;
        }
        case ValueType::UDim2: {
            const UDim2 u = std::get<UDim2>(value);
            static const char* const labelsX[2] = { "Scale", "Offset" };
            float x[2] = { u.xScale, u.xOffset }, y[2] = { u.yScale, u.yOffset };
            const bool none[2] = { false, false };
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kAxisColors[0], "X");
            ImGui::SameLine();
            const int cx = multiFloat("##x", x, 2, 0.01f, 0.0f, 0.0f, none, labelsX);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kAxisColors[1], "Y");
            ImGui::SameLine();
            const int cy = multiFloat("##y", y, 2, 0.01f, 0.0f, 0.0f, none, labelsX);
            if (cx >= 0 || cy >= 0) {
                std::vector<std::pair<uint64_t, Value>> values;
                for (Instance* i : list) {
                    UDim2 o = std::get<UDim2>(getProperty(*i, prop));
                    if (cx == 0) o.xScale = x[0];
                    if (cx == 1) o.xOffset = x[1];
                    if (cy == 0) o.yScale = y[0];
                    if (cy == 1) o.yOffset = y[1];
                    values.emplace_back(i->id, o);
                }
                editor.previewValues(prop.name, values);
            }
            break;
        }
        case ValueType::Enum: {
            const int index = std::get<EnumValue>(value).index;
            const char* current = mixed ? "--" : (index >= 0 && index < static_cast<int>(prop.enumNames.size()) ? prop.enumNames[index].c_str() : "?");
            if (ImGui::BeginCombo("##v", current)) {
                for (int k = 0; k < static_cast<int>(prop.enumNames.size()); ++k) {
                    if (ImGui::Selectable(prop.enumNames[k].c_str(), !mixed && k == index)) editor.setProperty(ids, prop.name, EnumValue{ k });
                }
                ImGui::EndCombo();
            }
            break;
        }
        case ValueType::Asset: {
            const std::string path = mixed ? std::string() : std::get<AssetPath>(value).path;
            const AssetKind kind = assetKindOf(prop);
            const float button = ImGui::GetFrameHeight();
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            // Image preview
            if (kind == AssetKind::Image && !path.empty()) {
                if (auto tex = ui.renderer.textures().get(path)) {
                    ImGui::Image(static_cast<ImTextureID>(tex->id()), ImVec2(button, button), ImVec2(0, 1), ImVec2(1, 0));
                    if (ImGui::BeginItemTooltip()) {
                        ImGui::Image(static_cast<ImTextureID>(tex->id()), ImVec2(160, 160), ImVec2(0, 1), ImVec2(1, 0));
                        ImGui::Text("%d x %d", tex->width(), tex->height());
                        ImGui::EndTooltip();
                    }
                    ImGui::SameLine(0.0f, spacing);
                }
            }
            std::string text = path;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (button + spacing) * (path.empty() ? 1 : 2));
            ImGui::InputTextWithHint("##v", mixed ? "(different values)" : "Drop a file here", &text);
            if (ImGui::IsItemDeactivatedAfterEdit()) editor.setProperty(ids, prop.name, AssetPath{ text });
            if (ImGui::BeginDragDropTarget()) {
                if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
                    const std::string dropped(static_cast<const char*>(p->Data));
                    if (assetKind(Paths::resolveAsset(dropped)) == kind) editor.setProperty(ids, prop.name, AssetPath{ dropped });
                    else editor.notify("That file is not the right kind for " + prop.name, NoticeLevel::Warning);
                }
                ImGui::EndDragDropTarget();
            }
            ImGui::SameLine(0.0f, spacing);
            if (ImGui::Button(ICON_FA_FOLDER_OPEN, ImVec2(button, button))) {
                std::vector<Platform::FileFilter> filters;
                if (kind == AssetKind::Sound) filters = { { "Sounds", "*.wav;*.mp3;*.ogg;*.flac" } };
                else if (kind == AssetKind::Model) filters = { { "3D models", "*.obj;*.gltf;*.glb" } };
                else filters = { { "Images", "*.png;*.jpg;*.jpeg;*.bmp;*.tga" } };
                const std::string start = editor.hasProject() ? Paths::toUtf8(editor.project().assetsFolder()) : std::string();
                const std::string file = Platform::openFileDialog("Choose a file for " + prop.name, filters, start);
                if (!file.empty()) {
                    const char* sub = kind == AssetKind::Sound ? "sounds" : kind == AssetKind::Model ? "models" : "textures";
                    editor.setProperty(ids, prop.name, AssetPath{ importAsset(editor, file, sub) });
                }
            }
            theme::tooltip("Choose a file (files outside the project are copied into its assets folder)");
            if (!path.empty()) {
                ImGui::SameLine(0.0f, spacing);
                if (ImGui::Button(ICON_FA_XMARK, ImVec2(button, button))) editor.setProperty(ids, prop.name, AssetPath{});
                theme::tooltip("Clear");
            }
            break;
        }
        case ValueType::NumberRange: {
            NumberRange r = std::get<NumberRange>(value);
            if (numberRangeField("##v", r, dragSpeed(prop), lo, hi)) editor.previewProperty(ids, prop.name, r);
            break;
        }
        case ValueType::NumberSequence: {
            if (numberSequencePreview("##v", std::get<NumberSequence>(value), lo, hi, prop.bounded())) ImGui::OpenPopup("##sequence");
            if (ImGui::BeginPopup("##sequence")) {
                ImGui::PushFont(theme::fonts().medium, 0.0f);
                ImGui::TextUnformatted(prop.name.c_str());
                ImGui::PopFont();
                if (!prop.tooltip.empty()) {
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 26.0f);
                    ImGui::TextColored(theme::color::textDim, "%s", prop.tooltip.c_str());
                    ImGui::PopTextWrapPos();
                }
                ImGui::Separator();
                NumberSequence seq = std::get<NumberSequence>(getProperty(primary, prop));
                bool discrete = false;
                if (numberSequenceEditor(seq, lo, hi, prop.bounded(), discrete)) {
                    if (discrete) editor.setProperty(ids, prop.name, seq);
                    else editor.previewProperty(ids, prop.name, seq);
                }
                ImGui::EndPopup();
            }
            break;
        }
        case ValueType::ColorSequence: {
            if (colorSequencePreview("##v", std::get<ColorSequence>(value))) ImGui::OpenPopup("##colors");
            if (ImGui::BeginPopup("##colors")) {
                ImGui::PushFont(theme::fonts().medium, 0.0f);
                ImGui::TextUnformatted(prop.name.c_str());
                ImGui::PopFont();
                ImGui::Separator();
                ColorSequence seq = std::get<ColorSequence>(getProperty(primary, prop));
                bool discrete = false;
                if (colorSequenceEditor(seq, discrete)) {
                    if (discrete) editor.setProperty(ids, prop.name, seq);
                    else editor.previewProperty(ids, prop.name, seq);
                }
                ImGui::EndPopup();
            }
            break;
        }
        case ValueType::Ref: {
            InstanceRef ref = mixed ? InstanceRef{} : std::get<InstanceRef>(value);
            if (attachmentPicker("##v", editor.scene(), ref, primary.id)) editor.setProperty(ids, prop.name, ref);
            break;
        }
        default:
            ImGui::TextDisabled("%s", toDisplayString(value).c_str());
            break;
    }
    if (readOnly) ImGui::EndDisabled();
}

void PropertiesPanel::drawAttributes(UiContext& ui, const std::vector<Instance*>& list) {
    Editor& editor = ui.editor;
    Instance& primary = *list.back();
    const bool open = categoryHeader("Attributes");
    if (!open) return;
    const std::vector<uint64_t> ids = idsOf(list);
    Scene& scene = editor.scene();

    if (!primary.attributes.empty() && ImGui::BeginTable("##attrs", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.38f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.62f);
        std::string removeName;
        for (const Attribute& a : std::vector<Attribute>(primary.attributes)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(a.name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(a.name.c_str());
            const float remove = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - remove);
            Value edited = a.value;
            bool changed = false, live = false;
            switch (typeOf(a.value)) {
                case ValueType::Bool: { bool v = std::get<bool>(a.value); if (ImGui::Checkbox("##v", &v)) { edited = v; changed = true; } break; }
                case ValueType::Int: { int v = std::get<int>(a.value); if (ImGui::DragInt("##v", &v)) { edited = v; live = true; } break; }
                case ValueType::Float: { float v = std::get<float>(a.value); if (ImGui::DragFloat("##v", &v, 0.1f, 0.0f, 0.0f, formatFor(v), ImGuiSliderFlags_NoRoundToFormat)) { edited = v; live = true; } break; }
                case ValueType::String: { std::string v = std::get<std::string>(a.value); ImGui::InputText("##v", &v); if (ImGui::IsItemDeactivatedAfterEdit()) { edited = v; changed = true; } break; }
                case ValueType::Vector3: {
                    glm::vec3 v = std::get<glm::vec3>(a.value);
                    if (ImGui::DragFloat3("##v", &v.x, 0.1f)) { edited = v; live = true; }
                    break;
                }
                case ValueType::Color3: {
                    Color3 c = std::get<Color3>(a.value);
                    if (ImGui::ColorEdit3("##v", &c.r, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_NoLabel)) { edited = c; live = true; }
                    break;
                }
                default: ImGui::TextDisabled("%s", toDisplayString(a.value).c_str()); break;
            }
            // Drags change the value live; the undo step is recorded when they end.
            if (ImGui::IsItemActivated()) {
                attrBefore_.clear();
                for (Instance* i : list) {
                    const Value* before = i->attribute(a.name);
                    attrBefore_.push_back({ i->id, a.name, before ? *before : Value{}, {} });
                }
            }
            if (live) for (Instance* i : list) scene.setAttribute(*i, a.name, edited);
            if (changed) editor.setAttribute(ids, a.name, edited);
            if (ImGui::IsItemDeactivatedAfterEdit() && !attrBefore_.empty() && typeOf(a.value) != ValueType::String && typeOf(a.value) != ValueType::Bool) {
                for (auto& c : attrBefore_) if (Instance* i = scene.find(c.id)) { const Value* now = i->attribute(c.name); c.after = now ? *now : Value{}; }
                editor.record(std::make_unique<AttributeCommand>("Set attribute " + a.name, std::move(attrBefore_)));
                attrBefore_.clear();
            }
            ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
            if (ImGui::Button(ICON_FA_XMARK, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) removeName = a.name;
            theme::tooltip("Remove this attribute");
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (!removeName.empty()) editor.setAttribute(ids, removeName, Value{});
    }

    // Add
    static const char* const kTypes[] = { "Number", "Boolean", "String", "Vector3", "Color3" };
    if (ImGui::Button(ICON_FA_PLUS "  Add Attribute")) {
        newAttrName_[0] = '\0';
        ImGui::OpenPopup("##addAttribute");
    }
    if (ImGui::BeginPopup("##addAttribute")) {
        ImGui::TextUnformatted("New attribute");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        const bool enter = ImGui::InputTextWithHint("##name", "Name", newAttrName_, sizeof(newAttrName_), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::Combo("##type", &newAttrType_, kTypes, IM_ARRAYSIZE(kTypes));
        const bool valid = newAttrName_[0] != '\0' && !primary.attribute(newAttrName_);
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Add", ImVec2(ImGui::GetFontSize() * 12.0f, 0.0f)) || (enter && valid)) {
            Value v;
            switch (newAttrType_) {
                case 0: v = 0.0f; break;
                case 1: v = false; break;
                case 2: v = std::string(); break;
                case 3: v = glm::vec3(0.0f); break;
                default: v = Color3(1.0f, 1.0f, 1.0f); break;
            }
            editor.setAttribute(ids, newAttrName_, v);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
}

void PropertiesPanel::drawTags(UiContext& ui, Instance& instance) {
    Editor& editor = ui.editor;
    const bool open = categoryHeader("Tags");
    if (!open) return;
    const float right = ImGui::GetWindowContentRegionMax().x;
    std::string remove;
    for (const std::string& tag : instance.tags) {
        const std::string label = tag + "  " ICON_FA_XMARK;
        const float w = ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        if (&tag != &instance.tags.front()) {
            ImGui::SameLine();
            if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.25f));
        if (ImGui::SmallButton(label.c_str())) remove = tag;
        ImGui::PopStyleColor();
        theme::tooltip("Remove tag (CollectionService)");
    }
    if (!remove.empty()) editor.removeTag(instance.id, remove);
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##tag", ICON_FA_TAG "  Add a tag and press Enter", newTag_, sizeof(newTag_), ImGuiInputTextFlags_EnterReturnsTrue)) {
        editor.addTag(instance.id, newTag_);
        newTag_[0] = '\0';
        ImGui::SetKeyboardFocusHere(-1);
    }
}

} // namespace rift
