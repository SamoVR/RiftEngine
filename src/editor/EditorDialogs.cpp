// The project launcher (welcome screen) and the editor's dialogs.
#include "editor/EditorApp.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "project/Exporter.h"
#include "render/Texture.h"

#include "IconsFontAwesome6.h"
#include <GLFW/glfw3.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cstdio>
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
        { "Ctrl+B / Ctrl+F5", "Build the game / Run it in the player" },
        { "Esc (while playing)", "Give the mouse back to the editor" },
    };
}

std::string EditorApp::defaultProjectsFolder() const {
    if (!settings_.projectsFolder.empty()) return settings_.projectsFolder; // Preferences > Projects
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
    // Examples are copied first (the originals stay untouched): ask where to.
    exampleSource_ = folder;
    exampleFolder_ = defaultProjectsFolder();
    std::string name = Paths::toUtf8(folder.filename());
    std::error_code ec;
    // A free name: DemoObby, DemoObby 2, ...
    for (int n = 2; fs::exists(Paths::fromUtf8(exampleFolder_) / Paths::fromUtf8(name), ec) && n < 100; ++n)
        name = Paths::toUtf8(folder.filename()) + " " + std::to_string(n);
    std::strncpy(exampleName_, name.c_str(), sizeof(exampleName_) - 1);
    showExample_ = true;
}

void EditorApp::openSceneNameDialog(bool saveAs) {
    if (!editor_->hasProject() || editor_->playing()) return;
    sceneNameIsSaveAs_ = saveAs;
    std::strncpy(sceneName_, saveAs ? (editor_->sceneName() + " Copy").c_str() : "NewScene", sizeof(sceneName_) - 1);
    showSceneName_ = true;
}

// ============================================================================
//  Build Game / Run in Player
// ============================================================================
void EditorApp::openBuildDialog() {
    if (!editor_->hasProject()) return;
    const Project& p = editor_->project();
    buildFolder_ = Paths::toUtf8(p.root() / "Builds" / Paths::fromUtf8(executableName(p.settings().name)));
    showBuild_ = true;
}

void EditorApp::buildGame() {
    Editor& e = *editor_;
    // The build is made from the files on disk.
    if (e.isDirty() && !e.saveScene()) return;
    ExportOptions options;
    options.outputFolder = Paths::fromUtf8(buildFolder_);
    options.playerExe = Paths::exeDir() / "RiftPlayer.exe";
    options.engineData = Paths::engineData();
    const ExportResult r = exportGame(e.project(), options);
    if (!r.ok) {
        e.notify("Build failed: " + r.error, NoticeLevel::Error);
        return;
    }
    char size[32];
    std::snprintf(size, sizeof(size), "%.1f MB", r.bytes / (1024.0 * 1024.0));
    e.notify("Built " + e.project().settings().name + " (" + std::to_string(r.files) + " files, " + size + ")", NoticeLevel::Success);
    if (buildOpenFolder_) Platform::revealInExplorer(Paths::toUtf8(r.executable));
    if (buildRun_) Platform::launch(Paths::toUtf8(r.executable), {}, Paths::toUtf8(r.executable.parent_path()));
}

void EditorApp::runInPlayer() {
    Editor& e = *editor_;
    if (!e.hasProject()) return;
    if (e.isDirty() && !e.saveScene()) return;
    const fs::path player = Paths::exeDir() / "RiftPlayer.exe";
    std::error_code ec;
    if (!fs::exists(player, ec)) {
        e.notify("RiftPlayer.exe is missing next to the editor; build the RiftPlayer project", NoticeLevel::Error);
        return;
    }
    Platform::launch(Paths::toUtf8(player), { Paths::toUtf8(e.project().root()) }, Paths::toUtf8(Paths::exeDir()));
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

    // ---- delete (when Preferences ask for a confirmation)
    if (e.deleteRequested && !ImGui::IsPopupOpen("Delete?")) ImGui::OpenPopup("Delete?");
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Delete?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const size_t n = e.selection().size();
        const Instance* one = n == 1 ? e.scene().find(e.primary()) : nullptr;
        if (one) ImGui::Text("Delete %s?", one->name.c_str());
        else ImGui::Text("Delete %d objects?", static_cast<int>(n));
        ImGui::TextDisabled(e.playing() ? "Only in the running game." : "Undo (Ctrl+Z) brings it back.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_TRASH_CAN "  Delete", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            e.deleteRequested = false;
            e.deleteSelection();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            e.deleteRequested = false;
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
        ImGui::Checkbox("Track changes with Git (version control)", &settings_.gitInitNewProjects);
        theme::tooltip("Makes the project a git repository with a first commit, so every version can be restored and backed up to GitHub.");
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

    // ---- open an example (copy it first)
    if (showExample_) {
        ImGui::OpenPopup("Open example");
        showExample_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Open example", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float w = fs_ * 30.0f;
        ImGui::PushTextWrapPos(w);
        ImGui::TextDisabled("Examples open as a copy, so you can change anything and the original stays as it is.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextUnformatted("Name of the copy");
        ImGui::SetNextItemWidth(w);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##exampleName", exampleName_, sizeof(exampleName_));
        ImGui::TextUnformatted("Location");
        ImGui::SetNextItemWidth(w - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::InputText("##exampleFolder", &exampleFolder_);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pickExample", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string picked = Platform::pickFolderDialog("Where should the copy go?", exampleFolder_);
            if (!picked.empty()) exampleFolder_ = picked;
        }
        std::error_code ec;
        const fs::path target = Paths::fromUtf8(exampleFolder_) / Paths::fromUtf8(exampleName_);
        const bool taken = validName(exampleName_) && fs::exists(target, ec);
        const bool ok = validName(exampleName_) && !exampleFolder_.empty() && !taken;
        if (taken) ImGui::TextColored(theme::color::error, "That folder already exists");
        else ImGui::TextDisabled("%s", Paths::toUtf8(target).c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(!ok);
        if (ImGui::Button(ICON_FA_COPY "  Copy and open", ImVec2(buttonWidth() * 1.3f, 0.0f)) || (ok && ImGui::IsKeyPressed(ImGuiKey_Enter))) {
            fs::create_directories(target.parent_path(), ec);
            fs::copy(exampleSource_, target, fs::copy_options::recursive, ec);
            ImGui::CloseCurrentPopup();
            if (ec) {
                e.notify("Could not copy the example: " + ec.message(), NoticeLevel::Error);
            } else {
                // The copy is its own game: give it the folder's name.
                Project copy;
                if (copy.load(target)) {
                    copy.settings().name = exampleName_;
                    copy.save();
                }
                e.requestDiscard([&e, target] { e.openProject(target); });
            }
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
        else ImGui::TextDisabled("%s/%s/scene.json", e.hasProject() ? e.project().settings().scenesFolder.c_str() : "scenes", sceneName_);
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

    drawPreferences();
    drawProjectSettings();
    drawUpdateDialog();

    // ---- build game
    if (showBuild_) {
        ImGui::OpenPopup("Build game");
        showBuild_ = false;
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Build game", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float w = fs_ * 32.0f;
        ImGui::PushTextWrapPos(w);
        ImGui::TextUnformatted("Makes a folder with the game, ready to zip and share: the game's .exe, the engine files it needs and the project's scenes, scripts and assets. Nothing else has to be installed to play it.");
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        ImGui::TextUnformatted("Folder");
        ImGui::SetNextItemWidth(w - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::InputText("##buildFolder", &buildFolder_);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pickBuild", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string picked = Platform::pickFolderDialog("Where should the game go?", buildFolder_);
            if (!picked.empty()) buildFolder_ = picked;
        }
        ImGui::TextDisabled("%s.exe", executableName(e.project().settings().name).c_str());
        ImGui::Checkbox("Show the folder afterwards", &buildOpenFolder_);
        ImGui::Checkbox("Run the game afterwards", &buildRun_);
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_BOX_OPEN "  Build", ImVec2(buttonWidth(), 0.0f))) {
            ImGui::CloseCurrentPopup();
            buildGame();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
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
