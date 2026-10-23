#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "render/Renderer.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rift {

namespace fs = std::filesystem;

namespace {
    ImVec4 kindColor(AssetKind kind) {
        switch (kind) {
            case AssetKind::Folder: return { 0.95f, 0.75f, 0.35f, 1.0f };
            case AssetKind::Image: return { 0.40f, 0.82f, 0.75f, 1.0f };
            case AssetKind::Sound: return { 0.40f, 0.85f, 0.85f, 1.0f };
            case AssetKind::Model: return { 0.62f, 0.68f, 0.95f, 1.0f };
            case AssetKind::Script: return { 0.62f, 0.55f, 0.98f, 1.0f };
            case AssetKind::Scene: return { 0.45f, 0.85f, 0.55f, 1.0f };
            default: return theme::color::textDim;
        }
    }

    bool containsNoCase(const std::string& text, const char* needle) {
        const size_t n = std::strlen(needle);
        if (!n) return true;
        for (size_t p = 0; p + n <= text.size(); ++p) {
            size_t k = 0;
            while (k < n && std::tolower(static_cast<unsigned char>(text[p + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
            if (k == n) return true;
        }
        return false;
    }

    std::string sizeText(uintmax_t bytes) {
        char buf[32];
        if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
        else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
        else std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        return buf;
    }

    // Files the browser does not show (editor bookkeeping).
    bool hidden(const fs::path& p) {
        const std::string name = Paths::toUtf8(p.filename());
        return name.empty() || name[0] == '.' || name == "scene.autosave.json" || name == "project.json" || name == "Builds";
    }
}

void AssetBrowserPanel::scan() {
    entries_.clear();
    dirty_ = false;
    std::error_code ec;
    if (folder_.empty() || !fs::is_directory(folder_, ec)) folder_ = root_;
    if (root_.empty()) return;
    for (const auto& item : fs::directory_iterator(folder_, fs::directory_options::skip_permission_denied, ec)) {
        if (hidden(item.path())) continue;
        Entry e;
        e.path = item.path();
        e.name = Paths::toUtf8(item.path().filename());
        e.kind = assetKind(item.path());
        if (e.kind != AssetKind::Folder) e.size = item.file_size(ec);
        entries_.push_back(std::move(e));
    }
    // Folders first, then by name.
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if ((a.kind == AssetKind::Folder) != (b.kind == AssetKind::Folder)) return a.kind == AssetKind::Folder;
        return a.name < b.name;
    });
}

void AssetBrowserPanel::draw(UiContext& ui, bool* open) {
    if (!ImGui::Begin(ICON_FA_FOLDER_OPEN "  Assets###Assets", open)) {
        ImGui::End();
        return;
    }
    Editor& editor = ui.editor;
    if (!editor.hasProject()) {
        ImGui::TextDisabled("Open a project to browse its files.");
        ImGui::End();
        return;
    }
    if (root_ != editor.project().root()) {
        root_ = editor.project().root();
        folder_ = editor.project().assetsFolder();
        std::error_code ec;
        if (!fs::is_directory(folder_, ec)) folder_ = root_;
        dirty_ = true;
    }
    refreshTimer_ += ui.dt;
    if (refreshTimer_ > 2.0f) {
        refreshTimer_ = 0.0f;
        dirty_ = true;
    }
    if (dirty_) scan();

    // ---- toolbar: up, breadcrumbs, filter, import, size
    std::error_code ec;
    const bool atRoot = fs::equivalent(folder_, root_, ec);
    ImGui::BeginDisabled(atRoot);
    if (theme::iconButton(ICON_FA_ARROW_UP, "Up one folder")) {
        folder_ = folder_.parent_path();
        dirty_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Breadcrumbs: Project / assets / textures
    {
        std::vector<fs::path> chain;
        for (fs::path p = folder_; ; p = p.parent_path()) {
            chain.push_back(p);
            if (fs::equivalent(p, root_, ec) || p == p.parent_path()) break;
        }
        std::reverse(chain.begin(), chain.end());
        for (size_t k = 0; k < chain.size(); ++k) {
            if (k) {
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled(ICON_FA_CHEVRON_RIGHT);
                ImGui::SameLine(0.0f, 2.0f);
            }
            const std::string label = k == 0 ? editor.project().settings().name : Paths::toUtf8(chain[k].filename());
            ImGui::PushID(static_cast<int>(k));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            if (ImGui::Button(label.c_str())) {
                folder_ = chain[k];
                dirty_ = true;
            }
            ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    const float right = ImGui::GetFontSize() * 16.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 8.0f, ImGui::GetWindowContentRegionMax().x - right));
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter", filter_, sizeof(filter_));
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_FILE_IMPORT, "Import files into this folder")) importFiles(ui);
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ROTATE, "Refresh")) dirty_ = true;
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_UP_RIGHT_FROM_SQUARE, "Show in File Explorer")) Platform::revealInExplorer(Paths::toUtf8(folder_));

    // ---- left: folder tree, right: tiles
    if (ImGui::BeginTable("##layout", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("tree", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 12.0f);
        ImGui::TableSetupColumn("files", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f));
        drawFolderTree(ui, root_);
        ImGui::EndChild();

        ImGui::TableSetColumnIndex(1);
        ImGui::BeginChild("##files", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
        // Ctrl + wheel resizes the tiles.
        if (ImGui::IsWindowHovered() && ImGui::GetIO().KeyCtrl && ImGui::GetIO().MouseWheel != 0.0f)
            tileSize_ = std::clamp(tileSize_ + ImGui::GetIO().MouseWheel * 8.0f, 56.0f, 180.0f);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float cell = tileSize_ + spacing;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + spacing) / cell));
        int column = 0;
        int shown = 0;
        for (const Entry& e : entries_) {
            if (!containsNoCase(e.name, filter_)) continue;
            if (column > 0) ImGui::SameLine();
            drawTile(ui, e);
            ++shown;
            column = (column + 1) % columns;
        }
        if (shown == 0) ImGui::TextDisabled(filter_[0] ? "No files match." : "This folder is empty. Drag files here from Windows, or use Import.");
        // Background: deselect / context menu
        if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_.clear();
        if (ImGui::BeginPopupContextWindow("##background", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            backgroundMenu(ui);
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }

    // Delete confirmation
    if (!pendingDelete_.empty()) ImGui::OpenPopup("Delete file?");
    if (ImGui::BeginPopupModal("Delete file?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Move \"%s\" to the Recycle Bin?", Paths::toUtf8(pendingDelete_.filename()).c_str());
        ImGui::TextDisabled("Scenes that use it will show it as missing.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_TRASH_CAN "  Delete", ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f))) {
            if (Platform::moveToRecycleBin(Paths::toUtf8(pendingDelete_))) {
                ui.renderer.textures().invalidate(Paths::toStored(pendingDelete_));
                editor.notify("Moved " + Paths::toUtf8(pendingDelete_.filename()) + " to the Recycle Bin");
            }
            pendingDelete_.clear();
            dirty_ = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            pendingDelete_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

void AssetBrowserPanel::drawFolderTree(UiContext& ui, const fs::path& dir) {
    std::error_code ec;
    std::vector<fs::path> subdirs;
    for (const auto& item : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec))
        if (item.is_directory(ec) && !hidden(item.path())) subdirs.push_back(item.path());
    std::sort(subdirs.begin(), subdirs.end());

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
    if (subdirs.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (fs::equivalent(dir, folder_, ec)) flags |= ImGuiTreeNodeFlags_Selected;
    const bool isRoot = fs::equivalent(dir, root_, ec);
    if (isRoot) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    // Keep the path to the current folder open.
    const std::string current = Paths::toUtf8(folder_), mine = Paths::toUtf8(dir);
    if (current.size() > mine.size() && current.compare(0, mine.size(), mine) == 0) ImGui::SetNextItemOpen(true, ImGuiCond_Appearing);

    const std::string label = std::string(isRoot ? ICON_FA_HOUSE : ICON_FA_FOLDER) + "  " + (isRoot ? ui.editor.project().settings().name : Paths::toUtf8(dir.filename()));
    ImGui::PushID(mine.c_str());
    const bool open = ImGui::TreeNodeEx("##dir", flags, "%s", label.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
        folder_ = dir;
        dirty_ = true;
    }
    if (open) {
        for (const fs::path& d : subdirs) drawFolderTree(ui, d);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::drawTile(UiContext& ui, const Entry& e) {
    ImGui::PushID(e.name.c_str());
    ImGui::BeginGroup();
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float labelHeight = ImGui::GetTextLineHeight() * 2.0f;
    const bool selected = selected_ == e.path;
    if (ImGui::Selectable("##tile", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(tileSize_, tileSize_ + labelHeight))) {
        selected_ = e.path;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) activate(ui, e);
    }
    const bool hoveredTile = ImGui::IsItemHovered();

    // Drag the file into the viewport, the Explorer or a property.
    if (e.kind != AssetKind::Folder && ImGui::BeginDragDropSource()) {
        const std::string stored = Paths::toStored(e.path);
        ImGui::SetDragDropPayload(kAssetPayload, stored.c_str(), stored.size() + 1);
        theme::iconText(assetIcon(e.kind), kindColor(e.kind), e.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##item")) {
        selected_ = e.path;
        itemMenu(ui, e);
        ImGui::EndPopup();
    }

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float inset = tileSize_ * 0.12f;
    const ImVec2 a(pos.x + inset, pos.y + inset), b(pos.x + tileSize_ - inset, pos.y + tileSize_ - inset);
    bool drewThumbnail = false;
    if (e.kind == AssetKind::Image) {
        if (auto tex = ui.renderer.textures().get(Paths::toStored(e.path))) {
            // Fit the image in the square, keeping its shape.
            const float w = static_cast<float>(std::max(tex->width(), 1)), h = static_cast<float>(std::max(tex->height(), 1));
            const float box = b.x - a.x;
            const ImVec2 size = w > h ? ImVec2(box, box * h / w) : ImVec2(box * w / h, box);
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            draw->AddImageRounded(static_cast<ImTextureID>(tex->id()), ImVec2(c.x - size.x * 0.5f, c.y - size.y * 0.5f), ImVec2(c.x + size.x * 0.5f, c.y + size.y * 0.5f),
                                  ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, 4.0f);
            drewThumbnail = true;
        }
    }
    if (!drewThumbnail) {
        draw->AddRectFilled(a, b, theme::u32(theme::color::frame), 8.0f);
        const float iconSize = tileSize_ * 0.38f;
        const char* icon = assetIcon(e.kind);
        const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
        draw->AddText(ImGui::GetFont(), iconSize, ImVec2((a.x + b.x - t.x) * 0.5f, (a.y + b.y - t.y) * 0.5f), theme::u32(kindColor(e.kind)), icon);
    }

    // Name (two lines at most) or the rename field.
    if (renaming_ == e.path) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + tileSize_));
        ImGui::SetNextItemWidth(tileSize_);
        if (renameFocus_) {
            ImGui::SetKeyboardFocusHere();
            renameFocus_ = false;
        }
        ImGui::InputText("##rename", &renameBuffer_, ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsItemDeactivated()) {
            if (ImGui::IsItemDeactivatedAfterEdit() && !ImGui::IsKeyPressed(ImGuiKey_Escape) && !renameBuffer_.empty() && renameBuffer_ != e.name) {
                std::error_code ec;
                const fs::path target = e.path.parent_path() / Paths::fromUtf8(renameBuffer_);
                if (fs::exists(target, ec)) ui.editor.notify("A file named " + renameBuffer_ + " already exists", NoticeLevel::Warning);
                else {
                    fs::rename(e.path, target, ec);
                    if (ec) ui.editor.notify("Could not rename: " + ec.message(), NoticeLevel::Error);
                    else ui.editor.notify("Renamed to " + renameBuffer_ + ". Scenes that used the old name need updating.", NoticeLevel::Info);
                }
                dirty_ = true;
            }
            renaming_.clear();
        }
    } else {
        const float wrap = tileSize_ - 4.0f;
        ImFont* font = ImGui::GetFont();
        std::string name = e.name;
        ImVec2 t = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, wrap, name.c_str());
        while (t.y > labelHeight + 1.0f && name.size() > 4) {
            name.erase(name.size() - 4);
            name += "...";
            t = font->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, wrap, name.c_str());
        }
        const float x = pos.x + std::max(2.0f, (tileSize_ - std::min(t.x, wrap)) * 0.5f);
        draw->AddText(font, ImGui::GetFontSize(), ImVec2(x, pos.y + tileSize_), ImGui::GetColorU32(ImGuiCol_Text), name.c_str(), nullptr, wrap);
    }
    ImGui::EndGroup();
    if (hoveredTile && !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const std::string tip = e.name + (e.kind == AssetKind::Folder ? std::string() : "\n" + sizeText(e.size) + "\n" + Paths::toStored(e.path));
        theme::tooltip(tip.c_str());
    }
    // Drop instances onto a folder? No; but files from tiles can move into folders.
    if (e.kind == AssetKind::Folder && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
            const fs::path from = Paths::resolveAsset(static_cast<const char*>(p->Data));
            std::error_code ec;
            fs::rename(from, e.path / from.filename(), ec);
            if (ec) ui.editor.notify("Could not move the file: " + ec.message(), NoticeLevel::Error);
            dirty_ = true;
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::PopID();
}

void AssetBrowserPanel::activate(UiContext& ui, const Entry& e) {
    Editor& editor = ui.editor;
    switch (e.kind) {
        case AssetKind::Folder:
            folder_ = e.path;
            dirty_ = true;
            break;
        case AssetKind::Scene: {
            const fs::path file = e.path;
            editor.requestDiscard([&editor, file] { editor.openScene(file); });
            break;
        }
        case AssetKind::Script: {
            // Scripts of the open scene open in the script editor; shared modules too.
            std::error_code ec;
            const fs::path sceneDir = editor.scenePath().parent_path();
            fs::path rel = fs::relative(e.path, sceneDir, ec);
            std::string key;
            if (!ec && !rel.empty() && rel.native()[0] != L'.') key = Paths::toUtf8(rel);
            else {
                rel = fs::relative(e.path, editor.project().root(), ec);
                if (!ec && !rel.empty() && rel.native()[0] != L'.') key = std::string(ScriptStore::kProjectPrefix) + Paths::toUtf8(rel);
            }
            std::replace(key.begin(), key.end(), '\\', '/');
            if (!key.empty()) editor.scriptRequests.push_back({ key, 0, 0 });
            else Platform::openWithShell(Paths::toUtf8(e.path));
            break;
        }
        default:
            Platform::openWithShell(Paths::toUtf8(e.path));
            break;
    }
}

void AssetBrowserPanel::itemMenu(UiContext& ui, const Entry& e) {
    if (ImGui::MenuItem(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE "  Open")) activate(ui, e);
    if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in File Explorer")) Platform::revealInExplorer(Paths::toUtf8(e.path));
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) Platform::setClipboard(Paths::toStored(e.path));
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename")) {
        renaming_ = e.path;
        renameBuffer_ = e.name;
        renameFocus_ = true;
    }
    if (ImGui::MenuItem(ICON_FA_TRASH_CAN "  Delete")) pendingDelete_ = e.path;
}

void AssetBrowserPanel::backgroundMenu(UiContext& ui) {
    if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder")) {
        std::error_code ec;
        fs::path p = folder_ / "New Folder";
        for (int n = 2; fs::exists(p, ec); ++n) p = folder_ / ("New Folder " + std::to_string(n));
        fs::create_directory(p, ec);
        renaming_ = p;
        renameBuffer_ = Paths::toUtf8(p.filename());
        renameFocus_ = true;
        dirty_ = true;
    }
    if (ImGui::MenuItem(ICON_FA_FILE_IMPORT "  Import Files...")) importFiles(ui);
    if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Show in File Explorer")) Platform::revealInExplorer(Paths::toUtf8(folder_));
    if (ImGui::MenuItem(ICON_FA_ROTATE "  Refresh")) dirty_ = true;
}

void AssetBrowserPanel::importFiles(UiContext& ui) {
    const std::string file = Platform::openFileDialog("Import a file", {
        { "Assets", "*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.wav;*.mp3;*.ogg;*.flac;*.obj;*.gltf;*.glb;*.lua" },
        { "All files", "*.*" } });
    if (file.empty()) return;
    const fs::path from = Paths::fromUtf8(file);
    std::error_code ec;
    fs::path to = folder_ / from.filename();
    for (int n = 2; fs::exists(to, ec); ++n) to = folder_ / (from.stem().wstring() + L"_" + std::to_wstring(n) + from.extension().wstring());
    fs::copy_file(from, to, ec);
    if (ec) ui.editor.notify("Import failed: " + ec.message(), NoticeLevel::Error);
    else ui.editor.notify("Imported " + Paths::toUtf8(to.filename()), NoticeLevel::Success);
    dirty_ = true;
}

} // namespace rift
