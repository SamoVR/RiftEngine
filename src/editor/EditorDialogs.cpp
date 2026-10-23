// The project launcher (welcome screen) and the editor's dialogs.
#include "editor/EditorApp.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "render/Texture.h"

#include "IconsFontAwesome6.h"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cstring>

namespace rift {

namespace fs = std::filesystem;

namespace {
    // A dialog's buttons are this wide.
    float buttonWidth() { return ImGui::GetFontSize() * 7.5f; }

    // Big clickable card with an icon, a title and a line of text.
    bool card(const char* id, const char* icon, const ImVec4& iconColor, const char* title, const char* text, float width, bool selected = false) {
        ImGui::PushID(id);
        const float h = ImGui::GetFontSize() * 3.6f;
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##card", ImVec2(width, h));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 b(pos.x + width, pos.y + h);
        draw->AddRectFilled(pos, b, theme::u32(hovered ? theme::color::frameHover : theme::color::frame), 10.0f);
        if (selected) draw->AddRect(pos, b, theme::u32(theme::color::accent), 10.0f, 0, 2.0f);
        const float pad = ImGui::GetFontSize();
        ImFont* font = ImGui::GetFont();
        const float iconSize = ImGui::GetFontSize() * 1.6f;
        draw->AddText(font, iconSize, ImVec2(pos.x + pad, pos.y + (h - iconSize) * 0.5f), theme::u32(iconColor), icon);
        const float x = pos.x + pad * 2.0f + iconSize;
        draw->AddText(theme::fonts().heading, ImGui::GetFontSize() * 1.05f, ImVec2(x, pos.y + h * 0.5f - ImGui::GetFontSize() * 1.1f), theme::u32(theme::color::text), title);
        draw->AddText(font, ImGui::GetFontSize() * 0.92f, ImVec2(x, pos.y + h * 0.5f + ImGui::GetFontSize() * 0.1f), theme::u32(theme::color::textDim), text);
        ImGui::PopID();
        return pressed;
    }

    bool validName(const char* name) {
        if (!*name) return false;
        for (const char* c = name; *c; ++c)
            if (std::strchr("\\/:*?\"<>|", *c)) return false;
        return std::strcmp(name, ".") != 0 && std::strcmp(name, "..") != 0;
    }

    struct Shortcut { const char* keys; const char* what; };
    const Shortcut kShortcuts[] = {
        { "F5 / Shift+F5 / F6", "Play / Stop / Pause" },
        { "Ctrl+S", "Save the scene and its scripts" },
        { "Ctrl+Z / Ctrl+Y", "Undo / Redo" },
        { "Ctrl+C / Ctrl+X / Ctrl+V", "Copy / Cut / Paste" },
        { "Ctrl+Shift+V", "Paste into the selection" },
        { "Ctrl+D / Del", "Duplicate / Delete" },
        { "Ctrl+G / Ctrl+U", "Group into a Model / Ungroup" },
        { "Ctrl+1 / 2 / 3 / 4", "Select / Move / Scale / Rotate tool" },
        { "Ctrl+L", "Switch local / world axes" },
        { "Ctrl (while dragging)", "Ignore snapping" },
        { "F2", "Rename" },
        { "F", "Zoom to the selection" },
        { "Right mouse + WASD / QE", "Fly the camera (Shift faster, Ctrl slower)" },
        { "Mouse wheel / middle drag", "Zoom / pan" },
        { "Alt + left drag", "Orbit" },
        { "Alt + click", "Select a part inside a model" },
        { "Ctrl / Shift + click", "Add to the selection" },
        { "Drag on empty space", "Box select" },
        { "Ctrl+N / Ctrl+Shift+N / Ctrl+O", "New scene / New project / Open project" },
        { "Ctrl+,", "Preferences" },
        { "Esc (while playing)", "Give the mouse back to the editor" },
    };
}

std::string EditorApp::defaultProjectsFolder() const {
    const std::string docs = Platform::documentsFolder();
    const fs::path base = docs.empty() ? Paths::userData() : Paths::fromUtf8(docs);
    return Paths::toUtf8(base / "RIFT Projects");
}

void EditorApp::openProjectDialog() {
    const std::string start = editor_->hasProject() ? Paths::toUtf8(editor_->project().root().parent_path()) : defaultProjectsFolder();
    const std::string folder = Platform::pickFolderDialog("Open a RIFT project (the folder with project.json)", start);
    if (folder.empty()) return;
    const fs::path p = Paths::fromUtf8(folder);
    std::error_code ec;
    if (!fs::exists(p / "project.json", ec)) {
        editor_->notify("That folder has no project.json. Pick the project's main folder.", NoticeLevel::Warning);
        return;
    }
    editor_->requestDiscard([this, p] { editor_->openProject(p); });
}

void EditorApp::openNewProject() {
    if (newProjectFolder_.empty()) newProjectFolder_ = defaultProjectsFolder();
    std::strncpy(newProjectName_, "My Game", sizeof(newProjectName_));
    showNewProject_ = true;
}

void EditorApp::openExample(const fs::path& folder) {
    // Examples are copied first, so the originals stay untouched.
    std::error_code ec;
    const fs::path base = Paths::fromUtf8(defaultProjectsFolder());
    fs::create_directories(base, ec);
    fs::path target = base / folder.filename();
    if (fs::exists(target / "project.json", ec)) {
        editor_->requestDiscard([this, target] { editor_->openProject(target); });
        return;
    }
    fs::copy(folder, target, fs::copy_options::recursive, ec);
    if (ec) {
        editor_->notify("Could not copy the example: " + ec.message(), NoticeLevel::Error);
        return;
    }
    editor_->requestDiscard([this, target] { editor_->openProject(target); });
}

void EditorApp::openSceneNameDialog(bool saveAs) {
    if (!editor_->hasProject() || editor_->playing()) return;
    sceneNameIsSaveAs_ = saveAs;
    std::strncpy(sceneName_, saveAs ? (editor_->sceneName() + " Copy").c_str() : "NewScene", sizeof(sceneName_) - 1);
    showSceneName_ = true;
}

// ============================================================================
//  Welcome screen (no project open)
// ============================================================================
void EditorApp::drawWelcome(ImGuiID dockspace) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color::bgDark);
    ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleColor();
    // The panels' dock layout must stay alive while it is not shown.
    ImGui::DockSpace(dockspace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);

    const float fs_ = ImGui::GetFontSize();
    const float width = std::min(vp->WorkSize.x - fs_ * 4.0f, fs_ * 58.0f);
    const float left = (vp->WorkSize.x - width) * 0.5f;
    ImGui::SetCursorPos(ImVec2(left, std::max(fs_ * 2.0f, vp->WorkSize.y * 0.12f)));
    ImGui::BeginGroup();

    // ---- header
    if (logo_ && logo_->isValid()) {
        ImGui::Image(static_cast<ImTextureID>(logo_->id()), ImVec2(fs_ * 4.2f, fs_ * 4.2f), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine(0.0f, fs_);
    }
    ImGui::BeginGroup();
    theme::pushHeading(2.6f);
    ImGui::TextUnformatted("RIFT");
    theme::pop();
    ImGui::TextDisabled("Game engine %s  -  build Roblox-style games with Lua", RIFT_VERSION_STRING);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, fs_ * 1.5f));

    // ---- left: actions, right: recent projects
    const float column = (width - fs_ * 2.0f) * 0.42f;
    ImGui::BeginGroup();
    theme::heading("Start", column);
    if (card("new", ICON_FA_FOLDER_PLUS, theme::color::accentHover, "New Project", "Empty, baseplate or obby template", column)) openNewProject();
    ImGui::Dummy(ImVec2(0.0f, fs_ * 0.3f));
    if (card("open", ICON_FA_FOLDER_OPEN, theme::color::warning, "Open Project", "A folder with a project.json", column)) openProjectDialog();
    ImGui::Dummy(ImVec2(0.0f, fs_ * 0.3f));
    // Examples shipped next to the engine's data folder.
    std::error_code ec;
    const fs::path examples = Paths::engineData().parent_path() / "examples";
    if (fs::is_directory(examples, ec)) {
        ImGui::Dummy(ImVec2(0.0f, fs_ * 0.6f));
        theme::heading("Examples", column);
        for (const auto& item : fs::directory_iterator(examples, ec)) {
            if (!fs::exists(item.path() / "project.json", ec)) continue;
            const std::string name = Paths::toUtf8(item.path().filename());
            if (card(name.c_str(), ICON_FA_GAMEPAD, theme::color::success, name.c_str(), "Opens a copy in your projects folder", column)) openExample(item.path());
            ImGui::Dummy(ImVec2(0.0f, fs_ * 0.3f));
        }
    }
    ImGui::EndGroup();

    ImGui::SameLine(0.0f, fs_ * 2.0f);
    ImGui::BeginGroup();
    const float recentWidth = width - column - fs_ * 2.0f;
    theme::heading("Recent projects", recentWidth);
    ImGui::BeginChild("##recent", ImVec2(recentWidth, std::max(fs_ * 12.0f, vp->WorkSize.y * 0.5f)));
    std::string remove;
    for (const std::string& path : settings_.recentProjects) {
        const fs::path p = Paths::fromUtf8(path);
        const bool exists = fs::exists(p / "project.json", ec);
        ImGui::PushID(path.c_str());
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        const float h = fs_ * 2.9f;
        ImGui::BeginDisabled(!exists);
        if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(recentWidth - fs_ * 2.5f, h)))
            editor_->openProject(p);
        ImGui::EndDisabled();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ImGui::GetFont(), fs_ * 1.3f, ImVec2(pos.x + fs_ * 0.5f, pos.y + (h - fs_ * 1.3f) * 0.5f), theme::u32(exists ? theme::color::warning : theme::color::textDim), ICON_FA_FOLDER);
        const std::string name = Paths::toUtf8(p.filename()) + (exists ? "" : "  (missing)");
        draw->AddText(theme::fonts().medium, fs_, ImVec2(pos.x + fs_ * 2.6f, pos.y + h * 0.5f - fs_ * 1.05f), theme::u32(exists ? theme::color::text : theme::color::textDim), name.c_str());
        draw->AddText(ImGui::GetFont(), fs_ * 0.85f, ImVec2(pos.x + fs_ * 2.6f, pos.y + h * 0.5f + fs_ * 0.05f), theme::u32(theme::color::textDim), path.c_str());
        ImGui::SameLine(recentWidth - fs_ * 2.2f);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetFrameHeight()) * 0.5f);
        if (theme::iconButton(ICON_FA_XMARK, "Remove from the list")) remove = path;
        ImGui::PopID();
    }
    if (settings_.recentProjects.empty()) ImGui::TextDisabled("Projects you open show up here.");
    ImGui::EndChild();
    ImGui::EndGroup();
    if (!remove.empty()) {
        settings_.recentProjects.erase(std::remove(settings_.recentProjects.begin(), settings_.recentProjects.end(), remove), settings_.recentProjects.end());
        settings_.save();
    }
    ImGui::EndGroup();
    ImGui::End();
}

// ============================================================================
//  Dialogs
// ============================================================================
void EditorApp::drawDialogs() {
    Editor& e = *editor_;
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    const float fs_ = ImGui::GetFontSize();

    // ---- unsaved changes
    if (e.pendingDiscard && !ImGui::IsPopupOpen("Unsaved changes")) ImGui::OpenPopup("Unsaved changes");
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Unsaved changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::Text("Save the changes to \"%s\" first?", e.sceneName().c_str());
        ImGui::TextDisabled("Otherwise they are lost.");
        ImGui::Spacing();
        auto finish = [&](bool save) {
            std::function<void()> action = std::move(e.pendingDiscard);
            e.pendingDiscard = nullptr;
            ImGui::CloseCurrentPopup();
            if (save && !e.saveScene()) return; // keep everything if saving failed
            if (action) action();
        };
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save", ImVec2(buttonWidth(), 0.0f))) finish(true);
        ImGui::SameLine();
        if (ImGui::Button("Don't Save", ImVec2(buttonWidth(), 0.0f))) finish(false);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            e.pendingDiscard = nullptr;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // ---- new project
    if (showNewProject_) {
        ImGui::OpenPopup("New project");
        showNewProject_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New project", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float w = fs_ * 30.0f;
        ImGui::TextUnformatted("Name");
        ImGui::SetNextItemWidth(w);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##name", newProjectName_, sizeof(newProjectName_));
        ImGui::TextUnformatted("Location");
        ImGui::SetNextItemWidth(w - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::InputText("##folder", &newProjectFolder_);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN, ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string picked = Platform::pickFolderDialog("Where should the project folder go?", newProjectFolder_);
            if (!picked.empty()) newProjectFolder_ = picked;
        }
        ImGui::Spacing();
        ImGui::TextUnformatted("Template");
        if (card("t0", ICON_FA_FILE, theme::color::textDim, "Empty", "Nothing but a spawn point", w, newProjectTemplate_ == 0)) newProjectTemplate_ = 0;
        if (card("t1", ICON_FA_TABLE_CELLS_LARGE, theme::color::accentHover, "Baseplate", "A big floor, ready to build on", w, newProjectTemplate_ == 1)) newProjectTemplate_ = 1;
        if (card("t2", ICON_FA_PERSON_RUNNING, theme::color::success, "Obby", "A small obstacle course with scripts", w, newProjectTemplate_ == 2)) newProjectTemplate_ = 2;
        ImGui::Spacing();
        const bool ok = validName(newProjectName_) && !newProjectFolder_.empty();
        if (!validName(newProjectName_) && newProjectName_[0]) ImGui::TextColored(theme::color::error, "The name cannot contain \\ / : * ? \" < > |");
        else ImGui::TextDisabled("%s", Paths::toUtf8(Paths::fromUtf8(newProjectFolder_) / Paths::fromUtf8(newProjectName_)).c_str());
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(ICON_FA_CHECK "  Create", ImVec2(buttonWidth(), 0.0f)) || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
            std::error_code ec;
            fs::create_directories(Paths::fromUtf8(newProjectFolder_), ec);
            const auto t = static_cast<Project::Template>(newProjectTemplate_);
            const fs::path parent = Paths::fromUtf8(newProjectFolder_);
            const std::string name = newProjectName_;
            ImGui::CloseCurrentPopup();
            e.requestDiscard([&e, parent, name, t] { e.createProject(parent, name, t); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- new scene / save scene as
    if (showSceneName_) {
        ImGui::OpenPopup("Scene name");
        showSceneName_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Scene name", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(sceneNameIsSaveAs_ ? "Save the scene under a new name" : "Name of the new scene");
        ImGui::SetNextItemWidth(fs_ * 20.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##scene", sceneName_, sizeof(sceneName_), ImGuiInputTextFlags_EnterReturnsTrue);
        std::error_code ec;
        const bool exists = e.hasProject() && fs::exists(e.project().scenesFolder() / Paths::fromUtf8(sceneName_), ec);
        const bool ok = validName(sceneName_) && !exists;
        if (exists) ImGui::TextColored(theme::color::error, "A scene with that name exists");
        else ImGui::TextDisabled("scenes/%s/scene.json", sceneName_);
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(sceneNameIsSaveAs_ ? ICON_FA_FLOPPY_DISK "  Save" : ICON_FA_PLUS "  Create", ImVec2(buttonWidth(), 0.0f)) || (ok && enter)) {
            const std::string name = sceneName_;
            ImGui::CloseCurrentPopup();
            if (sceneNameIsSaveAs_) e.saveSceneAs(name);
            else e.requestDiscard([&e, name] { e.newScene(name); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // ---- preferences
    if (showPreferences_) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(fs_ * 30.0f, fs_ * 26.0f), ImGuiCond_Appearing);
        if (ImGui::Begin(ICON_FA_SLIDERS "  Preferences###Preferences", &showPreferences_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
            EditorSettings& s = settings_;
            if (ImGui::BeginTabBar("##prefs")) {
                if (ImGui::BeginTabItem("General")) {
                    ImGui::SliderFloat("Interface size", &s.uiScale, 0.75f, 2.0f, "%.2fx");
                    theme::tooltip("Scales the whole editor (on top of the Windows display scaling).");
                    ImGui::SliderFloat("Script text size", &s.scriptFontSize, 10.0f, 28.0f, "%.0f px");
                    ImGui::Checkbox("Reopen the last project on start", &s.reopenLastProject);
                    ImGui::SliderInt("Autosave every", &s.autosaveMinutes, 0, 30, s.autosaveMinutes == 0 ? "off" : "%d min");
                    theme::tooltip("Autosaves go to scene.autosave.json next to the scene; the scene itself is only written when you save.");
                    if (ImGui::Checkbox("Show RIFT in Discord", &s.discordPresence)) {
                        if (s.discordPresence) presence_.init(s.discordClientId);
                        else presence_.shutdown();
                    }
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Viewport")) {
                    ImGui::Checkbox("Grid", &s.showGrid);
                    ImGui::DragFloat("Grid step", &s.gridStep, 0.05f, 0.1f, 64.0f, "%.2f studs");
                    ImGui::Checkbox("Shadows", &s.shadows);
                    ImGui::Checkbox("Wireframe", &s.wireframe);
                    ImGui::Checkbox("Statistics", &s.showStats);
                    if (ImGui::Checkbox("V-Sync", &s.vsync)) glfwSwapInterval(s.vsync ? 1 : 0);
                    ImGui::SliderFloat("Camera speed", &s.cameraSpeed, 2.0f, 200.0f, "%.0f studs/s", ImGuiSliderFlags_Logarithmic);
                    ImGui::SliderFloat("Mouse sensitivity", &s.mouseSensitivity, 0.05f, 1.0f, "%.2f");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Snapping")) {
                    ImGui::Checkbox("Snap while moving, rotating and resizing", &s.snap);
                    ImGui::DragFloat("Move step", &s.snapMove, 0.05f, 0.01f, 64.0f, "%.2f studs");
                    ImGui::DragFloat("Rotate step", &s.snapRotate, 0.5f, 1.0f, 180.0f, "%.0f degrees");
                    ImGui::DragFloat("Resize step", &s.snapScale, 0.05f, 0.01f, 64.0f, "%.2f studs");
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
        }
        ImGui::End();
        if (!showPreferences_) settings_.save();
    }

    // ---- project settings
    if (showProjectSettings_ && e.hasProject()) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(fs_ * 32.0f, 0.0f), ImGuiCond_Appearing);
        if (ImGui::Begin(ICON_FA_GEAR "  Project Settings###ProjectSettings", &showProjectSettings_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
            ProjectSettings& p = projectDraft_;
            ImGui::SeparatorText("Game");
            ImGui::InputText("Name", &p.name);
            ImGui::InputText("Version", &p.version);
            ImGui::InputText("Author", &p.author);
            if (ImGui::BeginCombo("Start scene", p.startScene.c_str())) {
                for (const fs::path& s : e.project().scenes()) {
                    std::error_code ec;
                    std::string rel = Paths::toUtf8(fs::relative(s, e.project().root(), ec));
                    std::replace(rel.begin(), rel.end(), '\\', '/');
                    if (ImGui::Selectable(rel.c_str(), rel == p.startScene)) p.startScene = rel;
                }
                ImGui::EndCombo();
            }
            ImGui::InputText("Icon (png)", &p.icon);
            ImGui::SeparatorText("Window");
            ImGui::InputInt("Width", &p.windowWidth, 16, 128);
            ImGui::InputInt("Height", &p.windowHeight, 16, 128);
            p.windowWidth = std::clamp(p.windowWidth, 320, 7680);
            p.windowHeight = std::clamp(p.windowHeight, 240, 4320);
            ImGui::Checkbox("Fullscreen", &p.fullscreen);
            ImGui::SameLine();
            ImGui::Checkbox("Resizable", &p.resizable);
            ImGui::SameLine();
            ImGui::Checkbox("V-Sync", &p.vsync);
            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save", ImVec2(buttonWidth(), 0.0f))) {
                e.project().settings() = p;
                if (e.saveProjectSettings()) showProjectSettings_ = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f))) showProjectSettings_ = false;
        }
        ImGui::End();
    }

    // ---- shortcuts
    if (showShortcuts_) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(fs_ * 34.0f, fs_ * 30.0f), ImGuiCond_Appearing);
        if (ImGui::Begin(ICON_FA_KEYBOARD "  Keyboard Shortcuts###Shortcuts", &showShortcuts_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
            if (ImGui::BeginTable("##keys", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, fs_ * 14.0f);
                ImGui::TableSetupColumn("Action");
                for (const Shortcut& s : kShortcuts) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushFont(theme::fonts().mono, 0.0f);
                    ImGui::TextColored(theme::color::accentHover, "%s", s.keys);
                    ImGui::PopFont();
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(s.what);
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    // ---- about
    if (showAbout_) {
        ImGui::OpenPopup("About RIFT");
        showAbout_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("About RIFT", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (logo_ && logo_->isValid()) {
            ImGui::Image(static_cast<ImTextureID>(logo_->id()), ImVec2(fs_ * 3.5f, fs_ * 3.5f), ImVec2(0, 1), ImVec2(1, 0));
            ImGui::SameLine(0.0f, fs_);
        }
        ImGui::BeginGroup();
        theme::pushHeading(1.8f);
        ImGui::TextUnformatted("RIFT");
        theme::pop();
        ImGui::TextDisabled("Version %s", RIFT_VERSION_STRING);
        ImGui::EndGroup();
        ImGui::Spacing();
        ImGui::TextUnformatted("Built with Dear ImGui, ImGuizmo, GLFW, GLEW, GLM, Jolt Physics,\nLua 5.4 + sol2, miniaudio, stb, nlohmann/json, ImGuiColorTextEdit,\nInter, JetBrains Mono and Font Awesome Free.");
        ImGui::TextDisabled("Licenses: third_party/README.md and data/fonts.");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace rift
