#include "player/PlayerApp.h"
#include "audio/AudioSystem.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "render/Renderer.h"
#include "scene/SceneIO.h"
#include "ui/GameOverlay.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <imgui.h>
#include <stb_image.h>

#include <algorithm>
#include <stdexcept>

namespace rift {

namespace fs = std::filesystem;

namespace {
    PlayerApp* appOf(GLFWwindow* w) { return static_cast<PlayerApp*>(glfwGetWindowUserPointer(w)); }

    const ImVec4 kAccent{ 0.545f, 0.361f, 0.965f, 1.0f };
}

fs::path findGameFolder(const fs::path& requested) {
    std::error_code ec;
    if (!requested.empty()) {
        if (requested.filename() == "project.json") return requested.parent_path();
        return requested;
    }
    return Paths::exeDir() / "game";
}

// ============================================================================
//  Window callbacks
// ============================================================================
void PlayerApp::onKey(GLFWwindow* w, int key, int, int action, int) {
    if (action == GLFW_PRESS) appOf(w)->input_.setKey(key, true);
    else if (action == GLFW_RELEASE) appOf(w)->input_.setKey(key, false);
}
void PlayerApp::onMouseButton(GLFWwindow* w, int button, int action, int) { appOf(w)->input_.setMouseButton(button, action == GLFW_PRESS); }
void PlayerApp::onCursorPos(GLFWwindow* w, double x, double y) { appOf(w)->input_.setMousePosition({ static_cast<float>(x), static_cast<float>(y) }); }
void PlayerApp::onScroll(GLFWwindow* w, double, double dy) { appOf(w)->input_.addScroll(static_cast<float>(dy)); }
void PlayerApp::onChar(GLFWwindow* w, unsigned int c) { appOf(w)->input_.addText(c); }
void PlayerApp::onFocus(GLFWwindow* w, int focused) {
    PlayerApp* app = appOf(w);
    app->input_.releaseAll();
    if (!focused && app->session_) app->menuOpen_ = true; // alt-tab pauses
}

// ============================================================================
//  Start-up
// ============================================================================
PlayerApp::PlayerApp() = default;

PlayerApp::~PlayerApp() {
    session_.reset();
    renderer_.reset();
    audio_.reset();
    if (window_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
    }
}

void PlayerApp::initWindow() {
    const ProjectSettings& s = project_.settings();
    if (!glfwInit()) throw std::runtime_error("Could not initialise the window system");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_RESIZABLE, s.resizable ? GLFW_TRUE : GLFW_FALSE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    window_ = glfwCreateWindow(s.windowWidth, s.windowHeight, s.name.c_str(), nullptr, nullptr);
    if (!window_) throw std::runtime_error("Could not create the game window. The game needs OpenGL 3.3 (update the graphics driver).");
    // Centre on the primary monitor.
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor()) {
        int mx, my, mw, mh;
        glfwGetMonitorWorkarea(monitor, &mx, &my, &mw, &mh);
        glfwSetWindowPos(window_, mx + std::max(0, (mw - s.windowWidth) / 2), my + std::max(0, (mh - s.windowHeight) / 2));
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(s.vsync ? 1 : 0);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) throw std::runtime_error("Could not load OpenGL functions");

    // Window icon: the game's, or RIFT's.
    std::string iconPath = s.icon.empty() ? std::string() : Paths::toUtf8(project_.root() / Paths::fromUtf8(s.icon));
    if (iconPath.empty() || !fs::exists(Paths::fromUtf8(iconPath))) iconPath = Paths::toUtf8(Paths::icon("rift.png"));
    GLFWimage icon{};
    int channels = 0;
    stbi_set_flip_vertically_on_load(false); // textures turn this on for OpenGL
        icon.pixels = stbi_load(iconPath.c_str(), &icon.width, &icon.height, &channels, 4);
    if (icon.pixels) {
        glfwSetWindowIcon(window_, 1, &icon);
        stbi_image_free(icon.pixels);
    }

    glfwSetWindowUserPointer(window_, this);
    glfwSetKeyCallback(window_, onKey);
    glfwSetMouseButtonCallback(window_, onMouseButton);
    glfwSetCursorPosCallback(window_, onCursorPos);
    glfwSetScrollCallback(window_, onScroll);
    glfwSetCharCallback(window_, onChar);
    glfwSetWindowFocusCallback(window_, onFocus);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");
    ImFontConfig config;
    config.OversampleH = 2;
    font_ = io.Fonts->AddFontFromFileTTF(Paths::toUtf8(Paths::font("Inter-Regular.ttf")).c_str(), 18.0f, &config);
    bold_ = io.Fonts->AddFontFromFileTTF(Paths::toUtf8(Paths::font("Inter-SemiBold.ttf")).c_str(), 18.0f, &config);
    if (!font_) font_ = io.Fonts->AddFontDefault();
    if (!bold_) bold_ = font_;
    io.FontDefault = font_;
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 12.0f;
    style.FrameRounding = 8.0f;
    style.FramePadding = ImVec2(14.0f, 10.0f);
    style.ItemSpacing = ImVec2(10.0f, 10.0f);
    style.WindowPadding = ImVec2(24.0f, 22.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.10f, 0.105f, 0.125f, 0.96f);
    style.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.22f, 1.0f);
    style.Colors[ImGuiCol_ButtonHovered] = kAccent;
    style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.45f, 0.29f, 0.85f, 1.0f);

    if (s.fullscreen) setFullscreen(true);
    glfwShowWindow(window_);
}

void PlayerApp::loadGame() {
    // The start scene, or the first scene there is.
    fs::path scenePath = project_.startScenePath();
    std::error_code ec;
    if (!fs::exists(scenePath, ec)) {
        const auto scenes = project_.scenes();
        if (scenes.empty()) throw std::runtime_error("The game has no scenes");
        scenePath = scenes.front();
    }
    scene_ = std::make_unique<Scene>();
    const SceneIO::LoadResult r = SceneIO::loadScene(*scene_, scenePath);
    if (!r.ok) throw std::runtime_error("Could not load " + Paths::toUtf8(scenePath) + ": " + r.error);
    for (const std::string& w : r.warnings) LOG_WARN("Player", w);
    scripts_.reset(Paths::toUtf8(scenePath.parent_path()));
    scripts_.setProjectRoot(Paths::toUtf8(project_.root()));
    restart();
}

void PlayerApp::restart() {
    session_.reset();
    if (audio_) audio_->stopAll();
    GameSession::Options options;
    options.audio = audio_.get();
    options.isStudio = false;
    options.dataStoreFolder = Paths::userData() / "datastores";
    session_ = std::make_unique<GameSession>(*scene_, scripts_, input_, options);
    session_->start();
    menuOpen_ = false;
}

void PlayerApp::setFullscreen(bool on) {
    if (on == fullscreen_) return;
    fullscreen_ = on;
    if (on) {
        glfwGetWindowPos(window_, &windowedPos_[0], &windowedPos_[1]);
        glfwGetWindowSize(window_, &windowedSize_[0], &windowedSize_[1]);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        glfwSetWindowMonitor(window_, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
    } else {
        glfwSetWindowMonitor(window_, nullptr, windowedPos_[0], windowedPos_[1], windowedSize_[0], windowedSize_[1], 0);
    }
    glfwSwapInterval(project_.settings().vsync ? 1 : 0);
}

int PlayerApp::run(const fs::path& gameFolder) {
    std::string error;
    if (!project_.load(gameFolder, &error)) throw std::runtime_error("No game found in " + Paths::toUtf8(gameFolder) + "\n" + error);
    Paths::setProjectRoot(project_.root());
    initWindow();
    renderer_ = std::make_unique<Renderer>();
    if (!renderer_->init(&error)) throw std::runtime_error("The renderer could not start: " + error);
    audio_ = AudioSystem::create();
    loadGame();

    double last = glfwGetTime();
    while (!glfwWindowShouldClose(window_)) {
        input_.newFrame();
        glfwPollEvents();
        if (glfwGetWindowAttrib(window_, GLFW_ICONIFIED)) {
            glfwWaitEventsTimeout(0.1);
            last = glfwGetTime();
            continue;
        }
        const double now = glfwGetTime();
        const float dt = static_cast<float>(std::min(now - last, 0.1));
        last = now;
        frame(dt);
    }
    return 0;
}

// ============================================================================
//  Frame
// ============================================================================
void PlayerApp::frame(float dt) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    if (input_.keyPressed(GLFW_KEY_ESCAPE)) menuOpen_ = !menuOpen_;
    if (input_.keyPressed(GLFW_KEY_F11) || (input_.alt() && input_.keyPressed(GLFW_KEY_ENTER))) setFullscreen(!fullscreen_);

    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    w = std::max(w, 1);
    h = std::max(h, 1);

    ViewportInput view;
    view.size = { static_cast<float>(w), static_cast<float>(h) };
    view.mouse = input_.mousePosition();
    view.hovered = !menuOpen_ && glfwGetWindowAttrib(window_, GLFW_FOCUSED);
    view.keyboard = !menuOpen_;
    session_->setPaused(menuOpen_);
    session_->update(dt, view);

    // The game decides when the mouse is locked (first person, right-drag to look around).
    const bool capture = !menuOpen_ && (session_->wantsMouseLock() || input_.mouseDown(GLFW_MOUSE_BUTTON_RIGHT));
    if (capture != mouseCaptured_) {
        glfwSetInputMode(window_, GLFW_CURSOR, capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        mouseCaptured_ = capture;
    }

    // 3D scene into the offscreen buffer, shown full-window behind the GUI.
    RenderCamera camera;
    const float aspect = static_cast<float>(w) / static_cast<float>(h);
    camera.view = session_->camera().view();
    camera.projection = session_->camera().projection(aspect);
    camera.position = session_->camera().position;
    RenderSettings settings;
    settings.editor = false;
    settings.grid = false;
    framebuffer_.resize(w, h, 4);
    renderer_->render(session_->scene(), camera, settings, framebuffer_);

    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddImage(static_cast<ImTextureID>(framebuffer_.texture()), ImVec2(0, 0), ImVec2(static_cast<float>(w), static_cast<float>(h)), ImVec2(0, 1), ImVec2(1, 0));
    const OverlayFonts fonts{ font_, bold_ };
    drawGameGui(*session_, renderer_->textures(), background, glm::vec2(0.0f), fonts);
    drawGameHud(*session_, background, glm::vec2(0.0f), view.size, fonts);
    if (session_->outcome() != GameSession::Outcome::None && !menuOpen_) {
        const char* hint = "Press Esc for the menu";
        const ImVec2 t = ImGui::CalcTextSize(hint);
        background->AddText(ImVec2((w - t.x) * 0.5f, h * 0.62f), IM_COL32(230, 230, 240, 220), hint);
    }
    if (menuOpen_) drawMenu();

    ImGui::Render();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
}

void PlayerApp::drawMenu() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::GetBackgroundDrawList()->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), IM_COL32(6, 6, 12, 150));
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    const float width = 300.0f;
    ImGui::PushFont(bold_, 28.0f);
    const std::string& name = project_.settings().name;
    ImGui::SetCursorPosX((width + ImGui::GetStyle().WindowPadding.x * 2.0f - ImGui::CalcTextSize(name.c_str()).x) * 0.5f);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(width, 6.0f));
    if (ImGui::Button("Resume", ImVec2(width, 0.0f))) menuOpen_ = false;
    if (ImGui::Button("Restart", ImVec2(width, 0.0f))) restart();
    if (ImGui::Button(fullscreen_ ? "Windowed" : "Fullscreen", ImVec2(width, 0.0f))) setFullscreen(!fullscreen_);
    if (ImGui::Button("Quit", ImVec2(width, 0.0f))) glfwSetWindowShouldClose(window_, GLFW_TRUE);
    ImGui::Dummy(ImVec2(width, 2.0f));
    ImGui::TextDisabled("Made with RIFT %s", RIFT_VERSION_STRING);
    ImGui::End();
}

} // namespace rift
