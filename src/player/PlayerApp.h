#pragma once
//
// PlayerApp.h -- RiftPlayer: runs a finished game. No editor, no panels:
// the start scene fills the window, Esc opens a small pause menu.
//
// Where the game comes from:
//   RiftPlayer.exe <project folder>   (testing from the editor)
//   RiftPlayer.exe                    looks for game/project.json next to itself
//                                     (the layout File > Build Game writes)
//
#include "game/GameSession.h"
#include "platform/Input.h"
#include "project/Project.h"
#include "render/Framebuffer.h"
#include "script/ScriptStore.h"

#include <filesystem>
#include <memory>
#include <string>

struct GLFWwindow;
struct ImFont;

namespace rift {

class AudioSystem;
class Renderer;

class PlayerApp {
public:
    PlayerApp();
    ~PlayerApp();
    int run(const std::filesystem::path& gameFolder);

private:
    GLFWwindow* window_ = nullptr;
    Input input_;
    Project project_;
    ScriptStore scripts_;
    std::unique_ptr<Scene> scene_;
    std::unique_ptr<GameSession> session_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<AudioSystem> audio_;
    Framebuffer framebuffer_;
    ImFont* font_ = nullptr;
    ImFont* bold_ = nullptr;
    bool menuOpen_ = false;
    bool mouseCaptured_ = false;
    bool fullscreen_ = false;
    int windowedPos_[2] = { 100, 100 };
    int windowedSize_[2] = { 1280, 720 };

    void initWindow();
    void loadGame();
    void restart();
    void frame(float dt);
    void drawMenu();
    void setFullscreen(bool on);

    static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow* w, int button, int action, int mods);
    static void onCursorPos(GLFWwindow* w, double x, double y);
    static void onScroll(GLFWwindow* w, double dx, double dy);
    static void onChar(GLFWwindow* w, unsigned int codepoint);
    static void onFocus(GLFWwindow* w, int focused);
};

// Finds the game: the given folder, or game/ next to the executable.
std::filesystem::path findGameFolder(const std::filesystem::path& requested);

} // namespace rift
