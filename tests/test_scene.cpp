// Instance tree, reflection and scene files.
#include "doctest.h"
#include "scene/Scene.h"
#include "scene/SceneIO.h"
#include "core/Paths.h"
#include "core/TransformUtil.h"

#include <filesystem>
#include <random>

using namespace rift;

namespace {
    float maxDiff(const glm::mat4& a, const glm::mat4& b) {
        float d = 0.0f;
        for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) d = std::max(d, std::abs(a[c][r] - b[c][r]));
        return d;
    }
    std::filesystem::path repoRoot() { return Paths::engineData().parent_path(); }
}

TEST_CASE("TransformUtil decompose inverts compose") {
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> ang(-179, 179), pos(-20, 20), scl(0.1f, 5.0f);
    float worst = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 p(pos(rng), pos(rng), pos(rng)), r(ang(rng), ang(rng) * 0.49f, ang(rng)), s(scl(rng), scl(rng), scl(rng));
        const glm::mat4 m = TransformUtil::compose(p, r, s);
        glm::vec3 p2, r2, s2;
        TransformUtil::decompose(m, p2, r2, s2);
        worst = std::max(worst, maxDiff(m, TransformUtil::compose(p2, r2, s2)));
    }
    CHECK(worst < 1e-3f);
}

TEST_CASE("a new scene has its services") {
    Scene scene;
    CHECK(scene.size() == kServiceCount + 2); // + StarterPlayerScripts, StarterCharacterScripts
    CHECK(scene.workspace().name == "Workspace");
    CHECK(scene.service(ClassId::Lighting)->isService());
    CHECK(scene.create(ClassId::Workspace) == nullptr); // services are unique
    scene.destroy(&scene.workspace());
    CHECK(scene.size() == kServiceCount + 2); // and cannot be destroyed
}

TEST_CASE("hierarchy rules") {
    Scene scene;
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    Instance* part = scene.create(ClassId::Part, model);
    REQUIRE(model);
    REQUIRE(part);
    CHECK(part->parent == model);
    CHECK(scene.isInTree(part));
    CHECK(part->isDescendantOf(&scene.workspace()));

    std::string error;
    CHECK_FALSE(scene.setParent(model, part, Scene::npos, &error)); // cycle
    CHECK_FALSE(error.empty());
    CHECK_FALSE(scene.setParent(&scene.lighting(), model));         // services stay put

    CHECK(scene.setParent(part, nullptr)); // detach (Parent = nil)
    CHECK_FALSE(scene.isInTree(part));
    CHECK(scene.find(part->id) == part);

    const uint64_t modelId = model->id;
    scene.setParent(part, model);
    scene.destroy(model);
    CHECK(scene.find(modelId) == nullptr);
    CHECK(scene.size() == kServiceCount + 2); // the part went with its model
}

TEST_CASE("sibling order is kept") {
    Scene scene;
    Instance* a = scene.create(ClassId::Part, &scene.workspace());
    Instance* b = scene.create(ClassId::Part, &scene.workspace());
    Instance* c = scene.create(ClassId::Part, &scene.workspace(), 0);
    scene.setParent(c, &scene.workspace(), 0);
    REQUIRE(scene.workspace().children.size() == 3);
    CHECK(scene.workspace().children[0] == c);
    CHECK(scene.workspace().children[1] == a);
    CHECK(scene.workspace().children[2] == b);
    CHECK(scene.siblingIndex(b) == 2);
}

TEST_CASE("size is not inherited by children") {
    Scene scene;
    Instance* parent = scene.create(ClassId::Part, &scene.workspace());
    parent->position = { 10, 0, 0 };
    parent->rotation = { 0, 90, 0 };
    parent->size = { 8, 1, 3 };
    Instance* child = scene.create(ClassId::Part, parent);
    child->position = { 0, 2, 0 };
    child->size = { 1, 1, 1 };
    const glm::vec3 wp = child->worldPosition();
    CHECK(wp.x == doctest::Approx(10.0f));
    CHECK(wp.y == doctest::Approx(2.0f)); // not 2 * parent size
    glm::vec3 p, r, s;
    TransformUtil::decompose(child->worldTransform(), p, r, s);
    CHECK(s.x == doctest::Approx(1.0f));
    CHECK(s.z == doctest::Approx(1.0f));

    // Setting a world transform keeps the world result.
    const glm::mat4 target = TransformUtil::compose({ 3, 4, 5 }, { 10, 20, 30 }, { 2, 3, 4 });
    child->setWorldTransform(target);
    CHECK(maxDiff(child->worldTransform(), target) < 1e-3f);
}

TEST_CASE("reflection: set with coercion, clamping and enums") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    const PropertyInfo* transparency = findProperty(ClassId::Part, "Transparency");
    REQUIRE(transparency);
    CHECK(scene.setProperty(*part, *transparency, 3)); // int -> float, clamped
    CHECK(part->transparency == doctest::Approx(1.0f));

    CHECK(scene.setProperty(*part, "Shape", std::string("Ball")));
    CHECK(part->shape == PartShape::Ball);
    std::string error;
    CHECK_FALSE(scene.setProperty(*part, "Shape", std::string("Banana"), &error));
    CHECK_FALSE(scene.setProperty(*part, "Size", std::string("big"), &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(scene.setProperty(*part, "NoSuchProperty", 1, &error));

    // Services keep their names.
    CHECK_FALSE(scene.setProperty(scene.workspace(), "Name", std::string("World")));
    CHECK(scene.workspace().name == "Workspace");

    // Every property's default round-trips through the getter/setter.
    for (int c = 0; c < kClassCount; ++c) {
        const ClassInfo& info = classInfo(static_cast<ClassId>(c));
        Instance probe;
        probe.cls = info.id;
        applyClassDefaults(probe);
        for (const PropertyInfo& p : info.props) {
            const Value v = p.get(probe);
            CHECK_MESSAGE(typeOf(v) == p.type, info.name << "." << p.name);
            if (!info.service || p.name != "Name") CHECK_MESSAGE(setProperty(probe, p, v), info.name << "." << p.name);
        }
    }
}

TEST_CASE("observers see changes") {
    struct Recorder : SceneObserver {
        int added = 0, removing = 0, changed = 0, reparented = 0;
        void onInstanceAdded(Instance&) override { ++added; }
        void onInstanceRemoving(Instance&) override { ++removing; }
        void onPropertyChanged(Instance&, const PropertyInfo&) override { ++changed; }
        void onParentChanged(Instance&, Instance*) override { ++reparented; }
    } rec;
    Scene scene;
    scene.addObserver(&rec);
    Instance* model = scene.create(ClassId::Model);
    scene.create(ClassId::Part, model);
    scene.create(ClassId::Part, model);
    CHECK(rec.added == 0);              // detached: not in the world yet
    scene.setParent(model, &scene.workspace());
    CHECK(rec.added == 3);              // model and both parts entered the world
    scene.setProperty(*model, "Position", glm::vec3(1, 2, 3));
    scene.setProperty(*model, "Position", glm::vec3(1, 2, 3)); // unchanged: no event
    CHECK(rec.changed == 1);
    scene.destroy(model);
    CHECK(rec.removing == 3);
    scene.removeObserver(&rec);
}

TEST_CASE("clone keeps ids and structure but is independent") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    part->name = "Floor";
    Instance* script = scene.create(ClassId::Script, part);
    auto copy = scene.clone();
    Instance* partCopy = copy->find(part->id);
    REQUIRE(partCopy);
    CHECK(partCopy != part);
    CHECK(partCopy->name == "Floor");
    CHECK(partCopy->parent == &copy->workspace());
    CHECK(copy->find(script->id)->parent == partCopy);
    CHECK(partCopy->scene == copy.get());
    copy->destroy(partCopy);
    CHECK(scene.find(part->id) == part); // original untouched
}

TEST_CASE("cloneSubtree gives new ids and skips non-archivable") {
    Scene scene;
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    Instance* keep = scene.create(ClassId::Part, model);
    Instance* skip = scene.create(ClassId::Part, model);
    skip->archivable = false;
    Instance* copy = scene.cloneSubtree(*model);
    REQUIRE(copy);
    CHECK(copy->id != model->id);
    CHECK(copy->parent == nullptr);
    REQUIRE(copy->children.size() == 1);
    CHECK(copy->children[0]->id != keep->id);
}

TEST_CASE("scene file round trip (format 4)") {
    Scene scene;
    scene.workspace().gravity = 42.0f;
    scene.lighting().clockTime = 18.5f;
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    model->name = "Tower";
    Instance* part = scene.create(ClassId::Part, model);
    part->name = "Block";
    part->position = { 1, 2, 3 };
    part->color = { 1, 0, 0 };
    part->material = Material::Neon;
    part->setAttribute("Health", 100.0f);
    part->setAttribute("Team", std::string("Red"));
    part->addTag("Enemy");
    Instance* gui = scene.create(ClassId::TextLabel, scene.create(ClassId::ScreenGui, scene.service(ClassId::StarterGui)));
    gui->text = "Score: 0";
    gui->guiPosition = { 0.5f, -100, 0, 20 };

    const nlohmann::json j = SceneIO::sceneToJson(scene);
    Scene loaded;
    const auto result = SceneIO::sceneFromJson(loaded, j);
    REQUIRE(result.ok);
    CHECK(result.warnings.empty());
    CHECK(loaded.workspace().gravity == doctest::Approx(42.0f));
    CHECK(loaded.lighting().clockTime == doctest::Approx(18.5f));
    Instance* p = loaded.find(part->id);
    REQUIRE(p);
    CHECK(p->parent->name == "Tower");
    CHECK(p->position == glm::vec3(1, 2, 3));
    CHECK(p->material == Material::Neon);
    CHECK(p->hasTag("Enemy"));
    REQUIRE(p->attribute("Health"));
    CHECK(std::get<float>(*p->attribute("Health")) == doctest::Approx(100.0f));
    Instance* g = loaded.find(gui->id);
    REQUIRE(g);
    CHECK(g->text == "Score: 0");
    CHECK(g->guiPosition == UDim2{ 0.5f, -100, 0, 20 });
    CHECK(SceneIO::sceneToText(loaded) == SceneIO::sceneToText(scene)); // stable output

    // Defaults are not written.
    const std::string text = SceneIO::sceneToText(scene);
    CHECK(text.find("\"Anchored\"") == std::string::npos);
}

TEST_CASE("unknown classes and properties are skipped with warnings") {
    nlohmann::json j = nlohmann::json::parse(R"({"format":4,"instances":[
        {"id":"0000000000000001","class":"Workspace"},
        {"id":"0000000000000002","class":"Spaceship","name":"X","parent":"0000000000000001"},
        {"id":"0000000000000003","class":"Part","name":"P","parent":"0000000000000001","props":{"Warp":9,"Size":"huge"}}
    ]})");
    Scene scene;
    const auto r = SceneIO::sceneFromJson(scene, j);
    REQUIRE(r.ok);
    CHECK(r.warnings.size() >= 2);
    CHECK(scene.workspace().children.size() == 1);
}

TEST_CASE("newer formats are refused") {
    Scene scene;
    const auto r = SceneIO::sceneFromJson(scene, nlohmann::json::parse(R"({"format":99,"instances":[]})"));
    CHECK_FALSE(r.ok);
    CHECK(r.error.find("newer") != std::string::npos);
}

TEST_CASE("subtree snapshots restore with the same ids") {
    Scene scene;
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    Instance* part = scene.create(ClassId::Part, model);
    const nlohmann::json snap = SceneIO::subtreesToJson({ model });
    const uint64_t modelId = model->id, partId = part->id;
    scene.destroy(model);
    const auto roots = SceneIO::subtreesFromJson(scene, snap, &scene.workspace(), true);
    REQUIRE(roots.size() == 1);
    CHECK(roots[0]->id == modelId);
    CHECK(scene.find(partId)->parent == roots[0]);

    // Without keepIds (paste) the copies get new ids.
    const auto pasted = SceneIO::subtreesFromJson(scene, snap, &scene.workspace(), false);
    REQUIRE(pasted.size() == 1);
    CHECK(pasted[0]->id != modelId);
}

TEST_CASE("every example scene loads (format 3 conversion)") {
    const auto examples = repoRoot() / "examples";
    REQUIRE(std::filesystem::exists(examples));
    int count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(examples)) {
        if (entry.path().filename() != "scene.json") continue;
        Scene scene;
        const auto r = SceneIO::loadScene(scene, entry.path());
        INFO(entry.path().string());
        CHECK(r.ok);
        CHECK(scene.workspace().children.size() > 0);
        ++count;
    }
    CHECK(count >= 9);
}

TEST_CASE("format 3: scripts, spawn points and transforms convert") {
    Scene scene;
    const auto r = SceneIO::loadScene(scene, repoRoot() / "examples/DemoObby/scenes/DemoObby/scene.json");
    REQUIRE(r.ok);
    int scripts = 0, spawns = 0;
    scene.forEachInTree([&](Instance& i) {
        if (i.cls == ClassId::Script) {
            ++scripts;
            CHECK(i.source.rfind("scripts/", 0) == 0);
        }
        if (i.cls == ClassId::SpawnLocation) ++spawns;
    });
    CHECK(scripts == 6);
    CHECK(spawns == 1);
    // Every script in the demo is attached to a part.
    scene.forEachInTree([&](Instance& i) {
        if (i.cls == ClassId::Script) CHECK(i.parent->isBasePart());
    });

    // A parentless format 3 script becomes a ScriptService script.
    Scene loose;
    const auto r2 = SceneIO::sceneFromJson(loose, nlohmann::json::parse(
        R"({"format":3,"objects":[{"class":"Script","id":"00000000000000aa","name":"Main","source":"scripts/Main.lua"}]})"));
    REQUIRE(r2.ok);
    CHECK(loose.service(ClassId::ScriptService)->children.size() == 1);
}
