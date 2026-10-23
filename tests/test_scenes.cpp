// Loads every scene in assets/scenes with the real loader, syntax-checks every
// Lua file, and plays demo_obby (the scripts that ship with the engine).
// Run from the project folder (the one that contains assets/).
#include "GameRuntime.h"
#include "ScriptStore.h"
#include "Scene.h"
#include "Camera.h"
#include "InputManager.h"
#include "Geometry.h"
#include <cstdio>
#include <filesystem>
#include <set>
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

// Textured scenes also reach Texture.cpp: stub its GL calls too.
extern "C" {
void glGenTextures(GLsizei n, GLuint* t) { for (GLsizei i = 0; i < n; ++i) t[i] = 1; }
void glBindTexture(GLenum, GLuint) {}
void glTexParameteri(GLenum, GLenum, GLint) {}
void glPixelStorei(GLenum, GLint) {}
void glTexImage2D(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) {}
void glDeleteTextures(GLsizei, const GLuint*) {}
}
static void APIENTRY stubGenMip(GLenum) {}
static void APIENTRY stubActiveTex(GLenum) {}
PFNGLGENERATEMIPMAPPROC __glewGenerateMipmap = stubGenMip;
PFNGLACTIVETEXTUREPROC __glewActiveTexture = stubActiveTex;

static int g_failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); ++g_failed; } else std::printf("[PASS] %s\n", msg); } while (0)
namespace fs = std::filesystem;

int main() {
    const fs::path root = "assets/scenes";
    if (!fs::exists(root)) { std::printf("FAIL: run this from the project folder (assets/scenes not found)\n"); return 1; }

    int scenes = 0;
    for (const auto& entry : fs::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        const fs::path file = entry.path() / "scene.json";
        if (!fs::exists(file)) continue;
        ++scenes;
        const std::string label = entry.path().filename().string();

        Scene scene;
        const bool loaded = scene.loadFromFile(file.string());
        CHECK(loaded && scene.objectCount() > 0, (label + ": loads").c_str());
        if (!loaded) continue;

        ScriptStore store;
        store.reset(entry.path().string());
        int scripts = 0, bad = 0;
        for (Object* o : scene.getObjects()) {
            if (o->kind != Object::Kind::Script) continue;
            ++scripts;
            if (!store.load(o->scriptPath) || !GameRuntime::checkSyntax(store.find(o->scriptPath)->text, o->scriptPath).empty()) ++bad;
            if (!o->parent) { /* scene-level script: fine */ }
        }
        CHECK(bad == 0, (label + ": all " + std::to_string(scripts) + " script file(s) exist and have valid Lua").c_str());
    }
    CHECK(scenes >= 1, "found at least one scene folder");

    // ---- play demo_obby with the real script files
    {
        Scene scene;
        Camera camera(16.0f / 9.0f);
        InputManager input;
        ScriptStore store;
        GameRuntime rt(scene, camera, input);
        rt.setScriptStore(&store);
        const fs::path dir = root / "demo_obby";
        CHECK(scene.loadFromFile((dir / "scene.json").string()), "demo_obby loads for play");
        store.reset(dir.string());
        for (Object* o : scene.getObjects()) if (o->kind == Object::Kind::Script) store.load(o->scriptPath);

        auto at = [&](const char* name) { return scene.findByName(name); };
        auto play = [&](int frames) { for (int i = 0; i < frames; ++i) rt.update(1.0f / 60.0f, false); };
        auto logHas = [&](const std::string& s) { for (const auto& l : rt.log()) if (l.text.find(s) != std::string::npos) return true; return false; };
        auto hasErrors = [&]() { for (const auto& l : rt.log()) if (l.isError) return true; return false; };

        rt.start();
        play(5);
        CHECK(!hasErrors(), "demo_obby: no script errors on start");

        // Spinner turns, coin bobs.
        const float spinnerBefore = at("Spinner")->rotation.y;
        play(30);
        CHECK(std::fabs(at("Spinner")->rotation.y - spinnerBefore) > 20.0f, "Spinner rotates (Heartbeat)");

        // Coin.
        rt.playerObject()->position = at("Coin")->position;
        play(3);
        CHECK(logHas("Coin collected!") && !at("Coin")->game.visible, "Coin: collected on touch and removed");

        // Kill brick.
        rt.playerObject()->position = at("KillBrick")->position + glm::vec3(0, 1.0f, 0);
        play(3);
        CHECK(rt.message().find("Ouch") != std::string::npos, "KillBrick: on-screen message shown");
        CHECK(rt.playerObject()->position.z > -2.0f, "KillBrick: player sent back to the spawn point");

        // Jump pad.
        rt.playerObject()->position = at("JumpPad")->position + glm::vec3(0, 1.0f, 0);
        play(2);
        const float y0 = rt.playerObject()->position.y;
        play(12);
        CHECK(rt.playerObject()->position.y > y0 + 1.0f, "JumpPad: launches the player");

        // Finish.
        rt.playerVelocity() = glm::vec3(0.0f); // still flying from the jump pad
        rt.playerObject()->position = at("Finish")->position + glm::vec3(0, 1.2f, 0);
        play(20); // the player falls onto the pad, then the touch fires
        CHECK(rt.outcome() == GameRuntime::Outcome::Won, "Finish: the run is won");
        CHECK(!hasErrors(), "demo_obby: no script errors during the whole run");

        rt.stop();
        CHECK(at("Coin")->game.visible && !at("Coin")->game.destroyed, "Stop restores the scene");
    }

    std::printf(g_failed == 0 ? "\nALL SCENE TESTS PASSED\n" : "\n%d SCENE TEST(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
