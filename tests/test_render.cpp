// Rendering smoke tests: a hidden window, the real shaders, the demo scene.
// Writes build/test-output/*.png so a person (or reviewer) can look at them.
#include "doctest.h"
#include "render/Renderer.h"
#include "render/Picking.h"
#include "render/ImageWrite.h"
#include "scene/SceneIO.h"
#include "core/Paths.h"

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

using namespace rift;

namespace {
    // One hidden GL context for every render test.
    struct GlContext {
        GLFWwindow* window = nullptr;
        bool ok = false;
        GlContext() {
            if (!glfwInit()) return;
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            window = glfwCreateWindow(64, 64, "RiftTests", nullptr, nullptr);
            if (!window) return;
            glfwMakeContextCurrent(window);
            glewExperimental = GL_TRUE;
            ok = glewInit() == GLEW_OK;
        }
        ~GlContext() {
            if (window) glfwDestroyWindow(window);
            glfwTerminate();
        }
    };
    GlContext& gl() {
        static GlContext context;
        return context;
    }

    std::filesystem::path outputDir() { return Paths::engineData().parent_path() / "build" / "test-output"; }

    RenderCamera lookFrom(const glm::vec3& eye, const glm::vec3& target, float aspect) {
        RenderCamera c;
        c.position = eye;
        c.view = glm::lookAt(eye, target, glm::vec3(0, 1, 0));
        c.projection = glm::perspective(glm::radians(70.0f), aspect, 0.1f, 2000.0f);
        return c;
    }
}

TEST_CASE("picking hits the exact shape of rotated and stretched parts") {
    Scene scene;
    Instance* wall = scene.create(ClassId::Part, &scene.workspace());
    wall->size = { 10, 4, 1 };
    wall->rotation = { 0, 90, 0 }; // now 1 wide in X, 10 long in Z
    wall->position = { 0, 2, 0 };
    Instance* ball = scene.create(ClassId::Part, &scene.workspace());
    ball->shape = PartShape::Ball;
    ball->size = { 2, 2, 2 };
    ball->position = { 0, 2, 10 };

    auto hit = pickPart(scene, { 10, 2, 3 }, { -1, 0, 0 });
    REQUIRE(hit);
    CHECK(hit->part == wall);
    CHECK(hit->position.x == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(hit->normal.x == doctest::Approx(1.0f).epsilon(0.01));
    CHECK_FALSE(pickPart(scene, { 10, 2, 6 }, { -1, 0, 0 })); // beyond the rotated wall's end
    // A ray that passes the ball's bounding box corner but misses the sphere.
    CHECK_FALSE(pickPart(scene, { 10, 2.95f, 10.95f }, { -1, 0, 0 }));
    hit = pickPart(scene, { 10, 2, 10 }, { -1, 0, 0 });
    REQUIRE(hit);
    CHECK(hit->part == ball);
    CHECK(hit->distance == doctest::Approx(9.0f).epsilon(0.01));
}

TEST_CASE("the renderer draws the demo scene") {
    if (!gl().ok) { MESSAGE("no OpenGL 3.3 context available; skipped"); return; }
    Renderer renderer;
    std::string error;
    REQUIRE_MESSAGE(renderer.init(&error), error);

    Scene scene;
    const auto file = Paths::engineData().parent_path() / "examples/DemoObby/scenes/DemoObby/scene.json";
    Paths::setProjectRoot(Paths::engineData().parent_path() / "examples/DemoObby");
    REQUIRE(SceneIO::loadScene(scene, file).ok);

    Framebuffer fb;
    const int w = 640, h = 360;
    fb.resize(w, h, 4);
    RenderSettings settings;
    settings.editor = true;
    Instance* first = scene.workspace().children.size() > 2 ? scene.workspace().children[2] : nullptr;
    if (first) settings.selected.insert(first->id);
    renderer.render(scene, lookFrom({ 18, 14, 22 }, { 0, 0, 0 }, float(w) / h), settings, fb);
    CHECK(glGetError() == GL_NO_ERROR);
    CHECK(renderer.stats().parts >= 8);

    std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
    fb.readPixels(pixels.data());
    CHECK(writePng(outputDir() / "demo_obby_editor.png", w, h, pixels.data()));
    // Not one flat colour: sky, parts and shading all show up.
    int distinct = 0;
    unsigned prev = 0;
    for (size_t i = 0; i < pixels.size(); i += 4 * 97) {
        const unsigned v = pixels[i] << 16 | pixels[i + 1] << 8 | pixels[i + 2];
        if (v != prev) ++distinct;
        prev = v;
    }
    CHECK(distinct > 100);
    // Top row is sky: blue-ish.
    const unsigned char* top = &pixels[(static_cast<size_t>(4) * w + w / 2) * 4];
    CHECK(top[2] > top[0]);

    // Sunset and night variants.
    scene.lighting().clockTime = 18.2f;
    renderer.render(scene, lookFrom({ 18, 14, 22 }, { 0, 0, 0 }, float(w) / h), settings, fb);
    fb.readPixels(pixels.data());
    writePng(outputDir() / "demo_obby_sunset.png", w, h, pixels.data());
    scene.lighting().clockTime = 0.0f;
    Instance* lamp = scene.create(ClassId::PointLight, &scene.workspace());
    lamp->position = { 0, 3, 0 };
    lamp->range = 20.0f;
    lamp->brightness = 3.0f;
    renderer.render(scene, lookFrom({ 18, 14, 22 }, { 0, 0, 0 }, float(w) / h), settings, fb);
    fb.readPixels(pixels.data());
    writePng(outputDir() / "demo_obby_night.png", w, h, pixels.data());
    CHECK(glGetError() == GL_NO_ERROR);
}

TEST_CASE("materials and shapes render") {
    if (!gl().ok) { MESSAGE("no OpenGL 3.3 context available; skipped"); return; }
    Renderer renderer;
    REQUIRE(renderer.init());
    Scene scene;
    Instance* floor = scene.create(ClassId::Part, &scene.workspace());
    floor->size = { 40, 1, 20 };
    floor->position = { 0, -0.5f, 0 };
    floor->material = Material::Concrete;
    floor->color = { 0.55f, 0.56f, 0.58f };
    const Material materials[] = { Material::Plastic, Material::SmoothPlastic, Material::Metal, Material::Neon, Material::Glass, Material::Wood, Material::Concrete };
    const PartShape shapes[] = { PartShape::Block, PartShape::Ball, PartShape::Cylinder, PartShape::Wedge, PartShape::CornerWedge, PartShape::Block, PartShape::Ball };
    const Color3 colors[] = { { 0.9f, 0.2f, 0.2f }, { 0.2f, 0.6f, 0.95f }, { 0.75f, 0.75f, 0.8f }, { 0.2f, 1.0f, 0.6f }, { 0.6f, 0.85f, 1.0f }, { 0.65f, 0.42f, 0.22f }, { 0.8f, 0.8f, 0.8f } };
    for (int i = 0; i < 7; ++i) {
        Instance* p = scene.create(ClassId::Part, &scene.workspace());
        p->shape = shapes[i];
        p->material = materials[i];
        p->color = colors[i];
        p->size = { 3, 3, 3 };
        p->position = { -15.0f + i * 5.0f, 1.5f, 0 };
    }
    Framebuffer fb;
    fb.resize(900, 300, 4);
    RenderSettings settings;
    renderer.render(scene, lookFrom({ 0, 7, 16 }, { 0, 1, 0 }, 3.0f), settings, fb);
    std::vector<unsigned char> pixels(900 * 300 * 4);
    fb.readPixels(pixels.data());
    CHECK(writePng(outputDir() / "materials.png", 900, 300, pixels.data()));
    CHECK(glGetError() == GL_NO_ERROR);
}
