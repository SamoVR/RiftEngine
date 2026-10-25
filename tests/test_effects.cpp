// Particles, beams and trails: the value types, files, scripts, cloning,
// the simulation (no GPU needed) and inserting them in the editor.
#include "doctest.h"
#include "editor/Editor.h"
#include "editor/EditorSettings.h"
#include "game/GameSession.h"
#include "platform/Input.h"
#include "render/Effects.h"
#include "scene/Reflection.h"
#include "scene/Scene.h"
#include "scene/SceneIO.h"
#include "script/ScriptStore.h"
#include "core/Paths.h"

#include <nlohmann/json.hpp>
#include <filesystem>

using namespace rift;
namespace fs = std::filesystem;

TEST_CASE("effects: sequences interpolate and survive JSON") {
    const NumberSequence s(std::vector<NumberKeypoint>{ { 0.0f, 0.0f, 0.0f }, { 0.5f, 10.0f, 1.0f }, { 1.0f, 0.0f, 0.0f } });
    CHECK(s.at(0.25f) == doctest::Approx(5.0f));
    CHECK(s.at(0.5f) == doctest::Approx(10.0f));
    CHECK(s.envelopeAt(0.75f) == doctest::Approx(0.5f));
    // Unsorted keys without ends are fixed up.
    const NumberSequence fixed(std::vector<NumberKeypoint>{ { 0.7f, 2.0f, 0.0f }, { 0.2f, 1.0f, 0.0f } });
    REQUIRE(fixed.keys.size() == 4);
    CHECK(fixed.keys.front().time == 0.0f);
    CHECK(fixed.keys.back().time == 1.0f);
    CHECK(fixed.at(0.1f) == doctest::Approx(1.0f));

    const ColorSequence c(Color3(1, 0, 0), Color3(0, 0, 1));
    CHECK(c.at(0.5f).r == doctest::Approx(0.5f));
    CHECK(c.at(0.5f).b == doctest::Approx(0.5f));

    for (const Value& v : { Value(NumberRange(1.0f, 3.0f)), Value(s), Value(c), Value(InstanceRef{ 0x1234abcdULL }), Value(InstanceRef{}) }) {
        const Value back = fromJson(toJson(v), typeOf(v));
        CHECK(valuesEqual(back, v));
        CHECK(valuesEqual(fromTaggedJson(toTaggedJson(v)), v));
    }
}

TEST_CASE("effects: emitters, beams and attachments save, load and clone with their links") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    Instance* a0 = scene.create(ClassId::Attachment, part);
    Instance* a1 = scene.create(ClassId::Attachment, part);
    a1->position = { 4, 0, 0 };
    Instance* beam = scene.create(ClassId::Beam, part);
    beam->attachment0.id = a0->id;
    beam->attachment1.id = a1->id;
    beam->width0 = 2.5f;
    Instance* em = scene.create(ClassId::ParticleEmitter, a0);
    em->lifetime = NumberRange(1.0f, 2.0f);
    em->colorSequence = ColorSequence(Color3(1, 0.5f, 0), Color3(0, 0, 1));
    em->sizeSequence = NumberSequence(3.0f, 0.5f);

    Scene copy;
    REQUIRE(SceneIO::sceneFromJson(copy, SceneIO::sceneToJson(scene)).ok);
    const Instance* beam2 = copy.find(beam->id);
    REQUIRE(beam2);
    CHECK(beam2->attachment0.id == a0->id);
    CHECK(beam2->attachment1.id == a1->id);
    CHECK(beam2->width0 == 2.5f);
    const Instance* em2 = copy.find(em->id);
    REQUIRE(em2);
    CHECK(em2->lifetime == NumberRange(1.0f, 2.0f));
    CHECK(em2->sizeSequence == em->sizeSequence);
    CHECK(em2->colorSequence == em->colorSequence);

    // A clone's beam points at the clone's attachments.
    Instance* clone = scene.cloneSubtree(*part);
    REQUIRE(clone);
    const Instance* cbeam = nullptr;
    std::vector<uint64_t> attachments;
    Scene::forEachDescendant(*clone, [&](Instance& i) {
        if (i.cls == ClassId::Beam) cbeam = &i;
        if (i.cls == ClassId::Attachment) attachments.push_back(i.id);
    });
    REQUIRE(cbeam);
    REQUIRE(attachments.size() == 2);
    CHECK(cbeam->attachment0.id == attachments[0]);
    CHECK(cbeam->attachment1.id == attachments[1]);
    CHECK(cbeam->attachment0.id != a0->id);

    // Attachments are positioned in their part's space.
    part->position = { 10, 5, 0 };
    CHECK(a1->worldPosition().x == doctest::Approx(14.0f));
    CHECK(a1->hasTransform());
}

TEST_CASE("effects: emitters spawn at their rate, on Emit, and Clear empties them") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    part->size = { 4, 1, 4 };
    Instance* em = scene.create(ClassId::ParticleEmitter, part);
    em->rate = 20.0f;
    em->lifetime = NumberRange(10.0f);
    em->speedRange = NumberRange(5.0f);
    Effects fx;
    const glm::vec3 eye(0, 5, 20), light(1.0f);
    for (int k = 0; k < 60; ++k) fx.update(scene, 1.0f / 60.0f, eye, light);
    CHECK(fx.particleCount() >= 18);
    CHECK(fx.particleCount() <= 21);
    CHECK(fx.vertexCount() == static_cast<size_t>(fx.particleCount()) * 6);

    em->enabled = false;
    em->emitRequests += 50; // ParticleEmitter:Emit(50)
    fx.update(scene, 1.0f / 60.0f, eye, light);
    const int afterEmit = fx.particleCount();
    CHECK(afterEmit >= 68);
    fx.update(scene, 1.0f / 60.0f, eye, light);
    CHECK(fx.particleCount() == afterEmit); // disabled: no more

    ++em->clearRequests; // :Clear()
    fx.update(scene, 1.0f / 60.0f, eye, light);
    CHECK(fx.particleCount() == 0);

    // Fully transparent particles are not drawn; a frozen frame (dt 0) adds nothing.
    em->enabled = true;
    fx.update(scene, 0.0f, eye, light);
    CHECK(fx.particleCount() == 0);
}

TEST_CASE("effects: beams and trails build geometry between their attachments") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    Instance* a0 = scene.create(ClassId::Attachment, part);
    Instance* a1 = scene.create(ClassId::Attachment, part);
    a0->position = { 0, 1, 0 };
    a1->position = { 0, -1, 0 };
    Instance* beam = scene.create(ClassId::Beam, part);
    beam->segments = 8;
    Effects fx;
    const glm::vec3 eye(0, 5, 20), light(1.0f);
    fx.update(scene, 0.016f, eye, light);
    CHECK_MESSAGE(fx.vertexCount() == 0, "no attachments"); // no attachments yet
    beam->attachment0.id = a0->id;
    beam->attachment1.id = a1->id;
    fx.update(scene, 0.016f, eye, light);
    CHECK(fx.vertexCount() == 8 * 6);
    beam->enabled = false;
    fx.update(scene, 0.016f, eye, light);
    CHECK_MESSAGE(fx.vertexCount() == 0, "disabled");

    // A trail grows as its part moves and fades after its lifetime.
    beam->enabled = false;
    Instance* trail = scene.create(ClassId::Trail, part);
    trail->attachment0.id = a0->id;
    trail->attachment1.id = a1->id;
    trail->trailLifetime = 0.5f;
    for (int k = 0; k < 20; ++k) {
        part->position.x = static_cast<float>(k);
        fx.update(scene, 0.016f, eye, light);
    }
    CHECK(fx.vertexCount() >= 10 * 6);
    for (int k = 0; k < 60; ++k) fx.update(scene, 0.016f, eye, light); // standing still for a second
    CHECK_MESSAGE(fx.vertexCount() == 0, "faded");
}

namespace {
    struct FxGame {
        Scene scene;
        ScriptStore store;
        Input input;
        fs::path folder;
        std::unique_ptr<GameSession> session;
        FxGame() {
            folder = fs::temp_directory_path() / ("rift_fx_game_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(folder);
            fs::create_directories(folder);
            store.reset(Paths::toUtf8(folder));
        }
        ~FxGame() {
            session.reset();
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
        bool printed(const std::string& text) const {
            for (const auto& line : session->logLines()) if (line.text == text) return true;
            return false;
        }
        std::string dump() const {
            std::string out;
            for (const auto& line : session->logLines()) out += line.text + "\n";
            return out;
        }
    };
}

TEST_CASE("effects: the Lua API (NumberSequence, ColorSequence, NumberRange, Emit, beams)") {
    FxGame g;
    g.store.create("scripts/Fx.lua", R"(
        local part = Instance.new("Part")
        part.Anchored = true
        part.Parent = workspace
        local a0 = Instance.new("Attachment", part)
        local a1 = Instance.new("Attachment", part)
        a1.Position = Vector3.new(0, 5, 0)
        local beam = Instance.new("Beam")
        beam.Attachment0 = a0
        beam.Attachment1 = a1
        beam.Parent = part
        print("linked", beam.Attachment0 == a0, beam.Attachment1 == a1)
        print("world", a1.WorldPosition.Y == part.Position.Y + 5)
        local ok = pcall(function() beam.Attachment0 = part end)
        print("part refused", not ok)
        beam.Attachment1 = nil
        print("cleared", beam.Attachment1 == nil)

        local e = Instance.new("ParticleEmitter", part)
        e.Size = NumberSequence.new({ NumberSequenceKeypoint.new(0, 1), NumberSequenceKeypoint.new(1, 4, 0.5) })
        e.Color = ColorSequence.new(Color3.new(1, 0, 0), Color3.new(0, 0, 1))
        e.Lifetime = NumberRange.new(2, 3)
        e.Transparency = 0.5
        print("size", #e.Size.Keypoints, e.Size.Keypoints[2].Value, e.Size.Keypoints[2].Envelope)
        print("color", e.Color.Keypoints[1].Value == Color3.new(1, 0, 0))
        print("range", e.Lifetime.Min, e.Lifetime.Max)
        print("fade", e.Transparency.Keypoints[1].Value)
        e:Emit(12)
        e:Clear()
        local copy = part:Clone()
        local cb = copy:FindFirstChildOfClass("Beam")
        print("clone", cb.Attachment0 ~= a0 and cb.Attachment0.Parent == copy)
    )");
    Instance* s = g.scene.create(ClassId::Script, g.scene.service(ClassId::ScriptService));
    s->source = "scripts/Fx.lua";
    g.session = std::make_unique<GameSession>(g.scene, g.store, g.input, GameSession::Options{});
    g.session->start();
    ViewportInput view;
    for (int k = 0; k < 10; ++k) {
        g.input.newFrame();
        g.session->update(1.0f / 60.0f, view);
    }
    CHECK_MESSAGE(g.printed("linked true true"), g.dump());
    CHECK(g.printed("world true"));
    CHECK(g.printed("part refused true"));
    CHECK(g.printed("cleared true"));
    CHECK(g.printed("size 2 4 0.5"));
    CHECK(g.printed("color true"));
    CHECK(g.printed("range 2 3"));
    CHECK(g.printed("fade 0.5"));
    CHECK(g.printed("clone true"));
    // The server's Emit reached the client's copy of the emitter.
    bool emitted = false;
    Scene::forEachDescendant(g.session->clientScene().workspace(), [&](Instance& i) {
        if (i.cls == ClassId::ParticleEmitter) emitted = i.emitRequests == 12 && i.clearRequests == 1;
    });
    CHECK(emitted);
}

TEST_CASE("effects: inserting beams and trails in the editor links new attachments") {
    EditorSettings settings;
    Input input;
    Editor editor(settings, input, nullptr);
    const fs::path parent = fs::temp_directory_path() / "rift-effects-editor";
    fs::remove_all(parent);
    fs::create_directories(parent);
    fs::path folder;
    REQUIRE(Project::create(parent, "Fx", Project::Template::Baseplate, &folder));
    REQUIRE(editor.openProject(folder));

    // Nothing selected: an attachment needs a part.
    CHECK(editor.insertEffect("Attachment") == nullptr);
    Instance* p1 = editor.insert(ClassId::Part);
    Instance* p2 = editor.insert(ClassId::Part);
    REQUIRE(p1);
    REQUIRE(p2);
    const uint64_t id1 = p1->id, id2 = p2->id;
    editor.setSelection({ id1, id2 });
    Instance* beam = editor.insertEffect("MagicBeam");
    REQUIRE(beam);
    CHECK(beam->cls == ClassId::Beam);
    const Instance* a0 = editor.scene().find(beam->attachment0.id);
    const Instance* a1 = editor.scene().find(beam->attachment1.id);
    REQUIRE(a0);
    REQUIRE(a1);
    CHECK(a0->parent->id == id1);
    CHECK(a1->parent->id == id2);
    const uint64_t beamId = beam->id;
    editor.undo(); // one step removes the beam and both attachments
    CHECK_FALSE(editor.scene().find(beamId));
    CHECK(editor.scene().find(id1)->children.empty());
    editor.redo();
    REQUIRE(editor.scene().find(beamId));

    // Copy + paste the part: the pasted beam uses the pasted attachments.
    editor.setSelection({ id1 });
    editor.copySelection();
    editor.paste();
    REQUIRE(editor.selection().size() == 1);
    const Instance* pasted = editor.scene().find(editor.selection()[0]);
    REQUIRE(pasted);
    const Instance* pastedBeam = nullptr;
    for (const Instance* c : pasted->children) if (c->cls == ClassId::Beam) pastedBeam = c;
    REQUIRE(pastedBeam);
    const Instance* pastedA0 = editor.scene().find(pastedBeam->attachment0.id);
    REQUIRE(pastedA0);
    CHECK(pastedA0->parent == pasted);

    // An emitter with nothing selected gets its own invisible part.
    editor.clearSelection();
    Instance* fire = editor.insertEffect("Fire");
    REQUIRE(fire);
    CHECK(fire->cls == ClassId::ParticleEmitter);
    CHECK(fire->lightEmission > 0.5f);
    REQUIRE(fire->parent);
    CHECK(fire->parent->isBasePart());
    CHECK(fire->parent->transparency == 1.0f);

    editor.closeProject();
    std::error_code ec;
    fs::remove_all(parent, ec);
}
