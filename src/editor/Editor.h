#pragma once
//
// Editor.h -- the editor's state and every operation the UI performs.
// Panels draw and call into this; nothing here draws.
//
//   project / scene files      openProject, openScene, saveScene, newScene ...
//   selection                  ids, primary (the gizmo's), Explorer + viewport
//   undoable edits             insert, delete, duplicate, copy/paste, group,
//                              reparent, property and attribute changes
//   play mode                  a GameSession on a clone of the scene; while it
//                              runs, scene() is the running copy and edits are
//                              temporary (like Roblox Studio)
//
#include "editor/Commands.h"
#include "editor/EditorCamera.h"
#include "editor/EditorSettings.h"
#include "game/GameSession.h"
#include "project/Project.h"
#include "script/ScriptStore.h"
#include "scene/Scene.h"

#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rift {

class Input;
class AudioSystem;
class Renderer;
struct ToolboxItem;

enum class Tool { Select, Move, Rotate, Scale };
enum class SelectMode { Replace, Toggle, Add };
enum class NoticeLevel { Info, Success, Warning, Error };

struct Notice {
    std::string text;
    NoticeLevel level = NoticeLevel::Info;
    float time = 0.0f; // seconds shown so far
};

// A line of the Output panel (engine log in edit mode, the game's log while playing).
struct OutputLine {
    std::string text;
    LogLevel level = LogLevel::Info;
    double time = 0.0;
    uint64_t script = 0;
    std::string source; // "Game" / tag
};

class Editor {
public:
    Editor(EditorSettings& settings, Input& input, AudioSystem* audio);
    ~Editor();

    // ---- frame
    void update(float dt);

    // ---- project
    bool openProject(const std::filesystem::path& folder);
    bool createProject(const std::filesystem::path& parent, const std::string& name, Project::Template t);
    void closeProject();
    bool hasProject() const { return project_.isOpen(); }
    Project& project() { return project_; }
    bool saveProjectSettings();
    // Moves the project's scenes to another folder inside it (saves first).
    bool changeScenesFolder(const std::string& folder, bool moveExisting);
    // Reads the project and the open scene again from disk, dropping what is
    // in memory (after a git pull or branch switch changed the files).
    bool reloadFromDisk();

    // ---- scenes
    bool openScene(const std::filesystem::path& sceneFile);
    bool saveScene();                                  // to the current file
    bool saveSceneAs(const std::string& name);         // to <project>/scenes/<name>/scene.json
    bool newScene(const std::string& name);            // creates, saves and opens it
    bool hasScene() const { return !scenePath_.empty(); }
    uint64_t saveCount() const { return saveCount_; }  // goes up with every save
    bool thumbnailRequest = false; // the viewport saves the project's thumbnail (set by saveScene)
    bool toolboxSaveRequest = false; // the Toolbox asks for a name and saves the selection
    const std::filesystem::path& scenePath() const { return scenePath_; }
    std::string sceneName() const;
    bool isDirty() const;
    // Runs `action` now, or after asking about unsaved changes (handled by the UI).
    void requestDiscard(std::function<void()> action);
    std::function<void()> pendingDiscard;              // set while the "Save changes?" dialog is open

    // The scene being shown: the edited one, or while playing the server's or
    // the client's world (viewSide, the Client / Server switch).
    Scene& scene() { return session_ ? session_->scene(viewSide) : *scene_; }
    Scene& editScene() { return *scene_; }
    ScriptStore& scripts() { return scripts_; }
    EditorSettings& settings() { return settings_; }
    EditorCamera& camera() { return camera_; }
    Input& input() { return input_; }
    AudioSystem* audio() const { return audio_; }

    // ---- selection
    const std::vector<uint64_t>& selection() const { return selection_; }
    uint64_t primary() const { return selection_.empty() ? 0 : selection_.back(); }
    bool isSelected(uint64_t id) const;
    void select(uint64_t id, SelectMode mode = SelectMode::Replace);
    void setSelection(std::vector<uint64_t> ids);
    void clearSelection() { selection_.clear(); }
    std::vector<Instance*> selectedInstances();
    // The topmost selected instances (no selected ancestor) that are not services.
    std::vector<Instance*> selectionRoots();
    void selectAll();

    // ---- tools
    Tool tool = Tool::Move;
    NetworkSide viewSide = NetworkSide::Client; // which world the editor shows while playing
    void setViewSide(NetworkSide side);
    // With several test players: which one the Client view (and the input) belongs to.
    void setViewPlayer(int index);
    // Playing and looking at the server: the editor camera, selection and gizmos
    // work (on the server's world) and the game gets no input.
    bool serverView() const { return session_ && viewSide == NetworkSide::Server; }

    // ---- terrain (the Terrain Editor panel and the viewport brush)
    struct TerrainTool {
        bool active = false;                       // the viewport sculpts instead of selecting
        TerrainData::Brush brush = TerrainData::Brush::Add;
        bool fixedPlane = false;                   // Flatten: level to planeHeight, not where the stroke starts
        float planeHeight = 0.0f;
    };
    TerrainTool terrainTool;
    // The rig the Animation Editor is posing (0 = none): the Move / Rotate
    // gizmo then turns a limb around its joint instead of its middle.
    uint64_t animatingRig = 0;
    // A brush stroke: dabs between begin and end go straight to the terrain
    // and become one undo step.
    void beginTerrainStroke();
    void endTerrainStroke(const std::string& label);
    bool strokingTerrain() const { return stroking_; }
    void generateTerrain(const TerrainData::GenerateOptions& options);
    void clearTerrain();

    // ---- undoable edits
    CommandHistory& history() { return history_; }
    void execute(std::unique_ptr<Command> command); // while playing: applied without history
    void record(std::unique_ptr<Command> command);  // already applied
    void undo();
    void redo();

    // A new instance in a sensible place (parts in front of the camera, scripts
    // inside the selection, GUI elements in a ScreenGui, ...). Parts take `shape`.
    Instance* insert(ClassId cls, PartShape shape = PartShape::Block);
    Instance* insertTemplate(const std::string& templateId);
    // Effects: "Attachment", "ParticleEmitter", "Beam", "Trail", or a ready-made
    // one ("Fire", "Smoke", "Sparkles", "MagicBeam", "SpeedTrail"). They go
    // into the selected part (emitters also into an attachment, or a new part);
    // beams and trails get their two attachments made and linked. One undo step.
    Instance* insertEffect(const std::string& kind);
    static const std::vector<std::pair<std::string, std::string>>& effectPresets(); // id, title
    // A character rig to animate (the default character's body, anchored), in front of the camera.
    Instance* insertRig();
    // Creates `cls` under `parent` and lets `setup` fill it in: one undo step, selected.
    Instance* insertWith(ClassId cls, Instance* parent, const std::function<void(Instance&)>& setup, const std::string& label = "");
    void deleteSelection();
    // Delete, or ask first (Preferences > General); the UI shows the question.
    void requestDelete();
    bool deleteRequested = false;
    void duplicateSelection();
    void copySelection();
    void cutSelection();
    void paste(bool intoSelection = false);
    bool canPaste() const { return !clipboard_.is_null(); }
    void groupSelection();     // into a new Model
    void ungroupSelection();   // children of the selected Models/Folders move up
    // Moves `ids` under `parent` at `index` (keeps their world positions).
    void reparent(const std::vector<uint64_t>& ids, uint64_t parent, size_t index = Scene::npos);
    void rename(uint64_t id, const std::string& name);
    void setProperty(const std::vector<uint64_t>& ids, const std::string& prop, const Value& value);
    // Live edits while a widget is dragged: preview applies without history,
    // commit records one undo step from the values captured at the first preview.
    void previewProperty(const std::vector<uint64_t>& ids, const std::string& prop, const Value& value);
    // The same with a different value per instance (one component of a vector on a multi-selection).
    void previewValues(const std::string& prop, const std::vector<std::pair<uint64_t, Value>>& values);
    void commitPreview();
    bool previewing() const { return preview_ != nullptr; }
    void setAttribute(const std::vector<uint64_t>& ids, const std::string& name, const Value& value);
    void addTag(uint64_t id, const std::string& tag);
    void removeTag(uint64_t id, const std::string& tag);
    void focusSelection();
    // Where a new part should go: on the surface in the middle of the view (or under a point of it).
    glm::vec3 placementPoint(const glm::vec3& size) const;
    glm::vec3 placementPointAt(const glm::vec2& viewPoint, const glm::vec3& size) const;

    // ---- Toolbox
    // The selection as a new item of the user's Toolbox (scripts and project files included).
    bool saveSelectionToToolbox(const std::string& name, const std::string& description, ToolboxItem* out = nullptr);
    // A Toolbox item in the scene: in the middle of the view, or under `viewPoint`. One undo step, selected.
    bool insertToolboxItem(const ToolboxItem& item, const glm::vec2* viewPoint = nullptr);
    Instance* insertionParent();

    // ---- scripts
    Instance* createScript(Instance* parent, ClassId cls, const std::string& name, const std::string& source);
    void openScript(uint64_t scriptId, int line = 0);
    struct ScriptRequest { std::string path; uint64_t scriptId = 0; int line = 0; };
    std::vector<ScriptRequest> scriptRequests; // consumed by the script editor panel
    bool saveScripts(); // writes every changed script that the scene uses

    // ---- play
    void play();
    void stop();
    bool playing() const { return session_ != nullptr; }
    GameSession* session() { return session_.get(); }

    // ---- notices and output
    void notify(const std::string& text, NoticeLevel level = NoticeLevel::Info);
    std::deque<Notice>& notices() { return notices_; }
    std::deque<OutputLine>& output() { return output_; }
    uint64_t outputCounter() const { return outputCounter_; }
    void clearOutput() { output_.clear(); }

    // Viewport hooks set by the viewport panel.
    glm::vec2 viewportSize{ 1280.0f, 720.0f };

private:
    EditorSettings& settings_;
    Input& input_;
    AudioSystem* audio_ = nullptr;
    Project project_;
    std::unique_ptr<Scene> scene_;
    std::filesystem::path scenePath_;
    ScriptStore scripts_;
    CommandHistory history_;
    EditorCamera camera_;
    std::unique_ptr<GameSession> session_;
    std::vector<uint64_t> selection_;
    nlohmann::json clipboard_;
    float autosaveTimer_ = 0.0f;
    float hotReloadTimer_ = 0.0f;
    std::deque<Notice> notices_;
    std::deque<OutputLine> output_;
    uint64_t outputCounter_ = 0;
    uint64_t lastSessionLog_ = 0;
    uint64_t saveCount_ = 0;
    int logListener_ = 0;
    bool stroking_ = false;

    struct Preview { std::string prop; std::vector<SetPropertyCommand::Change> changes; };
    std::unique_ptr<Preview> preview_;

    void resetSceneState();
    void addOutput(LogLevel level, const std::string& text, uint64_t script, const std::string& source);
    // Snapshot of `roots` with fresh ids and copies of their script files (paste / duplicate).
    nlohmann::json copySnapshot(const std::vector<const Instance*>& roots);
    nlohmann::json freshIds(nlohmann::json snapshot);
    void applyTemplate(Instance& part, const struct PartTemplate& t);
};

} // namespace rift
