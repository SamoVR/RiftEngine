#pragma once
//
// Commands.h -- undo / redo for the editor.
//
// Commands never hold Instance pointers: they record instance ids and, for
// creating / deleting, a JSON snapshot of the subtree (SceneIO). Whatever
// happened in between (deletes, reloads of the scene), an undo step finds
// its instances by id or recreates them with the same ids.
//
#include "scene/Scene.h"
#include "scene/Value.h"

#include <nlohmann/json.hpp>
#include <memory>
#include <string>
#include <vector>

namespace rift {

class Command {
public:
    virtual ~Command() = default;
    virtual void execute(Scene& scene) = 0;
    virtual void undo(Scene& scene) = 0;
    const std::string& label() const { return label_; }
protected:
    std::string label_;
};

class CommandHistory {
public:
    static constexpr size_t kMaxSteps = 500;

    // Runs the command and records it (clears the redo list).
    void execute(Scene& scene, std::unique_ptr<Command> command);
    // Records a command whose effect was already applied (e.g. a gizmo drag).
    void record(std::unique_ptr<Command> command);
    bool undo(Scene& scene);
    bool redo(Scene& scene);
    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back()->label(); }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back()->label(); }
    void clear();

    // Dirty tracking: the change counter moves with every execute/undo/redo.
    uint64_t version() const { return version_; }
    void markSaved() { savedVersion_ = version_; }
    bool isDirty() const { return version_ != savedVersion_; }

private:
    std::vector<std::unique_ptr<Command>> undo_, redo_;
    uint64_t version_ = 0, savedVersion_ = 0, counter_ = 0;
    std::vector<uint64_t> undoVersions_, redoVersions_;
};

// ---------------------------------------------------------------- creating / deleting
// The roots of the snapshot and where each one goes.
struct PlacedSubtree {
    uint64_t parent = 0;
    size_t index = Scene::npos;
};

class CreateCommand : public Command {
public:
    // `snapshot` = SceneIO::subtreesToJson(roots); placement per root.
    CreateCommand(std::string label, nlohmann::json snapshot, std::vector<PlacedSubtree> placement);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    const std::vector<uint64_t>& rootIds() const { return rootIds_; }
private:
    nlohmann::json snapshot_;
    std::vector<PlacedSubtree> placement_;
    std::vector<uint64_t> rootIds_;
};

class DeleteCommand : public Command {
public:
    explicit DeleteCommand(std::vector<uint64_t> ids);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    std::vector<uint64_t> ids_;
    std::vector<nlohmann::json> snapshots_;
    std::vector<PlacedSubtree> placement_;
};

// ---------------------------------------------------------------- properties
class SetPropertyCommand : public Command {
public:
    struct Change { uint64_t id; std::string prop; Value before, after; };
    SetPropertyCommand(std::string label, std::vector<Change> changes);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    std::vector<Change> changes_;
};

class AttributeCommand : public Command {
public:
    struct Change { uint64_t id; std::string name; Value before, after; };
    AttributeCommand(std::string label, std::vector<Change> changes);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    std::vector<Change> changes_;
};

class TagCommand : public Command {
public:
    TagCommand(uint64_t id, std::string tag, bool add);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    uint64_t id_;
    std::string tag_;
    bool add_;
};

// Position / rotation / size of several instances at once (gizmo drags,
// nudges, alignment tools).
class TransformCommand : public Command {
public:
    struct State { glm::vec3 position, rotation, size; };
    struct Change { uint64_t id; State before, after; };
    TransformCommand(std::string label, std::vector<Change> changes);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
    static State capture(const Instance& i) { return { i.position, i.rotation, i.size }; }
private:
    std::vector<Change> changes_;
    static void apply(Scene& scene, uint64_t id, const State& s);
};

// Moves instances to a new parent / position, keeping them where they are in
// the world (their local transform is recomputed for the new parent).
class ReparentCommand : public Command {
public:
    struct Move {
        uint64_t id;
        uint64_t oldParent; size_t oldIndex;
        uint64_t newParent; size_t newIndex;
        TransformCommand::State oldLocal, newLocal;
    };
    ReparentCommand(std::string label, std::vector<Move> moves);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    std::vector<Move> moves_;
};

class CompositeCommand : public Command {
public:
    CompositeCommand(std::string label, std::vector<std::unique_ptr<Command>> commands);
    void execute(Scene& scene) override;
    void undo(Scene& scene) override;
private:
    std::vector<std::unique_ptr<Command>> commands_;
};

} // namespace rift
