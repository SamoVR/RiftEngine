#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "script/ScriptStore.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cfloat>

namespace rift {

namespace {
    bool wordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

    // Where `needle` occurs in `hay` from `from` (npos if not), honouring the options.
    size_t findIn(const std::string& hay, const std::string& needle, size_t from, bool matchCase, bool wholeWord) {
        while (from <= hay.size()) {
            size_t at;
            if (matchCase) {
                at = hay.find(needle, from);
            } else {
                auto it = std::search(hay.begin() + static_cast<std::ptrdiff_t>(from), hay.end(), needle.begin(), needle.end(),
                                      [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
                at = it == hay.end() ? std::string::npos : static_cast<size_t>(it - hay.begin());
            }
            if (at == std::string::npos) return at;
            const bool before = at > 0 && wordChar(hay[at - 1]);
            const bool after = at + needle.size() < hay.size() && wordChar(hay[at + needle.size()]);
            if (!wholeWord || (!before && !after)) return at;
            from = at + 1;
        }
        return std::string::npos;
    }
}

void FindPanel::focus() {
    focusInput_ = true;
}

void FindPanel::search(Editor& editor) {
    results_.clear();
    total_ = 0;
    searched_ = query_;
    if (query_.empty()) return;
    ScriptStore& store = editor.scripts();
    // Every script the scene uses (a file shared by two scripts is searched once).
    std::vector<const Instance*> scripts;
    editor.editScene().forEachInTree([&](Instance& i) { if (i.isScript() && !i.source.empty()) scripts.push_back(&i); });
    std::sort(scripts.begin(), scripts.end(), [](const Instance* a, const Instance* b) { return a->fullName() < b->fullName(); });
    std::vector<std::string> done;
    for (const Instance* s : scripts) {
        if (std::find(done.begin(), done.end(), s->source) != done.end()) continue;
        done.push_back(s->source);
        if (!store.has(s->source) && !store.load(s->source)) continue;
        const ScriptStore::Entry* entry = store.find(s->source);
        if (!entry) continue;
        FileHits file;
        file.scriptId = s->id;
        file.path = s->source;
        file.title = s->fullName();
        const std::string& text = entry->text;
        int line = 1;
        size_t lineStart = 0;
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i < text.size() && text[i] != '\n') continue;
            const std::string lineText = text.substr(lineStart, i - lineStart);
            for (size_t at = findIn(lineText, query_, 0, matchCase_, wholeWord_); at != std::string::npos;
                 at = findIn(lineText, query_, at + std::max<size_t>(query_.size(), 1), matchCase_, wholeWord_)) {
                Hit h;
                h.line = line;
                h.column = at;
                h.text = lineText;
                file.hits.push_back(std::move(h));
                ++total_;
                if (total_ >= 2000) break;
            }
            lineStart = i + 1;
            ++line;
        }
        if (!file.hits.empty()) results_.push_back(std::move(file));
        if (total_ >= 2000) break;
    }
}

int FindPanel::replaceAll(Editor& editor) {
    if (query_.empty() || editor.playing()) return 0;
    ScriptStore& store = editor.scripts();
    int count = 0;
    for (const FileHits& file : results_) {
        std::string* text = store.editableText(file.path);
        if (!text) continue; // built-in scripts are read-only
        std::string out;
        size_t from = 0;
        for (size_t at = findIn(*text, query_, 0, matchCase_, wholeWord_); at != std::string::npos;
             at = findIn(*text, query_, from, matchCase_, wholeWord_)) {
            out.append(*text, from, at - from);
            out += replacement_;
            from = at + query_.size();
            ++count;
        }
        out.append(*text, from, std::string::npos);
        if (out != *text) {
            *text = out;
            store.touch(file.path);
        }
    }
    return count;
}

void FindPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    if (!ImGui::Begin(ICON_FA_MAGNIFYING_GLASS "  Find in Scripts###Find", open)) {
        ImGui::End();
        return;
    }
    const ImGuiStyle& style = ImGui::GetStyle();
    const float button = ImGui::GetFrameHeight();
    bool changed = false;

    // ---- query and options
    const float optionsW = button * 2.0f + 2.0f + style.ItemInnerSpacing.x;
    if (focusInput_) {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - optionsW);
    changed |= ImGui::InputTextWithHint("##query", ICON_FA_MAGNIFYING_GLASS "  Find in every script of the scene", &query_);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    if (theme::iconButton("Aa", "Match case", matchCase_)) { matchCase_ = !matchCase_; changed = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton("ab", "Whole words only", wholeWord_)) { wholeWord_ = !wholeWord_; changed = true; }
    // ---- replace
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - optionsW);
    ImGui::InputTextWithHint("##replace", ICON_FA_RIGHT_LEFT "  Replace with", &replacement_);
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    ImGui::BeginDisabled(results_.empty() || editor.playing());
    if (ImGui::Button("All", ImVec2(optionsW - style.ItemInnerSpacing.x, 0.0f))) ImGui::OpenPopup("Replace in all scripts?");
    ImGui::EndDisabled();
    theme::tooltip(editor.playing() ? "Stop the game first" : "Replace every match in every script");
    if (ImGui::BeginPopupModal("Replace in all scripts?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Replace %d match%s of \"%s\" with \"%s\"?", total_, total_ == 1 ? "" : "es", query_.c_str(), replacement_.c_str());
        ImGui::TextColored(theme::color::textDim, "The scripts change in the editor; nothing is written until you save.");
        ImGui::Spacing();
        const float w = ImGui::GetFontSize() * 6.0f;
        if (ImGui::Button("Replace", ImVec2(w, 0.0f))) {
            const int n = replaceAll(editor);
            editor.notify("Replaced " + std::to_string(n) + " match" + (n == 1 ? "" : "es"), NoticeLevel::Success);
            changed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // Scripts change while you look: search again now and then.
    refreshTimer_ -= ui.dt;
    if (changed || refreshTimer_ <= 0.0f) {
        search(editor);
        refreshTimer_ = 1.0f;
    }

    // ---- results
    ImGui::Spacing();
    if (query_.empty()) {
        ImGui::TextColored(theme::color::textDim, "Type to search. Ctrl+Shift+F opens this from anywhere.");
    } else if (results_.empty()) {
        ImGui::TextColored(theme::color::textDim, "No matches.");
    } else {
        ImGui::TextColored(theme::color::textDim, "%d match%s in %d script%s%s", total_, total_ == 1 ? "" : "es", static_cast<int>(results_.size()),
                           results_.size() == 1 ? "" : "s", total_ >= 2000 ? " (first 2000)" : "");
    }
    ImGui::BeginChild("##results", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    ImFont* mono = theme::fonts().mono;
    const ImU32 highlight = theme::u32(theme::color::accent, 0.45f);
    for (size_t f = 0; f < results_.size(); ++f) {
        const FileHits& file = results_[f];
        ImGui::PushID(static_cast<int>(f));
        const std::string header = std::string(ICON_FA_FILE_CODE "  ") + file.title + "###file";
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        const bool openNode = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding);
        ImGui::SameLine();
        ImGui::TextColored(theme::color::textDim, "%s  %d", file.path.c_str(), static_cast<int>(file.hits.size()));
        if (openNode) {
            for (size_t h = 0; h < file.hits.size(); ++h) {
                const Hit& hit = file.hits[h];
                ImGui::PushID(static_cast<int>(h));
                // The line with the match highlighted; click to open it there.
                const ImVec2 rowStart = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##hit", false, ImGuiSelectableFlags_SpanAllColumns, ImVec2(0.0f, ImGui::GetTextLineHeight())))
                    editor.openScript(file.scriptId, hit.line);
                ImGui::SameLine(0.0f, 0.0f);
                ImGui::SetCursorScreenPos(rowStart);
                ImGui::TextColored(theme::color::textDim, "%5d", hit.line);
                ImGui::SameLine();
                ImGui::PushFont(mono, 0.0f);
                // Leading spaces trimmed so deep code still fits.
                size_t lead = 0;
                while (lead < hit.text.size() && (hit.text[lead] == ' ' || hit.text[lead] == '\t')) ++lead;
                const size_t col = hit.column >= lead ? hit.column - lead : 0;
                const std::string shown = hit.text.substr(lead);
                const ImVec2 textPos = ImGui::GetCursorScreenPos();
                const float before = ImGui::CalcTextSize(shown.c_str(), shown.c_str() + std::min(col, shown.size())).x;
                const float width = ImGui::CalcTextSize(shown.c_str() + std::min(col, shown.size()),
                                                        shown.c_str() + std::min(col + query_.size(), shown.size())).x;
                ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(textPos.x + before - 1.0f, textPos.y),
                                                          ImVec2(textPos.x + before + width + 1.0f, textPos.y + ImGui::GetTextLineHeight()), highlight, 2.0f);
                ImGui::TextUnformatted(shown.c_str());
                ImGui::PopFont();
                ImGui::PopID();
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace rift
