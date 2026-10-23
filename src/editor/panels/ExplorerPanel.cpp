#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "platform/Platform.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cstring>

namespace rift {

namespace {
    bool matches(const Instance& i, const char* filter) {
        auto has = [&](const std::string& text) {
            const size_t n = std::strlen(filter);
            for (size_t p = 0; p + n <= text.size(); ++p) {
                size_t k = 0;
                while (k < n && std::tolower(static_cast<unsigned char>(text[p + k])) == std::tolower(static_cast<unsigned char>(filter[k]))) ++k;
                if (k == n) return true;
            }
            return false;
        };
        return has(i.name) || has(className(i.cls));
    }

    bool isIdentifier(const std::string& s) {
        if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0]))) return false;
        return std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; });
    }

    // How a script reaches this instance: workspace.Map.Door, game.Lighting, ...
    std::string luaPath(const Instance& i) {
        std::vector<const Instance*> chain;
        for (const Instance* p = &i; p; p = p->parent) chain.push_back(p);
        std::reverse(chain.begin(), chain.end());
        std::string out;
        for (const Instance* p : chain) {
            if (p->cls == ClassId::Workspace && p->isService()) { out = "workspace"; continue; }
            if (p->isService()) { out = "game:GetService(\"" + std::string(className(p->cls)) + "\")"; continue; }
            if (out.empty()) out = "game";
            out += isIdentifier(p->name) ? "." + p->name : "[\"" + p->name + "\"]";
        }
        return out;
    }

    std::vector<uint64_t> payloadIds(const ImGuiPayload* payload) {
        std::vector<uint64_t> ids(static_cast<size_t>(payload->DataSize) / sizeof(uint64_t));
        if (!ids.empty()) std::memcpy(ids.data(), payload->Data, ids.size() * sizeof(uint64_t));
        return ids;
    }
}

void ExplorerPanel::draw(UiContext& ui, bool* open) {
    if (!ImGui::Begin(ICON_FA_SITEMAP "  Explorer###Explorer", open)) {
        ImGui::End();
        return;
    }
    Editor& editor = ui.editor;
    Scene& scene = editor.scene();

    // Selected somewhere else (viewport click, undo): open the tree down to it.
    const uint64_t primary = editor.primary();
    if (primary != lastPrimary_) {
        if (!selectedHere_ && primary) revealId_ = primary;
        lastPrimary_ = primary;
    }
    selectedHere_ = false;

    if (renameRequest_) {
        if (Instance* i = scene.find(renameRequest_); i && !i->isService()) {
            renaming_ = i->id;
            renameBuffer_ = i->name;
            renameFocus_ = true;
            revealId_ = i->id;
            filter_[0] = '\0';
        }
        renameRequest_ = 0;
    }

    // ---- toolbar
    const float buttonWidth = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - buttonWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Search (name or class)", filter_, sizeof(filter_));
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_PLUS, "Insert object")) ImGui::OpenPopup("##insert");
    if (ImGui::BeginPopup("##insert")) {
        drawInsertMenuItems(editor);
        ImGui::EndPopup();
    }

    // ---- tree
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color::bg);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 2.0f));
    ImGui::BeginChild("##tree", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    std::swap(lastOrder_, visibleOrder_);
    visibleOrder_.clear();
    if (!editor.hasScene()) {
        ImGui::TextDisabled("No scene open");
    } else if (filter_[0]) {
        drawFiltered(ui);
    } else {
        for (Instance* service : scene.services()) {
            if (!service) continue;
            // Players only has something to show while the game runs.
            if (service->cls == ClassId::Players && service->children.empty()) continue;
            drawNode(ui, *service, 0);
        }
    }

    // Empty space below the rows: click to deselect, drop to move into Workspace.
    const ImVec2 rest = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##empty", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, ImGui::GetFrameHeight())));
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) editor.clearSelection();
    if (editor.hasScene() && ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kInstancesPayload)) editor.reparent(payloadIds(p), scene.workspace().id);
        ImGui::EndDragDropTarget();
    }
    if (editor.hasScene() && ImGui::BeginPopupContextItem("##emptyMenu")) {
        if (ImGui::IsWindowAppearing()) editor.clearSelection();
        if (ImGui::BeginMenu(ICON_FA_PLUS "  Insert into Workspace")) {
            drawInsertMenuItems(editor);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, editor.canPaste())) editor.paste();
        ImGui::EndPopup();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::End();
}

void ExplorerPanel::drawFiltered(UiContext& ui) {
    Editor& editor = ui.editor;
    int shown = 0;
    editor.scene().forEachInTree([&](Instance& i) {
        if (!matches(i, filter_) || shown > 500) return;
        ++shown;
        visibleOrder_.push_back(i.id);
        ImGui::PushID(&i);
        const bool selected = editor.isSelected(i.id);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, ImGui::GetTextLineHeight()))) {
            clickRow(ui, i);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (i.isScript()) editor.openScript(i.id);
                else editor.focusSelection();
            }
        }
        contextMenu(ui, i);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(pos, theme::u32(theme::classColor(i.cls)), theme::classIcon(i.cls));
        const float x = pos.x + ImGui::GetFontSize() * 1.5f;
        draw->AddText(ImVec2(x, pos.y), ImGui::GetColorU32(ImGuiCol_Text), i.name.c_str());
        const std::string where = i.parent ? "  " + i.parent->fullName() : std::string();
        draw->AddText(ImVec2(x + ImGui::CalcTextSize(i.name.c_str()).x, pos.y), ImGui::GetColorU32(ImGuiCol_TextDisabled), where.c_str());
        ImGui::PopID();
    });
    if (shown == 0) ImGui::TextDisabled("Nothing matches \"%s\"", filter_);
}

void ExplorerPanel::drawNode(UiContext& ui, Instance& node, int depth) {
    Editor& editor = ui.editor;
    Scene& scene = editor.scene();
    visibleOrder_.push_back(node.id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_NavLeftJumpsToParent;
    if (editor.isSelected(node.id)) flags |= ImGuiTreeNodeFlags_Selected;
    if (node.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (node.cls == ClassId::Workspace) ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (revealId_ && revealId_ != node.id) {
        const Instance* target = scene.find(revealId_);
        if (target && target->isDescendantOf(&node)) ImGui::SetNextItemOpen(true);
    }

    // A selected row keeps its colour under the mouse.
    const bool selectedRow = editor.isSelected(node.id);
    if (selectedRow) ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.38f));
    ImGui::PushID(&node);
    const float indentX = ImGui::GetCursorScreenPos().x; // the row spans the full width; the label follows the indent
    const bool open = ImGui::TreeNodeEx("##node", flags, "%s", "");
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    const bool doubleClicked = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen();
    if (selectedRow) ImGui::PopStyleColor();

    if (revealId_ == node.id) {
        ImGui::SetScrollHereY(0.4f);
        revealId_ = 0;
    }

    // Drag rows onto other rows to reparent / reorder.
    if (!node.isService() && renaming_ != node.id && ImGui::BeginDragDropSource()) {
        std::vector<uint64_t> ids;
        if (editor.isSelected(node.id)) for (Instance* i : editor.selectionRoots()) ids.push_back(i->id);
        if (ids.empty()) ids.push_back(node.id);
        ImGui::SetDragDropPayload(kInstancesPayload, ids.data(), ids.size() * sizeof(uint64_t));
        theme::iconText(theme::classIcon(node.cls), theme::classColor(node.cls), ids.size() == 1 ? node.name.c_str() : (std::to_string(ids.size()) + " objects").c_str());
        ImGui::EndDragDropSource();
    }
    dropTarget(ui, node);
    contextMenu(ui, node);

    // Icon and name (drawn over the row, after the arrow).
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float labelX = indentX + ImGui::GetTreeNodeToLabelSpacing();
    const float textY = rowMin.y + (rowMax.y - rowMin.y - ImGui::GetFontSize()) * 0.5f;
    draw->AddText(ImVec2(labelX, textY), theme::u32(theme::classColor(node.cls)), theme::classIcon(node.cls));
    const float nameX = labelX + ImGui::GetFontSize() * 1.45f;
    const bool dim = (node.isScript() || node.isLight()) && !node.enabled;
    if (renaming_ == node.id) {
        ImGui::SetCursorScreenPos(ImVec2(nameX - 4.0f, rowMin.y));
        ImGui::SetNextItemWidth(std::max(80.0f, rowMax.x - nameX));
        if (renameFocus_) {
            ImGui::SetKeyboardFocusHere();
            renameFocus_ = false;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0f, 0.0f));
        ImGui::InputText("##rename", &renameBuffer_, ImGuiInputTextFlags_AutoSelectAll);
        ImGui::PopStyleVar();
        if (ImGui::IsItemDeactivated()) {
            if (ImGui::IsItemDeactivatedAfterEdit() && !ImGui::IsKeyPressed(ImGuiKey_Escape) && !renameBuffer_.empty())
                editor.rename(node.id, renameBuffer_);
            renaming_ = 0;
        }
    } else {
        draw->AddText(ImVec2(nameX, textY), ImGui::GetColorU32(dim ? ImGuiCol_TextDisabled : ImGuiCol_Text), node.name.c_str());
        // Services and containers show how much is inside when closed.
        if (!open && !node.children.empty() && (node.isService() || node.cls == ClassId::Model || node.cls == ClassId::Folder)) {
            const std::string count = std::to_string(node.children.size());
            const float w = ImGui::CalcTextSize(count.c_str()).x;
            draw->AddText(ImVec2(rowMax.x - w - 8.0f, textY), ImGui::GetColorU32(ImGuiCol_TextDisabled), count.c_str());
        }
    }

    if (clicked) clickRow(ui, node);
    if (doubleClicked) {
        if (node.isScript()) editor.openScript(node.id);
        else if (!node.isService()) editor.focusSelection();
    }
    // F2 also works here; Enter on a script opens it.
    if (ImGui::IsWindowFocused() && editor.primary() == node.id && renaming_ == 0 && ImGui::IsKeyPressed(ImGuiKey_Enter) && node.isScript())
        editor.openScript(node.id);

    if (open) {
        for (Instance* child : std::vector<Instance*>(node.children)) {
            if (child->archivable || editor.playing()) drawNode(ui, *child, depth + 1);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ExplorerPanel::clickRow(UiContext& ui, Instance& node) {
    Editor& editor = ui.editor;
    const ImGuiIO& io = ImGui::GetIO();
    selectedHere_ = true;
    if (io.KeyShift && rangeAnchor_) {
        auto a = std::find(lastOrder_.begin(), lastOrder_.end(), rangeAnchor_);
        auto b = std::find(lastOrder_.begin(), lastOrder_.end(), node.id);
        if (a != lastOrder_.end() && b != lastOrder_.end()) {
            if (a > b) std::swap(a, b);
            std::vector<uint64_t> ids = io.KeyCtrl ? editor.selection() : std::vector<uint64_t>();
            for (auto it = a; it != b + 1; ++it) if (std::find(ids.begin(), ids.end(), *it) == ids.end()) ids.push_back(*it);
            // The clicked row is the primary (last).
            ids.erase(std::remove(ids.begin(), ids.end(), node.id), ids.end());
            ids.push_back(node.id);
            editor.setSelection(ids);
            return;
        }
    }
    if (io.KeyCtrl) editor.select(node.id, SelectMode::Toggle);
    else editor.select(node.id);
    rangeAnchor_ = node.id;
}

void ExplorerPanel::dropTarget(UiContext& ui, Instance& node) {
    if (!ImGui::BeginDragDropTarget()) return;
    Editor& editor = ui.editor;
    const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
    const float y = ImGui::GetMousePos().y, h = max.y - min.y;
    // Top / bottom quarter of a row: put before / after it; middle: inside it.
    int zone = 1;
    if (!node.isService() && node.parent) zone = y < min.y + h * 0.25f ? 0 : y > max.y - h * 0.25f ? 2 : 1;

    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kInstancesPayload, ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImU32 color = theme::u32(theme::color::accentHover);
        if (zone == 0) draw->AddLine(ImVec2(min.x, min.y), ImVec2(max.x, min.y), color, 2.0f);
        else if (zone == 2) draw->AddLine(ImVec2(min.x, max.y), ImVec2(max.x, max.y), color, 2.0f);
        else draw->AddRect(min, max, color, 4.0f, 0, 1.5f);
        if (p->IsDelivery()) {
            const std::vector<uint64_t> ids = payloadIds(p);
            if (zone == 1) editor.reparent(ids, node.id);
            else editor.reparent(ids, node.parent->id, editor.scene().siblingIndex(&node) + (zone == 2 ? 1 : 0));
        }
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
        dropAsset(editor, std::string(static_cast<const char*>(p->Data)), &node);
    }
    ImGui::EndDragDropTarget();
}

void ExplorerPanel::contextMenu(UiContext& ui, Instance& node) {
    if (!ImGui::BeginPopupContextItem("##menu")) return;
    Editor& editor = ui.editor;
    if (ImGui::IsWindowAppearing() && !editor.isSelected(node.id)) {
        editor.select(node.id);
        selectedHere_ = true;
    }
    const bool service = node.isService();
    const bool container = node.cls == ClassId::Model || node.cls == ClassId::Folder;

    if (ImGui::BeginMenu(ICON_FA_PLUS "  Insert Object")) {
        drawInsertMenuItems(editor);
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (node.isScript() && ImGui::MenuItem(ICON_FA_PEN_TO_SQUARE "  Open Script", "Enter")) editor.openScript(node.id);
    if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename", "F2", false, !service)) beginRename(node.id);
    if (ImGui::MenuItem(ICON_FA_EXPAND "  Zoom To", "F", false, !service)) editor.focusSelection();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_SCISSORS "  Cut", "Ctrl+X", false, !service)) editor.cutSelection();
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, !service)) editor.copySelection();
    if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, editor.canPaste())) editor.paste();
    if (ImGui::MenuItem(ICON_FA_PASTE "  Paste Into", "Ctrl+Shift+V", false, editor.canPaste())) editor.paste(true);
    if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", "Ctrl+D", false, !service)) editor.duplicateSelection();
    if (ImGui::MenuItem(ICON_FA_TRASH_CAN "  Delete", "Del", false, !service)) editor.deleteSelection();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Group", "Ctrl+G", false, !service)) editor.groupSelection();
    if (ImGui::MenuItem(ICON_FA_OBJECT_UNGROUP "  Ungroup", "Ctrl+U", false, container)) editor.ungroupSelection();
    if (ImGui::MenuItem(ICON_FA_LIST_CHECK "  Select Children", nullptr, false, !node.children.empty())) {
        std::vector<uint64_t> ids;
        for (Instance* c : node.children) ids.push_back(c->id);
        editor.setSelection(ids);
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_CODE "  Copy Lua Path")) {
        Platform::setClipboard(luaPath(node));
        editor.notify("Copied " + luaPath(node));
    }
    ImGui::EndPopup();
}

} // namespace rift
