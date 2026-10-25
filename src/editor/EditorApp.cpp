#include "editor/EditorApp.h"
#include "editor/Theme.h"
#include "audio/AudioSystem.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "render/Renderer.h"
#include "render/Texture.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui_internal.h>
#include <ImGuizmo.h>
#include <stb_image.h>
#include "IconsFontAwesome6.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace rift {

namespace fs = std::filesystem;

namespace {
    constexpr int kLayoutVersion = 4; // bump to reset everyone's panel layout after big changes

    EditorApp* appOf(GLFWwindow* w) { return static_cast<EditorApp*>(glfwGetWindowUserPointer(w)); }
}

// ============================================================================
//  Window callbacks (registered before ImGui's, which chain to them)
// ============================================================================
void EditorApp::onKey(GLFWwindow* w, int key, int, int action, int) {
    if (action == GLFW_PRESS) appOf(w)->input_.setKey(key, true);
    else if (action == GLFW_RELEASE) appOf(w)->input_.setKey(key, false);
}
void EditorApp::onMouseButton(GLFWwindow* w, int button, int action, int) { appOf(w)->input_.setMouseButton(button, action == GLFW_PRESS); }
void EditorApp::onCursorPos(GLFWwindow* w, double x, double y) { appOf(w)->input_.setMousePosition({ static_cast<float>(x), static_cast<float>(y) }); }
void EditorApp::onScroll(GLFWwindow* w, double, double dy) { appOf(w)->input_.addScroll(static_cast<float>(dy)); }
void EditorApp::onChar(GLFWwindow* w, unsigned int c) { appOf(w)->input_.addText(c); }
void EditorApp::onFocus(GLFWwindow* w, int focused) { if (!focused) appOf(w)->input_.releaseAll(); }
void EditorApp::onDrop(GLFWwindow* w, int count, const char** paths) {
    for (int i = 0; i < count; ++i) appOf(w)->droppedFiles_.emplace_back(paths[i]);
}
void EditorApp::onContentScale(GLFWwindow* w, float xs, float) { appOf(w)->dpiScale_ = std::max(xs, 0.5f); }

// ============================================================================
//  Start-up / shut-down
// ============================================================================
EditorApp::EditorApp() = default;

EditorApp::~EditorApp() { shutdown(); }

bool EditorApp::init() {
    settings_.load();
    if (settings_.layoutVersion != kLayoutVersion) resetLayout_ = true;

    glfwSetErrorCallback([](int code, const char* text) { LOG_ERROR("GLFW", std::to_string(code) + ": " + text); });
    if (!glfwInit()) throw std::runtime_error("Could not initialise GLFW (no display?)");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_MAXIMIZED, settings_.windowMaximized ? GLFW_TRUE : GLFW_FALSE);
    window_ = glfwCreateWindow(settings_.windowWidth, settings_.windowHeight, "RIFT", nullptr, nullptr);
    if (!window_) throw std::runtime_error("Could not create the window. RIFT needs OpenGL 3.3 (update the graphics driver).");
    // Restore the position if it is still on a monitor.
    if (settings_.windowX >= 0 && settings_.windowY >= 0 && !settings_.windowMaximized) {
        int count = 0;
        GLFWmonitor** monitors = glfwGetMonitors(&count);
        for (int i = 0; i < count; ++i) {
            int mx, my, mw, mh;
            glfwGetMonitorWorkarea(monitors[i], &mx, &my, &mw, &mh);
            if (settings_.windowX >= mx && settings_.windowY >= my && settings_.windowX < mx + mw - 100 && settings_.windowY < my + mh - 100) {
                glfwSetWindowPos(window_, settings_.windowX, settings_.windowY);
                break;
            }
        }
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(settings_.vsync ? 1 : 0);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) throw std::runtime_error("Could not load OpenGL functions (GLEW)");
    Platform::setMainWindow(window_);

    // Window icon
    {
        GLFWimage icon{};
        int channels = 0;
        const std::string path = Paths::toUtf8(Paths::icon("rift.png"));
        stbi_set_flip_vertically_on_load(false); // textures turn this on for OpenGL
        icon.pixels = stbi_load(path.c_str(), &icon.width, &icon.height, &channels, 4);
        if (icon.pixels) {
            glfwSetWindowIcon(window_, 1, &icon);
            stbi_image_free(icon.pixels);
        }
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, onKey);
    glfwSetMouseButtonCallback(window_, onMouseButton);
    glfwSetCursorPosCallback(window_, onCursorPos);
    glfwSetScrollCallback(window_, onScroll);
    glfwSetCharCallback(window_, onChar);
    glfwSetWindowFocusCallback(window_, onFocus);
    glfwSetDropCallback(window_, onDrop);
    glfwSetWindowContentScaleCallback(window_, onContentScale);
    float xs = 1.0f, ys = 1.0f;
    glfwGetWindowContentScale(window_, &xs, &ys);
    dpiScale_ = std::max(xs, 0.5f);

    // Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.ConfigDragClickToInputText = true;
    iniPath_ = Paths::toUtf8(Paths::userData() / "imgui.ini");
    io.IniFilename = iniPath_.c_str();
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    theme::setAccent(settings_.accent);
    theme::setTooltipsEnabled(settings_.showTooltips);
    theme::apply(dpiScale_ * settings_.uiScale);
    appliedScale_ = dpiScale_ * settings_.uiScale;

    // Engine
    renderer_ = std::make_unique<Renderer>();
    std::string error;
    if (!renderer_->init(&error)) throw std::runtime_error("The renderer could not start: " + error);
    audio_ = AudioSystem::create();
    if (!audio_) LOG_WARN("Editor", "No audio device; sounds are disabled");
    editor_ = std::make_unique<Editor>(settings_, input_, audio_.get());
    logo_ = std::make_unique<Texture>(Paths::toUtf8(Paths::icon("rift.png")));
    if (settings_.discordPresence) presence_.init(settings_.discordClientId);
    if (settings_.checkForUpdates) updater_.check(settings_.includePrereleases);
    // Files the last update could not replace while RIFT was running.
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(Paths::exeDir(), ec)) {
        if (item.path().extension() == ".old") fs::remove(item.path(), ec);
    }

    glfwShowWindow(window_);
    return true;
}

void EditorApp::shutdown() {
    if (!window_) return;
    // Remember the window for next time.
    settings_.windowMaximized = glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
    if (!settings_.windowMaximized && !glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
        glfwGetWindowPos(window_, &settings_.windowX, &settings_.windowY);
        glfwGetWindowSize(window_, &settings_.windowWidth, &settings_.windowHeight);
    }
    settings_.lastProject = editor_ && editor_->hasProject() ? Paths::toUtf8(editor_->project().root()) : std::string();
    settings_.save();

    presence_.shutdown();
    scripts_.closeAll();
    editor_.reset();
    logo_.reset();
    renderer_.reset();
    audio_.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
}

void EditorApp::openStartup(const std::string& path) {
    std::error_code ec;
    if (!path.empty()) {
        const fs::path p = Paths::fromUtf8(path);
        if (fs::is_directory(p, ec)) { editor_->openProject(p); return; }
        if (p.filename() == "project.json") { editor_->openProject(p.parent_path()); return; }
        if (p.extension() == ".json") {
            const fs::path root = Project::findRoot(p);
            if (!root.empty() && editor_->openProject(root)) editor_->openScene(p);
            else editor_->openScene(p);
            return;
        }
        editor_->notify("Cannot open " + path + " (expected a project folder or a scene)", NoticeLevel::Warning);
    }
    if (settings_.reopenLastProject && !settings_.lastProject.empty() && fs::exists(Paths::fromUtf8(settings_.lastProject) / "project.json", ec))
        editor_->openProject(Paths::fromUtf8(settings_.lastProject));
}

int EditorApp::run(const std::string& startupPath) {
    init();
    openStartup(startupPath);
    double last = glfwGetTime();
    while (!quitConfirmed_) {
        input_.newFrame();
        glfwPollEvents();
        if (glfwWindowShouldClose(window_)) {
            glfwSetWindowShouldClose(window_, GLFW_FALSE);
            requestQuit();
        }
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            last = glfwGetTime();
            continue;
        }
        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(now - last, 0.1));
        last = now;
        frame(dt);

        // Frame pacing: the limit from Preferences (without V-Sync), and a slow
        // pace while another window is active (never while the game runs).
        double target = 0.0;
        if (!settings_.vsync && settings_.fpsLimit > 0) target = 1.0 / settings_.fpsLimit;
        const bool background = !glfwGetWindowAttrib(window_, GLFW_FOCUSED) && !editor_->playing() && !updater_.busy();
        const double spent = glfwGetTime() - now;
        if (settings_.throttleInBackground && background && spent < 1.0 / 15.0) glfwWaitEventsTimeout(1.0 / 15.0 - spent);
        else if (target > 0.0 && spent < target) std::this_thread::sleep_for(std::chrono::duration<double>(target - spent));
    }
    shutdown();
    return 0;
}

void EditorApp::requestQuit() {
    if (editor_->playing()) editor_->stop();
    editor_->requestDiscard([this] { quitConfirmed_ = true; });
}

// ============================================================================
//  Frame
// ============================================================================
void EditorApp::frame(float dt) {
    const float scale = dpiScale_ * settings_.uiScale;
    if (std::abs(scale - appliedScale_) > 0.001f) {
        theme::apply(scale);
        appliedScale_ = scale;
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    handleShortcuts();
    handleDroppedFiles();
    editor_->update(dt);
    // A newer RIFT was found: say so once (unless the user skipped that version).
    if (updater_.takeNewsFlag() && settings_.skippedVersion != updater_.release().version)
        editor_->notify("RIFT " + updater_.release().version + " is available: Help > Check for Updates", NoticeLevel::Info);
    if (GameSession* s = editor_->session()) s->update(dt, viewport_.gameInput(*editor_));

    UiContext ui{ *editor_, *renderer_, window_, dt };
    drawMenuBar();
    if (editor_->hasProject()) drawToolbar();
    drawStatusBar();

    // Dock space under the menu and toolbar.
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiID dockspace = ImGui::GetID("RiftDockSpace");
    if (resetLayout_ || !ImGui::DockBuilderGetNode(dockspace)) {
        buildDefaultLayout(dockspace);
        resetLayout_ = false;
        settings_.layoutVersion = kLayoutVersion;
    }
    const bool project = editor_->hasProject();
    if (project) ImGui::DockSpaceOverViewport(dockspace, vp);

    if (project) {
        viewport_.draw(ui);
        // Script tabs dock next to the viewport.
        ImGuiID center = 0;
        if (ImGuiWindow* w = ImGui::FindWindowByName("###Viewport")) center = w->DockId;
        scripts_.draw(ui, center);
        if (settings_.showExplorer) explorer_.draw(ui, &settings_.showExplorer);
        if (settings_.showProperties) properties_.draw(ui, &settings_.showProperties);
        if (settings_.showOutput) output_.draw(ui, &settings_.showOutput);
        if (settings_.showAssets) assets_.draw(ui, &settings_.showAssets);
        if (settings_.showSourceControl) {
            // First shown (no saved layout for it): a tab next to the Explorer.
            if (ImGuiWindow* w = ImGui::FindWindowByName("###Explorer")) ImGui::SetNextWindowDockID(w->DockId, ImGuiCond_FirstUseEver);
            sourceControl_.draw(ui, &settings_.showSourceControl);
        }
        if (focusExplorer_) {
            ImGui::SetWindowFocus("###Explorer");
            ImGui::SetWindowFocus("###Viewport");
            focusExplorer_ = false;
        }
    } else {
        drawWelcome(dockspace);
    }
    drawDialogs();
    drawNotices();
    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);

    // A property dragged in a popup (colour picker) ends without the widget
    // reporting it: commit as soon as nothing is being edited.
    if (editor_->previewing() && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) editor_->commitPreview();

    ImGui::Render();
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_SCISSOR_TEST);
    const ImVec4 bg = theme::color::bgDark;
    glClearColor(bg.x, bg.y, bg.z, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);

    updateTitle();
    updatePresence(dt);
}

void EditorApp::buildDefaultLayout(ImGuiID dockspace) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockspace);
    ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspace, vp->WorkSize);
    ImGuiID center = dockspace;
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.23f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.56f, nullptr, &right);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.27f, nullptr, &center);
    ImGui::DockBuilderDockWindow("###Viewport", center);
    ImGui::DockBuilderDockWindow("###Explorer", right);
    ImGui::DockBuilderDockWindow("###SourceControl", right);
    ImGui::DockBuilderDockWindow("###Properties", rightBottom);
    ImGui::DockBuilderDockWindow("###Output", bottom);
    ImGui::DockBuilderDockWindow("###Assets", bottom);
    ImGui::DockBuilderFinish(dockspace);
    settings_.showExplorer = settings_.showProperties = settings_.showOutput = settings_.showAssets = settings_.showSourceControl = true;
    focusExplorer_ = true; // in front of Source Control once both exist
}

// ============================================================================
//  Shortcuts
// ============================================================================
void EditorApp::handleShortcuts() {
    const ImGuiIO& io = ImGui::GetIO();
    Editor& e = *editor_;
    auto key = [](ImGuiKeyChord chord) { return ImGui::IsKeyChordPressed(chord); };

    // Play controls work everywhere (even while the game has the keyboard).
    if (e.hasScene()) {
        if (key(ImGuiKey_F5) && !e.playing() && !ImGui::GetIO().KeyCtrl) play();
        if (key(ImGuiMod_Shift | ImGuiKey_F5) && e.playing()) e.stop();
        if (key(ImGuiKey_F6) && e.playing()) e.session()->setPaused(!e.session()->paused());
    }
    if (io.WantTextInput || scripts_.anyFocused() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) return;
    // While the game runs in a focused viewport, keys belong to the game.
    if (e.playing() && viewport_.focused()) return;

    if (key(ImGuiMod_Ctrl | ImGuiKey_O)) openProjectDialog();
    if (key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_N)) openNewProject();
    if (key(ImGuiMod_Ctrl | ImGuiKey_Comma)) showPreferences_ = true;
    if (key(ImGuiMod_Ctrl | ImGuiKey_B) && e.hasProject() && !e.playing()) openBuildDialog();
    if (key(ImGuiMod_Ctrl | ImGuiKey_F5) && e.hasProject() && !e.playing()) runInPlayer();
    if (!e.hasScene()) return;

    if (key(ImGuiMod_Ctrl | ImGuiKey_S)) e.saveScene();
    if (key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S)) openSceneNameDialog(true);
    if (key(ImGuiMod_Ctrl | ImGuiKey_N)) openSceneNameDialog(false);
    if (key(ImGuiMod_Ctrl | ImGuiKey_Z)) e.undo();
    if (key(ImGuiMod_Ctrl | ImGuiKey_Y) || key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) e.redo();
    if (key(ImGuiMod_Ctrl | ImGuiKey_C)) e.copySelection();
    if (key(ImGuiMod_Ctrl | ImGuiKey_X)) e.cutSelection();
    if (key(ImGuiMod_Ctrl | ImGuiKey_V)) e.paste();
    if (key(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V)) e.paste(true);
    if (key(ImGuiMod_Ctrl | ImGuiKey_D)) e.duplicateSelection();
    if (key(ImGuiKey_Delete)) e.requestDelete();
    if (key(ImGuiMod_Ctrl | ImGuiKey_A)) e.selectAll();
    if (key(ImGuiMod_Ctrl | ImGuiKey_G)) e.groupSelection();
    if (key(ImGuiMod_Ctrl | ImGuiKey_U)) e.ungroupSelection();
    if (key(ImGuiKey_F2) && e.primary()) {
        settings_.showExplorer = true;
        explorer_.beginRename(e.primary());
    }
    if (key(ImGuiKey_F) && !io.KeyCtrl && !io.KeyAlt && !viewport_.capturingMouse()) e.focusSelection();
    if (key(ImGuiKey_Escape) && !e.playing()) e.clearSelection();
    if (key(ImGuiMod_Ctrl | ImGuiKey_1)) e.tool = Tool::Select;
    if (key(ImGuiMod_Ctrl | ImGuiKey_2)) e.tool = Tool::Move;
    if (key(ImGuiMod_Ctrl | ImGuiKey_3)) e.tool = Tool::Scale;
    if (key(ImGuiMod_Ctrl | ImGuiKey_4)) e.tool = Tool::Rotate;
    if (key(ImGuiMod_Ctrl | ImGuiKey_L)) settings_.localSpace = !settings_.localSpace;
}

void EditorApp::handleDroppedFiles() {
    if (droppedFiles_.empty()) return;
    std::vector<std::string> files;
    files.swap(droppedFiles_);
    std::error_code ec;
    // A project folder (or its project.json) opens it.
    for (const std::string& f : files) {
        const fs::path p = Paths::fromUtf8(f);
        const fs::path folder = p.filename() == "project.json" ? p.parent_path() : p;
        if (fs::exists(folder / "project.json", ec)) {
            editor_->requestDiscard([this, folder] { editor_->openProject(folder); });
            return;
        }
    }
    if (!editor_->hasProject()) {
        editor_->notify("Open a project first, then drop files to import them", NoticeLevel::Info);
        return;
    }
    // Anything else is imported into assets/<kind>.
    int imported = 0;
    for (const std::string& f : files) {
        const fs::path from = Paths::fromUtf8(f);
        const AssetKind kind = assetKind(from);
        if (kind == AssetKind::Folder) continue;
        const char* sub = kind == AssetKind::Image ? "textures" : kind == AssetKind::Sound ? "sounds" : kind == AssetKind::Model ? "models" : "";
        const fs::path dir = editor_->project().assetsFolder() / sub;
        fs::create_directories(dir, ec);
        fs::path to = dir / from.filename();
        for (int n = 2; fs::exists(to, ec); ++n) to = dir / (from.stem().wstring() + L"_" + std::to_wstring(n) + from.extension().wstring());
        if (fs::copy_file(from, to, ec)) ++imported;
    }
    if (imported) {
        editor_->notify("Imported " + std::to_string(imported) + " file(s) into assets", NoticeLevel::Success);
        settings_.showAssets = true;
        assets_.refresh();
    }
}

void EditorApp::play() {
    if (!editor_->hasScene() || editor_->playing()) return;
    editor_->play();
    viewport_.focus();
}

// ============================================================================
//  Menu bar, toolbar, status bar
// ============================================================================
void EditorApp::drawMenuBar() {
    Editor& e = *editor_;
    if (!ImGui::BeginMainMenuBar()) return;
    // Logo
    if (logo_ && logo_->isValid()) {
        const float s = ImGui::GetFrameHeight() * 0.8f;
        ImGui::Image(static_cast<ImTextureID>(logo_->id()), ImVec2(s, s), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine();
    }
    const bool scene = e.hasScene();
    const bool editing = scene && !e.playing();

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Project...", "Ctrl+Shift+N")) openNewProject();
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open Project...", "Ctrl+O")) openProjectDialog();
        if (ImGui::BeginMenu(ICON_FA_CLOCK_ROTATE_LEFT "  Recent Projects", !settings_.recentProjects.empty())) {
            for (const std::string& path : std::vector<std::string>(settings_.recentProjects)) {
                if (ImGui::MenuItem(path.c_str())) {
                    const fs::path p = Paths::fromUtf8(path);
                    e.requestDiscard([&e, p] { e.openProject(p); });
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_XMARK "  Close Project", nullptr, false, e.hasProject())) e.requestDiscard([&e] { e.closeProject(); });
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FILE_CIRCLE_PLUS "  New Scene...", "Ctrl+N", false, e.hasProject() && !e.playing())) openSceneNameDialog(false);
        if (ImGui::BeginMenu(ICON_FA_MAP "  Open Scene", e.hasProject() && !e.playing())) {
            for (const fs::path& s : e.project().scenes()) {
                const std::string name = Paths::toUtf8(s.parent_path().filename());
                std::error_code ec;
                const bool current = fs::equivalent(s, e.scenePath(), ec);
                if (ImGui::MenuItem(name.c_str(), nullptr, current)) e.requestDiscard([&e, s] { e.openScene(s); });
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_FLOPPY_DISK "  Save Scene", "Ctrl+S", false, editing)) e.saveScene();
        if (ImGui::MenuItem(ICON_FA_COPY "  Save Scene As...", "Ctrl+Shift+S", false, editing)) openSceneNameDialog(true);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_GEAR "  Project Settings...", nullptr, false, e.hasProject())) {
            projectDraft_ = e.project().settings();
            showProjectSettings_ = true;
        }
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show Project Folder", nullptr, false, e.hasProject())) Platform::openWithShell(Paths::toUtf8(e.project().root()));
        if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Build Game...", "Ctrl+B", false, e.hasProject() && !e.playing())) openBuildDialog();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET "  Exit", "Alt+F4")) requestQuit();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit", scene)) {
        const std::string undo = e.history().canUndo() ? "Undo " + e.history().undoLabel() : std::string("Undo");
        const std::string redo = e.history().canRedo() ? "Redo " + e.history().redoLabel() : std::string("Redo");
        if (ImGui::MenuItem((ICON_FA_ROTATE_LEFT "  " + undo).c_str(), "Ctrl+Z", false, editing && e.history().canUndo())) e.undo();
        if (ImGui::MenuItem((ICON_FA_ROTATE_RIGHT "  " + redo).c_str(), "Ctrl+Y", false, editing && e.history().canRedo())) e.redo();
        ImGui::Separator();
        const bool any = !e.selection().empty();
        if (ImGui::MenuItem(ICON_FA_SCISSORS "  Cut", "Ctrl+X", false, any)) e.cutSelection();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy", "Ctrl+C", false, any)) e.copySelection();
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste", "Ctrl+V", false, e.canPaste())) e.paste();
        if (ImGui::MenuItem(ICON_FA_PASTE "  Paste Into", "Ctrl+Shift+V", false, e.canPaste() && any)) e.paste(true);
        if (ImGui::MenuItem(ICON_FA_CLONE "  Duplicate", "Ctrl+D", false, any)) e.duplicateSelection();
        if (ImGui::MenuItem(ICON_FA_TRASH_CAN "  Delete", "Del", false, any)) e.requestDelete();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Group", "Ctrl+G", false, any)) e.groupSelection();
        if (ImGui::MenuItem(ICON_FA_OBJECT_UNGROUP "  Ungroup", "Ctrl+U", false, any)) e.ungroupSelection();
        if (ImGui::MenuItem(ICON_FA_OBJECT_GROUP "  Select All", "Ctrl+A")) e.selectAll();
        if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename", "F2", false, any)) explorer_.beginRename(e.primary());
        if (ImGui::MenuItem(ICON_FA_EXPAND "  Zoom To Selection", "F", false, any)) e.focusSelection();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_SLIDERS "  Preferences...", "Ctrl+,")) showPreferences_ = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem(ICON_FA_SITEMAP "  Explorer", nullptr, &settings_.showExplorer);
        ImGui::MenuItem(ICON_FA_SLIDERS "  Properties", nullptr, &settings_.showProperties);
        ImGui::MenuItem(ICON_FA_TERMINAL "  Output", nullptr, &settings_.showOutput);
        ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Assets", nullptr, &settings_.showAssets);
        ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Source Control", nullptr, &settings_.showSourceControl);
        ImGui::Separator();
        ImGui::MenuItem(ICON_FA_TABLE_CELLS "  Grid", nullptr, &settings_.showGrid);
        ImGui::MenuItem(ICON_FA_DESKTOP "  GUI While Editing", nullptr, &settings_.showGui);
        ImGui::MenuItem(ICON_FA_CIRCLE_HALF_STROKE "  Shadows", nullptr, &settings_.shadows);
        ImGui::MenuItem(ICON_FA_DRAW_POLYGON "  Wireframe", nullptr, &settings_.wireframe);
        ImGui::MenuItem(ICON_FA_CHART_SIMPLE "  Statistics", nullptr, &settings_.showStats);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TABLE_COLUMNS "  Reset Layout")) resetLayout_ = true;
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Insert", editing)) {
        drawInsertMenuItems(e);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Game", scene)) {
        if (ImGui::MenuItem(ICON_FA_PLAY "  Play", "F5", false, !e.playing())) play();
        if (ImGui::MenuItem(ICON_FA_PAUSE "  Pause", "F6", e.playing() && e.session()->paused(), e.playing()) && e.playing()) e.session()->setPaused(!e.session()->paused());
        if (ImGui::MenuItem(ICON_FA_STOP "  Stop", "Shift+F5", false, e.playing())) e.stop();
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_UP_RIGHT_FROM_SQUARE "  Run in Player", "Ctrl+F5", false, !e.playing())) runInPlayer();
        theme::tooltip("Saves, then runs the project in the standalone player, the way players will see it.");
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem(ICON_FA_BOOK_OPEN "  Editor Guide")) Platform::openWithShell(std::string(RIFT_DOCS_URL) + "GUIDE.md");
        if (ImGui::MenuItem(ICON_FA_BOOK "  Scripting Reference")) Platform::openWithShell(std::string(RIFT_DOCS_URL) + "SCRIPTING.md");
        if (ImGui::MenuItem(ICON_FA_KEYBOARD "  Keyboard Shortcuts")) showShortcuts_ = true;
        ImGui::Separator();
        const bool update = updater_.state() == Updater::State::Available || updater_.state() == Updater::State::Ready;
        if (ImGui::MenuItem(update ? ICON_FA_GIFT "  Update Available..." : ICON_FA_CIRCLE_ARROW_UP "  Check for Updates...")) {
            showUpdate_ = true;
            if (!update && !updater_.busy()) updater_.check(settings_.includePrereleases);
        }
        if (ImGui::MenuItem(ICON_FA_GLOBE "  RIFT Website")) Platform::openWithShell(RIFT_WEBSITE);
        if (ImGui::MenuItem(ICON_FA_BUG "  Report a Problem")) Platform::openWithShell(std::string("https://github.com/") + RIFT_GITHUB_REPO + "/issues");
        if (ImGui::MenuItem(ICON_FA_FILE_LINES "  Open Log File")) Platform::revealInExplorer(Log::filePath());
        ImGui::Separator();
        ImGui::MenuItem(ICON_FA_FLASK "  ImGui Demo", nullptr, &showImGuiDemo_);
        if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About RIFT")) showAbout_ = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

void EditorApp::drawToolbar() {
    Editor& e = *editor_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float button = ImGui::GetFrameHeight() * 1.15f;
    const float height = button + style.WindowPadding.y * 1.1f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, (height - button) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color::bg);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDecoration;
    const bool open = ImGui::BeginViewportSideBar("##toolbar", vp, ImGuiDir_Up, height, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    const bool scene = e.hasScene();
    EditorSettings& s = settings_;
    auto separator = [&] {
        ImGui::SameLine(0.0f, style.ItemSpacing.x * 1.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 4.0f), ImVec2(p.x, p.y + button - 4.0f), theme::u32(theme::color::border), 1.0f);
        ImGui::Dummy(ImVec2(1.0f, button));
        ImGui::SameLine(0.0f, style.ItemSpacing.x * 1.5f);
    };

    ImGui::BeginDisabled(!scene);
    // Tools (Roblox order and shortcuts)
    if (theme::iconButton(ICON_FA_ARROW_POINTER, "Select (Ctrl+1)", e.tool == Tool::Select, button)) e.tool = Tool::Select;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_UP_DOWN_LEFT_RIGHT, "Move (Ctrl+2)", e.tool == Tool::Move, button)) e.tool = Tool::Move;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_UP_RIGHT_AND_DOWN_LEFT_FROM_CENTER, "Scale: drag a face of the part (Ctrl+3)", e.tool == Tool::Scale, button)) e.tool = Tool::Scale;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_ROTATE, "Rotate (Ctrl+4)", e.tool == Tool::Rotate, button)) e.tool = Tool::Rotate;
    separator();
    if (theme::iconButton(s.localSpace ? ICON_FA_CUBE : ICON_FA_GLOBE, s.localSpace ? "Local axes (Ctrl+L to switch to world)" : "World axes (Ctrl+L to switch to local)", false, button))
        s.localSpace = !s.localSpace;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_MAGNET, s.snap ? "Snapping on (hold Ctrl while dragging to ignore)" : "Snapping off", s.snap, button)) s.snap = !s.snap;
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.snap);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (button - ImGui::GetFrameHeight()) * 0.5f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.2f);
    ImGui::DragFloat("##snapMove", &s.snapMove, 0.05f, 0.01f, 64.0f, ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT " %.2g");
    theme::tooltip("Move step (studs)");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.2f);
    ImGui::DragFloat("##snapRotate", &s.snapRotate, 0.5f, 1.0f, 180.0f, ICON_FA_ROTATE " %.0f");
    theme::tooltip("Rotate step (degrees)");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.2f);
    ImGui::DragFloat("##snapScale", &s.snapScale, 0.05f, 0.01f, 64.0f, ICON_FA_EXPAND " %.2g");
    theme::tooltip("Resize step (studs)");
    ImGui::EndDisabled();
    separator();
    // Quick insert
    if (theme::iconButton(ICON_FA_CUBE, "Insert a part", false, button) && !e.playing()) e.insert(ClassId::Part);
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_FILE_CODE, "Insert a script (inside the selected object)", false, button) && !e.playing()) e.insert(ClassId::Script);
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_CARET_DOWN, "More objects", false, button)) ImGui::OpenPopup("##toolbarInsert");
    if (ImGui::BeginPopup("##toolbarInsert")) {
        drawInsertMenuItems(e);
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();

    // Play controls in the middle
    float playWidth = button * 3.0f + ImGui::CalcTextSize("Play").x + style.FramePadding.x * 4.0f;
    if (e.playing()) playWidth += ImGui::CalcTextSize(ICON_FA_LAPTOP "  Client" ICON_FA_SERVER "  Server").x + style.FramePadding.x * 4.0f + style.ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + style.ItemSpacing.x, (ImGui::GetWindowWidth() - playWidth) * 0.5f));
    if (!e.playing()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.55f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.66f, 0.42f, 1.0f));
        ImGui::BeginDisabled(!scene);
        if (ImGui::Button(ICON_FA_PLAY "  Play", ImVec2(0.0f, button))) play();
        ImGui::EndDisabled();
        ImGui::PopStyleColor(2);
        theme::tooltip("Play the scene (F5)");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.22f, 0.24f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.82f, 0.28f, 0.30f, 1.0f));
        if (ImGui::Button(ICON_FA_STOP "  Stop", ImVec2(0.0f, button))) e.stop();
        ImGui::PopStyleColor(2);
        theme::tooltip("Stop (Shift+F5). Changes made while playing are thrown away.");
        // The Stop button above may have ended the session this very frame.
        if (GameSession* session = e.session()) {
            ImGui::SameLine(0.0f, 2.0f);
            const bool paused = session->paused();
            if (theme::iconButton(paused ? ICON_FA_PLAY : ICON_FA_PAUSE, paused ? "Resume (F6)" : "Pause (F6)", paused, button)) session->setPaused(!paused);
            ImGui::SameLine();
            drawSideSwitch(e, button);
        }
    }

    // Scene name on the right
    const std::string title = e.project().settings().name + "  " ICON_FA_CHEVRON_RIGHT "  " + e.sceneName() + (e.isDirty() ? "  " ICON_FA_CIRCLE : "");
    const float w = ImGui::CalcTextSize(title.c_str()).x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + style.ItemSpacing.x, ImGui::GetWindowWidth() - w - style.WindowPadding.x * 1.5f));
    ImGui::SetCursorPosY((height - ImGui::GetTextLineHeight()) * 0.5f);
    ImGui::TextDisabled("%s", title.c_str());
    if (e.isDirty()) theme::tooltip("Unsaved changes (Ctrl+S)");
    ImGui::End();
}

void EditorApp::drawStatusBar() {
    Editor& e = *editor_;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float height = ImGui::GetTextLineHeight() + ImGui::GetStyle().FramePadding.y * 2.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ImGui::GetStyle().WindowPadding.x, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, e.playing() ? ImVec4(0.11f, 0.30f, 0.20f, 1.0f) : theme::color::bgDark);
    const bool open = ImGui::BeginViewportSideBar("##statusbar", vp, ImGuiDir_Down, height, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (!open) {
        ImGui::End();
        return;
    }
    // Left: state and the latest message
    if (e.playing()) {
        ImGui::TextColored(theme::color::success, e.session()->paused() ? ICON_FA_PAUSE "  Paused" : ICON_FA_PLAY "  Playing");
        ImGui::SameLine();
    }
    if (!e.notices().empty()) ImGui::TextDisabled("%s", e.notices().back().text.c_str());
    else if (!e.hasProject()) ImGui::TextDisabled("Ready");
    else {
        const size_t n = e.selection().size();
        if (n == 1) {
            if (const Instance* i = e.scene().find(e.primary())) ImGui::TextDisabled("%s  %s (%s)", theme::classIcon(i->cls), i->name.c_str(), className(i->cls));
        } else if (n > 1) {
            ImGui::TextDisabled("%d objects selected", static_cast<int>(n));
        } else {
            ImGui::TextDisabled("Ready");
        }
    }
    // Right: update badge, counts and version
    char right[128];
    std::snprintf(right, sizeof(right), "%d objects   RIFT %s", e.hasScene() ? static_cast<int>(e.scene().size()) : 0, RIFT_VERSION_STRING);
    const bool update = updater_.state() == Updater::State::Available || updater_.state() == Updater::State::Ready;
    const std::string badge = update ? ICON_FA_GIFT "  Update to " + updater_.release().version : std::string();
    const float badgeWidth = update ? ImGui::CalcTextSize(badge.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f : 0.0f;
    const float w = ImGui::CalcTextSize(right).x + badgeWidth;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - w - ImGui::GetStyle().WindowPadding.x));
    if (update) {
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::success);
        if (ImGui::Selectable(badge.c_str(), false, 0, ImVec2(ImGui::CalcTextSize(badge.c_str()).x, 0.0f))) showUpdate_ = true;
        ImGui::PopStyleColor();
        theme::tooltip("A newer RIFT is available. Click to see what's new.");
        ImGui::SameLine();
    }
    ImGui::TextDisabled("%s", right);
    ImGui::End();
}

// ============================================================================
//  Toasts
// ============================================================================
void EditorApp::drawNotices() {
    auto& notices = editor_->notices();
    if (notices.empty()) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList(vp);
    const float pad = ImGui::GetFontSize() * 0.8f;
    const float maxWidth = ImGui::GetFontSize() * 26.0f;
    float y = vp->WorkPos.y + vp->WorkSize.y - pad;
    for (auto it = notices.rbegin(); it != notices.rend(); ++it) {
        const Notice& n = *it;
        const float life = 6.0f;
        const float alpha = std::clamp(std::min(n.time * 6.0f, (life - n.time) * 2.0f), 0.0f, 1.0f);
        if (alpha <= 0.0f) continue;
        ImVec4 color = theme::color::accentHover;
        const char* icon = ICON_FA_CIRCLE_INFO;
        if (n.level == NoticeLevel::Success) { color = theme::color::success; icon = ICON_FA_CIRCLE_CHECK; }
        if (n.level == NoticeLevel::Warning) { color = theme::color::warning; icon = ICON_FA_TRIANGLE_EXCLAMATION; }
        if (n.level == NoticeLevel::Error) { color = theme::color::error; icon = ICON_FA_CIRCLE_XMARK; }
        const float iconWidth = ImGui::GetFontSize() * 1.6f;
        const ImVec2 t = ImGui::GetFont()->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, maxWidth, n.text.c_str());
        const ImVec2 size(t.x + iconWidth + pad * 2.0f, t.y + pad * 1.4f);
        const ImVec2 a(vp->WorkPos.x + vp->WorkSize.x - size.x - pad, y - size.y);
        const ImVec2 b(a.x + size.x, a.y + size.y);
        draw->AddRectFilled(ImVec2(a.x + 2, a.y + 3), ImVec2(b.x + 2, b.y + 3), IM_COL32(0, 0, 0, static_cast<int>(70 * alpha)), 8.0f);
        draw->AddRectFilled(a, b, theme::u32(ImVec4(0.14f, 0.145f, 0.17f, 0.97f), alpha), 8.0f);
        draw->AddRectFilled(a, ImVec2(a.x + 3.0f, b.y), theme::u32(color, alpha), 8.0f, ImDrawFlags_RoundCornersLeft);
        draw->AddText(ImVec2(a.x + pad, a.y + pad * 0.7f), theme::u32(color, alpha), icon);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(a.x + pad + iconWidth, a.y + pad * 0.7f), theme::u32(theme::color::text, alpha), n.text.c_str(), nullptr, maxWidth);
        y = a.y - pad * 0.5f;
    }
}

// ============================================================================
//  Title and Discord
// ============================================================================
void EditorApp::updateTitle() {
    std::string title = "RIFT";
    if (editor_->hasProject()) {
        title = editor_->project().settings().name + " - " + editor_->sceneName() + (editor_->isDirty() ? " *" : "") + " - RIFT";
        if (editor_->playing()) title += " [Playing]";
    }
    if (title != lastTitle_) {
        glfwSetWindowTitle(window_, title.c_str());
        lastTitle_ = title;
    }
}

void EditorApp::updatePresence(float dt) {
    presence_.update(dt);
    presenceTimer_ -= dt;
    if (presenceTimer_ > 0.0f) return;
    presenceTimer_ = 2.0f;
    if (!editor_->hasProject()) {
        presence_.setActivity("In the project launcher", "");
        return;
    }
    const std::string game = editor_->project().settings().name;
    if (editor_->playing()) presence_.setActivity("Playtesting " + game, editor_->sceneName());
    else presence_.setActivity("Editing " + game, editor_->sceneName() + " - " + std::to_string(editor_->scene().size()) + " objects");
}

} // namespace rift
