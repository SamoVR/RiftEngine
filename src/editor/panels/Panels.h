#pragma once
//
// Panels.h -- the editor's windows. Each panel draws itself with Dear ImGui
// and changes the scene only through Editor (so everything is undoable).
//
//   ViewportPanel      the 3D view: camera, picking, gizmos, play mode
//   ExplorerPanel      the instance tree: select, rename, drag to reparent
//   PropertiesPanel    reflected properties, attributes and tags
//   OutputPanel        engine and game log
//   ScriptEditorPanel  Lua editing, one dockable tab per script
//   AssetBrowserPanel  the project's files, drag them into the scene
//   TerrainPanel       Terrain Editor: generate, brushes, material colours
//   AnimationPanel     Animation Editor: pose a rig on a timeline, save .anim files
//   FindPanel          Find (and replace) in every script of the scene
//
#include "editor/Editor.h"
#include "game/GameSession.h"
#include "project/Git.h"
#include "render/Framebuffer.h"

#include <imgui.h>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct GLFWwindow;

namespace rift {

class Renderer;
class GuiSystem;

// What every panel can reach.
struct UiContext {
    Editor& editor;
    Renderer& renderer;
    GLFWwindow* window = nullptr;
    float dt = 0.0f;
};

// Drag and drop payload types.
inline constexpr const char* kAssetPayload = "RIFT_ASSET";        // stored asset path (UTF-8, NUL-terminated)
inline constexpr const char* kInstancesPayload = "RIFT_INSTANCES"; // uint64_t ids

// The Insert menu's items (main menu, Explorer context menu, toolbar).
void drawInsertMenuItems(Editor& editor);

// Saves the script file and opens it in the external editor from Preferences
// (or the program Windows uses for .lua files).
void openInExternalEditor(Editor& editor, const std::string& scriptPath);

// While playing: the Client / Server switch (which world the editor shows).
// `height` 0 = a normal frame height. Draws nothing while editing.
void drawSideSwitch(Editor& editor, float height = 0.0f);

// An asset file dropped onto an instance (or onto nothing): an image becomes
// the part's Texture / the GUI image, a sound becomes a Sound inside it, a
// scene opens. Undoable.
void dropAsset(Editor& editor, const std::string& storedPath, Instance* target);

// "Workspace.Part.Script:12: message" -> 12 (1-based), or -1. `chunk` is the
// script's full name (the chunk name scripts run under); empty = any.
int scriptErrorLine(const std::string& message, const std::string& chunk);

// Terrain brushes and materials by name (Terrain Editor, viewport hints).
const char* terrainBrushName(TerrainData::Brush brush);
const char* terrainBrushIcon(TerrainData::Brush brush);
const char* materialName(Material material);

// What kind of file an asset is, by extension.
enum class AssetKind { Folder, Image, Sound, Model, Script, Scene, Other };
AssetKind assetKind(const std::filesystem::path& path);
const char* assetIcon(AssetKind kind);

// ---------------------------------------------------------------------------
class ViewportPanel {
public:
    void draw(UiContext& ui);
    // The view the game sees (from the last frame's layout), for GameSession::update.
    ViewportInput gameInput(const Editor& editor) const;
    // True while the camera or the game holds the mouse (the cursor is hidden).
    bool capturingMouse() const { return capture_; }
    bool focused() const { return focused_; }
    void focus() { focusRequest_ = true; }

private:
    Framebuffer framebuffer_;
    glm::vec2 origin_{ 0.0f }, size_{ 1.0f };
    bool hovered_ = false, focused_ = false, capture_ = false, focusRequest_ = false;
    uint64_t hoveredId_ = 0;

    // Click / box selection
    bool pressing_ = false;
    glm::vec2 pressPos_{ 0.0f };
    bool marquee_ = false;

    // Gizmo drag
    bool gizmoDragging_ = false;
    std::vector<TransformCommand::Change> dragChanges_;
    std::vector<glm::mat4> dragStartWorld_;
    glm::mat4 dragPivotStart_{ 1.0f };
    glm::mat4 gizmoMatrix_{ 1.0f };

    // GUI element drag (moving a Frame / label with the mouse while editing)
    bool guiDragging_ = false;
    uint64_t guiDragId_ = 0;
    UDim2 guiDragStart_{};

    // Stats
    float fpsTimer_ = 0.0f, fps_ = 0.0f;
    int frames_ = 0;

    // Terrain brush (Terrain Editor tool active)
    bool brushHit_ = false;            // the mouse is over a place the brush can work
    glm::vec3 brushPoint_{ 0.0f }, brushNormal_{ 0.0f, 1.0f, 0.0f };
    bool brushing_ = false;            // a stroke is in progress (left button held)
    float brushTimer_ = 0.0f;
    float strokePlane_ = 0.0f;         // Flatten: the height the stroke levels to
    glm::vec3 strokeOrigin_{ 0.0f }, strokeNormal_{ 0.0f, 1.0f, 0.0f }; // Add: the surface where the stroke started

    // Writes <project>/.rift/thumbnail.png from the last rendered frame (the home screen shows it).
    void saveThumbnail(Editor& editor);
    void handleSelection(UiContext& ui, const glm::mat4& view, const glm::mat4& proj, const GuiSystem* gui);
    void drawGizmo(UiContext& ui, const glm::mat4& view, const glm::mat4& proj);
    void handleDrop(UiContext& ui);
    void drawOverlays(UiContext& ui, const glm::mat4& view, const glm::mat4& proj);
    void handleTerrainBrush(UiContext& ui, const glm::mat4& view, const glm::mat4& proj);
    void endStroke(Editor& editor);
};

// ---------------------------------------------------------------------------
// The Animation Editor: a rig (a model with Motor6D joints) posed at a time on
// a timeline. Moving or turning a limb with the editor's tools keys that joint
// at the current time; keyframes can be dragged, eased, named and played, and
// the clip is saved as a .anim file whose path is its AnimationId.
class AnimationPanel {
public:
    void draw(UiContext& ui, bool* open);
    bool attached() const { return rigId_ != 0; }
    void close(Editor& editor) { detach(editor); }

private:
    uint64_t rigId_ = 0;
    std::vector<std::string> joints_;      // the rig's joints (by Part1 name), in display order
    AnimClip clip_;
    std::string name_ = "NewAnimation";
    bool dirty_ = false, playing_ = false, loopPreview_ = true;
    float time_ = 0.0f, selectedTime_ = -1.0f;
    std::string selectedJoint_;            // "" = the whole keyframe
    bool scrubbing_ = false, draggingKey_ = false, dragSnapshot_ = false;
    float dragFrom_ = 0.0f;
    std::vector<AnimClip> undo_, redo_;
    std::unordered_map<std::string, glm::mat4> applied_; // each Part1's world frame after the last posing

    Instance* rig(Editor& editor) const;
    void attach(Editor& editor, Instance* model);
    void detach(Editor& editor);
    void newClip();
    void changed();
    void snapshot();
    AnimKeyframe* keyframeAt(float time, bool create);
    void pose(Editor& editor);
    std::filesystem::path fileFor(Editor& editor, const std::string& name) const;
    bool save(Editor& editor);
    void open(Editor& editor, const std::filesystem::path& file);
    void deleteSelected();
    void drawToolbar(UiContext& ui);
    void drawTimeline(UiContext& ui);
};

// ---------------------------------------------------------------------------
// Find in Scripts (Ctrl+Shift+F): every script the scene uses, searched as you
// type; a result opens its script at the line. Replace All edits the scripts
// in the editor (saved with the scene).
class FindPanel {
public:
    void draw(UiContext& ui, bool* open);
    void focus(); // put the cursor in the search box

private:
    struct Hit { int line = 0; size_t column = 0; std::string text; };
    struct FileHits { uint64_t scriptId = 0; std::string path, title; std::vector<Hit> hits; };
    std::string query_, replacement_, searched_;
    bool matchCase_ = false, wholeWord_ = false, focusInput_ = false;
    std::vector<FileHits> results_;
    int total_ = 0;
    float refreshTimer_ = 0.0f;

    void search(Editor& editor);
    int replaceAll(Editor& editor);
};

// ---------------------------------------------------------------------------
// The Terrain Editor: Create (generate, clear), Edit (brush tools, size,
// strength, material swatches) and Colours (water and material colours).
// The brush itself works in the viewport (ViewportPanel::handleTerrainBrush).
class TerrainPanel {
public:
    void draw(UiContext& ui, bool* open);
    void showEditTab() { editTabRequest_ = true; }

private:
    TerrainData::GenerateOptions generate_;
    bool editTabRequest_ = false;
    struct ColorEdit { Material material; Color3 before; };
    std::optional<ColorEdit> colorEdit_; // a material colour being dragged (one undo step at the end)

    void drawCreate(UiContext& ui);
    void drawEdit(UiContext& ui);
    void drawColors(UiContext& ui);
    void commitColor(Editor& editor);
};

// ---------------------------------------------------------------------------
// Git for the open project: changes, commit, history, branches, push / pull.
// git runs on a worker thread; the panel shows a spinner meanwhile.
class SourceControlPanel {
public:
    ~SourceControlPanel();
    void draw(UiContext& ui, bool* open);
    void refresh() { refreshTimer_ = 0.0f; }

    struct Result {
        bool ok = true;
        std::string output;
        GitStatus status;
        bool reload = false; // files of the open project changed: reload the scene
    };

private:
    std::future<Result> job_;
    std::string jobLabel_;
    GitStatus status_;
    bool haveStatus_ = false;
    std::filesystem::path root_;  // the project status_ is for
    std::string gitPath_;         // Preferences value the git below was found with
    std::string gitExe_;
    float refreshTimer_ = 0.0f;
    uint64_t seenSaves_ = 0;
    std::string message_;
    std::string lastLabel_, lastOutput_;
    bool lastOk_ = true;
    std::string remoteDraft_, branchDraft_, nameDraft_, emailDraft_;
    bool identityGlobal_ = true;
    GitChange discardTarget_;
    bool openDiscard_ = false, openRemote_ = false, openBranch_ = false;

    void start(UiContext& ui, const std::string& label, std::function<Result(const Git&, const std::filesystem::path&)> work, bool saveFirst);
    void poll(UiContext& ui);
    void drawHeader(UiContext& ui);
    void drawChanges(UiContext& ui);
    void drawHistory(UiContext& ui);
    void drawPopups(UiContext& ui);
};

// ---------------------------------------------------------------------------
class ExplorerPanel {
public:
    void draw(UiContext& ui, bool* open);
    void beginRename(uint64_t id) { renameRequest_ = id; }

private:
    char filter_[128] = {};
    uint64_t renaming_ = 0, renameRequest_ = 0;
    std::string renameBuffer_;
    bool renameFocus_ = false;
    uint64_t lastPrimary_ = 0;      // reveal the selection when it changes elsewhere
    uint64_t revealId_ = 0;
    bool selectedHere_ = false;
    std::vector<uint64_t> visibleOrder_, lastOrder_; // rows in draw order (Shift+click ranges)
    // Edits requested while the tree is being drawn run after it (deleting a
    // row while iterating the rows would leave dangling pointers).
    std::vector<std::function<void()>> deferred_;
    uint64_t rangeAnchor_ = 0;

    void drawNode(UiContext& ui, Instance& node, int depth);
    void drawFiltered(UiContext& ui);
    void contextMenu(UiContext& ui, Instance& node);
    void clickRow(UiContext& ui, Instance& node);
    void dropTarget(UiContext& ui, Instance& node);
};

// ---------------------------------------------------------------------------
class PropertiesPanel {
public:
    void draw(UiContext& ui, bool* open);

private:
    char filter_[96] = {};
    char newAttrName_[64] = {};
    int newAttrType_ = 0;
    char newTag_[64] = {};
    std::vector<AttributeCommand::Change> attrBefore_; // values when an attribute drag started

    void drawProperty(UiContext& ui, const std::vector<Instance*>& list, const PropertyInfo& prop);
    void drawAttributes(UiContext& ui, const std::vector<Instance*>& list);
    void drawTags(UiContext& ui, Instance& instance);
};

// ---------------------------------------------------------------------------
class OutputPanel {
public:
    void draw(UiContext& ui, bool* open);

private:
    bool showInfo_ = true, showWarnings_ = true, showErrors_ = true;
    bool autoScroll_ = true;
    char filter_[128] = {};
    uint64_t lastCounter_ = 0;
};

// ---------------------------------------------------------------------------
class ScriptEditorPanel {
public:
    ScriptEditorPanel();
    ~ScriptEditorPanel();
    // Opens new tabs for Editor::scriptRequests and draws every open script,
    // docked next to the viewport (`dockId`).
    void draw(UiContext& ui, ImGuiID dockId);
    bool anyFocused() const { return focused_; }
    void closeAll();
    size_t openCount() const { return docs_.size(); }

    struct Doc;
private:
    std::vector<std::unique_ptr<Doc>> docs_;
    std::filesystem::path scenePath_; // tabs belong to the scene they were opened in
    bool focused_ = false;

    void open(UiContext& ui, const Editor::ScriptRequest& request);
    bool drawDoc(UiContext& ui, Doc& doc, ImGuiID dockId); // false: closed
};

// ---------------------------------------------------------------------------
class AssetBrowserPanel {
public:
    void draw(UiContext& ui, bool* open);
    void refresh() { dirty_ = true; }

private:
    struct Entry {
        std::filesystem::path path;
        std::string name;
        AssetKind kind = AssetKind::Other;
        uintmax_t size = 0;
    };
    std::filesystem::path root_, folder_;
    std::vector<Entry> entries_;
    bool dirty_ = true;
    float refreshTimer_ = 0.0f;
    float tileSize_ = 84.0f;
    char filter_[96] = {};
    std::filesystem::path selected_;
    std::filesystem::path renaming_;
    std::string renameBuffer_;
    bool renameFocus_ = false;
    std::filesystem::path pendingDelete_;

    void scan();
    void drawFolderTree(UiContext& ui, const std::filesystem::path& dir);
    void drawTile(UiContext& ui, const Entry& e);
    void activate(UiContext& ui, const Entry& e);
    void itemMenu(UiContext& ui, const Entry& e);
    void backgroundMenu(UiContext& ui);
    void importFiles(UiContext& ui);
};

} // namespace rift
