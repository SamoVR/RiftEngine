// Runtime physics regression test. Runs the REAL GameRuntime / Scene / Object
// (GL entry points stubbed, like test_core.cpp). Scripts and player input are
// not involved: update() is called with allowPlayerInput = false.
//
// Scenario: a 1x1x1 body falling at 80 units/s lands on a 0.17-thick platform,
// stepped with the engine's worst-case 0.1 s frame. A single 0.1 s step moves
// it 8 units, which is more than platform + body thickness, so the old
// single-step physics tunnelled straight through. The speeds and frame rates
// below include cases where a time-only step cap (1/120 s) still tunnelled.
#include "GameRuntime.h"
#include "Scene.h"
#include "Camera.h"
#include "InputManager.h"
#include "Geometry.h"
#include <set>
#include <cstdio>
static std::set<GLuint> liveVAO, liveVBO;
static GLuint nextVAO = 1, nextVBO = 1;
static int badDeletes = 0;

static void APIENTRY stubGenVAO(GLsizei n, GLuint* a) { for (GLsizei i = 0; i < n; ++i) { a[i] = nextVAO++; liveVAO.insert(a[i]); } }
static void APIENTRY stubGenBuf(GLsizei n, GLuint* a) { for (GLsizei i = 0; i < n; ++i) { a[i] = nextVBO++; liveVBO.insert(a[i]); } }
static void APIENTRY stubDelVAO(GLsizei n, const GLuint* a) { for (GLsizei i = 0; i < n; ++i) if (a[i]) { if (!liveVAO.erase(a[i])) { ++badDeletes; std::printf("!! bad VAO delete %u\n", a[i]); } } }
static void APIENTRY stubDelBuf(GLsizei n, const GLuint* a) { for (GLsizei i = 0; i < n; ++i) if (a[i]) { if (!liveVBO.erase(a[i])) { ++badDeletes; std::printf("!! bad VBO delete %u\n", a[i]); } } }
static void APIENTRY stubBindVAO(GLuint) {}
static void APIENTRY stubBindBuf(GLenum, GLuint) {}
static void APIENTRY stubBufData(GLenum, GLsizeiptr, const void*, GLenum) {}
static void APIENTRY stubEnable(GLuint) {}
static void APIENTRY stubAttrib(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}

PFNGLGENVERTEXARRAYSPROC __glewGenVertexArrays = stubGenVAO;
PFNGLDELETEVERTEXARRAYSPROC __glewDeleteVertexArrays = stubDelVAO;
PFNGLBINDVERTEXARRAYPROC __glewBindVertexArray = stubBindVAO;
PFNGLGENBUFFERSPROC __glewGenBuffers = stubGenBuf;
PFNGLDELETEBUFFERSPROC __glewDeleteBuffers = stubDelBuf;
PFNGLBINDBUFFERPROC __glewBindBuffer = stubBindBuf;
PFNGLBUFFERDATAPROC __glewBufferData = stubBufData;
PFNGLENABLEVERTEXATTRIBARRAYPROC __glewEnableVertexAttribArray = stubEnable;
PFNGLVERTEXATTRIBPOINTERPROC __glewVertexAttribPointer = stubAttrib;

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); return 1; } else std::printf("[PASS] %s\n", msg); } while (0)

static Object* addBox(Scene& scene, const char* name, glm::vec3 pos, glm::vec3 scale, bool anchored) {
    auto o = std::make_unique<Object>(name, createCubeVertices());
    o->position = pos;
    o->scale = scale;
    o->game.anchored = anchored;
    Object* raw = o.get();
    scene.addObject(std::move(o));
    return raw;
}

// Drops a body at `speed` and steps frames of `frameDt` until it comes to rest.
static float dropAndRest(float frameDt, float speed, bool& stillVisible) {
    Scene scene;
    Camera camera(16.0f / 9.0f);
    InputManager input;
    addBox(scene, "Ground", { 0.0f, 0.0f, 0.0f }, { 40.0f, 0.17f, 40.0f }, true);
    Object* rock = addBox(scene, "Rock", { 8.0f, 12.0f, 8.0f }, { 1.0f, 1.0f, 1.0f }, false);

    GameRuntime runtime(scene, camera, input);
    runtime.start();
    rock->game.velocity = { 0.0f, -speed, 0.0f }; // start() zeroes velocities
    for (int i = 0; i < static_cast<int>(3.0f / frameDt); ++i) runtime.update(frameDt, false);
    stillVisible = rock->game.visible;
    const float y = rock->position.y;
    runtime.stop();
    return y;
}

int main() {
    const float groundTop = 0.085f;           // 0.17 / 2
    const float restY = groundTop + 0.5f;     // body half-height on top

    bool visible = false;
    float y = dropAndRest(1.0f / 60.0f, 20.0f, visible);
    CHECK(visible && std::fabs(y - restY) < 0.05f, "60 fps, 20 u/s: lands and rests on the platform");

    y = dropAndRest(0.1f, 80.0f, visible);
    CHECK(visible && y > 0.0f, "0.1 s frames, 80 u/s: does not tunnel through the thin platform");
    CHECK(std::fabs(y - restY) < 0.05f, "0.1 s frames, 80 u/s: comes to rest on top of the platform");

    y = dropAndRest(1.0f / 144.0f, 80.0f, visible);
    CHECK(visible && std::fabs(y - restY) < 0.05f, "144 fps, 80 u/s: same resting height");

    y = dropAndRest(1.0f / 60.0f, 80.0f, visible);
    CHECK(visible && std::fabs(y - restY) < 0.05f, "60 fps, 80 u/s: same resting height");

    y = dropAndRest(1.0f / 30.0f, 150.0f, visible);
    CHECK(visible && std::fabs(y - restY) < 0.05f, "30 fps, 150 u/s: same resting height");

    std::printf("\nALL RUNTIME TESTS PASSED\n");
    return 0;
}
