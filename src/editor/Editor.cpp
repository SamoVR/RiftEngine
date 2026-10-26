#include "editor/Editor.h"
#include "editor/Templates.h"
#include "game/Animation.h"
#include "render/Picking.h"
#include "scene/SceneIO.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/TransformUtil.h"
#include "platform/Input.h"
#include "project/Git.h"

#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace rift {

namespace fs = std::filesystem;

namespace {
    // Every instance in the subtree of `root` (root included).
    void collect(Instance& root, std::vector<Instance*>& out) {
        out.push_back(&root);
        Scene::forEachDescendant(root, [&](Instance& d) { out.push_back(&d); });
    }

    // World-space box around instances (parts, or the parts inside models).
    bool boundsOf(const std::vector<Instance*>& list, glm::vec3& lo, glm::vec3& hi) {
        lo = glm::vec3(1e30f);
        hi = glm::vec3(-1e30f);
        auto addPart = [&](const Instance& p) {
            const glm::mat4 w = p.worldTransform();
            for (int c = 0; c < 8; ++c) {
                const glm::vec3 v = glm::vec3(w * glm::vec4((c & 1) ? 0.5f : -0.5f, (c & 2) ? 0.5f : -0.5f, (c & 4) ? 0.5f : -0.5f, 1.0f));
                lo = glm::min(lo, v);
                hi = glm::max(hi, v);
            }
        };
        for (Instance* i : list) {
            if (i->isBasePart()) addPart(*i);
            else if (i->hasTransform()) { lo = glm::min(lo, i->worldPosition()); hi = glm::max(hi, i->worldPosition()); }
            Scene::forEachDescendant(*i, [&](Instance& d) { if (d.isBasePart()) addPart(d); });
        }
        return lo.x <= hi.x;
    }

    float snapValue(float v, float step) { return step > 0.0f ? std::round(v / step) * step : v; }

    // Positions a detached instance so that, once parented to `parent`, it
    // sits at `worldPos` (parents with a transform make positions relative).
    void placeAt(Instance& i, const Instance* parent, const glm::vec3& worldPos) {
        glm::mat4 parentFrame(1.0f);
        for (const Instance* p = parent; p; p = p->parent) if (p->hasTransform()) { parentFrame = p->worldFrame(); break; }
        i.position = glm::vec3(glm::inverse(parentFrame) * glm::vec4(worldPos, 1.0f));
    }
}

// ============================================================================
//  Lifetime
// ============================================================================
Editor::Editor(EditorSettings& settings, Input& input, AudioSystem* audio)
    : settings_(settings), input_(input), audio_(audio), scene_(std::make_unique<Scene>()) {
    // Engine warnings and errors show in the Output panel too.
    logListener_ = Log::addListener([this](LogLevel level, const std::string& tag, const std::string& message) {
        if (level == LogLevel::Info || tag == "Script") return; // script lines come from the session itself
        addOutput(level, message, 0, tag);
    });
}

Editor::~Editor() {
    Log::removeListener(logListener_);
    session_.reset();
}

// ============================================================================
//  Frame
// ============================================================================
void Editor::update(float dt) {
    for (Notice& n : notices_) n.time += dt;
    while (!notices_.empty() && notices_.front().time > 6.0f) notices_.pop_front();
    camera_.fieldOfView = settings_.fieldOfView;
    camera_.invertY = settings_.invertMouseY;

    if (session_) {
        // Copy the game's new output lines.
        const auto& lines = session_->logLines();
        const uint64_t total = session_->logCounter();
        const uint64_t fresh = std::min<uint64_t>(total - lastSessionLog_, lines.size());
        bool error = false;
        for (size_t k = lines.size() - fresh; k < lines.size(); ++k) {
            std::string source = lines[k].side == NetworkSide::Server ? "Server" : "Client";
            // Several players: say whose client printed it.
            if (lines[k].side == NetworkSide::Client && session_->clientCount() > 1) {
                if (const Instance* p = session_->scene().find(session_->client(lines[k].client).playerId)) source = p->name;
            }
            addOutput(lines[k].level, lines[k].text, lines[k].script, source);
            error |= lines[k].level == LogLevel::Error;
        }
        lastSessionLog_ = total;
        if (error && settings_.pauseOnError && !session_->paused()) {
            session_->setPaused(true);
            notify("Paused on a script error (Preferences > Playing)", NoticeLevel::Warning);
        }
        return;
    }

    // Scripts edited in another editor (VS Code, ...) are picked up.
    hotReloadTimer_ += dt;
    if (hotReloadTimer_ > 1.0f) {
        hotReloadTimer_ = 0.0f;
        std::vector<std::string> conflicts;
        const auto reloaded = scripts_.pollExternalChanges(&conflicts);
        for (const auto& path : reloaded) notify("Reloaded " + path + " (changed on disk)");
        for (const auto& path : conflicts) notify(path + " changed on disk while it has unsaved edits here; your version was kept", NoticeLevel::Warning);
    }

    // Autosave next to the scene (scene.autosave.json); never overwrites the scene itself.
    if (settings_.autosaveMinutes > 0 && hasScene() && history_.isDirty()) {
        autosaveTimer_ += dt;
        if (autosaveTimer_ > settings_.autosaveMinutes * 60.0f) {
            autosaveTimer_ = 0.0f;
            fs::path autosave = scenePath_;
            autosave.replace_filename("scene.autosave.json");
            if (SceneIO::saveScene(*scene_, autosave)) LOG_INFO("Editor", "Autosaved to " + Paths::toUtf8(autosave));
        }
    } else {
        autosaveTimer_ = 0.0f;
    }
}

void Editor::addOutput(LogLevel level, const std::string& text, uint64_t script, const std::string& source) {
    output_.push_back({ text, level, session_ ? session_->time() : 0.0, script, source });
    ++outputCounter_;
    while (output_.size() > static_cast<size_t>(std::max(settings_.outputMaxLines, 100))) output_.pop_front();
}

void Editor::notify(const std::string& text, NoticeLevel level) {
    notices_.push_back({ text, level, 0.0f });
    while (notices_.size() > 5) notices_.pop_front();
    if (level == NoticeLevel::Error) LOG_ERROR("Editor", text);
    else if (level == NoticeLevel::Warning) LOG_WARN("Editor", text);
    else LOG_INFO("Editor", text);
}

// ============================================================================
//  Projects
// ============================================================================
bool Editor::openProject(const fs::path& folder) {
    stop();
    Project p;
    std::string error;
    if (!p.load(folder, &error)) {
        notify("Cannot open project: " + error, NoticeLevel::Error);
        return false;
    }
    project_ = p;
    Paths::setProjectRoot(project_.root());
    scripts_.setProjectRoot(Paths::toUtf8(project_.root()));
    settings_.addRecentProject(Paths::toUtf8(project_.root()));
    settings_.save();

    fs::path start = project_.startScenePath();
    std::error_code ec;
    if (!fs::exists(start, ec)) {
        const auto scenes = project_.scenes();
        start = scenes.empty() ? fs::path() : scenes.front();
    }
    if (start.empty()) {
        if (!newScene("Main")) return false;
    } else if (!openScene(start)) {
        return false;
    }
    LOG_INFO("Editor", "Opened project " + project_.settings().name);
    return true;
}

bool Editor::createProject(const fs::path& parent, const std::string& name, Project::Template t) {
    fs::path folder;
    std::string error;
    if (!Project::create(parent, name, t, &folder, &error)) {
        notify("Cannot create project: " + error, NoticeLevel::Error);
        return false;
    }
    // Version control from the start (Preferences > Source Control).
    if (settings_.gitInitNewProjects) {
        const Git git(settings_.gitPath);
        if (git.available()) {
            std::string gitError;
            if (!git.init(folder, &gitError)) addOutput(LogLevel::Warning, "git: " + gitError, 0, "Git");
            else addOutput(LogLevel::Info, "Created a git repository for the project", 0, "Git");
        }
    }
    return openProject(folder);
}

bool Editor::reloadFromDisk() {
    if (!project_.isOpen()) return false;
    stop();
    const fs::path scene = scenePath_;
    Project p;
    std::string error;
    if (p.load(project_.root(), &error)) project_ = p;
    std::error_code ec;
    if (!scene.empty() && fs::exists(scene, ec)) return openScene(scene);
    // The scene does not exist here (another branch): the start scene, or any.
    fs::path start = project_.startScenePath();
    if (!fs::exists(start, ec)) {
        const auto scenes = project_.scenes();
        start = scenes.empty() ? fs::path() : scenes.front();
    }
    if (!start.empty()) return openScene(start);
    return newScene("Main");
}

void Editor::closeProject() {
    stop();
    project_.close();
    scene_ = std::make_unique<Scene>();
    scenePath_.clear();
    scripts_.reset(std::string());
    scripts_.setProjectRoot(std::string());
    resetSceneState();
}

bool Editor::changeScenesFolder(const std::string& folder, bool moveExisting) {
    stop();
    if (isDirty() && !saveScene()) return false;
    std::error_code ec;
    // Where the open scene is, relative to the scenes folder (to reopen it after a move).
    fs::path rel;
    if (hasScene()) {
        rel = fs::relative(scenePath_, project_.scenesFolder(), ec);
        if (ec || rel.empty() || *rel.begin() == "..") rel.clear();
    }
    std::string error;
    if (!project_.changeScenesFolder(folder, moveExisting, &error)) {
        notify("Cannot change the scenes folder: " + error, NoticeLevel::Error);
        return false;
    }
    if (moveExisting && !rel.empty()) openScene(project_.scenesFolder() / rel);
    notify("Scenes folder is now " + project_.settings().scenesFolder, NoticeLevel::Success);
    return true;
}

bool Editor::saveProjectSettings() {
    std::string error;
    if (!project_.save(&error)) { notify("Cannot save project settings: " + error, NoticeLevel::Error); return false; }
    notify("Project settings saved", NoticeLevel::Success);
    return true;
}

// ============================================================================
//  Scenes
// ============================================================================
void Editor::resetSceneState() {
    stroking_ = false;
    history_.clear();
    selection_.clear();
    preview_.reset();
    clipboard_ = nullptr;
    autosaveTimer_ = 0.0f;
}

std::string Editor::sceneName() const {
    if (scenePath_.empty()) return "Untitled";
    return Paths::toUtf8(scenePath_.parent_path().filename());
}

bool Editor::openScene(const fs::path& file) {
    stop();
    auto loaded = std::make_unique<Scene>();
    // Asset paths in the scene are relative to its project.
    const fs::path root = Project::findRoot(file);
    Paths::setProjectRoot(root.empty() ? file.parent_path() : root);
    const SceneIO::LoadResult r = SceneIO::loadScene(*loaded, file);
    if (!r.ok) {
        notify("Cannot open " + Paths::toUtf8(file) + ": " + r.error, NoticeLevel::Error);
        if (project_.isOpen()) Paths::setProjectRoot(project_.root());
        return false;
    }
    for (const std::string& w : r.warnings) addOutput(LogLevel::Warning, w, 0, "Scene");

    scene_ = std::move(loaded);
    scenePath_ = fs::absolute(file);
    resetSceneState();
    scripts_.reset(Paths::toUtf8(scenePath_.parent_path()));
    int missing = 0;
    scene_->forEachInTree([&](Instance& i) {
        if (i.isScript() && !i.source.empty() && !scripts_.load(i.source)) ++missing;
    });
    if (r.formatVersion < SceneIO::kFormatVersion) {
        notify("Converted from an older scene format. Save to keep the new format.", NoticeLevel::Info);
        history_.record(std::make_unique<CompositeCommand>("Convert scene", std::vector<std::unique_ptr<Command>>())); // marks unsaved
    }
    if (missing > 0) notify(std::to_string(missing) + " script file(s) of this scene are missing", NoticeLevel::Warning);

    // Look at the scene's content (a huge baseplate would push the camera far away).
    std::vector<Instance*> all, content;
    Scene::forEachDescendant(scene_->workspace(), [&](Instance& i) {
        if (!i.isBasePart()) return;
        all.push_back(&i);
        if (std::max({ i.size.x, i.size.y, i.size.z }) < 60.0f) content.push_back(&i);
    });
    if (content.empty()) content = all;
    glm::vec3 lo, hi;
    if (boundsOf(content, lo, hi)) {
        const glm::vec3 center = (lo + hi) * 0.5f;
        const float radius = std::min(glm::length(hi - lo) * 0.35f, 60.0f);
        camera_.lookAt(center + glm::normalize(glm::vec3(1.0f, 0.7f, 1.0f)) * std::max(radius * 1.6f, 12.0f), center);
    }
    LOG_INFO("Editor", "Opened " + Paths::toUtf8(scenePath_));
    return true;
}

bool Editor::saveScripts() {
    // Only scripts the scene actually uses (an undone insert leaves its file in
    // memory; it must not be written).
    std::unordered_set<std::string> used;
    scene_->forEachInTree([&](Instance& i) { if (i.isScript() && !i.source.empty()) used.insert(ScriptStore::normalize(i.source)); });
    bool ok = true;
    for (const std::string& path : scripts_.paths()) {
        const ScriptStore::Entry* e = scripts_.find(path);
        if (!e || !e->dirty) continue;
        if (!used.count(path) && path.rfind(ScriptStore::kProjectPrefix, 0) != 0) continue;
        if (!scripts_.save(path)) { notify("Could not save " + path, NoticeLevel::Error); ok = false; }
    }
    return ok;
}

bool Editor::saveScene() {
    if (playing()) { notify("Stop the game before saving", NoticeLevel::Warning); return false; }
    if (!hasScene()) return false;
    commitPreview();
    if (!saveScripts()) return false;
    std::string error;
    if (!SceneIO::saveScene(*scene_, scenePath_, &error)) {
        notify("Save failed: " + error, NoticeLevel::Error);
        return false;
    }
    history_.markSaved();
    std::error_code ec;
    fs::path autosave = scenePath_;
    autosave.replace_filename("scene.autosave.json");
    fs::remove(autosave, ec);
    ++saveCount_;
    thumbnailRequest = project_.isOpen();
    notify("Saved " + sceneName(), NoticeLevel::Success);
    return true;
}

bool Editor::saveSceneAs(const std::string& name) {
    if (playing()) { notify("Stop the game before saving", NoticeLevel::Warning); return false; }
    const fs::path base = project_.isOpen() ? project_.scenesFolder() : scenePath_.parent_path().parent_path();
    const fs::path folder = base / Paths::fromUtf8(name);
    const fs::path file = folder / "scene.json";
    commitPreview();
    // Every script the scene uses goes to the new folder.
    std::unordered_set<std::string> used;
    scene_->forEachInTree([&](Instance& i) { if (i.isScript() && !i.source.empty()) used.insert(ScriptStore::normalize(i.source)); });
    for (const std::string& path : used) scripts_.load(path);
    if (!scripts_.saveAll(Paths::toUtf8(folder))) { notify("Could not write the scripts to " + Paths::toUtf8(folder), NoticeLevel::Error); return false; }
    std::string error;
    if (!SceneIO::saveScene(*scene_, file, &error)) { notify("Save failed: " + error, NoticeLevel::Error); return false; }
    scenePath_ = fs::absolute(file);
    history_.markSaved();
    ++saveCount_;
    notify("Saved as " + name, NoticeLevel::Success);
    return true;
}

bool Editor::newScene(const std::string& name) {
    const fs::path base = project_.isOpen() ? project_.scenesFolder() : fs::path();
    if (base.empty()) { notify("Open or create a project first", NoticeLevel::Warning); return false; }
    const fs::path file = base / Paths::fromUtf8(name) / "scene.json";
    std::error_code ec;
    if (fs::exists(file, ec)) { notify("A scene named " + name + " already exists", NoticeLevel::Warning); return false; }
    Scene fresh;
    Instance* floor = fresh.create(ClassId::Part, &fresh.workspace());
    floor->name = "Baseplate";
    floor->size = { 128, 1, 128 };
    floor->position = { 0, -0.5f, 0 };
    floor->color = { 0.36f, 0.38f, 0.40f };
    floor->material = Material::Concrete;
    Instance* spawn = fresh.create(ClassId::SpawnLocation, &fresh.workspace());
    spawn->position = { 0, 0.2f, 0 };
    std::string error;
    if (!SceneIO::saveScene(fresh, file, &error)) { notify("Cannot create the scene: " + error, NoticeLevel::Error); return false; }
    return openScene(file);
}

bool Editor::isDirty() const {
    if (history_.isDirty()) return true;
    for (const std::string& path : scripts_.paths()) {
        const ScriptStore::Entry* e = scripts_.find(path);
        if (e && e->dirty) return true;
    }
    return false;
}

void Editor::requestDiscard(std::function<void()> action) {
    if (!isDirty()) { action(); return; }
    pendingDiscard = std::move(action);
}

// ============================================================================
//  Selection
// ============================================================================
bool Editor::isSelected(uint64_t id) const { return std::find(selection_.begin(), selection_.end(), id) != selection_.end(); }

void Editor::select(uint64_t id, SelectMode mode) {
    commitPreview();
    if (mode == SelectMode::Replace) { selection_ = { id }; return; }
    auto it = std::find(selection_.begin(), selection_.end(), id);
    if (it != selection_.end()) {
        if (mode == SelectMode::Toggle) selection_.erase(it);
        else { selection_.erase(it); selection_.push_back(id); } // Add: becomes primary
        return;
    }
    selection_.push_back(id);
}

void Editor::setSelection(std::vector<uint64_t> ids) {
    commitPreview();
    selection_ = std::move(ids);
}

std::vector<Instance*> Editor::selectedInstances() {
    std::vector<Instance*> out;
    for (uint64_t id : selection_) if (Instance* i = scene().find(id)) out.push_back(i);
    return out;
}

std::vector<Instance*> Editor::selectionRoots() {
    std::vector<Instance*> out;
    for (Instance* i : selectedInstances()) {
        if (i->isService() || i->isFixedContainer() || i->cls == ClassId::Player) continue;
        bool ancestor = false;
        for (Instance* a = i->parent; a && !ancestor; a = a->parent) ancestor = isSelected(a->id);
        if (!ancestor) out.push_back(i);
    }
    return out;
}

void Editor::selectAll() {
    selection_.clear();
    for (Instance* i : scene().workspace().children)
        if (!i->isFixedContainer()) selection_.push_back(i->id); // not the Terrain
}

// ============================================================================
//  Terrain
// ============================================================================
void Editor::beginTerrainStroke() {
    if (session_ || stroking_) return;
    scene_->terrain().beginRecording();
    stroking_ = true;
}

void Editor::endTerrainStroke(const std::string& label) {
    if (!stroking_) return;
    stroking_ = false;
    TerrainData& terrain = scene_->terrain();
    TerrainData::Snapshot before = terrain.endRecording();
    if (before.empty()) return;
    TerrainData::Snapshot after = terrain.capture(before);
    history_.record(std::make_unique<TerrainCommand>(label, std::move(before), std::move(after)));
}

void Editor::generateTerrain(const TerrainData::GenerateOptions& options) {
    if (session_) return;
    endTerrainStroke("Terrain");
    TerrainData& terrain = scene_->terrain();
    terrain.beginRecording();
    terrain.generate(options);
    TerrainData::Snapshot before = terrain.endRecording();
    if (before.empty()) return;
    TerrainData::Snapshot after = terrain.capture(before);
    history_.record(std::make_unique<TerrainCommand>("Generate Terrain", std::move(before), std::move(after)));
    notify("Terrain generated");
}

void Editor::clearTerrain() {
    if (session_) return;
    endTerrainStroke("Terrain");
    TerrainData& terrain = scene_->terrain();
    if (terrain.empty()) return;
    terrain.beginRecording();
    terrain.clear();
    TerrainData::Snapshot before = terrain.endRecording();
    TerrainData::Snapshot after = terrain.capture(before);
    history_.record(std::make_unique<TerrainCommand>("Clear Terrain", std::move(before), std::move(after)));
    notify("Terrain cleared (Ctrl+Z brings it back)");
}

// ============================================================================
//  Commands
// ============================================================================
void Editor::execute(std::unique_ptr<Command> command) {
    commitPreview();
    if (session_) { command->execute(scene()); return; } // play-mode edits are temporary
    history_.execute(*scene_, std::move(command));
}

void Editor::record(std::unique_ptr<Command> command) {
    if (session_) return;
    history_.record(std::move(command));
}

void Editor::requestDelete() {
    if (selection_.empty()) return;
    if (settings_.confirmDelete) deleteRequested = true;
    else deleteSelection();
}

void Editor::undo() {
    if (session_) { notify("Undo is not available while the game runs", NoticeLevel::Warning); return; }
    if (stroking_) return; // finish the terrain stroke first
    commitPreview();
    const std::string label = history_.undoLabel();
    if (history_.undo(*scene_)) notify("Undo: " + label);
}

void Editor::redo() {
    if (session_ || stroking_) return;
    commitPreview();
    const std::string label = history_.redoLabel();
    if (history_.redo(*scene_)) notify("Redo: " + label);
}

// ---------------------------------------------------------------- inserting
Instance* Editor::insertionParent() {
    Instance* sel = selection_.empty() ? nullptr : scene().find(primary());
    if (sel && (sel->cls == ClassId::Model || sel->cls == ClassId::Folder)) return sel;
    return &scene().workspace();
}

glm::vec3 Editor::placementPoint(const glm::vec3& size) const {
    glm::vec3 origin, dir;
    camera_.ray(viewportSize * 0.5f, viewportSize, origin, dir);
    glm::vec3 point = camera_.position() + camera_.forward() * 16.0f;
    if (auto hit = pickSurface(session_ ? session_->scene() : *scene_, origin, dir)) {
        point = hit->position + hit->normal * (std::abs(hit->normal.y) > 0.5f ? size.y * 0.5f : std::max(size.x, size.z) * 0.5f);
    } else if (auto ground = pickGround(origin, dir); ground && glm::length(*ground - origin) < 400.0f) {
        point = *ground + glm::vec3(0.0f, size.y * 0.5f, 0.0f);
    }
    if (settings_.snap) {
        point.x = snapValue(point.x, settings_.snapMove);
        point.z = snapValue(point.z, settings_.snapMove);
    }
    return point;
}

namespace {
    // A new instance is built detached, snapshotted, destroyed, and then
    // re-created by a CreateCommand: one code path for insert, undo and redo.
    std::unique_ptr<CreateCommand> commitNew(Scene& scene, Instance* detached, Instance* parent, const std::string& label) {
        nlohmann::json snapshot = SceneIO::subtreesToJson({ detached });
        scene.destroy(detached);
        return std::make_unique<CreateCommand>(label, std::move(snapshot), std::vector<PlacedSubtree>{ { parent ? parent->id : 0, Scene::npos } });
    }

    // The same for several new instances, each with its own parent.
    std::unique_ptr<CreateCommand> commitNewMany(Scene& scene, const std::vector<std::pair<Instance*, Instance*>>& items, const std::string& label) {
        std::vector<const Instance*> roots;
        std::vector<PlacedSubtree> placement;
        for (const auto& [detached, parent] : items) {
            roots.push_back(detached);
            placement.push_back({ parent ? parent->id : 0, Scene::npos });
        }
        nlohmann::json snapshot = SceneIO::subtreesToJson(roots);
        for (const auto& item : items) scene.destroy(item.first);
        return std::make_unique<CreateCommand>(label, std::move(snapshot), std::move(placement));
    }

    // Ready-made particle effects.
    void presetFire(Instance& e) {
        e.name = "Fire";
        e.colorSequence = ColorSequence(std::vector<ColorKeypoint>{ { 0.0f, Color3(1.0f, 0.82f, 0.32f) }, { 0.3f, Color3(1.0f, 0.45f, 0.08f) }, { 1.0f, Color3(0.65f, 0.1f, 0.03f) } });
        e.transparencySequence = NumberSequence(std::vector<NumberKeypoint>{ { 0.0f, 0.55f, 0.0f }, { 0.25f, 0.3f, 0.0f }, { 1.0f, 1.0f, 0.0f } });
        e.sizeSequence = NumberSequence(std::vector<NumberKeypoint>{ { 0.0f, 1.6f, 0.3f }, { 0.6f, 1.2f, 0.2f }, { 1.0f, 0.2f, 0.0f } });
        e.lifetime = NumberRange(0.6f, 1.1f);
        e.speedRange = NumberRange(2.5f, 5.0f);
        e.rate = 45.0f;
        e.spreadAngle = { 18.0f, 18.0f };
        e.acceleration = { 0.0f, 5.0f, 0.0f };
        e.drag = 1.0f;
        e.rotationRange = NumberRange(0.0f, 360.0f);
        e.rotSpeed = NumberRange(-90.0f, 90.0f);
        e.lightEmission = 0.75f; // glows, but keeps its colour in daylight
    }
    void presetSmoke(Instance& e) {
        e.name = "Smoke";
        e.colorSequence = ColorSequence(Color3(0.62f, 0.62f, 0.64f), Color3(0.4f, 0.4f, 0.42f));
        e.transparencySequence = NumberSequence(std::vector<NumberKeypoint>{ { 0.0f, 1.0f, 0.0f }, { 0.15f, 0.55f, 0.0f }, { 1.0f, 1.0f, 0.0f } });
        e.sizeSequence = NumberSequence(1.5f, 6.0f);
        e.lifetime = NumberRange(3.0f, 5.0f);
        e.speedRange = NumberRange(1.0f, 2.5f);
        e.rate = 8.0f;
        e.spreadAngle = { 12.0f, 12.0f };
        e.acceleration = { 0.6f, 1.2f, 0.0f };
        e.drag = 0.3f;
        e.rotationRange = NumberRange(0.0f, 360.0f);
        e.rotSpeed = NumberRange(-25.0f, 25.0f);
        e.lightInfluence = 1.0f;
    }
    void presetSparkles(Instance& e) {
        e.name = "Sparkles";
        e.colorSequence = ColorSequence(Color3(1.0f, 0.85f, 0.35f), Color3(1.0f, 1.0f, 1.0f));
        e.transparencySequence = NumberSequence(0.0f, 1.0f);
        e.sizeSequence = NumberSequence(0.4f, 0.0f);
        e.lifetime = NumberRange(0.8f, 1.6f);
        e.speedRange = NumberRange(3.0f, 7.0f);
        e.rate = 25.0f;
        e.spreadAngle = { 180.0f, 180.0f };
        e.acceleration = { 0.0f, -3.0f, 0.0f };
        e.drag = 1.5f;
        e.lightEmission = 1.0f;
        e.brightness = 2.0f;
    }
}

Instance* Editor::insertRig() {
    Scene& s = scene();
    Instance* rig = buildCharacterRig(s, RigColors{}, true);
    placeAt(*rig, insertionParent(), placementPoint(glm::vec3(1.0f, 2.0f, 1.0f)));
    const uint64_t id = rig->id;
    execute(commitNew(s, rig, insertionParent(), "Insert Rig"));
    selection_ = { id };
    return s.find(id);
}

const std::vector<std::pair<std::string, std::string>>& Editor::effectPresets() {
    static const std::vector<std::pair<std::string, std::string>> list = {
        { "Fire", "Fire" }, { "Smoke", "Smoke" }, { "Sparkles", "Sparkles" }, { "MagicBeam", "Magic Beam" }, { "SpeedTrail", "Speed Trail" },
    };
    return list;
}

Instance* Editor::insertEffect(const std::string& kind) {
    Scene& s = scene();
    std::vector<Instance*> parts;
    for (Instance* i : selectedInstances()) if (i->isBasePart()) parts.push_back(i);
    Instance* selected = selection_.empty() ? nullptr : s.find(primary());
    Instance* part = parts.empty() ? nullptr : parts.back();
    const bool emitter = kind == "ParticleEmitter" || kind == "Fire" || kind == "Smoke" || kind == "Sparkles";
    const bool beam = kind == "Beam" || kind == "MagicBeam";
    const bool trail = kind == "Trail" || kind == "SpeedTrail";

    std::vector<std::pair<Instance*, Instance*>> items; // new instance, parent
    uint64_t selectId = 0;
    if (kind == "Attachment") {
        if (!part) {
            notify("Select a part first: attachments go inside parts", NoticeLevel::Warning);
            return nullptr;
        }
        Instance* a = s.create(ClassId::Attachment);
        items.emplace_back(a, part);
        selectId = a->id;
    } else if (emitter) {
        // Into the selected part or attachment; otherwise a new invisible part to hold it.
        Instance* holder = selected && (selected->isBasePart() || selected->cls == ClassId::Attachment) ? selected : nullptr;
        Instance* e = s.create(ClassId::ParticleEmitter);
        if (kind == "Fire") presetFire(*e);
        else if (kind == "Smoke") presetSmoke(*e);
        else if (kind == "Sparkles") presetSparkles(*e);
        if (!holder) {
            Instance* p = s.create(ClassId::Part);
            p->name = e->name == "ParticleEmitter" ? "Emitter" : e->name;
            p->size = { 2.0f, 1.0f, 2.0f };
            p->transparency = 1.0f;
            p->canCollide = false;
            p->castShadow = false;
            placeAt(*p, insertionParent(), placementPoint(p->size));
            s.setParent(e, p);
            items.emplace_back(p, insertionParent());
        } else {
            items.emplace_back(e, holder);
        }
        selectId = e->id;
    } else if (beam || trail) {
        if (!part) {
            notify(beam ? "Select one or two parts first: the beam goes between them" : "Select a part first: the trail follows it", NoticeLevel::Warning);
            return nullptr;
        }
        // Two attachments: on two parts (beam between them), or at the ends of one part.
        Instance* first = parts.size() >= 2 ? parts[parts.size() - 2] : part;
        Instance* second = part;
        Instance* a0 = s.create(ClassId::Attachment);
        Instance* a1 = s.create(ClassId::Attachment);
        a0->name = beam ? "BeamStart" : "TrailTop";
        a1->name = beam ? "BeamEnd" : "TrailBottom";
        if (trail) {
            a0->position = { 0.0f, part->size.y * 0.5f, 0.0f };
            a1->position = { 0.0f, -part->size.y * 0.5f, 0.0f };
        } else if (first == second) {
            a0->position = { -part->size.x * 0.5f, 0.0f, 0.0f };
            a1->position = { part->size.x * 0.5f, 0.0f, 0.0f };
        }
        Instance* fx = s.create(beam ? ClassId::Beam : ClassId::Trail);
        fx->attachment0.id = a0->id;
        fx->attachment1.id = a1->id;
        if (kind == "MagicBeam") {
            fx->name = "MagicBeam";
            fx->colorSequence = ColorSequence(Color3(0.65f, 0.35f, 1.0f), Color3(0.3f, 0.85f, 1.0f));
            fx->transparencySequence = NumberSequence(std::vector<NumberKeypoint>{ { 0.0f, 0.6f, 0.0f }, { 0.5f, 0.1f, 0.0f }, { 1.0f, 0.6f, 0.0f } });
            fx->lightEmission = 0.45f;
            fx->brightness = 1.3f;
            fx->width0 = fx->width1 = 0.6f;
            fx->curveSize0 = 3.0f;
            fx->curveSize1 = -3.0f;
            fx->segments = 24;
            fx->faceCamera = true;
        } else if (kind == "SpeedTrail") {
            fx->name = "SpeedTrail";
            fx->colorSequence = ColorSequence(Color3(1.0f, 1.0f, 1.0f), Color3(0.55f, 0.4f, 1.0f));
            fx->transparencySequence = NumberSequence(0.15f, 1.0f);
            fx->sizeSequence = NumberSequence(1.0f, 0.0f);
            fx->trailLifetime = 0.45f;
            fx->lightEmission = 0.6f;
        } else if (beam) {
            fx->faceCamera = true;
        }
        items.emplace_back(a0, first);
        items.emplace_back(a1, second);
        items.emplace_back(fx, first);
        selectId = fx->id;
    } else {
        return nullptr;
    }
    const std::string title = kind == "MagicBeam" ? "Magic Beam" : kind == "SpeedTrail" ? "Speed Trail" : kind;
    execute(commitNewMany(s, items, "Insert " + title));
    selection_ = { selectId };
    return s.find(selectId);
}

Instance* Editor::createScript(Instance* parent, ClassId cls, const std::string& name, const std::string& source) {
    Scene& s = scene();
    Instance* script = s.create(cls);
    script->name = name;
    script->source = scripts_.uniquePath(name);
    scripts_.create(script->source, source);
    if (parent) s.setParent(script, parent);
    return script;
}

Instance* Editor::insert(ClassId cls, PartShape shape) {
    Scene& s = scene();
    Instance* parent = insertionParent();
    Instance* selected = selection_.empty() ? nullptr : s.find(primary());
    Instance* i = nullptr;

    if (cls == ClassId::Script || cls == ClassId::LocalScript || cls == ClassId::ModuleScript) {
        // Inside the selected object. Without one: Scripts go to ScriptService,
        // LocalScripts to StarterPlayerScripts, modules to ReplicatedStorage.
        Instance* target = selected;
        if (!target) {
            if (cls == ClassId::Script) target = s.service(ClassId::ScriptService);
            else if (cls == ClassId::ModuleScript) target = s.service(ClassId::ReplicatedStorage);
            else {
                s.ensureDefaults();
                for (Instance* c : s.starterPlayer().children) if (c->cls == ClassId::StarterPlayerScripts) target = c;
            }
        }
        const char* name = cls == ClassId::Script ? "Script" : cls == ClassId::LocalScript ? "LocalScript" : "ModuleScript";
        const char* source = cls == ClassId::Script ? newScriptSource() : cls == ClassId::LocalScript ? newLocalScriptSource() : newModuleSource();
        i = createScript(nullptr, cls, name, source);
        parent = target;
    } else if (cls == ClassId::Tool) {
        // A tool with a Handle to hold, in StarterPack (or the selected container).
        i = s.create(cls);
        Instance* handle = s.create(ClassId::Part, i);
        handle->name = "Handle";
        handle->size = { 0.3f, 0.3f, 1.0f };
        handle->anchored = false;
        const bool container = selected && (selected->cls == ClassId::Folder || selected->cls == ClassId::Backpack || selected->isService());
        parent = container ? selected : s.service(ClassId::StarterPack);
        if (parent == &s.workspace()) placeAt(*handle, i, placementPoint(handle->size));
    } else if (cls == ClassId::ScreenGui) {
        i = s.create(cls);
        parent = s.service(ClassId::StarterGui);
    } else if (cls == ClassId::ProximityPrompt) {
        // Into the selected part (or Attachment / Model).
        if (!selected || !(selected->isBasePart() || selected->cls == ClassId::Attachment || selected->cls == ClassId::Model)) {
            notify("Select a part to put the ProximityPrompt in", NoticeLevel::Warning);
            return nullptr;
        }
        i = s.create(cls);
        parent = selected;
    } else if (cls == ClassId::BillboardGui || cls == ClassId::SurfaceGui) {
        // On the selected part, with a label to start from.
        if (!selected || !selected->isBasePart()) {
            notify(std::string("Select a part to put the ") + classInfo(cls).name + " on", NoticeLevel::Warning);
            return nullptr;
        }
        i = s.create(cls);
        parent = selected;
        Instance* label = s.create(ClassId::TextLabel, i);
        label->name = "TextLabel";
        label->guiSize = { 1.0f, 0.0f, 1.0f, 0.0f };
        label->text = cls == ClassId::BillboardGui ? "Hello!" : "Sign";
        label->textScaled = true;
        label->font = GuiFont::GothamBold;
        label->textColor = Color3(1.0f, 1.0f, 1.0f);
        if (cls == ClassId::BillboardGui) {
            i->guiSize = { 4.0f, 0.0f, 1.0f, 0.0f }; // 4 by 1 studs
            i->studsOffset = { 0.0f, selected->size.y * 0.5f + 1.0f, 0.0f };
            label->backgroundTransparency = 1.0f;
            label->textStrokeTransparency = 0.4f;
        } else {
            label->color = Color3(0.12f, 0.12f, 0.16f);
            label->textColor = Color3(1.0f, 1.0f, 1.0f);
        }
    } else if (cls == ClassId::UIGradient) {
        // Shades the GUI element it is in.
        if (!selected || !selected->isGuiObject()) {
            notify("Select a GUI element (Frame, TextLabel, ...) to put the UIGradient in", NoticeLevel::Warning);
            return nullptr;
        }
        i = s.create(cls);
        parent = selected;
    } else if (cls > ClassId::ScreenGui && cls <= ClassId::ImageButton) {
        // GUI elements go into the selected ScreenGui / BillboardGui / SurfaceGui / Frame, or a new ScreenGui.
        Instance* container = nullptr;
        for (Instance* c = selected; c && !container; c = c->parent) if (c->isLayerCollector() || c->cls == ClassId::Frame) container = c;
        if (!container) {
            for (Instance* g : s.service(ClassId::StarterGui)->children) if (g->cls == ClassId::ScreenGui) { container = g; break; }
        }
        if (!container) {
            Instance* gui = insert(ClassId::ScreenGui);
            container = gui;
        }
        i = s.create(cls);
        i->guiPosition = { 0.5f, 0.0f, 0.5f, 0.0f };
        i->anchorPoint = { 0.5f, 0.5f };
        parent = container;
    } else if (cls == ClassId::Attachment || cls == ClassId::ParticleEmitter || cls == ClassId::Beam || cls == ClassId::Trail) {
        return insertEffect(classInfo(cls).name);
    } else if (cls == ClassId::PointLight || cls == ClassId::SpotLight || cls == ClassId::Sound) {
        i = s.create(cls);
        if (selected && selected->isBasePart()) parent = selected; // attached to the part
        else if (cls == ClassId::Sound) parent = s.service(ClassId::SoundService);
        else placeAt(*i, parent, placementPoint(glm::vec3(1.0f)) + glm::vec3(0.0f, 3.0f, 0.0f));
        if (cls == ClassId::SpotLight && selected && selected->isBasePart()) i->rotation = { -90.0f, 0.0f, 0.0f }; // shine down
    } else if (cls >= ClassId::RemoteEvent && cls <= ClassId::BindableFunction) {
        // Remotes live where both sides can see them.
        i = s.create(cls);
        parent = selected && (selected->cls == ClassId::Folder || selected->isService()) ? selected : s.service(ClassId::ReplicatedStorage);
    } else if (cls == ClassId::Humanoid || (cls >= ClassId::IntValue && cls <= ClassId::Color3Value)) {
        i = s.create(cls);
        if (selected && !selected->isService()) parent = selected;
    } else {
        i = s.create(cls);
        if (cls == ClassId::Part && shape != PartShape::Block) {
            i->shape = shape;
            i->size = shape == PartShape::Ball || shape == PartShape::Cylinder ? glm::vec3(4.0f) : glm::vec3(4.0f, 2.0f, 4.0f);
            if (shape == PartShape::Cylinder) i->size = { 3.0f, 4.0f, 3.0f };
        }
        if (i->isBasePart() || i->cls == ClassId::Model) {
            const glm::vec3 size = i->isBasePart() ? i->size : glm::vec3(1.0f);
            placeAt(*i, parent, placementPoint(size));
        }
    }
    if (!i) return nullptr;
    const uint64_t id = i->id;
    auto cmd = commitNew(s, i, parent, "Insert " + classInfo(cls).name);
    execute(std::move(cmd));
    selection_ = { id };
    if (cls == ClassId::Script || cls == ClassId::LocalScript || cls == ClassId::ModuleScript) openScript(id);
    return s.find(id);
}

void Editor::applyTemplate(Instance& p, const PartTemplate& t) {
    p.shape = t.shape;
    p.size = t.size;
    p.color = t.color;
    p.material = t.material;
    p.anchored = t.anchored;
    p.canCollide = t.canCollide;
    p.transparency = t.transparency;
}

Instance* Editor::insertTemplate(const std::string& templateId) {
    const PartTemplate* t = findTemplate(templateId);
    if (!t) return nullptr;
    Scene& s = scene();
    if (t->sceneScript) {
        Instance* script = createScript(nullptr, ClassId::Script, t->scriptName, t->scriptSource);
        const uint64_t id = script->id;
        execute(commitNew(s, script, s.service(ClassId::ScriptService), "Insert " + t->title));
        selection_ = { id };
        return s.find(id);
    }
    if (t->tool) {
        Instance* tool = s.create(ClassId::Tool);
        tool->name = t->id;
        tool->gripPos = t->gripPos;
        Instance* handle = s.create(ClassId::Part, tool);
        handle->name = "Handle";
        applyTemplate(*handle, *t);
        if (t->build) t->build(s, *handle);
        if (!t->scriptSource.empty()) createScript(tool, ClassId::Script, t->scriptName, t->scriptSource);
        const uint64_t id = tool->id;
        execute(commitNew(s, tool, s.service(ClassId::StarterPack), "Insert " + t->title));
        selection_ = { id };
        return s.find(id);
    }
    Instance* part = s.create(t->cls);
    part->name = t->id;
    applyTemplate(*part, *t);
    placeAt(*part, &s.workspace(), placementPoint(t->size));
    if (!t->scriptSource.empty()) createScript(part, ClassId::Script, t->scriptName, t->scriptSource);
    const uint64_t id = part->id;
    execute(commitNew(s, part, &s.workspace(), "Insert " + t->title));
    selection_ = { id };
    return s.find(id);
}

Instance* Editor::insertWith(ClassId cls, Instance* parent, const std::function<void(Instance&)>& setup, const std::string& label) {
    Scene& s = scene();
    Instance* i = s.create(cls);
    if (setup) setup(*i);
    const uint64_t id = i->id;
    execute(commitNew(s, i, parent, label.empty() ? "Insert " + classInfo(cls).name : label));
    selection_ = { id };
    return s.find(id);
}

// ---------------------------------------------------------------- delete / copy / paste
void Editor::deleteSelection() {
    std::vector<uint64_t> ids;
    for (Instance* i : selectionRoots()) ids.push_back(i->id);
    if (ids.empty()) return;
    execute(std::make_unique<DeleteCommand>(ids));
    selection_.clear();
}

nlohmann::json Editor::freshIds(nlohmann::json snapshot) {
    // New ids for every entry; parent links inside the snapshot follow.
    std::unordered_map<std::string, std::string> remap;
    for (auto& e : snapshot) {
        const std::string fresh = idToString(newInstanceId());
        if (e.contains("id") && e["id"].is_string()) remap[e["id"].get<std::string>()] = fresh;
        e["id"] = fresh;
    }
    for (auto& e : snapshot) {
        if (e.contains("parent") && e["parent"].is_string()) {
            auto it = remap.find(e["parent"].get<std::string>());
            if (it != remap.end()) e["parent"] = it->second;
        }
        // References inside the copy (a Beam's attachments) follow too.
        if (e.contains("props") && e["props"].is_object()) {
            for (const char* ref : { "Attachment0", "Attachment1", "Part0", "Part1" }) {
                if (!e["props"].contains(ref) || !e["props"][ref].is_string()) continue;
                auto it = remap.find(e["props"][ref].get<std::string>());
                if (it != remap.end()) e["props"][ref] = it->second;
            }
        }
    }
    // Each copied script gets its own file (Roblox copies the Source).
    for (auto& e : snapshot) {
        const std::string cls = e.value("class", std::string());
        if (cls != "Script" && cls != "ModuleScript") continue;
        std::string text;
        if (e.contains("_text") && e["_text"].is_string()) text = e["_text"].get<std::string>();
        e.erase("_text");
        const std::string name = e.value("name", std::string("Script"));
        const std::string path = scripts_.uniquePath(name);
        scripts_.create(path, text);
        e["props"]["Source"] = path;
    }
    return snapshot;
}

nlohmann::json Editor::copySnapshot(const std::vector<const Instance*>& roots) {
    nlohmann::json snapshot = SceneIO::subtreesToJson(roots);
    // Carry the script text along, so pasting into another scene works too.
    for (auto& e : snapshot) {
        const std::string cls = e.value("class", std::string());
        if (cls != "Script" && cls != "ModuleScript") continue;
        const std::string source = e.contains("props") && e["props"].contains("Source") ? e["props"]["Source"].get<std::string>() : std::string();
        if (!source.empty() && scripts_.load(source)) {
            if (const ScriptStore::Entry* entry = scripts_.find(source)) e["_text"] = entry->text;
        }
    }
    return snapshot;
}

void Editor::copySelection() {
    std::vector<const Instance*> roots;
    for (Instance* i : selectionRoots()) roots.push_back(i);
    if (roots.empty()) return;
    clipboard_ = copySnapshot(roots);
    notify(roots.size() == 1 ? "Copied " + roots[0]->name : "Copied " + std::to_string(roots.size()) + " objects");
}

void Editor::cutSelection() {
    copySelection();
    deleteSelection();
}

void Editor::paste(bool intoSelection) {
    if (clipboard_.is_null() || clipboard_.empty()) return;
    Instance* parent = &scene().workspace();
    if (intoSelection && !selection_.empty()) {
        if (Instance* target = scene().find(primary())) parent = target;
    }
    nlohmann::json snapshot = freshIds(clipboard_);
    std::vector<PlacedSubtree> placement;
    std::vector<uint64_t> roots;
    for (const auto& e : snapshot) {
        if (e.contains("parent")) continue;
        placement.push_back({ parent->id, Scene::npos });
        roots.push_back(idFromString(e["id"].get<std::string>()));
    }
    execute(std::make_unique<CreateCommand>("Paste", std::move(snapshot), std::move(placement)));
    selection_ = roots;
}

void Editor::duplicateSelection() {
    std::vector<const Instance*> roots;
    std::vector<PlacedSubtree> placement;
    for (Instance* i : selectionRoots()) {
        roots.push_back(i);
        placement.push_back({ i->parent ? i->parent->id : 0, scene().siblingIndex(i) + 1 });
    }
    if (roots.empty()) return;
    nlohmann::json snapshot = freshIds(copySnapshot(roots));
    std::vector<uint64_t> newRoots;
    for (const auto& e : snapshot) if (!e.contains("parent")) newRoots.push_back(idFromString(e["id"].get<std::string>()));
    // Each copy goes right after its original in the Explorer.
    auto cmd = std::make_unique<CreateCommand>(roots.size() == 1 ? "Duplicate " + roots[0]->name : "Duplicate", std::move(snapshot), std::move(placement));
    execute(std::move(cmd));
    selection_ = newRoots;
}

// ---------------------------------------------------------------- hierarchy
void Editor::reparent(const std::vector<uint64_t>& ids, uint64_t parentId, size_t index) {
    Scene& s = scene();
    Instance* parent = s.find(parentId);
    if (!parent) return;
    std::vector<ReparentCommand::Move> moves;
    size_t at = index;
    for (uint64_t id : ids) {
        Instance* i = s.find(id);
        if (!i || i->isService()) continue;
        std::string error;
        if (!s.canParent(i, parent, &error)) { notify(error, NoticeLevel::Warning); continue; }
        ReparentCommand::Move m;
        m.id = id;
        m.oldParent = i->parent ? i->parent->id : 0;
        m.oldIndex = s.siblingIndex(i);
        m.newParent = parentId;
        // Moving within the same parent: account for the removal shifting indices.
        size_t target = at;
        if (target != Scene::npos && i->parent == parent && m.oldIndex < target) --target;
        m.newIndex = target;
        if (at != Scene::npos) ++at;
        m.oldLocal = TransformCommand::capture(*i);
        // Keep the world frame: local = inverse(new parent frame) * world frame.
        glm::mat4 newParentFrame(1.0f);
        for (Instance* p = parent; p; p = p->parent) if (p->hasTransform()) { newParentFrame = p->worldFrame(); break; }
        glm::vec3 pos, rot, scale;
        TransformUtil::decompose(glm::inverse(newParentFrame) * i->worldFrame(), pos, rot, scale);
        m.newLocal = { i->hasTransform() ? pos : i->position, i->hasTransform() ? rot : i->rotation, i->size };
        moves.push_back(m);
    }
    if (moves.empty()) return;
    execute(std::make_unique<ReparentCommand>(moves.size() == 1 ? "Move " + s.find(moves[0].id)->name : "Move objects", std::move(moves)));
}

void Editor::groupSelection() {
    std::vector<Instance*> roots = selectionRoots();
    if (roots.empty()) return;
    Scene& s = scene();
    Instance* parent = roots[0]->parent ? roots[0]->parent : &s.workspace();
    glm::vec3 lo, hi;
    const glm::vec3 center = boundsOf(roots, lo, hi) ? glm::vec3((lo.x + hi.x) * 0.5f, lo.y, (lo.z + hi.z) * 0.5f) : glm::vec3(0.0f);
    Instance* model = s.create(ClassId::Model);
    model->position = center;
    const uint64_t modelId = model->id;
    std::vector<uint64_t> ids;
    for (Instance* r : roots) ids.push_back(r->id);
    std::vector<std::unique_ptr<Command>> steps;
    auto create = commitNew(s, model, parent, "Group");
    create->execute(s); // so reparent can find the model
    steps.push_back(std::move(create));
    {
        Instance* m = s.find(modelId);
        std::vector<ReparentCommand::Move> moves;
        for (uint64_t id : ids) {
            Instance* i = s.find(id);
            if (!i) continue;
            ReparentCommand::Move mv;
            mv.id = id;
            mv.oldParent = i->parent ? i->parent->id : 0;
            mv.oldIndex = s.siblingIndex(i);
            mv.newParent = modelId;
            mv.newIndex = Scene::npos;
            mv.oldLocal = TransformCommand::capture(*i);
            glm::vec3 pos, rot, scale;
            TransformUtil::decompose(glm::inverse(m->worldFrame()) * i->worldFrame(), pos, rot, scale);
            mv.newLocal = { pos, rot, i->size };
            moves.push_back(mv);
        }
        auto move = std::make_unique<ReparentCommand>("Group", std::move(moves));
        move->execute(s);
        steps.push_back(std::move(move));
    }
    auto group = std::make_unique<CompositeCommand>(roots.size() == 1 ? "Group " + roots[0]->name : "Group " + std::to_string(roots.size()) + " objects", std::move(steps));
    record(std::move(group)); // already applied above
    selection_ = { modelId };
}

void Editor::ungroupSelection() {
    std::vector<uint64_t> containers;
    for (Instance* i : selectionRoots()) if (i->cls == ClassId::Model || i->cls == ClassId::Folder) containers.push_back(i->id);
    if (containers.empty()) return;
    Scene& s = scene();
    std::vector<std::unique_ptr<Command>> steps;
    std::vector<uint64_t> freed;
    for (uint64_t cid : containers) {
        Instance* c = s.find(cid);
        if (!c || !c->parent) continue;
        std::vector<ReparentCommand::Move> moves;
        const size_t index = s.siblingIndex(c);
        size_t at = index;
        for (Instance* child : std::vector<Instance*>(c->children)) {
            ReparentCommand::Move mv;
            mv.id = child->id;
            mv.oldParent = cid;
            mv.oldIndex = s.siblingIndex(child);
            mv.newParent = c->parent->id;
            mv.newIndex = at++;
            mv.oldLocal = TransformCommand::capture(*child);
            glm::mat4 parentFrame = c->parentWorldFrame();
            glm::vec3 pos, rot, scale;
            TransformUtil::decompose(glm::inverse(parentFrame) * child->worldFrame(), pos, rot, scale);
            mv.newLocal = { child->hasTransform() ? pos : child->position, child->hasTransform() ? rot : child->rotation, child->size };
            moves.push_back(mv);
            freed.push_back(child->id);
        }
        auto move = std::make_unique<ReparentCommand>("Ungroup", std::move(moves));
        move->execute(s);
        steps.push_back(std::move(move));
        auto del = std::make_unique<DeleteCommand>(std::vector<uint64_t>{ cid });
        del->execute(s);
        steps.push_back(std::move(del));
    }
    record(std::make_unique<CompositeCommand>("Ungroup", std::move(steps)));
    selection_ = freed;
}

void Editor::rename(uint64_t id, const std::string& name) {
    if (name.empty()) return;
    setProperty({ id }, "Name", name);
}

// ---------------------------------------------------------------- properties
void Editor::setProperty(const std::vector<uint64_t>& ids, const std::string& prop, const Value& value) {
    commitPreview();
    Scene& s = scene();
    std::vector<SetPropertyCommand::Change> changes;
    for (uint64_t id : ids) {
        Instance* i = s.find(id);
        if (!i) continue;
        const PropertyInfo* p = findProperty(i->cls, prop);
        if (!p || p->has(PF_ReadOnlyUI)) continue;
        const Value before = p->get(*i);
        Instance probe = *i;
        if (!rift::setProperty(probe, *p, value)) continue;
        const Value after = p->get(probe);
        if (!valuesEqual(before, after)) changes.push_back({ id, prop, before, after });
    }
    if (changes.empty()) return;
    const std::string label = (prop == "Name" ? "Rename " : "Change " + prop + " of ") + (changes.size() == 1 ? s.find(changes[0].id)->name : std::to_string(changes.size()) + " objects");
    execute(std::make_unique<SetPropertyCommand>(label, std::move(changes)));
}

void Editor::previewProperty(const std::vector<uint64_t>& ids, const std::string& prop, const Value& value) {
    std::vector<std::pair<uint64_t, Value>> values;
    for (uint64_t id : ids) values.emplace_back(id, value);
    previewValues(prop, values);
}

void Editor::previewValues(const std::string& prop, const std::vector<std::pair<uint64_t, Value>>& values) {
    Scene& s = scene();
    if (preview_ && preview_->prop != prop) commitPreview();
    if (!preview_) {
        preview_ = std::make_unique<Preview>();
        preview_->prop = prop;
    }
    for (const auto& [id, value] : values) {
        Instance* i = s.find(id);
        const PropertyInfo* p = i ? findProperty(i->cls, prop) : nullptr;
        if (!p) continue;
        // The value before the first preview is what undo goes back to.
        const bool known = std::any_of(preview_->changes.begin(), preview_->changes.end(), [&](const auto& c) { return c.id == id; });
        if (!known) preview_->changes.push_back({ id, prop, p->get(*i), {} });
        s.setProperty(*i, *p, value);
    }
}

void Editor::commitPreview() {
    if (!preview_) return;
    std::unique_ptr<Preview> p = std::move(preview_);
    Scene& s = scene();
    std::vector<SetPropertyCommand::Change> changes;
    for (auto& c : p->changes) {
        Instance* i = s.find(c.id);
        const PropertyInfo* prop = i ? findProperty(i->cls, c.prop) : nullptr;
        if (!prop) continue;
        c.after = prop->get(*i);
        if (!valuesEqual(c.before, c.after)) changes.push_back(c);
    }
    if (changes.empty()) return;
    const std::string label = "Change " + p->prop;
    record(std::make_unique<SetPropertyCommand>(label, std::move(changes)));
}

void Editor::setAttribute(const std::vector<uint64_t>& ids, const std::string& name, const Value& value) {
    std::vector<AttributeCommand::Change> changes;
    for (uint64_t id : ids) {
        Instance* i = scene().find(id);
        if (!i) continue;
        const Value* before = i->attribute(name);
        changes.push_back({ id, name, before ? *before : Value{}, value });
    }
    if (!changes.empty()) execute(std::make_unique<AttributeCommand>(typeOf(value) == ValueType::None ? "Remove attribute " + name : "Set attribute " + name, std::move(changes)));
}

void Editor::addTag(uint64_t id, const std::string& tag) {
    if (Instance* i = scene().find(id); i && !tag.empty() && !i->hasTag(tag)) execute(std::make_unique<TagCommand>(id, tag, true));
}

void Editor::removeTag(uint64_t id, const std::string& tag) {
    if (Instance* i = scene().find(id); i && i->hasTag(tag)) execute(std::make_unique<TagCommand>(id, tag, false));
}

void Editor::focusSelection() {
    std::vector<Instance*> list = selectedInstances();
    for (Instance*& i : list) if (i->isScript() && i->parent) i = i->parent;
    glm::vec3 lo, hi;
    if (list.empty() || !boundsOf(list, lo, hi)) return;
    camera_.frame((lo + hi) * 0.5f, std::max(glm::length(hi - lo) * 0.5f, 1.0f));
}

// ============================================================================
//  Scripts
// ============================================================================
void Editor::openScript(uint64_t scriptId, int line) {
    Instance* s = scene().find(scriptId);
    if (!s || !s->isScript()) return;
    if (s->source.empty()) {
        s->source = scripts_.uniquePath(s->name);
        scripts_.create(s->source, newScriptSource());
    }
    if (!scripts_.has(s->source) && !scripts_.load(s->source)) {
        scripts_.create(s->source, newScriptSource());
        notify(s->source + " was missing; a new file was started", NoticeLevel::Warning);
    }
    scriptRequests.push_back({ ScriptStore::normalize(s->source), scriptId, line });
}

// ============================================================================
//  Play
// ============================================================================
void Editor::play() {
    if (session_ || !hasScene()) return;
    commitPreview();
    endTerrainStroke("Terrain");
    if (settings_.saveBeforePlay && isDirty()) saveScene();
    if (settings_.clearOutputOnPlay) output_.clear();
    GameSession::Options options;
    options.audio = audio_;
    options.isStudio = true;
    const std::string gameName = project_.isOpen() ? project_.settings().name : sceneName();
    options.dataStoreFolder = Paths::userData() / "datastores" / Paths::fromUtf8(gameName);
    options.playerCount = std::clamp(settings_.playPlayers, 1, 8);
    session_ = std::make_unique<GameSession>(*scene_, scripts_, input_, options);
    lastSessionLog_ = 0;
    viewSide = NetworkSide::Client;
    addOutput(LogLevel::Info, "Game started", 0, "Game");
    session_->start();
}

void Editor::setViewPlayer(int index) {
    if (!session_) return;
    commitPreview();
    session_->setFocus(index);
    if (viewSide != NetworkSide::Client) setViewSide(NetworkSide::Client);
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [this](uint64_t id) { return !scene().find(id); }), selection_.end());
    if (const Instance* p = session_->player()) notify("Playing as " + p->name);
}

void Editor::setViewSide(NetworkSide side) {
    if (!session_ || side == viewSide) return;
    commitPreview();
    if (side == NetworkSide::Server) {
        // Start where the player's camera is, like Studio does.
        const GameCamera& cam = session_->camera();
        camera_.lookAt(cam.position, cam.position + cam.lookVector() * 10.0f);
    }
    viewSide = side;
    // Objects only one side has (the character's LocalScripts, server-only storage) drop out.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [this](uint64_t id) { return !scene().find(id); }), selection_.end());
    notify(side == NetworkSide::Server ? "Viewing the server" : "Viewing the client");
}

void Editor::stop() {
    if (!session_) return;
    session_.reset();
    addOutput(LogLevel::Info, "Game stopped", 0, "Game");
    // Selection may point at things that only existed in the running game.
    selection_.erase(std::remove_if(selection_.begin(), selection_.end(), [this](uint64_t id) { return !scene_->find(id); }), selection_.end());
}

} // namespace rift
