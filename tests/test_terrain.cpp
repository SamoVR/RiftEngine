// Voxel terrain: shapes, meshes, ray casts, files, brushes, physics, scripts.
#include "doctest.h"
#include "scene/Terrain.h"
#include "scene/Scene.h"
#include "scene/SceneIO.h"
#include "game/GameSession.h"
#include "physics/PhysicsWorld.h"
#include "script/ScriptStore.h"
#include "platform/Input.h"
#include "core/Paths.h"

#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>
#include <filesystem>

using namespace rift;
namespace fs = std::filesystem;

TEST_CASE("terrain: a ball of grass is solid inside, air outside, and meshes round") {
    TerrainData t;
    CHECK(t.empty());
    t.fillBall({ 0, 0, 0 }, 10.0f, Material::Grass);
    CHECK_FALSE(t.empty());
    CHECK(t.density({ 0, 0, 0 }) > 0.99f);
    CHECK(t.density({ 0, 15, 0 }) < 0.01f);
    CHECK(t.materialAt({ 0, 0, 0 }) == Material::Grass);

    size_t triangles = 0;
    float worst = 0.0f;
    for (const auto& [key, chunk] : t.chunks()) {
        TerrainMesh mesh;
        t.meshChunk(key, mesh, nullptr);
        triangles += mesh.indices.size() / 3;
        for (const TerrainVertex& v : mesh.vertices) worst = std::max(worst, std::abs(glm::length(v.position) - 10.0f));
        // Normals point away from the centre (outwards).
        for (const TerrainVertex& v : mesh.vertices) {
            if (glm::length(v.position) > 5.0f) CHECK(glm::dot(v.normal, glm::normalize(v.position)) > 0.5f);
        }
        // Triangles wind counter-clockwise seen from outside (the renderer culls back faces).
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
            const glm::vec3 p0 = mesh.vertices[mesh.indices[i]].position, p1 = mesh.vertices[mesh.indices[i + 1]].position, p2 = mesh.vertices[mesh.indices[i + 2]].position;
            const glm::vec3 face = glm::cross(p1 - p0, p2 - p0);
            if (glm::length(face) > 1e-4f) CHECK(glm::dot(face, (p0 + p1 + p2) / 3.0f) > 0.0f);
        }
    }
    CHECK(triangles > 200);
    CHECK(worst < TerrainData::kVoxelSize * 1.2f); // the surface is close to the sphere
}

TEST_CASE("terrain: blocks, cylinders, carving and ray casts") {
    TerrainData t;
    t.fillBlock(glm::translate(glm::mat4(1.0f), glm::vec3(0, -4, 0)), { 40, 8, 40 }, Material::Rock);
    auto hit = t.raycast({ 3, 20, 3 }, { 0, -1, 0 }, 100.0f);
    REQUIRE(hit);
    CHECK(hit->position.y == doctest::Approx(0.0f).epsilon(0.05));
    CHECK(hit->normal.y > 0.9f);
    CHECK(hit->material == Material::Rock);

    // Carve a hole with Air: the ray goes deeper.
    t.fillBall({ 3, 0, 3 }, 3.0f, Material::Air);
    auto deeper = t.raycast({ 3, 20, 3 }, { 0, -1, 0 }, 100.0f);
    REQUIRE(deeper);
    CHECK(deeper->position.y < -1.5f);

    t.fillCylinder(glm::translate(glm::mat4(1.0f), glm::vec3(-10, 6, -10)), 12.0f, 3.0f, Material::Sand);
    CHECK(t.materialAt({ -10, 6, -10 }) == Material::Sand);
    CHECK(t.density({ -10, 6, -15 }) < 0.01f);
    CHECK_FALSE(t.raycast({ 100, 50, 100 }, { 0, 1, 0 }, 50.0f)); // pointing away
}

TEST_CASE("terrain: files round trip, and scenes save terrain.bin next to scene.json") {
    TerrainData t;
    t.fillBall({ 0, 0, 0 }, 7.0f, Material::Snow);
    t.fillBlock(glm::translate(glm::mat4(1.0f), glm::vec3(20, 0, 0)), { 6, 6, 6 }, Material::Water);
    t.setMaterialColor(Material::Snow, Color3(1.0f, 0.5f, 0.5f));
    const fs::path folder = fs::temp_directory_path() / "rift_terrain_io";
    fs::remove_all(folder);
    fs::create_directories(folder);
    REQUIRE(t.save(folder / "t.bin"));
    TerrainData u;
    REQUIRE(u.load(folder / "t.bin"));
    for (int x = -6; x <= 14; ++x)
        for (int y = -5; y <= 5; ++y)
            for (int z = -5; z <= 5; ++z) CHECK(t.voxel(x, y, z) == u.voxel(x, y, z));
    CHECK(u.materialColor(Material::Snow).g == doctest::Approx(0.5f));

    Scene scene;
    scene.terrain().fillBall({ 0, 0, 0 }, 5.0f, Material::Mud);
    REQUIRE(SceneIO::saveScene(scene, folder / "scene.json"));
    CHECK(fs::exists(folder / "terrain.bin"));
    Scene loaded;
    REQUIRE(SceneIO::loadScene(loaded, folder / "scene.json").ok);
    CHECK(loaded.terrain().materialAt({ 0, 0, 0 }) == Material::Mud);
    CHECK(loaded.terrainInstance());
    // No terrain: no file.
    scene.terrain().clear();
    REQUIRE(SceneIO::saveScene(scene, folder / "scene.json"));
    CHECK_FALSE(fs::exists(folder / "terrain.bin"));
    fs::remove_all(folder);
}

TEST_CASE("terrain: brushes, and undo snapshots restore the chunks") {
    TerrainData t;
    t.fillBlock(glm::translate(glm::mat4(1.0f), glm::vec3(0, -4, 0)), { 30, 8, 30 }, Material::Grass);
    const float before = t.density({ 0, 2, 0 });
    t.beginRecording();
    TerrainData::Dab add;
    add.brush = TerrainData::Brush::Add;
    add.center = { 0, 2, 0 };
    add.radius = 4.0f;
    add.strength = 1.0f;
    add.material = Material::Rock;
    for (int i = 0; i < 5; ++i) t.applyDab(add);
    CHECK(t.density({ 0, 2, 0 }) > before + 0.5f);
    CHECK(t.materialAt({ 0, 3, 0 }) == Material::Rock);

    TerrainData::Dab flatten = add;
    flatten.brush = TerrainData::Brush::Flatten;
    flatten.planeHeight = 0.0f;
    flatten.radius = 6.0f;
    for (int i = 0; i < 20; ++i) t.applyDab(flatten);
    auto hit = t.raycast({ 1, 20, 1 }, { 0, -1, 0 }, 100.0f);
    REQUIRE(hit);
    CHECK(hit->position.y == doctest::Approx(0.0f).epsilon(0.08)); // the bump is levelled

    TerrainData::Dab paint = add;
    paint.brush = TerrainData::Brush::Paint;
    paint.material = Material::Sand;
    t.applyDab(paint);
    CHECK(t.materialAt({ 0, -1, 0 }) == Material::Sand);

    const TerrainData::Snapshot undo = t.endRecording();
    CHECK_FALSE(undo.empty());
    t.restore(undo);
    CHECK(t.density({ 0, 2, 0 }) == doctest::Approx(before));
    CHECK(t.materialAt({ 0, -1, 0 }) == Material::Grass);
}

TEST_CASE("terrain: the generator makes ground, water and beaches") {
    TerrainData t;
    TerrainData::GenerateOptions o;
    o.size = { 128.0f, 128.0f };
    o.seed = 7;
    t.generate(o);
    CHECK_FALSE(t.empty());
    auto hit = t.raycast({ 0, 200, 0 }, { 0, -1, 0 }, 400.0f, true);
    REQUIRE(hit);
    CHECK(hit->position.y > -6.0f);
    // Near the edges the land fades down to the base height, below the water.
    auto edge = t.raycast({ 62, 200, 62 }, { 0, -1, 0 }, 400.0f, true);
    REQUIRE(edge);
    CHECK(edge->material == Material::Water);
}

namespace {
    struct TerrainGame {
        Scene scene;
        ScriptStore store;
        Input input;
        fs::path folder;
        std::unique_ptr<GameSession> session;
        TerrainGame() {
            folder = fs::temp_directory_path() / ("rift_terrain_game_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(folder);
            fs::create_directories(folder);
            store.reset(Paths::toUtf8(folder));
        }
        ~TerrainGame() {
            session.reset();
            std::error_code ec;
            fs::remove_all(folder, ec);
        }
        void start() {
            session = std::make_unique<GameSession>(scene, store, input, GameSession::Options{});
            session->start();
        }
        void run(float seconds) {
            ViewportInput view;
            for (float t = 0.0f; t < seconds; t += 1.0f / 60.0f) {
                input.newFrame();
                session->update(1.0f / 60.0f, view);
            }
        }
        std::string dump() const {
            std::string out;
            for (const auto& line : session->logLines()) out += line.text + "\n";
            return out;
        }
    };
}

TEST_CASE("terrain: characters stand on it, swim in its water, and scripts change it") {
    TerrainGame g;
    // A grass field with a pond; no parts at all.
    g.scene.terrain().fillBlock(glm::translate(glm::mat4(1.0f), glm::vec3(0, -4, 0)), { 80, 8, 80 }, Material::Grass);
    g.scene.terrain().fillBlock(glm::translate(glm::mat4(1.0f), glm::vec3(30, -4, 30)), { 14, 8, 14 }, Material::Water);
    const std::string path = "scripts/Build.lua";
    g.store.create(path, R"(
        local terrain = workspace.Terrain
        terrain:FillBlock(CFrame.new(-20, 4, -20), Vector3.new(8, 8, 8), Enum.Material.Rock)
        local result = workspace:Raycast(Vector3.new(-20, 50, -20), Vector3.new(0, -100, 0))
        print("hit", result and result.Instance == terrain, result and result.Material == Enum.Material.Rock)
        local ok = pcall(function() terrain:FillBall(Vector3.new(0, 0, 0), 4, Enum.Material.Neon) end)
        print("neon refused", not ok)
    )");
    Instance* s = g.scene.create(ClassId::Script, g.scene.service(ClassId::ScriptService));
    s->source = path;
    g.start();
    g.run(1.5f);
    GameSession& session = *g.session;
    CHECK_MESSAGE(session.logLines().size() > 0, g.dump());
    bool hit = false, refused = false;
    for (const auto& line : session.logLines()) {
        hit |= line.text == "hit true true";
        refused |= line.text == "neon refused true";
    }
    CHECK_MESSAGE(hit, g.dump());
    CHECK(refused);
    // Standing on the terrain (it spawned at y=3 with no SpawnLocation).
    Instance* root = session.rootPart();
    REQUIRE(root);
    CHECK(root->worldPosition().y == doctest::Approx(1.0f).epsilon(0.3));
    // The client got the server's change.
    CHECK(session.clientScene().terrain().materialAt({ -20, 4, -20 }) == Material::Rock);

    // Into the pond: it swims (does not sink to the bottom).
    root->setWorldFrame({ 30.0f, -2.0f, 30.0f }, glm::vec3(0.0f));
    if (const PropertyInfo* p = findProperty(root->cls, "Position")) session.scene().notifyPropertyChanged(*root, *p);
    g.input.setKey(GLFW_KEY_SPACE, true);
    g.run(1.5f);
    g.input.setKey(GLFW_KEY_SPACE, false);
    CHECK(root->worldPosition().y > -1.0f);
}
