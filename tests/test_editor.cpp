// Editor operations (no window needed): undo/redo, copy/paste, grouping,
// reparenting, property previews, projects and saving.
#include "doctest.h"
#include "editor/Editor.h"
#include "editor/EditorSettings.h"
#include "platform/Input.h"
#include "core/Paths.h"
#include "scene/SceneIO.h"

#include <glm/gtc/epsilon.hpp>
#include <filesystem>
#include <fstream>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) { return glm::all(glm::epsilonEqual(a, b, eps)); }

    // A fresh project folder in the temp directory, removed afterwards.
    struct TempProject {
        fs::path parent;
        fs::path folder;
        explicit TempProject(Project::Template t = Project::Template::Baseplate) {
            parent = fs::temp_directory_path() / ("rift-editor-test-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(parent);
            fs::create_directories(parent);
            std::string error;
            REQUIRE(Project::create(parent, "Test Game", t, &folder, &error));
        }
        ~TempProject() {
            std::error_code ec;
            fs::remove_all(parent, ec);
        }
    };

    struct Fixture {
        EditorSettings settings;
        Input input;
        Editor editor{ settings, input, nullptr };
        TempProject project;
        Fixture() { REQUIRE(editor.openProject(project.folder)); }
        Instance* find(const std::string& name) {
            Instance* found = nullptr;
            editor.scene().forEachInTree([&](Instance& i) { if (!found && i.name == name) found = &i; });
            return found;
        }
    };
}

TEST_CASE("insert, undo and redo keep the same instance id") {
    Fixture f;
    const size_t before = f.editor.scene().size();
    Instance* part = f.editor.insert(ClassId::Part);
    REQUIRE(part);
    const uint64_t id = part->id;
    CHECK(f.editor.scene().size() == before + 1);
    CHECK(f.editor.primary() == id);
    CHECK(f.editor.isDirty());

    f.editor.undo();
    CHECK(f.editor.scene().find(id) == nullptr);
    CHECK(f.editor.scene().size() == before);
    f.editor.redo();
    REQUIRE(f.editor.scene().find(id));
    CHECK(f.editor.scene().find(id)->parent == &f.editor.scene().workspace());
}

TEST_CASE("shaped parts get sensible sizes") {
    Fixture f;
    Instance* ball = f.editor.insert(ClassId::Part, PartShape::Ball);
    REQUIRE(ball);
    CHECK(ball->shape == PartShape::Ball);
    CHECK(ball->size.x == ball->size.y);
    CHECK(ball->size.y == ball->size.z);
}

TEST_CASE("delete and undo restore a subtree with its script") {
    Fixture f;
    Instance* part = f.editor.insertTemplate("KillBrick");
    REQUIRE(part);
    REQUIRE(part->children.size() == 1);
    const uint64_t partId = part->id, scriptId = part->children[0]->id;
    const std::string source = part->children[0]->source;
    CHECK(f.editor.scripts().has(source));

    f.editor.setSelection({ partId });
    f.editor.deleteSelection();
    CHECK(f.editor.scene().find(partId) == nullptr);
    CHECK(f.editor.scene().find(scriptId) == nullptr);
    f.editor.undo();
    REQUIRE(f.editor.scene().find(partId));
    REQUIRE(f.editor.scene().find(scriptId));
    CHECK(f.editor.scene().find(scriptId)->parent->id == partId);
    CHECK(f.editor.scene().find(scriptId)->source == source);
}

TEST_CASE("duplicate copies scripts into their own files") {
    Fixture f;
    Instance* part = f.editor.insertTemplate("Spinner");
    REQUIRE(part);
    const std::string original = part->children[0]->source;
    f.editor.setSelection({ part->id });
    f.editor.duplicateSelection();
    REQUIRE(f.editor.selection().size() == 1);
    Instance* copy = f.editor.scene().find(f.editor.primary());
    REQUIRE(copy);
    CHECK(copy->id != part->id);
    CHECK(copy->name == part->name);
    REQUIRE(copy->children.size() == 1);
    const std::string copied = copy->children[0]->source;
    CHECK(copied != original);
    REQUIRE(f.editor.scripts().find(copied));
    CHECK(f.editor.scripts().find(copied)->text == f.editor.scripts().find(original)->text);
    // Right after the original in the Explorer.
    CHECK(f.editor.scene().siblingIndex(copy) == f.editor.scene().siblingIndex(part) + 1);
}

TEST_CASE("copy and paste make new instances") {
    Fixture f;
    Instance* part = f.editor.insert(ClassId::Part);
    part->name = "Original";
    f.editor.copySelection();
    CHECK(f.editor.canPaste());
    f.editor.paste();
    Instance* pasted = f.editor.scene().find(f.editor.primary());
    REQUIRE(pasted);
    CHECK(pasted != part);
    CHECK(pasted->name == "Original");
    f.editor.paste();
    CHECK(f.editor.scene().find(f.editor.primary()) != pasted);
}

TEST_CASE("group and ungroup keep world positions") {
    Fixture f;
    Instance* a = f.editor.insert(ClassId::Part);
    Instance* b = f.editor.insert(ClassId::Part);
    REQUIRE(a);
    REQUIRE(b);
    a->position = { 10, 2, 0 };
    b->position = { -6, 3, 4 };
    const uint64_t ida = a->id, idb = b->id;
    f.editor.setSelection({ ida, idb });
    f.editor.groupSelection();
    Instance* model = f.editor.scene().find(f.editor.primary());
    REQUIRE(model);
    CHECK(model->cls == ClassId::Model);
    Scene& s = f.editor.scene();
    CHECK(s.find(ida)->parent == model);
    CHECK(near(s.find(ida)->worldPosition(), { 10, 2, 0 }));
    CHECK(near(s.find(idb)->worldPosition(), { -6, 3, 4 }));

    // One undo step takes it all back.
    f.editor.undo();
    CHECK(s.find(model->id) == nullptr);
    CHECK(s.find(ida)->parent == &s.workspace());
    CHECK(near(s.find(ida)->worldPosition(), { 10, 2, 0 }));
    f.editor.redo();
    Instance* again = s.find(ida)->parent;
    REQUIRE(again);
    CHECK(again->cls == ClassId::Model);

    f.editor.setSelection({ again->id });
    f.editor.ungroupSelection();
    CHECK(s.find(ida)->parent == &s.workspace());
    CHECK(near(s.find(idb)->worldPosition(), { -6, 3, 4 }));
}

TEST_CASE("reparenting keeps the world transform and undoes") {
    Fixture f;
    Scene& s = f.editor.scene();
    Instance* model = f.editor.insert(ClassId::Model);
    REQUIRE(model);
    model->position = { 5, 0, 5 };
    model->rotation = { 0, 90, 0 };
    f.editor.clearSelection(); // otherwise the part goes inside the selected model
    Instance* part = f.editor.insert(ClassId::Part);
    CHECK(part->parent == &s.workspace());
    part->position = { 1, 2, 3 };
    const uint64_t id = part->id;
    f.editor.reparent({ id }, model->id);
    CHECK(s.find(id)->parent == model);
    CHECK(near(s.find(id)->worldPosition(), { 1, 2, 3 }));
    f.editor.undo();
    CHECK(s.find(id)->parent == &s.workspace());
    CHECK(near(s.find(id)->position, { 1, 2, 3 }));
}

TEST_CASE("a service cannot be moved and a part cannot go inside itself") {
    Fixture f;
    Scene& s = f.editor.scene();
    Instance* a = f.editor.insert(ClassId::Model);
    f.editor.clearSelection();
    Instance* b = f.editor.insert(ClassId::Part);
    f.editor.reparent({ b->id }, a->id);
    f.editor.reparent({ a->id }, b->id); // cycle: refused
    CHECK(s.find(a->id)->parent == &s.workspace());
    f.editor.reparent({ s.lighting().id }, a->id);
    CHECK(s.lighting().parent == nullptr);
}

TEST_CASE("property previews become one undo step") {
    Fixture f;
    Instance* part = f.editor.insert(ClassId::Part);
    const glm::vec3 start = part->size;
    for (int i = 1; i <= 5; ++i) f.editor.previewProperty({ part->id }, "Size", glm::vec3(4.0f + i, 1.0f, 2.0f));
    CHECK(f.editor.previewing());
    f.editor.commitPreview();
    CHECK(near(part->size, { 9, 1, 2 }));
    f.editor.undo();
    CHECK(near(f.editor.scene().find(part->id)->size, start));
}

TEST_CASE("per-instance previews change one axis of each") {
    Fixture f;
    Instance* a = f.editor.insert(ClassId::Part);
    Instance* b = f.editor.insert(ClassId::Part);
    a->position = { 1, 1, 1 };
    b->position = { 7, 8, 9 };
    f.editor.previewValues("Position", { { a->id, glm::vec3(1, 5, 1) }, { b->id, glm::vec3(7, 5, 9) } });
    f.editor.commitPreview();
    CHECK(near(a->position, { 1, 5, 1 }));
    CHECK(near(b->position, { 7, 5, 9 }));
    f.editor.undo();
    CHECK(near(f.editor.scene().find(b->id)->position, { 7, 8, 9 }));
}

TEST_CASE("setProperty on several instances and invalid values") {
    Fixture f;
    Instance* a = f.editor.insert(ClassId::Part);
    Instance* b = f.editor.insert(ClassId::Part);
    f.editor.setProperty({ a->id, b->id }, "Transparency", 0.5f);
    CHECK(a->transparency == doctest::Approx(0.5f));
    CHECK(b->transparency == doctest::Approx(0.5f));
    f.editor.setProperty({ a->id }, "Transparency", 7.0f); // clamped to 1
    CHECK(a->transparency == doctest::Approx(1.0f));
    f.editor.undo();
    f.editor.undo();
    CHECK(f.editor.scene().find(b->id)->transparency == doctest::Approx(0.0f));
}

TEST_CASE("attributes and tags are undoable") {
    Fixture f;
    Instance* part = f.editor.insert(ClassId::Part);
    f.editor.setAttribute({ part->id }, "Speed", 12.0f);
    REQUIRE(part->attribute("Speed"));
    f.editor.addTag(part->id, "Lava");
    CHECK(part->hasTag("Lava"));
    f.editor.undo();
    CHECK_FALSE(f.editor.scene().find(part->id)->hasTag("Lava"));
    f.editor.undo();
    CHECK(f.editor.scene().find(part->id)->attribute("Speed") == nullptr);
}

TEST_CASE("GUI elements go into a ScreenGui in StarterGui") {
    Fixture f;
    f.editor.clearSelection();
    Instance* label = f.editor.insert(ClassId::TextLabel);
    REQUIRE(label);
    REQUIRE(label->parent);
    CHECK(label->parent->cls == ClassId::ScreenGui);
    CHECK(label->parent->parent == f.editor.scene().service(ClassId::StarterGui));
}

TEST_CASE("saving writes the scene and its scripts, and clears the dirty flag") {
    Fixture f;
    Instance* part = f.editor.insertTemplate("Coin");
    REQUIRE(part);
    const std::string source = part->children[0]->source;
    CHECK(f.editor.isDirty());
    REQUIRE(f.editor.saveScene());
    CHECK_FALSE(f.editor.isDirty());
    const fs::path sceneDir = f.editor.scenePath().parent_path();
    CHECK(fs::exists(sceneDir / source));

    // Reopen: everything is still there.
    REQUIRE(f.editor.openScene(f.editor.scenePath()));
    CHECK(f.find("Coin"));
    CHECK(f.editor.scripts().has(source));
}

TEST_CASE("undoing an insert does not write its script") {
    Fixture f;
    Instance* script = f.editor.insert(ClassId::Script);
    REQUIRE(script);
    const std::string source = script->source;
    f.editor.undo();
    REQUIRE(f.editor.saveScene());
    CHECK_FALSE(fs::exists(f.editor.scenePath().parent_path() / source));
}

TEST_CASE("new scene, save as and reopening") {
    Fixture f;
    REQUIRE(f.editor.newScene("Second"));
    CHECK(f.editor.sceneName() == "Second");
    CHECK_FALSE(f.editor.newScene("Second")); // exists
    f.editor.insert(ClassId::Part);
    REQUIRE(f.editor.saveSceneAs("Third"));
    CHECK(f.editor.sceneName() == "Third");
    CHECK(f.editor.project().scenes().size() == 3);
}

TEST_CASE("playing works on a copy of the scene") {
    Fixture f;
    Instance* part = f.editor.insert(ClassId::Part);
    const uint64_t id = part->id;
    f.editor.play();
    REQUIRE(f.editor.playing());
    CHECK(&f.editor.scene() != &f.editor.editScene());
    f.editor.scene().destroy(f.editor.scene().find(id));
    f.editor.session()->update(1.0f / 60.0f, ViewportInput{});
    f.editor.stop();
    CHECK_FALSE(f.editor.playing());
    CHECK(f.editor.scene().find(id));
}

TEST_CASE("projects: templates and settings round trip") {
    TempProject p(Project::Template::Obby);
    Project project;
    std::string error;
    REQUIRE(project.load(p.folder, &error));
    CHECK(project.settings().name == "Test Game");
    CHECK(fs::exists(project.startScenePath()));
    CHECK(project.scenes().size() == 1);
    project.settings().author = "Someone";
    project.settings().windowWidth = 1920;
    REQUIRE(project.save(&error));
    Project again;
    REQUIRE(again.load(p.folder, &error));
    CHECK(again.settings().author == "Someone");
    CHECK(again.settings().windowWidth == 1920);
    CHECK(Project::findRoot(project.startScenePath()) == fs::absolute(p.folder));
    // Creating over an existing project fails.
    CHECK_FALSE(Project::create(p.parent, "Test Game", Project::Template::Empty, nullptr, &error));
}
