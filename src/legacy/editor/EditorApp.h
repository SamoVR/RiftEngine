#pragma once


#include "render/Camera.h"
#include "scene/Object.h"
#include "scene/Scene.h"
#include "render/Shader.h"
#include "editor/Interface.h"
#include "render/Geometry.h"
#include "platform/InputManager.h"
#include "editor/KeybindManager.h"
#include "core/Settings.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "platform/Platform.h"
#include "game/GameRuntime.h"
#include "platform/RichPresence.h"

#include "stb_image.h"
#include <imgui.h>
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <memory>
#include <string>
#include <vector>

// OWNERSHIP / SHUTDOWN: every GL object (shaders, buffers, the UI's textures,
// every Object's VAO/VBO) must be released while the GL context still exists.
// The original destroyed the window first (and did so twice: run() and the
// destructor both called cleanup()), and Scene -- a member -- only freed its
// objects afterwards, i.e. after the context was gone. cleanup() is now
// idempotent and releases everything in the right order.
class EditorApp {
public:
    EditorApp();
    ~EditorApp();

    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // Runs until the window closes. Returns 0 on a clean exit; throws
    // std::runtime_error (caught in main) if start-up fails.
    // startupScene: optional scene file to open (command-line argument).
    int run(const std::string& startupScene = "");

private:
    // ---- world
    Scene scene;
    AppSettings settings;
    std::unique_ptr<Camera> camera;
    std::unique_ptr<InputManager> inputManager;
    std::unique_ptr<KeybindManager> keybindManager;
    Object* targetVisualizer = nullptr; // owned by `scene` (internal object)
    std::unique_ptr<GameRuntime> runtime;  // play mode: physics, scripts, player
    RichPresence discord;
    float presenceTimer = 0.0f;
    void updatePresence(float deltaTime);

    // ---- rendering
    std::unique_ptr<Shader> shader;
    std::unique_ptr<Shader> backgroundShader;
    std::unique_ptr<Shader> gridShader;
    std::unique_ptr<Shader> lineShader;
    GLuint lineVAO = 0, lineVBO = 0;
    GLuint gridVAO = 0, gridVBO = 0;
    GLuint fullScreenQuadVAO = 0, fullScreenQuadVBO = 0;
    int gridLineCount = 0;

    // ---- window / UI
    GLFWwindow* window = nullptr;
    std::unique_ptr<Interface> UI;
    std::string windowTitle = "RIFT";
    int width = 1600;
    int height = 800;
    double lastFrame = 0.0;
    bool glfwStarted = false;
    bool cleanedUp = false;

    // Camera drags only start in the 3D view (not on a UI panel) and keep
    // going until the button is released, even if the cursor crosses a panel.
    bool orbiting = false;
    bool panning = false;

    static void framebufferSizeCallback(GLFWwindow* window, int width, int height);
    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset);

    bool init();
    void setupDefaultKeybinds();
    void update(float deltaTime);
    void render(float deltaTime);
    void processInput();
    void cleanup();

    void initLines();
    void initGrid();
    void initFullScreenQuad();
    void drawLine(const glm::vec3& start, const glm::vec3& end, const ImVec4& color);
    void renderGrid();
    void renderBackground();
    void renderMeasurementLines(Object* object);
    void performObjectPicking(double mouseX, double mouseY);
};
