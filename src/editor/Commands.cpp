#include "editor/Commands.h"
#include "scene/SceneIO.h"
#include "core/Log.h"

#include <algorithm>

namespace rift {

// ============================================================================
//  History
// ============================================================================
void CommandHistory::execute(Scene& scene, std::unique_ptr<Command> command) {
    command->execute(scene);
    record(std::move(command));
}

void CommandHistory::record(std::unique_ptr<Command> command) {
    undo_.push_back(std::move(command));
    version_ = ++counter_;
    undoVersions_.push_back(version_);
    redo_.clear();
    redoVersions_.clear();
    if (undo_.size() > kMaxSteps) {
        undo_.erase(undo_.begin());
        undoVersions_.erase(undoVersions_.begin());
    }
}

bool CommandHistory::undo(Scene& scene) {
    if (undo_.empty()) return false;
    std::unique_ptr<Command> c = std::move(undo_.back());
    undo_.pop_back();
    redoVersions_.push_back(undoVersions_.back());
    undoVersions_.pop_back();
    c->undo(scene);
    redo_.push_back(std::move(c));
    // Back to the version the scene had before that step (0 = as loaded).
    version_ = undoVersions_.empty() ? 0 : undoVersions_.back();
    return true;
}

bool CommandHistory::redo(Scene& scene) {
    if (redo_.empty()) return false;
    std::unique_ptr<Command> c = std::move(redo_.back());
    redo_.pop_back();
    c->execute(scene);
    undo_.push_back(std::move(c));
    version_ = redoVersions_.back();
    undoVersions_.push_back(version_);
    redoVersions_.pop_back();
    return true;
}

void CommandHistory::clear() {
    undo_.clear();
    redo_.clear();
    undoVersions_.clear();
    redoVersions_.clear();
    version_ = savedVersion_ = 0;
}

// ============================================================================
//  Create / delete
// ============================================================================
CreateCommand::CreateCommand(std::string label, nlohmann::json snapshot, std::vector<PlacedSubtree> placement)
    : snapshot_(std::move(snapshot)), placement_(std::move(placement)) {
    label_ = std::move(label);
}

void CreateCommand::execute(Scene& scene) {
    // Re-create detached (same ids), then place each root.
    const std::vector<Instance*> roots = SceneIO::subtreesFromJson(scene, snapshot_, nullptr, true);
    rootIds_.clear();
    for (size_t i = 0; i < roots.size(); ++i) {
        const PlacedSubtree p = i < placement_.size() ? placement_[i] : PlacedSubtree{};
        Instance* parent = p.parent ? scene.find(p.parent) : &scene.workspace();
        if (!parent) parent = &scene.workspace();
        scene.setParent(roots[i], parent, p.index);
        rootIds_.push_back(roots[i]->id);
    }
}

void CreateCommand::undo(Scene& scene) {
    for (uint64_t id : rootIds_) if (Instance* i = scene.find(id)) scene.destroy(i);
}

DeleteCommand::DeleteCommand(std::vector<uint64_t> ids) : ids_(std::move(ids)) {
    label_ = ids_.size() == 1 ? "Delete" : "Delete " + std::to_string(ids_.size()) + " objects";
}

void DeleteCommand::execute(Scene& scene) {
    // Only the topmost of the selected instances are deleted (descendants go with them).
    std::vector<Instance*> roots;
    for (uint64_t id : ids_) {
        Instance* i = scene.find(id);
        if (!i || i->isService()) continue;
        bool ancestorListed = false;
        for (Instance* a = i->parent; a && !ancestorListed; a = a->parent) {
            ancestorListed = std::find(ids_.begin(), ids_.end(), a->id) != ids_.end();
        }
        if (!ancestorListed) roots.push_back(i);
    }
    snapshots_.clear();
    placement_.clear();
    for (Instance* r : roots) {
        snapshots_.push_back(SceneIO::subtreesToJson({ r }));
        placement_.push_back({ r->parent ? r->parent->id : 0, scene.siblingIndex(r) });
    }
    if (roots.size() == 1) label_ = "Delete " + roots[0]->name;
    for (Instance* r : roots) scene.destroy(r);
}

void DeleteCommand::undo(Scene& scene) {
    // Restore in reverse so sibling indices are valid again.
    for (size_t k = snapshots_.size(); k-- > 0;) {
        const auto roots = SceneIO::subtreesFromJson(scene, snapshots_[k], nullptr, true);
        if (roots.empty()) continue;
        Instance* parent = placement_[k].parent ? scene.find(placement_[k].parent) : nullptr;
        if (!parent) parent = &scene.workspace();
        scene.setParent(roots[0], parent, placement_[k].index);
    }
}

// ============================================================================
//  Properties
// ============================================================================
SetPropertyCommand::SetPropertyCommand(std::string label, std::vector<Change> changes) : changes_(std::move(changes)) {
    label_ = std::move(label);
}

void SetPropertyCommand::execute(Scene& scene) {
    for (const Change& c : changes_) if (Instance* i = scene.find(c.id)) scene.setProperty(*i, c.prop, c.after);
}

void SetPropertyCommand::undo(Scene& scene) {
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) if (Instance* i = scene.find(it->id)) scene.setProperty(*i, it->prop, it->before);
}

AttributeCommand::AttributeCommand(std::string label, std::vector<Change> changes) : changes_(std::move(changes)) {
    label_ = std::move(label);
}

void AttributeCommand::execute(Scene& scene) {
    for (const Change& c : changes_) if (Instance* i = scene.find(c.id)) scene.setAttribute(*i, c.name, c.after);
}

void AttributeCommand::undo(Scene& scene) {
    for (auto it = changes_.rbegin(); it != changes_.rend(); ++it) if (Instance* i = scene.find(it->id)) scene.setAttribute(*i, it->name, it->before);
}

TagCommand::TagCommand(uint64_t id, std::string tag, bool add) : id_(id), tag_(std::move(tag)), add_(add) {
    label_ = (add ? "Add tag " : "Remove tag ") + tag_;
}

void TagCommand::execute(Scene& scene) {
    if (Instance* i = scene.find(id_)) { add_ ? i->addTag(tag_) : i->removeTag(tag_); scene.bumpRevision(); }
}

void TagCommand::undo(Scene& scene) {
    if (Instance* i = scene.find(id_)) { add_ ? i->removeTag(tag_) : i->addTag(tag_); scene.bumpRevision(); }
}

// ============================================================================
//  Transforms and hierarchy
// ============================================================================
TransformCommand::TransformCommand(std::string label, std::vector<Change> changes) : changes_(std::move(changes)) {
    label_ = std::move(label);
}

void TransformCommand::apply(Scene& scene, uint64_t id, const State& s) {
    Instance* i = scene.find(id);
    if (!i) return;
    i->position = s.position;
    i->rotation = s.rotation;
    i->size = s.size;
    for (const char* name : { "Position", "Rotation", "Size" }) {
        if (const PropertyInfo* p = findProperty(i->cls, name)) scene.notifyPropertyChanged(*i, *p);
    }
}

void TransformCommand::execute(Scene& scene) { for (const Change& c : changes_) apply(scene, c.id, c.after); }
void TransformCommand::undo(Scene& scene) { for (const Change& c : changes_) apply(scene, c.id, c.before); }

ReparentCommand::ReparentCommand(std::string label, std::vector<Move> moves) : moves_(std::move(moves)) {
    label_ = std::move(label);
}

void ReparentCommand::execute(Scene& scene) {
    for (const Move& m : moves_) {
        Instance* i = scene.find(m.id);
        Instance* p = m.newParent ? scene.find(m.newParent) : nullptr;
        if (!i || !p) continue;
        scene.setParent(i, p, m.newIndex);
        i->position = m.newLocal.position;
        i->rotation = m.newLocal.rotation;
        i->size = m.newLocal.size;
    }
}

void ReparentCommand::undo(Scene& scene) {
    for (auto it = moves_.rbegin(); it != moves_.rend(); ++it) {
        Instance* i = scene.find(it->id);
        Instance* p = it->oldParent ? scene.find(it->oldParent) : nullptr;
        if (!i || !p) continue;
        scene.setParent(i, p, it->oldIndex);
        i->position = it->oldLocal.position;
        i->rotation = it->oldLocal.rotation;
        i->size = it->oldLocal.size;
    }
}

// ============================================================================
TerrainCommand::TerrainCommand(std::string label, TerrainData::Snapshot before, TerrainData::Snapshot after, std::vector<ColorChange> colors)
    : before_(std::move(before)), after_(std::move(after)), colors_(std::move(colors)) {
    label_ = std::move(label);
}

void TerrainCommand::execute(Scene& scene) {
    scene.terrain().restore(after_);
    for (const ColorChange& c : colors_) scene.terrain().setMaterialColor(c.material, c.after);
}

void TerrainCommand::undo(Scene& scene) {
    scene.terrain().restore(before_);
    for (const ColorChange& c : colors_) scene.terrain().setMaterialColor(c.material, c.before);
}

// ============================================================================
CompositeCommand::CompositeCommand(std::string label, std::vector<std::unique_ptr<Command>> commands) : commands_(std::move(commands)) {
    label_ = std::move(label);
}

void CompositeCommand::execute(Scene& scene) { for (auto& c : commands_) c->execute(scene); }
void CompositeCommand::undo(Scene& scene) { for (auto it = commands_.rbegin(); it != commands_.rend(); ++it) (*it)->undo(scene); }

} // namespace rift
