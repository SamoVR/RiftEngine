// Preferences, Project Settings and the update dialog.
#include "editor/EditorApp.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "project/Git.h"

#include "IconsFontAwesome6.h"
#include <GLFW/glfw3.h>
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cstdio>

namespace rift {

namespace fs = std::filesystem;

namespace {
    float buttonWidth() { return ImGui::GetFontSize() * 7.5f; }

    // One row: label on the left, the widget on the right (drawn by `widget`).
    template <class F> void row(const char* label, const char* help, F&& widget) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        if (help) theme::tooltip(help);
        ImGui::TableSetColumnIndex(1);
        ImGui::PushID(label);
        ImGui::SetNextItemWidth(-FLT_MIN);
        widget();
        ImGui::PopID();
    }

    bool beginRows(const char* id) {
        if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) return false;
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 0.42f);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.58f);
        return true;
    }

    // A path field with a folder / file picker button.
    bool pathField(const char* id, std::string& value, const char* hint, bool folder, const char* title) {
        bool changed = false;
        ImGui::PushID(id);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
        changed |= ImGui::InputTextWithHint("##path", hint, &value);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(folder ? ICON_FA_FOLDER_OPEN : ICON_FA_FILE, ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string picked = folder ? Platform::pickFolderDialog(title, value)
                                              : Platform::openFileDialog(title, { { "Programs", "*.exe" } }, value.empty() ? std::string() : Paths::toUtf8(Paths::fromUtf8(value).parent_path()));
            if (!picked.empty()) { value = picked; changed = true; }
        }
        ImGui::PopID();
        return changed;
    }

    std::string megabytes(std::uint64_t bytes) {
        char text[32];
        std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return text;
    }
}

// ============================================================================
//  Preferences
// ============================================================================
void EditorApp::drawPreferences() {
    if (!showPreferences_) return;
    const float fs_ = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs_ * 46.0f, fs_ * 32.0f), ImGuiCond_Appearing);
    ImGui::SetNextWindowSizeConstraints(ImVec2(fs_ * 34.0f, fs_ * 20.0f), ImVec2(FLT_MAX, FLT_MAX));
    if (!ImGui::Begin(ICON_FA_SLIDERS "  Preferences###Preferences", &showPreferences_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!showPreferences_) settings_.save();
        return;
    }
    EditorSettings& s = settings_;
    struct Page { const char* icon; const char* name; };
    static const Page pages[] = {
        { ICON_FA_HOUSE, "General" }, { ICON_FA_PALETTE, "Appearance" }, { ICON_FA_CUBE, "Viewport" },
        { ICON_FA_UP_DOWN_LEFT_RIGHT, "Editing" }, { ICON_FA_CODE, "Scripts" }, { ICON_FA_PLAY, "Playing" },
        { ICON_FA_TERMINAL, "Output" }, { ICON_FA_CODE_BRANCH, "Source Control" }, { ICON_FA_CIRCLE_ARROW_UP, "Updates" },
    };
    constexpr int pageCount = static_cast<int>(sizeof(pages) / sizeof(pages[0]));

    // Categories on the left
    ImGui::BeginChild("##pages", ImVec2(fs_ * 11.0f, -ImGui::GetFrameHeightWithSpacing() - fs_ * 0.4f), ImGuiChildFlags_Borders);
    for (int k = 0; k < pageCount; ++k) {
        const std::string label = std::string(pages[k].icon) + "  " + pages[k].name;
        if (ImGui::Selectable(label.c_str(), preferencesPage_ == k, 0, ImVec2(0.0f, fs_ * 1.7f))) preferencesPage_ = k;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // The page
    ImGui::BeginChild("##page", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - fs_ * 0.4f), ImGuiChildFlags_None);
    theme::heading(pages[preferencesPage_].name);
    if (beginRows("##rows")) {
        switch (preferencesPage_) {
            case 0: { // General
                row("Projects folder", "Where new projects and example copies go.", [&] {
                    std::string folder = s.projectsFolder.empty() ? defaultProjectsFolder() : s.projectsFolder;
                    if (pathField("projects", folder, "Documents\\RIFT Projects", true, "Where should new projects go?")) s.projectsFolder = folder;
                });
                row("Reopen the last project on start", nullptr, [&] { ImGui::Checkbox("##reopen", &s.reopenLastProject); });
                row("Autosave", "Autosaves go to scene.autosave.json next to the scene; the scene itself is only written when you save.", [&] {
                    ImGui::SliderInt("##autosave", &s.autosaveMinutes, 0, 30, s.autosaveMinutes == 0 ? "off" : "every %d min");
                });
                row("Ask before deleting objects", nullptr, [&] { ImGui::Checkbox("##confirmDelete", &s.confirmDelete); });
                row("Show RIFT in Discord", "Your Discord status shows the game you are working on.", [&] {
                    if (ImGui::Checkbox("##discord", &s.discordPresence)) {
                        if (s.discordPresence) presence_.init(s.discordClientId);
                        else presence_.shutdown();
                    }
                });
                break;
            }
            case 1: { // Appearance
                row("Interface size", "Scales the whole editor (on top of the Windows display scaling).", [&] {
                    ImGui::SliderFloat("##scale", &s.uiScale, 0.75f, 2.0f, "%.2fx");
                });
                row("Accent colour", nullptr, [&] {
                    for (int k = 0; k < theme::accentCount(); ++k) {
                        if (k) ImGui::SameLine(0.0f, fs_ * 0.35f);
                        ImGui::PushID(k);
                        const ImVec4 c = theme::accentColor(k);
                        ImGui::PushStyleColor(ImGuiCol_Button, c);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, c);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, fs_);
                        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, s.accent == k ? 2.0f : 0.0f);
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1, 1, 1, 0.9f));
                        if (ImGui::Button(s.accent == k ? ICON_FA_CHECK : "", ImVec2(fs_ * 1.6f, fs_ * 1.6f))) {
                            s.accent = k;
                            theme::setAccent(k);
                        }
                        ImGui::PopStyleColor();
                        ImGui::PopStyleVar(2);
                        ImGui::PopStyleColor(3);
                        theme::tooltip(theme::accentName(k));
                        ImGui::PopID();
                    }
                });
                row("Tooltips", "Explanations when you hover over buttons, like this one.", [&] {
                    if (ImGui::Checkbox("##tooltips", &s.showTooltips)) theme::setTooltipsEnabled(s.showTooltips);
                });
                row("Statistics in the viewport", "Frame rate, parts, draw calls.", [&] { ImGui::Checkbox("##stats", &s.showStats); });
                row("Panels", nullptr, [&] {
                    ImGui::Checkbox("Explorer", &s.showExplorer);
                    ImGui::SameLine();
                    ImGui::Checkbox("Properties", &s.showProperties);
                    ImGui::Checkbox("Output", &s.showOutput);
                    ImGui::SameLine();
                    ImGui::Checkbox("Assets", &s.showAssets);
                    ImGui::SameLine();
                    ImGui::Checkbox("Source Control", &s.showSourceControl);
                });
                row("Layout", nullptr, [&] { if (ImGui::Button(ICON_FA_TABLE_COLUMNS "  Reset the panel layout")) resetLayout_ = true; });
                break;
            }
            case 2: { // Viewport
                row("Grid", nullptr, [&] { ImGui::Checkbox("##grid", &s.showGrid); });
                row("Grid step", nullptr, [&] { ImGui::DragFloat("##gridStep", &s.gridStep, 0.05f, 0.1f, 64.0f, "%.2f studs"); });
                row("GUI while editing", "Draws StarterGui over the scene while editing.", [&] { ImGui::Checkbox("##gui", &s.showGui); });
                row("Shadows", nullptr, [&] { ImGui::Checkbox("##shadows", &s.shadows); });
                row("Wireframe", nullptr, [&] { ImGui::Checkbox("##wire", &s.wireframe); });
                row("Anti-aliasing", "Smooths edges (multisampling). Lower is faster.", [&] {
                    const char* names[] = { "Off", "2x", "4x", "8x" };
                    const int values[] = { 1, 2, 4, 8 };
                    int current = s.msaa >= 8 ? 3 : s.msaa >= 4 ? 2 : s.msaa >= 2 ? 1 : 0;
                    if (ImGui::Combo("##msaa", &current, names, 4)) s.msaa = values[current];
                });
                row("V-Sync", "Waits for the monitor's refresh: smooth and cool. Off: as fast as possible.", [&] {
                    if (ImGui::Checkbox("##vsync", &s.vsync)) glfwSwapInterval(s.vsync ? 1 : 0);
                });
                row("Frame rate limit", "Used when V-Sync is off.", [&] {
                    ImGui::BeginDisabled(s.vsync);
                    ImGui::SliderInt("##fps", &s.fpsLimit, 0, 360, s.fpsLimit == 0 ? "no limit" : "%d FPS");
                    ImGui::EndDisabled();
                });
                row("Slow down in the background", "About 15 frames per second while another window is active (not while playing). Saves power.", [&] {
                    ImGui::Checkbox("##throttle", &s.throttleInBackground);
                });
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::Dummy(ImVec2(0.0f, fs_ * 0.4f));
                ImGui::TextDisabled("Camera");
                row("Fly speed", nullptr, [&] { ImGui::SliderFloat("##speed", &s.cameraSpeed, 2.0f, 200.0f, "%.0f studs/s", ImGuiSliderFlags_Logarithmic); });
                row("Mouse sensitivity", nullptr, [&] { ImGui::SliderFloat("##sens", &s.mouseSensitivity, 0.05f, 1.0f, "%.2f"); });
                row("Field of view", nullptr, [&] { ImGui::SliderFloat("##fov", &s.fieldOfView, 30.0f, 110.0f, "%.0f degrees"); });
                row("Invert mouse Y", nullptr, [&] { ImGui::Checkbox("##invert", &s.invertMouseY); });
                break;
            }
            case 3: { // Editing
                row("Snap while moving, rotating and resizing", "Hold Ctrl while dragging to ignore snapping for a moment.", [&] { ImGui::Checkbox("##snap", &s.snap); });
                row("Move step", nullptr, [&] { ImGui::DragFloat("##move", &s.snapMove, 0.05f, 0.01f, 64.0f, "%.2f studs"); });
                row("Rotate step", nullptr, [&] { ImGui::DragFloat("##rotate", &s.snapRotate, 0.5f, 1.0f, 180.0f, "%.0f degrees"); });
                row("Resize step", nullptr, [&] { ImGui::DragFloat("##resize", &s.snapScale, 0.05f, 0.01f, 64.0f, "%.2f studs"); });
                row("Local axes", "Move and rotate along the object's own axes (Ctrl+L).", [&] { ImGui::Checkbox("##local", &s.localSpace); });
                break;
            }
            case 4: { // Scripts
                row("Text size", nullptr, [&] { ImGui::SliderFloat("##font", &s.scriptFontSize, 10.0f, 28.0f, "%.0f px"); });
                row("Tab width", nullptr, [&] { ImGui::SliderInt("##tab", &s.tabSize, 2, 8, "%d spaces"); });
                row("Show spaces and tabs", nullptr, [&] { ImGui::Checkbox("##ws", &s.showWhitespace); });
                row("Mini map", "A small overview of the whole file at the right.", [&] { ImGui::Checkbox("##minimap", &s.showMinimap); });
                row("Close brackets and quotes", nullptr, [&] { ImGui::Checkbox("##pairs", &s.autoClosePairs); });
                row("External editor", "Program for \"Open in External Editor\", for example VS Code (Code.exe). Empty: what Windows uses for .lua files.", [&] {
                    pathField("external", s.externalEditor, "Windows default", false, "Choose the editor program");
                });
                break;
            }
            case 5: { // Playing
                row("Clear the Output on Play", nullptr, [&] { ImGui::Checkbox("##clear", &s.clearOutputOnPlay); });
                row("Save before playing", nullptr, [&] { ImGui::Checkbox("##savePlay", &s.saveBeforePlay); });
                row("Pause on script errors", "The game pauses when a script error happens, so you can look around.", [&] { ImGui::Checkbox("##pause", &s.pauseOnError); });
                break;
            }
            case 6: { // Output
                row("Show the game time", nullptr, [&] { ImGui::Checkbox("##time", &s.outputTimestamps); });
                row("Keep at most", nullptr, [&] { ImGui::SliderInt("##lines", &s.outputMaxLines, 200, 20000, "%d lines", ImGuiSliderFlags_Logarithmic); });
                break;
            }
            case 7: { // Source Control
                row("Git program", "git.exe; empty finds Git for Windows by itself.", [&] {
                    pathField("git", s.gitPath, "found automatically", false, "Choose git.exe");
                });
                row("", nullptr, [&] {
                    static std::string found, version;
                    static std::string foundFor = "\x01";
                    if (foundFor != s.gitPath) {
                        foundFor = s.gitPath;
                        const Git git(s.gitPath);
                        found = git.exe();
                        version = git.available() ? git.version() : std::string();
                    }
                    if (found.empty()) {
                        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION "  Git is not installed");
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Download")) Platform::openWithShell("https://git-scm.com/download/win");
                    } else {
                        ImGui::TextColored(theme::color::success, ICON_FA_CIRCLE_CHECK);
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", version.empty() ? found.c_str() : version.c_str());
                    }
                });
                row("New projects use Git", "Each new project starts as a git repository with a first commit.", [&] { ImGui::Checkbox("##gitNew", &s.gitInitNewProjects); });
                row("Push after committing", nullptr, [&] { ImGui::Checkbox("##gitPush", &s.gitPushAfterCommit); });
                break;
            }
            case 8: { // Updates
                row("Check for updates on start", nullptr, [&] { ImGui::Checkbox("##check", &s.checkForUpdates); });
                row("Include previews", "Pre-release versions: new features sooner, a few more bugs.", [&] { ImGui::Checkbox("##pre", &s.includePrereleases); });
                row("This version", nullptr, [&] {
                    ImGui::AlignTextToFramePadding();
                    ImGui::Text("RIFT %s%s", RIFT_VERSION_STRING, Updater::installed() ? "" : " (not installed by the setup)");
                });
                row("", nullptr, [&] {
                    ImGui::BeginDisabled(updater_.busy());
                    if (ImGui::Button(ICON_FA_CIRCLE_ARROW_UP "  Check now")) {
                        updater_.check(s.includePrereleases);
                        showUpdate_ = true;
                    }
                    ImGui::EndDisabled();
                });
                break;
            }
            default: break;
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Footer
    if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Reset to defaults")) ImGui::OpenPopup("Reset preferences?");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FILE_LINES "  Open settings file")) {
        settings_.save();
        Platform::openWithShell(Paths::toUtf8(Paths::userData() / "settings.json"));
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - buttonWidth());
    if (ImGui::Button("Close", ImVec2(buttonWidth(), 0.0f))) showPreferences_ = false;

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Reset preferences?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Every preference goes back to its default.");
        ImGui::TextDisabled("Recent projects, the window and the panel layout are kept.");
        ImGui::Spacing();
        if (ImGui::Button("Reset", ImVec2(buttonWidth(), 0.0f))) {
            settings_.resetPreferences();
            theme::setAccent(settings_.accent);
            theme::setTooltipsEnabled(settings_.showTooltips);
            glfwSwapInterval(settings_.vsync ? 1 : 0);
            settings_.save();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
    if (!showPreferences_) settings_.save();
}

// ============================================================================
//  Project Settings
// ============================================================================
void EditorApp::drawProjectSettings() {
    Editor& e = *editor_;
    if (!showProjectSettings_ || !e.hasProject()) return;
    const float fs_ = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs_ * 36.0f, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin(ICON_FA_GEAR "  Project Settings###ProjectSettings", &showProjectSettings_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) {
            scenesFolderDraft_ = e.project().settings().scenesFolder;
            scenesFolderMove_ = true;
        }
        ProjectSettings& p = projectDraft_;
        ImGui::SeparatorText("Game");
        if (beginRows("##game")) {
            row("Name", nullptr, [&] { ImGui::InputText("##name", &p.name); });
            row("Version", nullptr, [&] { ImGui::InputText("##version", &p.version); });
            row("Author", nullptr, [&] { ImGui::InputText("##author", &p.author); });
            row("Start scene", "The scene the game starts with.", [&] {
                if (ImGui::BeginCombo("##start", p.startScene.c_str())) {
                    for (const fs::path& sc : e.project().scenes()) {
                        std::error_code ec;
                        std::string rel = Paths::toUtf8(fs::relative(sc, e.project().root(), ec));
                        std::replace(rel.begin(), rel.end(), '\\', '/');
                        if (ImGui::Selectable(rel.c_str(), rel == p.startScene)) p.startScene = rel;
                    }
                    ImGui::EndCombo();
                }
            });
            row("Icon (png)", "Used for the game window and the built .exe.", [&] { ImGui::InputTextWithHint("##icon", "assets/icon.png", &p.icon); });
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Window");
        if (beginRows("##window")) {
            row("Size", nullptr, [&] {
                int size[2] = { p.windowWidth, p.windowHeight };
                if (ImGui::InputInt2("##size", size)) {
                    p.windowWidth = std::clamp(size[0], 320, 7680);
                    p.windowHeight = std::clamp(size[1], 240, 4320);
                }
            });
            row("Options", nullptr, [&] {
                ImGui::Checkbox("Fullscreen", &p.fullscreen);
                ImGui::SameLine();
                ImGui::Checkbox("Resizable", &p.resizable);
                ImGui::SameLine();
                ImGui::Checkbox("V-Sync", &p.vsync);
            });
            ImGui::EndTable();
        }
        if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save", ImVec2(buttonWidth(), 0.0f))) {
            // The scenes folder is changed with its own button below.
            p.scenesFolder = e.project().settings().scenesFolder;
            e.project().settings() = p;
            if (e.saveProjectSettings()) showProjectSettings_ = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(buttonWidth(), 0.0f))) showProjectSettings_ = false;

        // ---- where the scenes live
        ImGui::Spacing();
        ImGui::SeparatorText("Scenes folder");
        ImGui::PushTextWrapPos(fs_ * 34.0f);
        ImGui::TextDisabled("Every scene is a folder (scene.json and its scripts) inside this folder of the project. "
                            "Pick any folder inside the project, for example Levels or Content/Scenes.");
        ImGui::PopTextWrapPos();
        const fs::path root = e.project().root();
        ImGui::SetNextItemWidth(fs_ * 34.0f - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
        ImGui::InputTextWithHint("##scenes", "scenes", &scenesFolderDraft_);
        ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "##pickScenes", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
            const std::string picked = Platform::pickFolderDialog("Pick the scenes folder (inside the project)", Paths::toUtf8(root));
            if (!picked.empty()) {
                std::error_code ec;
                const fs::path rel = fs::relative(Paths::fromUtf8(picked), root, ec);
                std::string text = Paths::toUtf8(rel);
                std::replace(text.begin(), text.end(), '\\', '/');
                if (ec || text.empty() || text.rfind("..", 0) == 0) e.notify("The scenes folder must be inside the project folder", NoticeLevel::Warning);
                else scenesFolderDraft_ = text;
            }
        }
        const bool valid = Project::isValidScenesFolder(scenesFolderDraft_);
        const bool same = scenesFolderDraft_ == e.project().settings().scenesFolder;
        if (!valid && !scenesFolderDraft_.empty()) ImGui::TextColored(theme::color::error, "Use a folder inside the project (not assets, scripts or Builds)");
        else ImGui::TextDisabled("%s", Paths::toUtf8(root / Paths::fromUtf8(scenesFolderDraft_)).c_str());
        ImGui::Checkbox("Move the existing scenes there", &scenesFolderMove_);
        ImGui::BeginDisabled(!valid || same || e.playing());
        if (ImGui::Button(ICON_FA_CHECK "  Change scenes folder")) {
            if (e.changeScenesFolder(scenesFolderDraft_, scenesFolderMove_)) {
                projectDraft_.startScene = e.project().settings().startScene;
                projectDraft_.scenesFolder = e.project().settings().scenesFolder;
            }
        }
        ImGui::EndDisabled();
    }
    ImGui::End();
}

// ============================================================================
//  Updates
// ============================================================================
void EditorApp::drawUpdateDialog() {
    if (!showUpdate_) return;
    const float fs_ = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(fs_ * 34.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::Begin(ICON_FA_CIRCLE_ARROW_UP "  RIFT Updates###Updates", &showUpdate_, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    const Updater::State state = updater_.state();
    const Updater::Release rel = updater_.release();
    const float w = fs_ * 32.0f;
    ImGui::PushTextWrapPos(w);
    ImGui::TextDisabled("You have RIFT %s.", RIFT_VERSION_STRING);

    switch (state) {
        case Updater::State::Idle:
            ImGui::TextUnformatted("Look for a newer version on GitHub.");
            if (ImGui::Button(ICON_FA_CIRCLE_ARROW_UP "  Check for updates")) updater_.check(settings_.includePrereleases);
            break;
        case Updater::State::Checking:
            ImGui::TextUnformatted(ICON_FA_HOURGLASS_HALF "  Checking for updates...");
            break;
        case Updater::State::UpToDate:
            ImGui::TextColored(theme::color::success, ICON_FA_CIRCLE_CHECK "  RIFT is up to date.");
            if (!rel.version.empty()) ImGui::TextDisabled("The newest release is %s.", rel.version.c_str());
            if (ImGui::Button("Check again")) updater_.check(settings_.includePrereleases);
            break;
        case Updater::State::Failed:
            ImGui::TextColored(theme::color::error, ICON_FA_CIRCLE_XMARK "  %s", updater_.error().c_str());
            if (ImGui::Button("Try again")) updater_.check(settings_.includePrereleases);
            ImGui::SameLine();
            if (ImGui::Button("Open the downloads page")) Platform::openWithShell(std::string("https://github.com/") + RIFT_GITHUB_REPO + "/releases");
            break;
        case Updater::State::Available:
        case Updater::State::Downloading:
        case Updater::State::Ready: {
            theme::pushHeading(1.4f);
            ImGui::Text("RIFT %s%s", rel.version.c_str(), rel.prerelease ? " (preview)" : "");
            theme::pop();
            if (!rel.published.empty()) ImGui::TextDisabled("Released %s%s%s", rel.published.c_str(), rel.setupSize ? "  -  " : "", rel.setupSize ? megabytes(rel.setupSize).c_str() : "");
            if (!rel.notes.empty()) {
                ImGui::Spacing();
                ImGui::BeginChild("##notes", ImVec2(w, std::min(fs_ * 16.0f, fs_ * (4.0f + static_cast<float>(std::count(rel.notes.begin(), rel.notes.end(), '\n')) * 1.3f))), ImGuiChildFlags_Borders);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextUnformatted(rel.notes.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndChild();
            }
            ImGui::Spacing();
            if (state == Updater::State::Available) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::success.x * 0.75f, theme::color::success.y * 0.75f, theme::color::success.z * 0.75f, 1.0f));
                if (ImGui::Button(ICON_FA_DOWNLOAD "  Download and install")) updater_.download();
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Release page")) Platform::openWithShell(rel.page);
                ImGui::SameLine();
                if (ImGui::Button("Skip this version")) {
                    settings_.skippedVersion = rel.version;
                    settings_.save();
                    showUpdate_ = false;
                }
            } else if (state == Updater::State::Downloading) {
                char overlay[64];
                std::snprintf(overlay, sizeof(overlay), "%s / %s", megabytes(updater_.downloaded()).c_str(), megabytes(rel.setupSize).c_str());
                ImGui::ProgressBar(updater_.progress(), ImVec2(w, 0.0f), overlay);
                if (ImGui::Button("Cancel")) updater_.cancel();
            } else {
                ImGui::TextColored(theme::color::success, ICON_FA_CIRCLE_CHECK "  Downloaded.");
                ImGui::TextDisabled(Updater::installed() ? "RIFT closes, updates itself and starts again (a few seconds)."
                                                         : "This copy of RIFT was not installed by the setup, so the setup opens: install RIFT from there.");
                if (ImGui::Button(ICON_FA_ROTATE "  Install and restart")) {
                    editor_->requestDiscard([this] {
                        if (updater_.install()) quitConfirmed_ = true;
                    });
                }
            }
            break;
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

} // namespace rift
