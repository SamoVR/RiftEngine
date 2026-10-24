#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "script/LuaRuntime.h"
#include "core/Paths.h"

#include "IconsFontAwesome6.h"
#include <TextEditor.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace rift {

namespace {
    constexpr float kCheckDelay = 0.35f; // seconds after the last keystroke

    // Lua plus the names RIFT scripts use all the time, so they stand out.
    const TextEditor::Language* riftLua() {
        static const TextEditor::Language language = [] {
            TextEditor::Language l = *TextEditor::Language::Lua();
            for (const char* name : { "game", "workspace", "script", "Instance", "Vector3", "Vector2", "CFrame", "Color3", "UDim2", "UDim",
                                      "BrickColor", "Enum", "TweenInfo", "task", "wait", "spawn", "delay", "tick", "time", "warn",
                                      "typeof", "require", "Players", "RunService", "UserInputService", "TweenService",
                                      "Debris", "CollectionService", "DataStoreService", "SoundService", "Lighting", "ReplicatedStorage",
                                      "ScriptService", "StarterGui", "Random", "math", "string", "table", "coroutine", "utf8", "os" })
                l.identifiers.insert(name);
            return l;
        }();
        return &language;
    }

    TextEditor::Palette palette() {
        TextEditor::Palette p = TextEditor::GetDarkPalette();
        using C = TextEditor::Color;
        auto set = [&](C c, ImU32 v) { p[static_cast<size_t>(c)] = v; };
        set(C::background, IM_COL32(22, 23, 28, 255));
        set(C::text, IM_COL32(222, 224, 232, 255));
        set(C::keyword, IM_COL32(197, 146, 255, 255));
        set(C::declaration, IM_COL32(197, 146, 255, 255));
        set(C::number, IM_COL32(247, 178, 103, 255));
        set(C::string, IM_COL32(152, 214, 140, 255));
        set(C::punctuation, IM_COL32(170, 174, 190, 255));
        set(C::identifier, IM_COL32(222, 224, 232, 255));
        set(C::knownIdentifier, IM_COL32(110, 190, 255, 255));
        set(C::comment, IM_COL32(110, 116, 136, 255));
        set(C::cursor, IM_COL32(190, 160, 255, 255));
        set(C::selection, IM_COL32(139, 92, 246, 90));
        set(C::lineNumber, IM_COL32(88, 92, 108, 255));
        set(C::currentLineNumber, IM_COL32(200, 200, 215, 255));
        set(C::currentLineHighlight, IM_COL32(255, 255, 255, 8));
        set(C::currentLineHighlightBorder, IM_COL32(255, 255, 255, 14));
        set(C::matchingBracketBackground, IM_COL32(139, 92, 246, 60));
        set(C::matchingBracketActive, IM_COL32(197, 146, 255, 255));
        return p;
    }
}

int scriptErrorLine(const std::string& message, const std::string& chunk) {
    const size_t at = chunk.empty() ? std::string::npos : message.find(chunk + ":");
    size_t start = at == std::string::npos ? message.find(':') : at + chunk.size();
    while (start != std::string::npos && start + 1 < message.size()) {
        const char* p = message.c_str() + start + 1;
        char* end = nullptr;
        const long line = std::strtol(p, &end, 10);
        if (end != p && *end == ':' && line > 0) return static_cast<int>(line);
        start = message.find(':', start + 1);
    }
    return -1;
}

namespace {
    // Text after "chunk:line: " (what to show in the status bar).
    std::string messageOf(const std::string& message) {
        for (size_t k = 0; k + 1 < message.size(); ++k) {
            if (message[k] != ':' || !std::isdigit(static_cast<unsigned char>(message[k + 1]))) continue;
            size_t end = k + 1;
            while (end < message.size() && std::isdigit(static_cast<unsigned char>(message[end]))) ++end;
            if (end + 1 < message.size() && message[end] == ':' && message[end + 1] == ' ') return message.substr(end + 2);
        }
        return message;
    }
}

struct ScriptEditorPanel::Doc {
    std::string path;            // ScriptStore key ("scripts/Coin.lua", "project:scripts/Util.lua")
    uint64_t scriptId = 0;       // the Script instance that opened it (0: opened as a file)
    std::string title;
    TextEditor editor;
    std::string synced;          // the store text the editor currently shows
    float checkTimer = 0.0f;     // > 0: a syntax check is due
    std::string error;           // syntax error ("" = none)
    int errorLine = -1;          // 1-based
    std::string runtimeError;    // the last error this script raised while playing
    int runtimeLine = -1;
    uint64_t outputSeen = 0;
    int gotoLine = 0;
    bool focus = true;
    bool open = true;
    float fontSize = 0.0f;
};

ScriptEditorPanel::ScriptEditorPanel() = default;
ScriptEditorPanel::~ScriptEditorPanel() = default;

void ScriptEditorPanel::closeAll() { docs_.clear(); }

void ScriptEditorPanel::open(UiContext& ui, const Editor::ScriptRequest& request) {
    for (auto& d : docs_) {
        if (d->path == request.path) {
            d->focus = true;
            if (request.line > 0) d->gotoLine = request.line;
            if (request.scriptId) d->scriptId = request.scriptId;
            return;
        }
    }
    ScriptStore& store = ui.editor.scripts();
    if (!store.has(request.path) && !store.load(request.path)) {
        ui.editor.notify("Cannot open " + request.path, NoticeLevel::Error);
        return;
    }
    auto doc = std::make_unique<Doc>();
    doc->path = request.path;
    doc->scriptId = request.scriptId;
    doc->gotoLine = request.line;
    std::string name = request.path;
    if (const size_t slash = name.find_last_of('/'); slash != std::string::npos) name = name.substr(slash + 1);
    doc->title = name;

    TextEditor& e = doc->editor;
    e.SetLanguage(riftLua());
    e.SetPalette(palette());
    e.SetTabSize(4);
    e.SetInsertSpacesOnTabs(true);
    e.SetAutoIndentEnabled(true);
    e.SetShowMatchingBrackets(true);
    e.SetCompletePairedGlyphs(true);
    e.SetShowLineNumbersEnabled(true);
    e.SetShowMiniMapEnabled(false);
    e.SetFindReplaceEnabled(true);
    e.SetLineSpacing(1.15f);
    e.SetShowWhitespacesEnabled(false);
    doc->synced = store.find(request.path)->text;
    e.SetText(doc->synced);
    doc->checkTimer = 0.01f;
    docs_.push_back(std::move(doc));
}

void ScriptEditorPanel::draw(UiContext& ui, ImGuiID dockId) {
    Editor& editor = ui.editor;
    // Tabs belong to one scene: another scene has other script files.
    if (editor.scenePath() != scenePath_) {
        docs_.clear();
        scenePath_ = editor.scenePath();
    }
    for (const auto& request : editor.scriptRequests) open(ui, request);
    editor.scriptRequests.clear();

    focused_ = false;
    for (size_t k = 0; k < docs_.size();) {
        if (!drawDoc(ui, *docs_[k], dockId)) docs_.erase(docs_.begin() + static_cast<long>(k));
        else ++k;
    }
}

bool ScriptEditorPanel::drawDoc(UiContext& ui, Doc& doc, ImGuiID dockId) {
    Editor& editor = ui.editor;
    ScriptStore& store = editor.scripts();
    const ScriptStore::Entry* entry = store.find(doc.path);
    if (!entry) return false; // the store was reset (new scene)

    // Changed outside this tab (hot reload from disk, another tool): show the new text.
    if (entry->text != doc.synced) {
        const TextEditor::DocPos cursor = doc.editor.GetMainCursorPosition();
        doc.synced = entry->text;
        doc.editor.SetText(doc.synced);
        doc.editor.SetCursor(cursor);
        doc.checkTimer = 0.01f;
    }

    // Runtime errors this script printed while playing.
    if (editor.outputCounter() != doc.outputSeen) {
        const auto& out = editor.output();
        const uint64_t fresh = std::min<uint64_t>(editor.outputCounter() - doc.outputSeen, out.size());
        for (size_t k = out.size() - fresh; k < out.size(); ++k) {
            const OutputLine& line = out[k];
            if (line.level != LogLevel::Error || !doc.scriptId || line.script != doc.scriptId) continue;
            const Instance* s = editor.scene().find(doc.scriptId);
            doc.runtimeError = messageOf(line.text);
            doc.runtimeLine = scriptErrorLine(line.text, s ? s->fullName() : std::string());
        }
        doc.outputSeen = editor.outputCounter();
    }

    if (doc.focus) {
        ImGui::SetNextWindowFocus();
        doc.editor.SetFocus(); // the text, not just the tab
        doc.focus = false;
    }
    if (dockId) ImGui::SetNextWindowDockID(dockId, ImGuiCond_FirstUseEver);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (entry->dirty) flags |= ImGuiWindowFlags_UnsavedDocument;
    const std::string title = std::string(ICON_FA_FILE_CODE "  ") + doc.title + "###script:" + doc.path;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(title.c_str(), &doc.open, flags);
    ImGui::PopStyleVar();
    if (!visible) {
        ImGui::End();
        return doc.open;
    }
    const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
    if (windowFocused) focused_ = true;

    // ---- toolbar
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetCursorPos(ImVec2(ImGui::GetCursorPosX() + style.ItemSpacing.x, ImGui::GetCursorPosY() + style.ItemSpacing.y * 0.6f));
    ImGui::BeginGroup();
    auto saveNow = [&] {
        if (editor.playing()) {
            if (store.save(doc.path)) editor.notify("Saved " + doc.path, NoticeLevel::Success);
            else editor.notify("Could not save " + doc.path, NoticeLevel::Error);
        } else {
            editor.saveScene();
        }
    };
    if (theme::iconButton(ICON_FA_FLOPPY_DISK, "Save (Ctrl+S)")) saveNow();
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ROTATE_LEFT, "Undo (Ctrl+Z)") && doc.editor.CanUndo()) doc.editor.Undo();
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ROTATE_RIGHT, "Redo (Ctrl+Y)") && doc.editor.CanRedo()) doc.editor.Redo();
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_MAGNIFYING_GLASS, "Find and replace (Ctrl+F)")) doc.editor.OpenFindReplaceWindow();
    ImGui::SameLine();
    if (doc.scriptId && theme::iconButton(ICON_FA_CROSSHAIRS, "Select the script in the Explorer")) editor.select(doc.scriptId);
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_UP_RIGHT_FROM_SQUARE, "Open in the external editor (Preferences > Scripts)")) openInExternalEditor(editor, doc.path);
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();

    // Status: syntax error, runtime error, or OK.
    if (!doc.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::error);
        const std::string text = std::string(ICON_FA_CIRCLE_XMARK "  Line ") + std::to_string(doc.errorLine) + ": " + messageOf(doc.error);
        if (ImGui::Selectable(text.c_str(), false, 0, ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f)) && doc.errorLine > 0)
            doc.gotoLine = doc.errorLine;
        ImGui::PopStyleColor();
        theme::tooltip(doc.error.c_str());
    } else if (!doc.runtimeError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::warning);
        const std::string text = std::string(ICON_FA_TRIANGLE_EXCLAMATION "  ") + (doc.runtimeLine > 0 ? "Line " + std::to_string(doc.runtimeLine) + ": " : "") + doc.runtimeError;
        if (ImGui::Selectable(text.c_str(), false, 0, ImVec2(ImGui::GetContentRegionAvail().x * 0.6f, 0.0f)) && doc.runtimeLine > 0)
            doc.gotoLine = doc.runtimeLine;
        ImGui::PopStyleColor();
        theme::tooltip("Runtime error from the last play session. Click to go to the line.");
    } else {
        ImGui::TextColored(theme::color::success, ICON_FA_CIRCLE_CHECK);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", doc.path.c_str());
    }

    // Right side: cursor position and font size.
    const TextEditor::DocPos cursor = doc.editor.GetMainCursorPosition();
    char pos[48];
    std::snprintf(pos, sizeof(pos), "Ln %zu, Col %zu", cursor.line + 1, cursor.index + 1);
    const float right = ImGui::CalcTextSize(pos).x + ImGui::GetFrameHeight() * 2.0f + style.ItemSpacing.x * 4.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - right));
    ImGui::TextDisabled("%s", pos);
    ImGui::SameLine();
    EditorSettings& settings = editor.settings();
    if (theme::iconButton(ICON_FA_MINUS, "Smaller text")) settings.scriptFontSize = std::max(9.0f, settings.scriptFontSize - 1.0f);
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_PLUS, "Larger text")) settings.scriptFontSize = std::min(32.0f, settings.scriptFontSize + 1.0f);
    ImGui::EndGroup();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + style.ItemSpacing.y * 0.3f);

    // ---- markers (syntax error = red, runtime error = amber)
    doc.editor.ClearMarkers();
    if (doc.errorLine > 0)
        doc.editor.AddMarker(static_cast<size_t>(doc.errorLine - 1), IM_COL32(240, 82, 82, 255), IM_COL32(240, 82, 82, 40), doc.error, doc.error);
    else if (doc.runtimeLine > 0)
        doc.editor.AddMarker(static_cast<size_t>(doc.runtimeLine - 1), IM_COL32(245, 165, 36, 255), IM_COL32(245, 165, 36, 32), doc.runtimeError, doc.runtimeError);

    if (doc.gotoLine > 0) {
        const size_t line = static_cast<size_t>(doc.gotoLine - 1);
        doc.editor.SetCursor(TextEditor::DocPos(line, 0));
        doc.editor.ScrollToLine(line);
        doc.editor.SetFocus();
        doc.gotoLine = 0;
    }

    // Ctrl+S / Ctrl+F while the text has focus.
    if (windowFocused && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteGlobal)) saveNow();

    // Preferences > Scripts (cheap setters, applied every frame).
    doc.editor.SetTabSize(settings.tabSize);
    doc.editor.SetShowWhitespacesEnabled(settings.showWhitespace);
    doc.editor.SetShowMiniMapEnabled(settings.showMinimap);
    doc.editor.SetCompletePairedGlyphs(settings.autoClosePairs);

    // ---- the text
    ImGui::PushFont(theme::fonts().mono, settings.scriptFontSize);
    const bool changed = doc.editor.Render("##text", ImVec2(0.0f, 0.0f));
    ImGui::PopFont();
    if (changed) {
        if (std::string* text = store.editableText(doc.path)) {
            *text = doc.editor.GetText();
            doc.synced = *text;
            store.touch(doc.path);
        }
        doc.checkTimer = kCheckDelay;
        doc.runtimeError.clear();
        doc.runtimeLine = -1;
    }

    // Debounced syntax check.
    if (doc.checkTimer > 0.0f) {
        doc.checkTimer -= ui.dt;
        if (doc.checkTimer <= 0.0f) {
            const Instance* s = doc.scriptId ? editor.scene().find(doc.scriptId) : nullptr;
            const std::string chunk = s ? s->fullName() : doc.title;
            doc.error = LuaRuntime::checkSyntax(doc.synced, chunk);
            doc.errorLine = doc.error.empty() ? -1 : scriptErrorLine(doc.error, chunk);
        }
    }
    ImGui::End();
    return doc.open;
}

} // namespace rift
