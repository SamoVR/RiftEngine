// GUI in the 3D world: BillboardGuis placed by a camera, SurfaceGuis laid out
// on a part's face, and clicks on a SurfaceGui's buttons.
#include "doctest.h"
#include "game/GuiSystem.h"
#include "scene/Scene.h"

#include <glm/gtc/matrix_transform.hpp>

using namespace rift;

namespace {
    bool near(const glm::vec3& a, const glm::vec3& b) { return glm::length(a - b) < 1e-3f; }
}

TEST_CASE("a SurfaceGui covers a face of its part, and its buttons can be clicked") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    part->size = { 4.0f, 2.0f, 1.0f };
    Instance* surface = scene.create(ClassId::SurfaceGui, part);
    Instance* button = scene.create(ClassId::TextButton, surface);
    button->guiSize = { 0.5f, 0.0f, 0.5f, 0.0f };
    button->guiPosition = { 0.5f, 0.0f, 0.5f, 0.0f };
    button->anchorPoint = { 0.5f, 0.5f };

    // Front (-Z): read from the front, the canvas's right is -X.
    std::array<glm::vec3, 4> corners;
    glm::vec3 normal;
    glm::vec2 studs;
    GuiSystem::faceCorners(*part, NormalId::Front, corners, normal, studs);
    CHECK(near(corners[0], { 2.0f, 1.0f, -0.51f }));
    CHECK(near(corners[2], { -2.0f, -1.0f, -0.51f }));
    CHECK(near(normal, { 0.0f, 0.0f, -1.0f }));
    CHECK(studs == glm::vec2(4.0f, 2.0f));
    // Top: the canvas's top edge is towards the front.
    GuiSystem::faceCorners(*part, NormalId::Top, corners, normal, studs);
    CHECK(near(corners[0], { -2.0f, 1.01f, -0.5f }));
    CHECK(near(normal, { 0.0f, 1.0f, 0.0f }));

    GuiSystem gui(scene);
    int clicks = 0;
    gui.onEvent = [&](Instance& i, GuiEvent e) { if (&i == button && e == GuiEvent::Activated) ++clicks; };
    gui.layout({ 1280.0f, 720.0f });
    CHECK(gui.drawList().empty()); // nothing on the screen itself
    REQUIRE(gui.surfaces().size() == 1);
    const GuiSystem::Surface& s = gui.surfaces()[0];
    CHECK(s.canvas == glm::vec2(800.0f, 600.0f));
    REQUIRE(s.items.size() == 1);
    CHECK(s.items[0].rect.pos == glm::vec2(200.0f, 150.0f));
    CHECK(s.items[0].rect.size == glm::vec2(400.0f, 300.0f));

    // A ray at the middle of the face lands in the middle of the canvas.
    GuiSystem::SurfacePoint point;
    REQUIRE(gui.surfaceUnder({ 0.0f, 0.0f, -10.0f }, { 0.0f, 0.0f, 1.0f }, 100.0f, point));
    CHECK(point.gui == surface->id);
    CHECK(point.point.x == doctest::Approx(400.0f).epsilon(0.01));
    CHECK(point.point.y == doctest::Approx(300.0f).epsilon(0.01));
    CHECK_FALSE(gui.surfaceUnder({ 0.0f, 0.0f, 10.0f }, { 0.0f, 0.0f, -1.0f }, 100.0f, point)); // from behind
    CHECK_FALSE(gui.surfaceUnder({ 0.0f, 0.0f, -10.0f }, { 0.0f, 0.0f, 1.0f }, 5.0f, point));   // something closer is in the way

    gui.handleMouse({ 0.0f, 0.0f }, true, true, &point);
    gui.handleMouse({ 0.0f, 0.0f }, true, false, &point);
    CHECK(clicks == 1);
    CHECK(gui.mouseOverGui());

    // PixelsPerStud: the canvas grows with the face.
    surface->sizingMode = SurfaceGuiSizingMode::PixelsPerStud;
    surface->pixelsPerStud = 100.0f;
    gui.layout({ 1280.0f, 720.0f });
    CHECK(gui.surfaces()[0].canvas == glm::vec2(400.0f, 200.0f));
    // Disabled: gone.
    surface->enabled = false;
    gui.layout({ 1280.0f, 720.0f });
    CHECK(gui.surfaces().empty());
}

TEST_CASE("a BillboardGui floats over its part at a size in studs and pixels") {
    Scene scene;
    Instance* part = scene.create(ClassId::Part, &scene.workspace());
    part->position = { 0.0f, 0.0f, 0.0f };
    Instance* billboard = scene.create(ClassId::BillboardGui, part);
    billboard->guiSize = { 4.0f, 0.0f, 0.0f, 50.0f }; // 4 studs wide, 50 pixels tall
    billboard->studsOffset = { 0.0f, 2.0f, 0.0f };
    Instance* label = scene.create(ClassId::TextLabel, billboard);
    label->guiSize = { 1.0f, 0.0f, 1.0f, 0.0f };

    const glm::vec2 view(1000.0f, 800.0f);
    const glm::mat4 viewMatrix = glm::lookAt(glm::vec3(0.0f, 2.0f, 20.0f), glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 projection = glm::perspective(glm::radians(70.0f), view.x / view.y, 0.1f, 1000.0f);
    const BillboardPlacement p = GuiSystem::placeWithCamera(*billboard, *part, viewMatrix, projection, view);
    REQUIRE(p.visible);
    CHECK(p.center.x == doctest::Approx(500.0f));
    CHECK(p.center.y == doctest::Approx(400.0f)); // 2 studs up: level with the camera
    CHECK(p.depth == doctest::Approx(20.0f));
    // 20 studs away with a 70 degree view: 800 px cover 2 * 20 * tan(35) studs.
    CHECK(p.pixelsPerStud == doctest::Approx(800.0f / (2.0f * 20.0f * std::tan(glm::radians(35.0f)))).epsilon(0.01));

    GuiSystem gui(scene);
    gui.layout(view);
    CHECK(gui.drawList().empty()); // no camera, no billboards
    gui.placeBillboard = [&](const Instance& g, const Instance& onPart) { return GuiSystem::placeWithCamera(g, onPart, viewMatrix, projection, view); };
    gui.layout(view);
    REQUIRE(gui.drawList().size() == 1);
    const GuiRect r = gui.drawList()[0].rect;
    CHECK(r.size.x == doctest::Approx(4.0f * p.pixelsPerStud));
    CHECK(r.size.y == doctest::Approx(50.0f));
    CHECK(r.pos.x + r.size.x * 0.5f == doctest::Approx(500.0f));

    // Too far away (MaxDistance), or behind the camera: not shown.
    billboard->maxDistance = 10.0f;
    CHECK_FALSE(GuiSystem::placeWithCamera(*billboard, *part, viewMatrix, projection, view).visible);
    billboard->maxDistance = 0.0f;
    const glm::mat4 away = glm::lookAt(glm::vec3(0.0f, 2.0f, 20.0f), glm::vec3(0.0f, 2.0f, 40.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    CHECK_FALSE(GuiSystem::placeWithCamera(*billboard, *part, away, projection, view).visible);
}
