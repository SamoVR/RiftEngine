// Runs the REAL Object.cpp / Scene.cpp (no mirrors). Only the GL entry points
// are replaced with stubs that track which VAO/VBO ids are alive, so ownership
// bugs (double delete, leak, shared handle) are directly observable.
#include "Object.h"
#include "Scene.h"
#include "Geometry.h"
#include <set>
#include <cassert>
#include <cstdio>
#include <fstream>

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

int main() {
    auto cube = createCubeVertices();

    // ---- 1. copy independence (RE-07) ----
    {
        auto a = std::make_unique<Object>("A", cube);
        auto b = std::make_unique<Object>(*a);
        CHECK(liveVAO.size() == 2 && liveVBO.size() == 2, "copy allocates its OWN VAO/VBO (2 live each)");
        a.reset();
        CHECK(liveVAO.size() == 1 && liveVBO.size() == 1, "deleting the original leaves the copy's buffers alive");
        b.reset();
        CHECK(liveVAO.empty() && liveVBO.empty() && badDeletes == 0, "deleting the copy frees the rest; no double delete, no leak");
    }

    // ---- 2. move leaves the source inert ----
    {
        Object a("A", cube);
        { Object b(std::move(a)); CHECK(liveVAO.size() == 1, "move transfers handles without allocating"); }
        CHECK(liveVAO.empty() && badDeletes == 0, "moved-from object's destructor does not double free");
    }

    // ---- 3. global transform + cycle rejection (RE-02 / graph safety) ----
    {
        Object g("G", cube), p("P", cube), c("C", cube);
        g.position = {100, 0, 0}; p.position = {10, 0, 0}; c.position = {1, 0, 0};
        p.setParent(&g); c.setParent(&p);
        glm::vec3 w(c.getGlobalTransform()[3]);
        CHECK(std::abs(w.x - 111.0f) < 1e-4f, "3-level hierarchy: child world position is (111,0,0)");
        g.setParent(&c); // would create a cycle
        CHECK(g.parent == nullptr, "parenting an object under its own descendant is rejected");
        g.setParent(&g);
        CHECK(g.parent == nullptr, "self-parenting is rejected");
        c.setParent(nullptr);
        CHECK(p.children.empty() && c.parent == nullptr, "re-parenting to null cleanly detaches both sides");
    }

    // ---- 4. Scene removal detaches hierarchy (RE-01 / RE-08) ----
    {
        Scene scene;
        scene.addObject(std::make_unique<Object>("Parent", cube));
        scene.addObject(std::make_unique<Object>("Child1", cube));
        scene.addObject(std::make_unique<Object>("Child2", cube));
        Object* parent = scene.findByName("Parent");
        Object* c1 = scene.findByName("Child1");
        Object* c2 = scene.findByName("Child2");
        c1->setParent(parent); c2->setParent(parent);
        CHECK(scene.removeObject(parent), "removeObject(parent) succeeds");
        CHECK(scene.objectCount() == 2, "children are NOT deleted along with the parent");
        CHECK(c1->parent == nullptr && c2->parent == nullptr, "no child is left with a dangling parent pointer");
        CHECK(!scene.removeObject(parent), "removing the already-removed pointer fails gracefully (no UB)");
        CHECK(liveVAO.size() == 2, "exactly the two surviving objects still hold GPU buffers");
        scene.clear();
        CHECK(liveVAO.empty() && liveVBO.empty() && badDeletes == 0, "clear() frees everything, no double delete");
    }

    // ---- 5. scene format 3: stable ids, parents by id, Script objects ----
    {
        Scene s;
        auto first = std::make_unique<Object>("Cube", cube);
        auto second = std::make_unique<Object>("Cube", cube);       // names may repeat
        second->position = { 5, 0, 0 };
        Object* firstRaw = first.get();
        Object* secondRaw = second.get();
        s.addObject(std::move(first));
        s.addObject(std::move(second));
        auto kidOwner = std::make_unique<Object>("Kid", cube);
        Object* kidRaw = kidOwner.get();
        s.addObject(std::move(kidOwner));
        kidRaw->setParent(secondRaw);   // the SECOND "Cube": a name-based file could not tell them apart
        kidRaw->position = { 1, 2, 3 };
        auto scriptOwner = std::make_unique<Object>("Brain", std::vector<Vertex>());
        scriptOwner->kind = Object::Kind::Script;
        scriptOwner->scriptPath = "scripts/Brain.lua";
        scriptOwner->scriptEnabled = false;
        Object* scriptRaw = scriptOwner.get();
        s.addObject(std::move(scriptOwner));
        scriptRaw->setParent(kidRaw);   // a script attached to a part
        const uint64_t kidId = kidRaw->id, firstId = firstRaw->id, secondId = secondRaw->id;
        CHECK(kidId != 0 && kidId != firstId && firstId != secondId, "every object gets its own non-zero id");
        CHECK(s.saveToFile("/tmp/rt_scene.json"), "saveToFile reports success");

        Scene t;
        CHECK(t.loadFromFile("/tmp/rt_scene.json"), "loadFromFile reports success");
        CHECK(t.objectCount() == 4, "all four objects load back");
        CHECK(t.findById(kidId) && t.findById(firstId) && t.findById(secondId), "ids survive the round trip");
        Object* kid = t.findById(kidId);
        CHECK(kid->parent == t.findById(secondId) && kid->parent != t.findById(firstId), "parent link follows the id, not the (duplicate) name");
        CHECK(t.findById(firstId)->name == "Cube" && t.findById(secondId)->name == "Cube", "duplicate names are kept as they are");
        CHECK(kid->position == glm::vec3(1, 2, 3), "transform restored");
        CHECK(glm::length(kid->vertices[0].normal) > 0.99f, "vertex normals survive the round trip");
        Object* brain = t.findByName("Brain");
        CHECK(brain && brain->kind == Object::Kind::Script && brain->scriptPath == "scripts/Brain.lua" && !brain->scriptEnabled, "Script object: kind, source path and enabled flag survive");
        CHECK(brain->parent == kid && brain->vertices.empty(), "a script stays attached to its part and has no mesh");

        // malformed or wrong-format files must not wipe the existing scene
        { std::ofstream bad("/tmp/rt_bad.json"); bad << "{ this is not json"; }
        CHECK(!t.loadFromFile("/tmp/rt_bad.json") && t.objectCount() == 4, "unparseable file is rejected and leaves the scene intact");
        { std::ofstream bad("/tmp/rt_bad2.json"); bad << R"({"format":3,"objects":[{"name":"X","position":"oops"}]})"; }
        CHECK(!t.loadFromFile("/tmp/rt_bad2.json") && t.objectCount() == 4, "structurally wrong file is rejected and leaves the scene intact");
        { std::ofstream old("/tmp/rt_v2.json"); old << R"({"version":2,"objects":[{"name":"Old","position":[0,0,0]}]})"; }
        CHECK(!t.loadFromFile("/tmp/rt_v2.json") && t.objectCount() == 4, "an old (pre-format-3) file is rejected with a clear error");

        // hand-edited files: a missing parent and a repeated id are survivable
        { std::ofstream f("/tmp/rt_edit.json");
          f << R"({"format":3,"objects":[
              {"id":"00000000000000aa","name":"A","class":"Part","mesh":"cube"},
              {"id":"00000000000000aa","name":"B","class":"Part","mesh":"cube"},
              {"id":"00000000000000bb","name":"Orphan","class":"Part","mesh":"cube","parent":"00000000000000ff"}]})"; }
        Scene u;
        CHECK(u.loadFromFile("/tmp/rt_edit.json") && u.objectCount() == 3, "file with a repeated id still loads");
        CHECK(u.findByName("A")->id != u.findByName("B")->id, "the repeated id was replaced so ids stay unique");
        CHECK(u.findByName("Orphan")->parent == nullptr, "a parent id that does not exist leaves the object unparented");
        CHECK(u.findByName("A")->vertices.size() == 36, "\"mesh\": \"cube\" loads the built-in cube");
    }
    {
        Object a("A", cube);
        Object copy(a);
        CHECK(copy.id != a.id && copy.id != 0, "a copy gets a fresh id");
        const uint64_t id = a.id;
        Object moved(std::move(a));
        CHECK(moved.id == id, "a move keeps the id");
        CHECK(Object::idFromString(Object::idToString(id)) == id, "id <-> string round trip");
        CHECK(Object::idFromString("nonsense") == 0 && Object::idFromString("") == 0 && Object::idFromString("11111111111111111") == 0, "malformed id strings parse to 0");
    }
    CHECK(liveVAO.empty() && liveVBO.empty() && badDeletes == 0, "after everything: no leaked or double-freed GPU buffers");

    std::printf("\nALL CORE TESTS PASSED\n");
    return 0;
}
