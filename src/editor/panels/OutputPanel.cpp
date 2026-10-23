#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "platform/Platform.h"

#include "IconsFontAwesome6.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace rift {

namespace {
    bool containsNoCase(const std::string& text, const char* needle) {
        if (!*needle) return true;
        const size_t n = std::strlen(needle);
        for (size_t i = 0; i + n <= text.size(); ++i) {
            size_t k = 0;
            while (k < n && std::tolower(static_cast<unsigned char>(text[i + k])) == std::tolower(static_cast<unsigned char>(needle[k]))) ++k;
            if (k == n) return true;
        }
        return false;
    }

    // A filter chip: icon + count, highlighted while on.
    bool chip(const char* icon, int count, const ImVec4& color, bool& on, const char* tip) {
        char label[48];
        std::snprintf(label, sizeof(label), "%s %d", icon, count);
        ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(color.x, color.y, color.z, 0.18f) : ImGui::GetStyleColorVec4(ImGuiCol_Button));
        ImGui::PushStyleColor(ImGuiCol_Text, on ? color : theme::color::textDim);
        const bool pressed = ImGui::Button(label);
        ImGui::PopStyleColor(2);
        theme::tooltip(tip);
        if (pressed) on = !on;
        return pressed;
    }
}

void OutputPanel::draw(UiContext& ui, bool* open) {
    if (!ImGui::Begin(ICON_FA_TERMINAL "  Output###Output", open)) {
        ImGui::End();
        return;
    }
    Editor& editor = ui.editor;
    auto& lines = editor.output();

    int infos = 0, warnings = 0, errors = 0;
    for (const OutputLine& l : lines) {
        if (l.level == LogLevel::Error) ++errors;
        else if (l.level == LogLevel::Warning) ++warnings;
        else ++infos;
    }

    // ---- toolbar
    if (theme::iconButton(ICON_FA_TRASH_CAN, "Clear the output")) editor.clearOutput();
    ImGui::SameLine();
    chip(ICON_FA_CIRCLE_INFO, infos, theme::color::text, showInfo_, "Show messages");
    ImGui::SameLine(0.0f, 4.0f);
    chip(ICON_FA_TRIANGLE_EXCLAMATION, warnings, theme::color::warning, showWarnings_, "Show warnings");
    ImGui::SameLine(0.0f, 4.0f);
    chip(ICON_FA_CIRCLE_XMARK, errors, theme::color::error, showErrors_, "Show errors");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(80.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x));
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter", filter_, sizeof(filter_));
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ANGLES_DOWN, autoScroll_ ? "Auto-scroll is on" : "Auto-scroll is off", autoScroll_)) autoScroll_ = !autoScroll_;

    // ---- lines
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme::color::bg);
    ImGui::BeginChild("##lines", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PushFont(theme::fonts().mono, 0.0f);
    std::string copied;
    for (size_t k = 0; k < lines.size(); ++k) {
        const OutputLine& l = lines[k];
        if (l.level == LogLevel::Info && !showInfo_) continue;
        if (l.level == LogLevel::Warning && !showWarnings_) continue;
        if (l.level == LogLevel::Error && !showErrors_) continue;
        if (!containsNoCase(l.text, filter_)) continue;

        ImGui::PushID(static_cast<int>(k));
        const ImVec4 color = l.level == LogLevel::Error ? theme::color::error : l.level == LogLevel::Warning ? theme::color::warning : theme::color::text;
        const char* icon = l.level == LogLevel::Error ? ICON_FA_CIRCLE_XMARK : l.level == LogLevel::Warning ? ICON_FA_TRIANGLE_EXCLAMATION : " ";
        if (l.time > 0.0) {
            ImGui::TextDisabled("%7.2f", l.time);
            ImGui::SameLine();
        }
        ImGui::TextColored(color, "%s", icon);
        ImGui::SameLine();
        if (!l.source.empty() && l.source != "Game") {
            ImGui::TextDisabled("[%s]", l.source.c_str());
            ImGui::SameLine();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::TextUnformatted(l.text.c_str());
        ImGui::PopStyleColor();

        // A line printed by a script opens it (at the line of an error).
        if (l.script && ImGui::IsItemHovered()) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), b, theme::u32(color, 0.6f));
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                const Instance* s = editor.scene().find(l.script);
                if (!s) s = editor.editScene().find(l.script);
                if (s) editor.openScript(l.script, std::max(0, scriptErrorLine(l.text, s->fullName())));
            }
        }
        if (ImGui::BeginPopupContextItem("##line")) {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy line")) copied = l.text;
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy everything")) {
                for (const OutputLine& o : lines) copied += o.text + "\n";
            }
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TRASH_CAN "  Clear")) editor.clearOutput();
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    if (lines.empty()) ImGui::TextDisabled("Nothing here yet. print() from scripts shows up in this panel.");
    ImGui::PopFont();

    if (autoScroll_ && editor.outputCounter() != lastCounter_) ImGui::SetScrollHereY(1.0f);
    lastCounter_ = editor.outputCounter();
    // Scrolling up pauses auto-scroll; reaching the bottom again resumes it.
    if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel > 0.0f) autoScroll_ = false;
    else if (!autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f && ImGui::GetIO().MouseWheel < 0.0f) autoScroll_ = true;
    ImGui::EndChild();
    ImGui::PopStyleColor();
    if (!copied.empty()) Platform::setClipboard(copied);
    ImGui::End();
}

} // namespace rift
