#pragma once
//
// EditorApp.h -- the RIFT editor application: the window, Dear ImGui, the
// docking layout, menus, toolbar, shortcuts, dialogs and the panels.
// Editing logic lives in Editor; this file only wires it to the screen.
//
#include "editor/Editor.h"
#include "editor/EditorSettings.h"
#include "editor/Updater.h"
#include "editor/panels/Panels.h"
#include "platform/Input.h"
#include "platform/RichPresence.h"
#include "project/Project.h"

#include <imgui.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct GLFWwindow;
class Texture;

namespace rift {

class AudioSystem;
class Renderer;

class EditorApp {
public:
    EditorApp();
    ~EditorApp();
    EditorApp(const EditorApp&) = delete;
    EditorApp& operator=(const EditorApp&) = delete;

    // Runs until the window is closed. `startupPath`: a project folder,
    // project.json or scene.json to open (empty: the last project).
    int run(const std::string& startupPath);

private:
    GLFWwindow* window_ = nullptr;
    EditorSettings settings_;
    Input input_;
    std::unique_ptr<AudioSystem> audio_;
    std::unique_ptr<Renderer> renderer_;
    std::unique_ptr<Editor> editor_;
    std::unique_ptr<Texture> logo_;
    RichPresence presence_;

    ViewportPanel viewport_;
    ExplorerPanel explorer_;
    PropertiesPanel properties_;
    OutputPanel output_;
    ScriptEditorPanel scripts_;
    AssetBrowserPanel assets_;
    SourceControlPanel sourceControl_;
    TerrainPanel terrain_;
    AnimationPanel animation_;
    FindPanel find_;
    Updater updater_;

    std::string iniPath_;
    float dpiScale_ = 1.0f;
    float appliedScale_ = 0.0f;
    bool resetLayout_ = false;
    bool focusExplorer_ = false;
    bool quitConfirmed_ = false;
    std::string lastTitle_;
    std::vector<std::string> droppedFiles_;
    float presenceTimer_ = 0.0f;

    // Home screen (no project open)
    int homePage_ = 0;                 // 0 projects, 1 learn
    char homeSearch_[128] = {};
    int homeSort_ = 0;                 // 0 last opened, 1 name
    struct Thumbnail { std::unique_ptr<Texture> texture; long long stamp = 0; };
    std::map<std::string, Thumbnail> thumbnails_; // project folder -> its .rift/thumbnail.png
    float thumbnailTimer_ = 0.0f;
    Texture* thumbnailFor(const std::string& projectFolder);

    // Dialogs
    bool showPreferences_ = false;
    int preferencesPage_ = 0;
    bool showUpdate_ = false;
    bool showExample_ = false;
    std::filesystem::path exampleSource_;
    char exampleName_[64] = {};
    std::string exampleFolder_;
    std::string scenesFolderDraft_;
    bool scenesFolderMove_ = true;
    bool showProjectSettings_ = false;
    bool showAbout_ = false;
    bool showShortcuts_ = false;
    bool showNewProject_ = false;
    bool showSceneName_ = false;
    bool sceneNameIsSaveAs_ = false;
    bool showImGuiDemo_ = false;
    bool showBuild_ = false;
    std::string buildFolder_;
    bool buildOpenFolder_ = true;
    bool buildRun_ = false;
    char sceneName_[64] = {};
    char newProjectName_[64] = "My Game";
    std::string newProjectFolder_;
    int newProjectTemplate_ = 1;
    ProjectSettings projectDraft_;

    bool init();
    void shutdown();
    void openStartup(const std::string& path);
    void frame(float dt);

    void handleShortcuts();
    void handleDroppedFiles();
    void drawMenuBar();
    void drawToolbar();
    void drawStatusBar();
    void buildDefaultLayout(ImGuiID dockspace);
    void drawWelcome(ImGuiID dockspace); // keeps the (hidden) dock layout alive
    void drawDialogs();
    void drawPreferences();
    void drawProjectSettings();
    void drawUpdateDialog();
    void drawHomeProjects(float width);
    void drawHomeLearn(float width);
    void drawNotices();
    void updateTitle();
    void updatePresence(float dt);

    void requestQuit();
    void openProjectDialog();
    void openNewProject();
    void openExample(const std::filesystem::path& folder);
    void openSceneNameDialog(bool saveAs);
    void play();
    void openBuildDialog();
    void buildGame();
    void runInPlayer();
    std::string defaultProjectsFolder() const;

    static void onKey(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void onMouseButton(GLFWwindow* w, int button, int action, int mods);
    static void onCursorPos(GLFWwindow* w, double x, double y);
    static void onScroll(GLFWwindow* w, double dx, double dy);
    static void onChar(GLFWwindow* w, unsigned int codepoint);
    static void onFocus(GLFWwindow* w, int focused);
    static void onDrop(GLFWwindow* w, int count, const char** paths);
    static void onContentScale(GLFWwindow* w, float xs, float ys);
};

} // namespace rift
