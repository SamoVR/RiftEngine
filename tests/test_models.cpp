// Model files for MeshParts: OBJ, glTF and GLB.
#include "doctest.h"
#include "render/ModelLoader.h"

#include <glm/gtc/epsilon.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    struct TempDir {
        fs::path path = fs::temp_directory_path() / ("rift-models-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        TempDir() { fs::remove_all(path); fs::create_directories(path); }
        ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    };

    bool near(const glm::vec3& a, const glm::vec3& b) { return glm::all(glm::epsilonEqual(a, b, 1e-4f)); }

    // A right triangle 2 wide and 4 tall, as float positions and uint16 indices.
    std::string triangleBuffer() {
        const float positions[9] = { 0, 0, 0, 2, 0, 0, 0, 4, 0 };
        const uint16_t indices[3] = { 0, 1, 2 };
        std::string b(reinterpret_cast<const char*>(positions), sizeof(positions));
        b.append(reinterpret_cast<const char*>(indices), sizeof(indices));
        b.append(2, '\0'); // pad to 4 bytes
        return b;
    }

    std::string triangleJson(const std::string& bufferUri) {
        std::string uri = bufferUri.empty() ? "" : "\"uri\":\"" + bufferUri + "\",";
        return R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],
            "nodes":[{"mesh":0,"translation":[10,0,0]}],
            "meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
            "materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}}],
            "buffers":[{)" + uri + R"("byteLength":44}],
            "bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
            "accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
                         {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]})";
    }
}

TEST_CASE("OBJ: quads, negative indices, normals and materials") {
    TempDir dir;
    std::ofstream(dir.path / "box.mtl") << "newmtl Red\nKd 0.8 0.1 0.1\nmap_Kd wood.png\n";
    std::ofstream(dir.path / "box.obj") <<
        "mtllib box.mtl\n"
        "o Plane\n"
        "v 0 0 0\nv 4 0 0\nv 4 0 2\nv 0 0 2\nv 2 1 1\n"
        "vt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
        "vn 0 1 0\n"
        "usemtl Red\n"
        "f 1/1/1 2/2/1 3/3/1 4/4/1\n"   // quad -> 2 triangles, shared vertices
        "f -5 -4 -1\n";                 // relative indices, no normal -> flat normal
    ModelData m;
    std::string error;
    REQUIRE_MESSAGE(loadModel(dir.path / "box.obj", m, &error), error);
    CHECK(m.mesh.indices.size() == 9);
    CHECK(near(m.nativeSize, { 4, 1, 2 }));
    CHECK(near(m.mesh.boundsMin, { -0.5f, -0.5f, -0.5f }));
    CHECK(near(m.mesh.boundsMax, { 0.5f, 0.5f, 0.5f }));
    CHECK(m.hasColor);
    CHECK(m.color.x == doctest::Approx(0.8f));
    CHECK(m.texture.filename() == "wood.png");
    for (const Vertex& v : m.mesh.vertices) CHECK(glm::length(v.normal) == doctest::Approx(1.0f).epsilon(0.01));
}

TEST_CASE("glTF with an external buffer and node transforms") {
    TempDir dir;
    std::ofstream(dir.path / "tri.bin", std::ios::binary) << triangleBuffer();
    std::ofstream(dir.path / "tri.gltf") << triangleJson("tri.bin");
    ModelData m;
    std::string error;
    REQUIRE_MESSAGE(loadModel(dir.path / "tri.gltf", m, &error), error);
    CHECK(m.mesh.vertices.size() == 3);
    CHECK(m.mesh.indices.size() == 3);
    CHECK(m.meshCount == 1);
    CHECK(m.nativeSize.x == doctest::Approx(2.0f));
    CHECK(m.nativeSize.y == doctest::Approx(4.0f));
    CHECK(m.hasColor);
    CHECK(m.color.x == doctest::Approx(1.0f));
    CHECK(m.color.y == doctest::Approx(0.0f));
}

TEST_CASE("GLB: JSON and binary chunks in one file") {
    TempDir dir;
    std::string json = triangleJson("");
    while (json.size() % 4) json += ' ';
    const std::string bin = triangleBuffer();
    std::string glb = "glTF";
    auto u32 = [&](uint32_t v) { glb.append(reinterpret_cast<const char*>(&v), 4); };
    u32(2);
    u32(static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
    u32(static_cast<uint32_t>(json.size()));
    u32(0x4E4F534A);
    glb += json;
    u32(static_cast<uint32_t>(bin.size()));
    u32(0x004E4942);
    glb += bin;
    std::ofstream(dir.path / "tri.glb", std::ios::binary) << glb;
    ModelData m;
    std::string error;
    REQUIRE_MESSAGE(loadModel(dir.path / "tri.glb", m, &error), error);
    CHECK(m.mesh.indices.size() == 3);
    CHECK(m.nativeSize.y == doctest::Approx(4.0f));
}

TEST_CASE("broken model files are reported, not crashed on") {
    TempDir dir;
    std::ofstream(dir.path / "bad.obj") << "v 0 0 0\nf 1 2 3\n";
    std::ofstream(dir.path / "bad.gltf") << "{ not json";
    std::ofstream(dir.path / "empty.glb", std::ios::binary) << "nope";
    ModelData m;
    std::string error;
    CHECK_FALSE(loadModel(dir.path / "bad.obj", m, &error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(loadModel(dir.path / "bad.gltf", m, &error));
    CHECK_FALSE(loadModel(dir.path / "empty.glb", m, &error));
    CHECK_FALSE(loadModel(dir.path / "missing.obj", m, &error));
    CHECK_FALSE(loadModel(dir.path / "model.fbx", m, &error));
}
