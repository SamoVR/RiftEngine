// Toolbox: the asset library. Ready-made models that come with RIFT and the
// ones you saved (right-click in the Explorer > Save to Toolbox), as cards with
// a picture; click one to insert it in the middle of the view, or drag it to
// where it should go.
#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "render/ImageWrite.h"
#include "render/Renderer.h"
#include "scene/SceneIO.h"

#include "IconsFontAwesome6.h"
#include <glm/gtc/matrix_transform.hpp>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>

namespace rift {

namespace fs = std::filesystem;

namespace {
    bool containsNoCase(const std::string& text, const std::string& needle) {
        if (needle.empty()) return true;
        auto lower = [](char ch) { return static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); };
        return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
    }
    ClassId rootClassId(const ToolboxItem& item) {
        const ClassInfo* info = classByName(item.rootClass);
        return info ? info->id : ClassId::Model;
    }
}

fs::path ToolboxPanel::thumbnailPath(const ToolboxItem& item) const {
    // Built-in items may sit in a read-only folder: their pictures are kept with the user's data.
    if (item.builtIn) return Paths::userData() / "ToolboxThumbnails" / (Paths::toUtf8(item.folder.filename()) + ".png");
    return item.thumbnail();
}

bool ToolboxPanel::renderThumbnail(UiContext& ui, const ToolboxItem& item) {
    nlohmann::json snapshot;
    if (!Toolbox::load(item, fs::path(), snapshot, nullptr)) return false;
    // The item alone in a little world of its own, seen from the front-right, above.
    Scene world;
    std::vector<Instance*> roots = SceneIO::subtreesFromJson(world, snapshot, &world.workspace(), true);
    glm::vec3 lo(1e30f), hi(-1e30f);
    bool any = false;
    for (Instance* r : roots) {
        Scene::forEachDescendant(*r, [&](Instance& p) {
            if (!p.isBasePart()) return;
            const glm::mat4 f = p.worldFrame();
            const glm::vec3 half = (glm::abs(glm::vec3(f[0])) * p.size.x + glm::abs(glm::vec3(f[1])) * p.size.y + glm::abs(glm::vec3(f[2])) * p.size.z) * 0.5f;
            lo = glm::min(lo, glm::vec3(f[3]) - half);
            hi = glm::max(hi, glm::vec3(f[3]) + half);
            any = true;
        });
    }
    if (!any) return false;
    const glm::vec3 center = (lo + hi) * 0.5f;
    const float radius = std::max(glm::length(hi - lo) * 0.5f, 0.5f);
    const float fov = glm::radians(40.0f);
    const glm::vec3 eye = center + glm::normalize(glm::vec3(-0.9f, 0.7f, -1.0f)) * (radius / std::sin(fov * 0.5f) * 1.05f);
    RenderCamera camera;
    camera.view = glm::lookAt(eye, center, glm::vec3(0.0f, 1.0f, 0.0f));
    camera.projection = glm::perspective(fov, 1.0f, 0.05f, 2000.0f);
    camera.position = eye;
    RenderSettings settings;
    settings.editor = false;
    settings.grid = false;
    settings.shadows = false;
    constexpr int size = 192;
    thumbnailTarget_.resize(size, size, 4);
    ui.renderer.render(world, camera, settings, thumbnailTarget_);
    std::vector<unsigned char> pixels(static_cast<size_t>(size) * size * 4);
    thumbnailTarget_.readPixels(pixels.data());
    std::error_code ec;
    const fs::path file = thumbnailPath(item);
    fs::create_directories(file.parent_path(), ec);
    const bool ok = writePng(file, size, size, pixels.data());
    ui.renderer.textures().invalidate(Paths::toUtf8(file)); // a new picture for an old path
    return ok;
}

void ToolboxPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    if (editor.toolboxSaveRequest) {
        editor.toolboxSaveRequest = false;
        std::vector<Instance*> roots = editor.selectionRoots();
        nameDraft_ = roots.empty() ? std::string() : roots[0]->name;
        descriptionDraft_.clear();
        openSave_ = true;
    }
    if (!ImGui::Begin(ICON_FA_TOOLBOX "  Toolbox###Toolbox", open)) {
        ImGui::End();
        drawPopups(ui);
        return;
    }
    const float fs_ = ImGui::GetFontSize();
    if (stale_ || ImGui::GetTime() - listedAt_ > 5.0) {
        items_ = Toolbox::list();
        stale_ = false;
        listedAt_ = ImGui::GetTime();
    }

    // Tabs, search, save.
    const char* tabs[] = { "All", "RIFT", "My Models" };
    for (int k = 0; k < 3; ++k) {
        if (k) ImGui::SameLine();
        if (ImGui::RadioButton(tabs[k], tab_ == k)) tab_ = k;
    }
    ImGui::SameLine();
    const float saveWidth = ImGui::CalcTextSize(ICON_FA_FLOPPY_DISK "  Save Selection").x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetNextItemWidth(std::max(fs_ * 6.0f, ImGui::GetContentRegionAvail().x - saveWidth - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##toolboxSearch", ICON_FA_MAGNIFYING_GLASS "  Search", &filter_);
    ImGui::SameLine();
    ImGui::BeginDisabled(!editor.hasScene() || editor.selection().empty() || editor.playing());
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save Selection")) editor.toolboxSaveRequest = true;
    ImGui::EndDisabled();
    theme::tooltip("Saves the selected objects (with their scripts and images) as a model you can insert in any project.");

    std::vector<const ToolboxItem*> shown;
    for (const ToolboxItem& item : items_) {
        if ((tab_ == 1 && !item.builtIn) || (tab_ == 2 && item.builtIn)) continue;
        if (!containsNoCase(item.name, filter_) && !containsNoCase(item.description, filter_)) continue;
        shown.push_back(&item);
    }

    if (ImGui::BeginChild("##toolboxGrid", ImVec2(0.0f, 0.0f))) {
        if (shown.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled(tab_ == 2 && filter_.empty() ? "Nothing saved yet. Select something in the scene, then Save Selection (or right-click > Save to Toolbox)."
                                                             : "Nothing matches.");
            ImGui::PopTextWrapPos();
        }
        const float card = std::floor(fs_ * 7.5f);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (card + gap)));
        bool renderedOne = false; // one new picture per frame
        for (size_t k = 0; k < shown.size(); ++k) {
            const ToolboxItem& item = *shown[k];
            if (k % columns) ImGui::SameLine();
            ImGui::PushID(Paths::toUtf8(item.folder).c_str());
            const ImVec2 a = ImGui::GetCursorScreenPos();
            const float textHeight = fs_ * 2.4f;
            const bool clicked = ImGui::InvisibleButton("##card", ImVec2(card, card + textHeight));
            const bool hovered = ImGui::IsItemHovered();
            const bool visible = ImGui::IsItemVisible();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 b(a.x + card, a.y + card + textHeight);
            dl->AddRectFilled(a, b, theme::u32(hovered ? theme::color::frameHover : theme::color::frame), fs_ * 0.4f);
            // The picture (made the first time the card shows).
            const fs::path picture = thumbnailPath(item);
            std::error_code ec;
            const std::string key = Paths::toUtf8(picture);
            if (visible && !fs::exists(picture, ec) && !renderedOne && !failed_.count(key) && editor.hasScene()) {
                renderedOne = true;
                if (!renderThumbnail(ui, item)) failed_.insert(key);
            }
            const float pad = fs_ * 0.35f;
            if (auto tex = fs::exists(picture, ec) ? ui.renderer.textures().get(key) : nullptr) {
                dl->AddImageRounded(static_cast<ImTextureID>(tex->id()), ImVec2(a.x + pad, a.y + pad), ImVec2(a.x + card - pad, a.y + card - pad), ImVec2(0, 1),
                                    ImVec2(1, 0), IM_COL32_WHITE, fs_ * 0.3f);
            } else {
                const ClassId cls = rootClassId(item);
                const char* icon = theme::classIcon(cls);
                const float size = card * 0.35f;
                const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, icon);
                dl->AddText(ImGui::GetFont(), size, ImVec2(a.x + (card - t.x) * 0.5f, a.y + (card - t.y) * 0.5f), theme::u32(theme::classColor(cls)), icon);
            }
            if (!item.builtIn) dl->AddText(ImVec2(a.x + pad * 1.4f, a.y + pad * 1.2f), theme::u32(theme::color::accentHover), ICON_FA_USER);
            // Name (two lines at most).
            dl->PushClipRect(ImVec2(a.x + pad, a.y + card - pad * 0.5f), ImVec2(b.x - pad, b.y - pad * 0.3f), true);
            dl->AddText(ImGui::GetFont(), fs_, ImVec2(a.x + pad, a.y + card - pad * 0.3f), theme::u32(theme::color::text), item.name.c_str(), nullptr, card - pad * 2.0f);
            dl->PopClipRect();
            if (hovered) {
                std::string tip = item.name;
                if (!item.description.empty()) tip += "\n" + item.description;
                tip += "\n" + std::to_string(item.objects) + (item.objects == 1 ? " object" : " objects") + (item.builtIn ? ", comes with RIFT" : ", saved by you");
                tip += "\nClick to insert, or drag it into the viewport.";
                ImGui::SetTooltip("%s", tip.c_str());
            }
            // Drag into the viewport.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const std::string payload = Paths::toUtf8(item.folder) + "\n" + item.name;
                ImGui::SetDragDropPayload(kToolboxPayload, payload.c_str(), payload.size() + 1);
                ImGui::Text(ICON_FA_CUBE "  %s", item.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (clicked && editor.hasScene() && !editor.playing()) editor.insertToolboxItem(item);
            if (ImGui::BeginPopupContextItem("##cardMenu")) {
                if (ImGui::MenuItem(ICON_FA_PLUS "  Insert", nullptr, false, editor.hasScene() && !editor.playing())) editor.insertToolboxItem(item);
                if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename...", nullptr, false, !item.builtIn)) {
                    target_ = item;
                    nameDraft_ = item.name;
                    openRename_ = true;
                }
                if (ImGui::MenuItem(ICON_FA_IMAGE "  New Picture", nullptr, false, editor.hasScene())) {
                    if (renderThumbnail(ui, item)) failed_.erase(key);
                }
                if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in Explorer")) Platform::revealInExplorer(Paths::toUtf8(item.folder / "model.json"));
                ImGui::Separator();
                if (ImGui::MenuItem(ICON_FA_TRASH "  Delete...", nullptr, false, !item.builtIn)) {
                    target_ = item;
                    openDelete_ = true;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
    drawPopups(ui);
}

void ToolboxPanel::drawPopups(UiContext& ui) {
    Editor& editor = ui.editor;
    const float fs_ = ImGui::GetFontSize();
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    if (openSave_) { ImGui::OpenPopup("Save to Toolbox"); openSave_ = false; }
    if (openRename_) { ImGui::OpenPopup("Rename model"); openRename_ = false; }
    if (openDelete_) { ImGui::OpenPopup("Delete model?"); openDelete_ = false; }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Save to Toolbox", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextDisabled("The selection, with its scripts and the images, models and sounds it uses,\nbecomes a model you can insert into any project.");
        ImGui::SetNextItemWidth(fs_ * 24.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##name", "Name", &nameDraft_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SetNextItemWidth(fs_ * 24.0f);
        ImGui::InputTextWithHint("##description", "Description (optional)", &descriptionDraft_);
        ImGui::Spacing();
        const bool valid = !nameDraft_.empty();
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save", ImVec2(fs_ * 8.0f, 0.0f)) || (enter && valid)) {
            ToolboxItem item;
            if (editor.saveSelectionToToolbox(nameDraft_, descriptionDraft_, &item)) {
                renderThumbnail(ui, item);
                stale_ = true;
                tab_ = 2;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename model", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::SetNextItemWidth(fs_ * 20.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##rename", &nameDraft_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::BeginDisabled(nameDraft_.empty());
        if (ImGui::Button("Rename", ImVec2(fs_ * 8.0f, 0.0f)) || (enter && !nameDraft_.empty())) {
            std::string error;
            if (!Toolbox::rename(target_, nameDraft_, &error)) editor.notify("Could not rename: " + error, NoticeLevel::Error);
            stale_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete model?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("Delete \"%s\" from your Toolbox?", target_.name.c_str());
        ImGui::TextDisabled("Copies already inserted into scenes stay.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_TRASH "  Delete", ImVec2(fs_ * 8.0f, 0.0f))) {
            std::string error;
            if (!Toolbox::remove(target_, &error)) editor.notify("Could not delete: " + error, NoticeLevel::Error);
            stale_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace rift
