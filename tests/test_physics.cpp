// Jolt-backed physics: bodies follow the scene, queries, touches, character.
#include "doctest.h"
#include "physics/PhysicsWorld.h"
#include "scene/Scene.h"

using namespace rift;

namespace {
    Instance* addPart(Scene& scene, Instance* parent, glm::vec3 pos, glm::vec3 size, bool anchored) {
        Instance* p = scene.create(ClassId::Part, parent);
        p->position = pos;
        p->size = size;
        p->anchored = anchored;
        return p;
    }
    void run(PhysicsWorld& world, float seconds, float dt = 1.0f / 60.0f) {
        for (float t = 0.0f; t < seconds; t += dt) world.step(dt);
    }
}

TEST_CASE("a dynamic part falls and rests on an anchored floor") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 20, 1, 20 }, true);
    Instance* box = addPart(scene, &scene.workspace(), { 0, 5, 0 }, { 1, 1, 1 }, false);
    PhysicsWorld world(scene);
    run(world, 3.0f);
    CHECK(box->position.y == doctest::Approx(0.5f).epsilon(0.03));
    CHECK(glm::length(box->velocity) < 0.1f);
    CHECK(world.bodyCount() == 2);
}

TEST_CASE("fast parts do not tunnel through thin platforms at any frame rate") {
    for (float fps : { 30.0f, 60.0f, 144.0f, 10.0f }) {
        for (float speed : { 20.0f, 80.0f, 150.0f }) {
            Scene scene;
            addPart(scene, &scene.workspace(), { 0, 0, 0 }, { 10, 0.17f, 10 }, true);
            Instance* body = addPart(scene, &scene.workspace(), { 0, 8, 0 }, { 1, 1, 1 }, false);
            body->velocity = { 0, -speed, 0 };
            PhysicsWorld world(scene);
            run(world, 2.0f, 1.0f / fps);
            INFO("fps " << fps << " speed " << speed);
            CHECK(body->position.y > 0.0f);
        }
    }
}

TEST_CASE("raycasts hit the closest part and honour filters") {
    Scene scene;
    Instance* floor = addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 20, 1, 20 }, true);
    Instance* ghost = addPart(scene, &scene.workspace(), { 0, 3, 0 }, { 2, 2, 2 }, true);
    ghost->canCollide = false;
    PhysicsWorld world(scene);
    world.step(1.0f / 60.0f);

    auto hit = world.raycast({ 0, 10, 0 }, { 0, -20, 0 });
    REQUIRE(hit);
    CHECK(hit->instance == ghost->id); // rays see CanCollide = false parts by default (like Roblox)
    CHECK(hit->position.y == doctest::Approx(4.0f).epsilon(0.01));

    RaycastFilter respect;
    respect.respectCanCollide = true;
    hit = world.raycast({ 0, 10, 0 }, { 0, -20, 0 }, respect);
    REQUIRE(hit);
    CHECK(hit->instance == floor->id);
    CHECK(hit->normal.y == doctest::Approx(1.0f));

    RaycastFilter exclude;
    exclude.instances = { floor->id, ghost->id };
    CHECK_FALSE(world.raycast({ 0, 10, 0 }, { 0, -20, 0 }, exclude));

    RaycastFilter include;
    include.include = true;
    include.instances = { floor->id };
    hit = world.raycast({ 0, 10, 0 }, { 0, -20, 0 }, include);
    REQUIRE(hit);
    CHECK(hit->instance == floor->id);

    CHECK_FALSE(world.raycast({ 0, 10, 0 }, { 0, 5, 0 })); // too short
}

TEST_CASE("touch events begin and end") {
    Scene scene;
    Instance* pad = addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 20, 1, 20 }, true);
    Instance* ball = addPart(scene, &scene.workspace(), { 0, 3, 0 }, { 1, 1, 1 }, false);
    ball->shape = PartShape::Ball;
    PhysicsWorld world(scene);
    world.watchTouches(pad->id, true);
    run(world, 2.0f);
    auto events = world.takeTouchEvents();
    REQUIRE(events.size() == 1);
    CHECK(events[0].began);
    CHECK(events[0].part == pad->id);
    CHECK(events[0].other == ball->id);
    CHECK(world.touchingParts(pad->id).size() == 1);

    ball->position = { 0, 50, 0 }; // a script teleports it away
    world.step(1.0f / 60.0f);
    events = world.takeTouchEvents();
    REQUIRE(events.size() == 1);
    CHECK_FALSE(events[0].began);
}

TEST_CASE("CanCollide = false parts are touched but not collided with") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 20, 1, 20 }, true);
    Instance* coin = addPart(scene, &scene.workspace(), { 0, 1, 0 }, { 1, 1, 1 }, true);
    coin->canCollide = false;
    Instance* box = addPart(scene, &scene.workspace(), { 0, 4, 0 }, { 1, 1, 1 }, false);
    PhysicsWorld world(scene);
    world.watchTouches(coin->id, true);
    run(world, 2.0f);
    CHECK(box->position.y == doctest::Approx(0.5f).epsilon(0.05)); // fell through the coin
    bool touched = false;
    for (const auto& e : world.takeTouchEvents()) touched |= e.began && e.other == box->id;
    CHECK(touched);
}

TEST_CASE("child parts are welded to an unanchored parent") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 40, 1, 40 }, true);
    Instance* body = addPart(scene, &scene.workspace(), { 0, 4, 0 }, { 2, 1, 2 }, false);
    Instance* mast = addPart(scene, body, { 0, 1.5f, 0 }, { 0.5f, 2, 0.5f }, true); // its own Anchored is ignored
    PhysicsWorld world(scene);
    run(world, 3.0f);
    CHECK(world.bodyCount() == 2); // floor + one welded assembly
    CHECK(body->position.y < 1.5f);
    const glm::vec3 offset = mast->worldPosition() - body->worldPosition();
    CHECK(glm::length(offset) == doctest::Approx(1.5f).epsilon(0.01)); // stayed attached
}

TEST_CASE("anchored parts moved by scripts carry what stands on them") {
    Scene scene;
    Instance* platform = addPart(scene, &scene.workspace(), { 0, 0, 0 }, { 6, 1, 6 }, true);
    Instance* crate = addPart(scene, &scene.workspace(), { 0, 1.5f, 0 }, { 1, 1, 1 }, false);
    crate->friction = 1.0f;
    PhysicsWorld world(scene);
    run(world, 1.0f);
    const float startX = crate->position.x;
    for (int i = 0; i < 120; ++i) {
        platform->position.x += 2.0f / 60.0f; // like a Heartbeat script: 2 studs/s
        world.step(1.0f / 60.0f);
    }
    CHECK(platform->position.x == doctest::Approx(4.0f));
    CHECK(crate->position.x - startX > 3.0f); // the crate rode along
}

TEST_CASE("destroying a part mid-simulation removes its body") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 20, 1, 20 }, true);
    Instance* box = addPart(scene, &scene.workspace(), { 0, 3, 0 }, { 1, 1, 1 }, false);
    PhysicsWorld world(scene);
    run(world, 0.2f);
    CHECK(world.bodyCount() == 2);
    scene.destroy(box);
    run(world, 0.2f);
    CHECK(world.bodyCount() == 1);
    // Parts created later join the simulation.
    Instance* late = addPart(scene, &scene.workspace(), { 3, 5, 0 }, { 1, 1, 1 }, false);
    run(world, 2.0f);
    CHECK(late->position.y == doctest::Approx(0.5f).epsilon(0.05));
}

TEST_CASE("parts that fall out of the world are reported") {
    Scene scene;
    scene.workspace().fallenPartsHeight = -20.0f;
    Instance* rock = addPart(scene, &scene.workspace(), { 0, 0, 0 }, { 1, 1, 1 }, false);
    PhysicsWorld world(scene);
    std::vector<uint64_t> fallen;
    for (int i = 0; i < 240 && fallen.empty(); ++i) {
        world.step(1.0f / 60.0f);
        fallen = world.takeFallenParts();
    }
    REQUIRE(fallen.size() == 1);
    CHECK(fallen[0] == rock->id);
}

TEST_CASE("character walks, is blocked by walls, jumps and climbs steps") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 60, 1, 60 }, true);
    addPart(scene, &scene.workspace(), { 0, 2, -10 }, { 20, 4, 1 }, true);      // wall in front
    addPart(scene, &scene.workspace(), { 10, 0.15f, 0 }, { 4, 0.3f, 4 }, true);  // low step to the right
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    Instance* root = addPart(scene, model, { 0, 1, 0 }, { 1, 2, 1 }, false);
    PhysicsWorld world(scene);
    world.setIgnored(model->id, true);
    CharacterController* c = world.createCharacter(root->id, { 0, 0.05f, 0 }, 0.4f, 1.8f);
    run(world, 0.5f);
    CHECK(c->onGround());
    CHECK(world.bodyCount() == 3); // the character's own parts have no bodies

    CharacterController::Input input;
    input.moveDirection = { 0, 0, -1 }; // towards the wall
    c->setInput(input);
    run(world, 3.0f);
    CHECK(c->feetPosition().z > -9.6f); // stopped by the wall (wall face at z = -9.5)
    CHECK(c->feetPosition().z < -8.0f);

    input.moveDirection = { 0, 0, 0 };
    input.jump = true;
    c->setInput(input);
    float maxHeight = 0.0f;
    for (int i = 0; i < 60; ++i) {
        world.step(1.0f / 60.0f);
        maxHeight = std::max(maxHeight, c->feetPosition().y);
        if (i == 0) { input.jump = false; c->setInput(input); }
    }
    CHECK(maxHeight > 1.5f); // 12^2 / (2 * 30) = 2.4 studs
    run(world, 1.0f);
    CHECK(c->onGround());

    c->teleport({ 6, 0.05f, 0 });
    input.moveDirection = { 1, 0, 0 };
    c->setInput(input);
    for (int i = 0; i < 120 && c->feetPosition().x < 9.5f; ++i) world.step(1.0f / 60.0f);
    input.moveDirection = { 0, 0, 0 };
    c->setInput(input);
    run(world, 0.3f);
    CHECK(c->feetPosition().x > 9.0f);  // walked up onto the step (spans x 8..12) ...
    CHECK(c->feetPosition().x < 12.0f);
    CHECK(c->feetPosition().y > 0.25f); // ... and stands on it (top at 0.3)
}

TEST_CASE("the character is found by touch queries on parts") {
    Scene scene;
    addPart(scene, &scene.workspace(), { 0, -0.5f, 0 }, { 60, 1, 60 }, true);
    Instance* killBrick = addPart(scene, &scene.workspace(), { 5, 0.2f, 0 }, { 2, 0.4f, 2 }, true);
    Instance* model = scene.create(ClassId::Model, &scene.workspace());
    Instance* root = addPart(scene, model, {}, { 1, 2, 1 }, false);
    PhysicsWorld world(scene);
    world.setIgnored(model->id, true);
    world.watchTouches(killBrick->id, true);
    CharacterController* c = world.createCharacter(root->id, { 0, 0.05f, 0 }, 0.4f, 1.8f);
    CharacterController::Input input;
    input.moveDirection = { 1, 0, 0 };
    c->setInput(input);
    bool touched = false;
    for (int i = 0; i < 120 && !touched; ++i) {
        world.step(1.0f / 60.0f);
        for (const auto& e : world.takeTouchEvents()) touched |= e.began && e.part == killBrick->id && e.other == root->id;
    }
    CHECK(touched);
}
