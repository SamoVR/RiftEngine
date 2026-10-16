// Lua scripting tests. Runs the REAL GameRuntime / LuaRuntime / Scene / Object
// (only the GL entry points are stubbed, like test_core.cpp), with scripts held
// in an in-memory ScriptStore. Build with address/UB sanitizers to catch
// lifetime bugs (see tests/README.md).
#include "GameRuntime.h"
#include "ScriptStore.h"
#include "Scene.h"
#include "Camera.h"
#include "InputManager.h"
#include "Geometry.h"
#include <chrono>
#include <cstdio>
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

static int g_failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); ++g_failed; } else std::printf("[PASS] %s\n", msg); } while (0)

struct Env {
    Scene scene;
    Camera camera{ 16.0f / 9.0f };
    InputManager input;
    ScriptStore store;
    GameRuntime rt{ scene, camera, input };
    int scriptCounter = 0;
    Env() { rt.setScriptStore(&store); }

    Object* part(const char* name, glm::vec3 pos, glm::vec3 scale = glm::vec3(1.0f), bool anchored = true) {
        auto o = std::make_unique<Object>(name, createCubeVertices());
        o->position = pos; o->scale = scale; o->game.anchored = anchored;
        Object* raw = o.get();
        scene.addObject(std::move(o));
        return raw;
    }
    // Script object (parent == nullptr: scene level). Source lives in the store.
    Object* script(const char* name, const std::string& source, Object* parent = nullptr) {
        auto o = std::make_unique<Object>(name, std::vector<Vertex>());
        o->kind = Object::Kind::Script;
        o->scriptPath = "scripts/" + std::string(name) + std::to_string(scriptCounter++) + ".lua";
        store.create(o->scriptPath, source);
        Object* raw = o.get();
        scene.addObject(std::move(o));
        if (parent) raw->setParent(parent);
        return raw;
    }
    void module(const std::string& path, const std::string& source) { store.create(path, source); }
    void frames(int n, float dt = 1.0f / 60.0f) { for (int i = 0; i < n; ++i) rt.update(dt, false); }
    std::string log() const { std::string all; for (const auto& l : rt.log()) all += l.text + "\n"; return all; }
    bool logHas(const std::string& s) const { return log().find(s) != std::string::npos; }
};

int main() {
    // ---------------------------------------------------------------- basics
    {
        Env e;
        e.part("Ground", { 0, -0.5f, 0 }, { 40, 1, 40 });
        e.script("Hello", "print('hello', 1, true, nil)");
        e.rt.start();
        CHECK(e.logHas("[Hello] hello\t1\ttrue\tnil"), "print goes to the game log with the script's label");
        e.rt.stop();
    }
    {
        Env e;
        e.script("Broken", "local x = = 1");
        e.script("Fine", "print('still running')");
        e.rt.start();
        CHECK(e.logHas("syntax error") && e.logHas("Broken"), "syntax error is reported with the script name");
        CHECK(e.logHas("still running"), "a broken script does not stop the others");
        e.rt.stop();
    }
    {
        Env e;
        e.script("Boom", "error('boom')");
        e.script("Nil", "local x = nil; x.y = 1");
        e.script("Fine", "print('ok')");
        e.rt.start();
        CHECK(e.logHas("boom"), "error() is logged");
        CHECK(e.logHas("attempt to index a nil value"), "runtime error is logged");
        CHECK(e.logHas("ok"), "other scripts are unaffected by runtime errors");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- scheduler
    {
        Env e;
        e.script("Timer", "task.wait(0.5); print('woke'); local dt = task.wait(0.25); print('second', dt >= 0.25)");
        e.rt.start();
        e.frames(20);
        CHECK(!e.logHas("woke"), "task.wait(0.5) has not returned after 0.33 s");
        e.frames(15);
        CHECK(e.logHas("woke"), "task.wait(0.5) returns after ~0.58 s");
        e.frames(30);
        CHECK(e.logHas("second\ttrue"), "task.wait returns the time actually waited");
        e.rt.stop();
    }
    {
        Env e;
        e.script("HB", "local n = 0; RunService.Heartbeat:Connect(function(dt) n = n + 1; if n == 10 then print('hb10', dt > 0) end end)\n"
                       "task.delay(0.2, print, 'delayed'); task.spawn(print, 'spawned', 7)\n"
                       "local dt = RunService.Heartbeat:Wait(); print('waited', dt > 0)");
        e.rt.start();
        CHECK(e.logHas("spawned\t7"), "task.spawn runs immediately with its arguments");
        e.frames(12);
        CHECK(e.logHas("waited\ttrue"), "Signal:Wait() resumes with the signal's arguments");
        CHECK(e.logHas("hb10\ttrue"), "Heartbeat fires every frame with dt");
        e.frames(10);
        CHECK(e.logHas("delayed"), "task.delay runs after the delay");
        e.rt.stop();
    }
    {
        Env e;
        e.script("Loop", "while true do end");
        e.script("Fine", "print('after loop script')");
        const auto t0 = std::chrono::steady_clock::now();
        e.rt.start();
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(e.logHas("ran too long"), "an infinite loop is stopped and reported");
        CHECK(seconds < 3.0, "... within a couple of seconds");
        CHECK(e.logHas("after loop script"), "scripts after a runaway script still start");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- sandbox
    {
        Env e;
        e.script("Sandbox", "print(io, os.execute, os.remove, dofile, load, loadfile, package, debug, string.dump, require ~= nil, os.clock ~= nil)");
        e.rt.start();
        CHECK(e.logHas("nil\tnil\tnil\tnil\tnil\tnil\tnil\tnil\tnil\ttrue\ttrue"), "no io/os.execute/load/dofile/package/debug; require and os.clock exist");
        e.rt.stop();
    }
    {
        Env e;
        e.script("Env1", "shared_global = 123");
        e.script("Env2", "print('leak?', shared_global)");
        e.rt.start();
        CHECK(e.logHas("leak?\tnil"), "each script has its own globals");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- require
    {
        Env e;
        e.module("scripts/util.lua", "local M = {}; function M.double(x) return x * 2 end; return M");
        e.module("scripts/lib/vec.lua", "return { name = 'vec' }");
        e.script("Req", "print(require('util').double(21), require('lib.vec').name, require('util') == require('util'))\n"
                        "print(pcall(require, 'nope'))");
        e.rt.start();
        CHECK(e.logHas("42\tvec\ttrue"), "require loads modules from scripts/, dots become folders, results are cached");
        CHECK(e.logHas("false") && e.logHas("module 'nope' not found"), "require of a missing module is a catchable error");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- values
    {
        Env e;
        e.script("Math", "local v = Vector3.new(1,2,3) + Vector3.new(1,1,1) * 2\n"
                         "print(v.X, v.Y, v.Z, Vector3.new(3,4,0).Magnitude, Vector3.new(0,5,0).Unit.Y, (2 * Vector3.one).X)\n"
                         "print(Vector3.new(1,0,0):Cross(Vector3.new(0,1,0)).Z, Vector3.new(1,2,3):Dot(Vector3.new(1,1,1)), Vector3.new(1,2,3) == Vector3.new(1,2,3))\n"
                         "local c = Color3.fromRGB(255, 0, 128); print(c.R, c.G, c.B > 0.5, typeof(c), typeof(v), typeof(workspace), typeof(1))\n"
                         "print(math.clamp(5, 0, 3), math.sign(-2), math.round(2.5))");
        e.rt.start();
        CHECK(e.logHas("3.0\t4.0\t5.0\t5.0\t1.0\t2.0"), "Vector3 arithmetic, Magnitude, Unit, scalar multiply");
        CHECK(e.logHas("1.0\t6.0\ttrue"), "Cross, Dot and == on vectors");
        CHECK(e.logHas("1.0\t0.0\ttrue\tColor3\tVector3\tInstance\tnumber"), "Color3 and typeof");
        CHECK(e.logHas("3\t-1\t3"), "math.clamp / sign / round helpers");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- the old demo scripts, in Lua
    {
        Env e;
        e.part("Ground", { 0, -0.5f, 0 }, { 40, 1, 40 });
        Object* coin = e.part("Coin", { 6, 1, 0 }, glm::vec3(1.0f));
        e.script("Coin",
            "local coin = script.Parent\n"
            "local collected = false\n"
            "coin.Touched:Connect(function(other)\n"
            "    if other == Players.LocalPlayer.Character and not collected then\n"
            "        collected = true\n"
            "        print('Coin collected!', other.Name)\n"
            "        coin.Visible = false\n"
            "    end\n"
            "end)", coin);
        e.rt.start();
        e.frames(5);
        CHECK(!e.logHas("Coin collected"), "coin is not collected while the player is far away");
        e.rt.playerObject()->position = coin->position;
        e.frames(3);
        CHECK(e.logHas("Coin collected!\tPlayer"), "script.Parent + Touched + Players.LocalPlayer.Character work together");
        CHECK(!coin->game.visible, "the script hid the coin");
        e.frames(30);
        e.rt.stop();
        CHECK(coin->game.visible && !coin->game.destroyed, "Stop restores the coin");
    }
    {
        Env e;
        e.part("Ground", { 0, -0.5f, 0 }, { 40, 1, 40 });
        Object* pad = e.part("JumpPad", { 0, 0.15f, 0 }, { 4, 0.3f, 4 });
        e.script("Pad",
            "local power = 28\n"
            "script.Parent.Touched:Connect(function(other)\n"
            "    if other.Name == 'Player' then other.Velocity = Vector3.new(0, power, 0) end\n"
            "end)", pad);
        e.script("Win", "task.wait(0.1); game:Win('Finished!')");
        e.rt.start();
        e.rt.playerObject()->position = { 0, 1, 0 };
        e.frames(3);
        CHECK(e.rt.playerObject()->position.y > 1.2f, "JumpPad launches the player (Character.Velocity)");
        e.frames(10);
        CHECK(e.rt.outcome() == GameRuntime::Outcome::Won && e.rt.message() == "Finished!", "game:Win(text) ends the run");
        e.rt.stop();
    }

    // ---------------------------------------------------------------- Instance API
    {
        Env e;
        Object* box = e.part("Box", { 0, 2, 0 });
        e.script("Make",
            "local p = Instance.new('Part')\n"
            "p.Name = 'Spawned'; p.Anchored = true; p.Position = Vector3.new(0, 5, 0)\n"
            "p.Size = Vector3.new(2, 2, 2); p.Color = Color3.fromRGB(255, 0, 0)\n"
            "print('before parent', p.Parent)\n"
            "p.Parent = workspace\n"
            "print(workspace.Spawned.Position.Y, workspace.Spawned.Size.X, p.Parent == workspace, p.ClassName)\n"
            "local c = p:Clone(); c.Name = 'Copy'; c.Parent = p\n"
            "print(p:FindFirstChild('Copy') ~= nil, c.Parent == p, #p:GetChildren(), p:FindFirstChild('Copy', true) == c, c:IsDescendantOf(workspace), c:GetFullName())\n"
            "workspace.Box.Parent = p\n"
            "print(#workspace:GetChildren(), workspace.Spawned.Box ~= nil)\n"
            "c:Destroy()\n"
            "print(pcall(function() return c.Name end))\n"
            "print(pcall(function() return workspace.Typo end))\n"
            "print(pcall(function() p.Colour = 1 end))\n"
            "p.Name = 'Renamed'; print(workspace.Renamed ~= nil)");
        const size_t before = e.scene.objectCount(); // Box + the Script object
        e.rt.start();
        CHECK(e.logHas("before parent\tnil"), "Instance.new parts start with Parent = nil");
        CHECK(e.logHas("5.0\t2.0\ttrue\tPart"), "set properties, Parent = workspace, workspace.Child lookup");
        CHECK(e.logHas("true\ttrue\t1\ttrue\ttrue\tWorkspace.Spawned.Copy"), "Clone, FindFirstChild (recursive), IsDescendantOf, GetFullName");
        CHECK(e.logHas("2\ttrue"), "re-parenting a scene object under a script-created part works");
        CHECK(e.logHas("false") && e.logHas("destroyed"), "using a destroyed Instance is a catchable error");
        CHECK(e.logHas("'Typo' is not a valid member of Workspace"), "unknown child names are errors, not nil");
        CHECK(e.logHas("'Colour' is not a valid property of Part"), "setting an unknown property is a clear error");
        CHECK(e.logHas("true\n") , "renaming updates name lookups");
        e.frames(2);
        if (g_failed) std::printf("--- script log ---\n%s------------------\n", e.log().c_str());
        e.rt.stop();
        CHECK(e.scene.objectCount() == before, "Stop removes everything the script created");
        CHECK(box->parent == nullptr && e.scene.getObjects().size() == before, "Stop restores the re-parented scene object");
    }
    {
        Env e;
        Object* wall = e.part("Wall", { 0, 1, 0 });
        Object* other = e.part("Other", { 5, 1, 0 });
        e.script("Kill", "workspace.Wall:Destroy(); print(workspace:FindFirstChild('Wall'))");
        e.script("Move", "workspace.Other.Name = 'Renamed'");
        e.rt.start();
        CHECK(e.logHas("nil"), "a destroyed scene object disappears from lookups");
        CHECK(wall->game.destroyed && !wall->game.visible && !wall->game.canCollide, "scene objects are only hidden while playing");
        e.rt.stop();
        CHECK(!wall->game.destroyed && wall->game.visible && wall->game.canCollide, "Stop brings destroyed scene objects back");
        CHECK(other->name == "Other" || other->name == "Renamed", "name handled"); // names are not restored by design (see below)
    }

    // ---------------------------------------------------------------- signals + destruction safety
    {
        Env e;
        e.part("Ground", { 0, -0.5f, 0 }, { 40, 1, 40 });
        Object* a = e.part("A", { 0, 0.5f, 0 });
        Object* b = e.part("B", { 0, 0.5f, 0 }, glm::vec3(1.0f), false);
        e.script("Chaos",
            "local a, b = workspace.A, workspace.B\n"
            "a.Touched:Connect(function(other) print('a touched', other.Name); a:Destroy(); b:Destroy() end)\n"
            "b.Touched:Connect(function(other) print('b touched', other.Name) end)\n"
            "workspace.ChildAdded:Connect(function(child) print('added', child.Name) end)\n"
            "local n = Instance.new('Part'); n.Name = 'Late'; n.Parent = workspace");
        e.rt.start();
        e.frames(10);
        CHECK(e.logHas("added\tLate"), "ChildAdded fires on workspace");
        CHECK(e.logHas("a touched"), "Touched fires");
        CHECK(a->game.destroyed && b->game.destroyed, "destroying objects from inside a Touched handler is safe");
        e.frames(10);
        e.rt.stop();
    }
    {
        Env e;
        e.script("Conn", "local hits = 0; local conn; conn = RunService.Heartbeat:Connect(function() hits = hits + 1; if hits == 3 then conn:Disconnect(); print('disconnected', conn.Connected) end end)\n"
                         "RunService.Heartbeat:Once(function() print('once') end)");
        e.rt.start();
        e.frames(8);
        CHECK(e.logHas("disconnected\tfalse"), "Disconnect stops a handler");
        e.rt.stop();
    }
    {
        Env e;
        Object* a = e.part("A", { 0, 0, 0 });
        e.script("Spawner", "RunService.Heartbeat:Connect(function() local p = Instance.new('Part'); p.Parent = workspace; p:Destroy() end)");
        const size_t before = e.scene.objectCount();
        e.rt.start();
        e.frames(60);
        CHECK(e.scene.objectCount() <= before + 2, "parts created and destroyed every frame do not pile up in the scene");
        e.rt.stop();
        CHECK(e.scene.objectCount() == before && a->parent == nullptr, "scene is intact after heavy create/destroy");
    }
    {
        Env e;
        Object* holder = e.part("Holder", { 0, 0, 0 });
        e.script("Cycle", "print(pcall(function() workspace.Holder.Parent = workspace.Holder end))\n"
                          "local p = Instance.new('Part'); local q = Instance.new('Part'); q.Parent = p\n"
                          "print(pcall(function() p.Parent = q end))");
        e.rt.start();
        CHECK(e.logHas("cannot be parented under itself"), "parenting an object under itself or its descendant is an error");
        e.rt.stop();
        (void)holder;
    }
    {
        Env e;
        Object* scripted = e.part("Host", { 0, 0, 0 });
        e.script("Clones", "local host = workspace.Host\n"
                           "print(host:FindFirstChild('Inner') ~= nil)", scripted);
        // A Script inside a template that gets cloned must start when the clone enters the world.
        Object* template_ = e.part("Template", { 0, 0, 0 });
        e.script("Inner", "print('inner started under', script.Parent.Name)", template_);
        e.script("Driver", "local c = workspace.Template:Clone(); c.Name = 'Clone1'; c.Parent = workspace");
        e.rt.start();
        CHECK(e.logHas("inner started under\tTemplate") && e.logHas("inner started under\tClone1"), "scripts inside a cloned template start when the clone enters the world");
        e.rt.stop();
    }

    std::printf(g_failed == 0 ? "\nALL LUA TESTS PASSED\n" : "\n%d LUA TEST(S) FAILED\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
