#include "render/ModelLoader.h"
#include "core/Paths.h"

#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <tuple>

namespace rift {

namespace fs = std::filesystem;

namespace {
    bool fail(std::string* error, const std::string& message) {
        if (error) *error = message;
        return false;
    }

    bool readAll(const fs::path& file, std::string& out) {
        std::ifstream in(file, std::ios::binary);
        if (!in.is_open()) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    }

    std::string lower(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    // Gives triangles without normals a flat normal.
    void fillMissingNormals(MeshData& m) {
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3) {
            Vertex& a = m.vertices[m.indices[t]];
            Vertex& b = m.vertices[m.indices[t + 1]];
            Vertex& c = m.vertices[m.indices[t + 2]];
            const glm::vec3 n = glm::cross(b.position - a.position, c.position - a.position);
            if (glm::length(n) < 1e-12f) continue;
            const glm::vec3 nn = glm::normalize(n);
            for (Vertex* v : { &a, &b, &c }) if (glm::length(v->normal) < 1e-6f) v->normal = nn;
        }
    }

    // Centre the mesh and scale it into the unit cube; returns its real size.
    glm::vec3 normalise(MeshData& m) {
        m.computeBounds();
        const glm::vec3 center = (m.boundsMin + m.boundsMax) * 0.5f;
        const glm::vec3 extent = glm::max(m.boundsMax - m.boundsMin, glm::vec3(1e-4f));
        for (Vertex& v : m.vertices) {
            v.position = (v.position - center) / extent;
            // Normals transform with the inverse transpose of the scale.
            if (glm::length(v.normal) > 1e-6f) v.normal = glm::normalize(v.normal * extent);
        }
        m.computeBounds();
        return extent;
    }

    // ------------------------------------------------------------------ OBJ
    struct ObjMaterial { glm::vec3 kd{ 1.0f }; bool hasKd = false; std::string mapKd; };

    std::map<std::string, ObjMaterial> readMtl(const fs::path& file) {
        std::map<std::string, ObjMaterial> out;
        std::ifstream in(file);
        std::string line, current;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string key;
            ls >> key;
            if (key == "newmtl") { ls >> current; out[current]; }
            else if (key == "Kd" && !current.empty()) { ls >> out[current].kd.x >> out[current].kd.y >> out[current].kd.z; out[current].hasKd = true; }
            else if (key == "map_Kd" && !current.empty()) {
                std::string rest;
                std::getline(ls, rest);
                // The file name is the last token (options like -s come first).
                const size_t start = rest.find_first_not_of(" \t");
                if (start != std::string::npos) {
                    rest = rest.substr(start);
                    const size_t space = rest.rfind(' ');
                    out[current].mapKd = space == std::string::npos ? rest : rest.substr(space + 1);
                }
            }
        }
        return out;
    }

    bool loadObj(const fs::path& file, ModelData& out, std::string* error) {
        std::ifstream in(file);
        if (!in.is_open()) return fail(error, "cannot open the file");
        std::vector<glm::vec3> positions, normals;
        std::vector<glm::vec2> uvs;
        std::map<std::tuple<int, int, int>, uint32_t> unique;
        std::map<std::string, ObjMaterial> materials;
        std::string firstMaterial;
        MeshData& m = out.mesh;
        int objects = 0;

        auto resolve = [](int index, size_t count) { return index < 0 ? static_cast<int>(count) + index : index - 1; };
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ls(line);
            std::string key;
            ls >> key;
            if (key == "v") { glm::vec3 p; ls >> p.x >> p.y >> p.z; positions.push_back(p); }
            else if (key == "vt") { glm::vec2 t; ls >> t.x >> t.y; uvs.push_back(t); }
            else if (key == "vn") { glm::vec3 n; ls >> n.x >> n.y >> n.z; normals.push_back(n); }
            else if (key == "o" || key == "g") ++objects;
            else if (key == "mtllib") {
                std::string name;
                std::getline(ls, name);
                name.erase(0, name.find_first_not_of(" \t"));
                while (!name.empty() && (name.back() == '\r' || name.back() == ' ')) name.pop_back();
                const auto loaded = readMtl(file.parent_path() / Paths::fromUtf8(name));
                materials.insert(loaded.begin(), loaded.end());
            } else if (key == "usemtl") {
                if (firstMaterial.empty()) ls >> firstMaterial;
            } else if (key == "f") {
                std::vector<uint32_t> face;
                std::string token;
                while (ls >> token) {
                    int vi = 0, ti = 0, ni = 0;
                    const size_t s1 = token.find('/');
                    vi = std::atoi(token.substr(0, s1).c_str());
                    if (s1 != std::string::npos) {
                        const size_t s2 = token.find('/', s1 + 1);
                        const std::string t = token.substr(s1 + 1, s2 == std::string::npos ? std::string::npos : s2 - s1 - 1);
                        if (!t.empty()) ti = std::atoi(t.c_str());
                        if (s2 != std::string::npos) ni = std::atoi(token.substr(s2 + 1).c_str());
                    }
                    const int p = resolve(vi, positions.size());
                    const int t = ti ? resolve(ti, uvs.size()) : -1;
                    const int n = ni ? resolve(ni, normals.size()) : -1;
                    if (p < 0 || p >= static_cast<int>(positions.size())) return fail(error, "a face uses a vertex that does not exist");
                    Vertex v;
                    v.position = positions[p];
                    if (t >= 0 && t < static_cast<int>(uvs.size())) v.texCoord = uvs[t];
                    const bool hasNormal = n >= 0 && n < static_cast<int>(normals.size());
                    if (hasNormal) v.normal = normals[n];
                    // Vertices with normals are shared; without, each face keeps
                    // its own so it gets a flat normal later.
                    if (!hasNormal) {
                        face.push_back(static_cast<uint32_t>(m.vertices.size()));
                        m.vertices.push_back(v);
                        continue;
                    }
                    const auto key3 = std::make_tuple(p, t, n);
                    auto it = unique.find(key3);
                    if (it == unique.end()) {
                        it = unique.emplace(key3, static_cast<uint32_t>(m.vertices.size())).first;
                        m.vertices.push_back(v);
                    }
                    face.push_back(it->second);
                }
                // Polygons become triangle fans.
                for (size_t k = 1; k + 1 < face.size(); ++k) {
                    const uint32_t tri[3] = { face[0], face[k], face[k + 1] };
                    m.indices.insert(m.indices.end(), tri, tri + 3);
                }
            }
        }
        if (m.indices.empty()) return fail(error, "the file has no faces");
        out.meshCount = std::max(objects, 1);
        const std::string matName = firstMaterial.empty() && !materials.empty() ? materials.begin()->first : firstMaterial;
        if (auto it = materials.find(matName); it != materials.end()) {
            out.hasColor = it->second.hasKd;
            out.color = it->second.kd;
            if (!it->second.mapKd.empty()) out.texture = file.parent_path() / Paths::fromUtf8(it->second.mapKd);
        }
        return true;
    }

    // ------------------------------------------------------------------ glTF
    std::string base64Decode(const std::string& in) {
        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        int val = 0, bits = -8;
        for (unsigned char c : in) {
            const size_t p = chars.find(static_cast<char>(c));
            if (p == std::string::npos) continue;
            val = (val << 6) + static_cast<int>(p);
            bits += 6;
            if (bits >= 0) {
                out.push_back(static_cast<char>((val >> bits) & 0xFF));
                bits -= 8;
            }
        }
        return out;
    }

    struct Gltf {
        nlohmann::json j;
        std::vector<std::string> buffers;
        fs::path folder;

        // Reads accessor `index` as floats (count x components); integers are converted.
        bool read(int index, std::vector<float>& out, int& components) const {
            const auto& accessors = j.at("accessors");
            if (index < 0 || index >= static_cast<int>(accessors.size())) return false;
            const auto& a = accessors[index];
            const std::string type = a.value("type", "SCALAR");
            components = type == "SCALAR" ? 1 : type == "VEC2" ? 2 : type == "VEC3" ? 3 : type == "VEC4" ? 4 : type == "MAT4" ? 16 : 0;
            if (!components) return false;
            const int componentType = a.value("componentType", 5126);
            const size_t count = a.value("count", 0);
            const bool normalized = a.value("normalized", false);
            out.assign(count * components, 0.0f);
            if (!a.contains("bufferView")) return true; // all zeros (sparse-only accessors are not supported)
            const auto& view = j.at("bufferViews").at(a["bufferView"].get<int>());
            const int bufferIndex = view.value("buffer", 0);
            if (bufferIndex < 0 || bufferIndex >= static_cast<int>(buffers.size())) return false;
            const std::string& buffer = buffers[bufferIndex];
            const size_t componentSize = componentType == 5126 || componentType == 5125 ? 4 : componentType == 5123 || componentType == 5122 ? 2 : 1;
            size_t stride = view.value("byteStride", static_cast<size_t>(0));
            if (stride == 0) stride = componentSize * components;
            const size_t offset = view.value("byteOffset", static_cast<size_t>(0)) + a.value("byteOffset", static_cast<size_t>(0));
            if (count && offset + stride * (count - 1) + componentSize * components > buffer.size()) return false;
            for (size_t i = 0; i < count; ++i) {
                const unsigned char* base = reinterpret_cast<const unsigned char*>(buffer.data()) + offset + stride * i;
                for (int c = 0; c < components; ++c) {
                    const unsigned char* p = base + componentSize * c;
                    float v = 0.0f;
                    switch (componentType) {
                        case 5126: std::memcpy(&v, p, 4); break;
                        case 5125: { uint32_t x; std::memcpy(&x, p, 4); v = static_cast<float>(x); break; }
                        case 5123: { uint16_t x; std::memcpy(&x, p, 2); v = normalized ? x / 65535.0f : static_cast<float>(x); break; }
                        case 5122: { int16_t x; std::memcpy(&x, p, 2); v = normalized ? std::max(x / 32767.0f, -1.0f) : static_cast<float>(x); break; }
                        case 5121: v = normalized ? *p / 255.0f : static_cast<float>(*p); break;
                        case 5120: { int8_t x; std::memcpy(&x, p, 1); v = normalized ? std::max(x / 127.0f, -1.0f) : static_cast<float>(x); break; }
                        default: return false;
                    }
                    out[i * components + c] = v;
                }
            }
            return true;
        }
    };

    glm::mat4 nodeMatrix(const nlohmann::json& node) {
        if (node.contains("matrix") && node["matrix"].size() == 16) {
            glm::mat4 m;
            for (int i = 0; i < 16; ++i) glm::value_ptr(m)[i] = node["matrix"][i].get<float>();
            return m;
        }
        glm::vec3 t(0.0f), s(1.0f);
        glm::quat r(1.0f, 0.0f, 0.0f, 0.0f);
        if (node.contains("translation")) t = { node["translation"][0].get<float>(), node["translation"][1].get<float>(), node["translation"][2].get<float>() };
        if (node.contains("scale")) s = { node["scale"][0].get<float>(), node["scale"][1].get<float>(), node["scale"][2].get<float>() };
        if (node.contains("rotation")) r = glm::quat(node["rotation"][3].get<float>(), node["rotation"][0].get<float>(), node["rotation"][1].get<float>(), node["rotation"][2].get<float>());
        return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
    }

    bool addPrimitive(const Gltf& g, const nlohmann::json& prim, const glm::mat4& world, ModelData& out, std::string* error) {
        if (prim.value("mode", 4) != 4) return true; // points / lines are skipped
        const auto& attrs = prim.at("attributes");
        if (!attrs.contains("POSITION")) return true;
        std::vector<float> pos, nrm, uv, idx;
        int pc = 0, nc = 0, uc = 0, ic = 0;
        if (!g.read(attrs["POSITION"].get<int>(), pos, pc) || pc != 3) return fail(error, "unreadable vertex positions");
        if (attrs.contains("NORMAL") && (!g.read(attrs["NORMAL"].get<int>(), nrm, nc) || nc != 3)) nrm.clear();
        if (attrs.contains("TEXCOORD_0") && (!g.read(attrs["TEXCOORD_0"].get<int>(), uv, uc) || uc != 2)) uv.clear();
        const size_t count = pos.size() / 3;
        const glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(world)));
        const uint32_t base = static_cast<uint32_t>(out.mesh.vertices.size());
        const size_t firstIndex = out.mesh.indices.size();
        for (size_t i = 0; i < count; ++i) {
            Vertex v;
            v.position = glm::vec3(world * glm::vec4(pos[i * 3], pos[i * 3 + 1], pos[i * 3 + 2], 1.0f));
            if (nrm.size() == pos.size()) v.normal = glm::normalize(normalMatrix * glm::vec3(nrm[i * 3], nrm[i * 3 + 1], nrm[i * 3 + 2]));
            if (uv.size() / 2 == count) v.texCoord = { uv[i * 2], 1.0f - uv[i * 2 + 1] }; // glTF's V points down
            out.mesh.vertices.push_back(v);
        }
        if (prim.contains("indices")) {
            if (!g.read(prim["indices"].get<int>(), idx, ic) || ic != 1) return fail(error, "unreadable triangle indices");
            for (float f : idx) out.mesh.indices.push_back(base + static_cast<uint32_t>(f));
        } else {
            for (size_t i = 0; i < count; ++i) out.mesh.indices.push_back(base + static_cast<uint32_t>(i));
        }
        // Mirrored transforms flip the winding.
        if (glm::determinant(glm::mat3(world)) < 0.0f) {
            for (size_t t = firstIndex; t + 2 < out.mesh.indices.size(); t += 3)
                std::swap(out.mesh.indices[t + 1], out.mesh.indices[t + 2]);
        }
        // First material: colour and texture.
        if (!out.hasColor && prim.contains("material") && g.j.contains("materials")) {
            const auto& mat = g.j["materials"].at(prim["material"].get<int>());
            const auto pbr = mat.value("pbrMetallicRoughness", nlohmann::json::object());
            if (pbr.contains("baseColorFactor")) {
                const auto& f = pbr["baseColorFactor"];
                // glTF colours are linear; the engine stores sRGB-ish values.
                out.color = glm::pow(glm::vec3(f[0].get<float>(), f[1].get<float>(), f[2].get<float>()), glm::vec3(1.0f / 2.2f));
                out.hasColor = true;
            }
            if (pbr.contains("baseColorTexture") && g.j.contains("textures") && g.j.contains("images")) {
                const int texIndex = pbr["baseColorTexture"].value("index", -1);
                if (texIndex >= 0 && texIndex < static_cast<int>(g.j["textures"].size())) {
                    const int source = g.j["textures"][texIndex].value("source", -1);
                    if (source >= 0 && source < static_cast<int>(g.j["images"].size())) {
                        const std::string uri = g.j["images"][source].value("uri", std::string());
                        if (!uri.empty() && uri.rfind("data:", 0) != 0) out.texture = g.folder / Paths::fromUtf8(uri);
                    }
                }
            }
        }
        return true;
    }

    bool addNode(const Gltf& g, int index, const glm::mat4& parent, ModelData& out, std::string* error, int depth) {
        if (depth > 64) return fail(error, "the node tree is too deep");
        const auto& node = g.j.at("nodes").at(index);
        const glm::mat4 world = parent * nodeMatrix(node);
        if (node.contains("mesh")) {
            const auto& mesh = g.j.at("meshes").at(node["mesh"].get<int>());
            for (const auto& prim : mesh.at("primitives")) if (!addPrimitive(g, prim, world, out, error)) return false;
            ++out.meshCount;
        }
        if (node.contains("children"))
            for (const auto& c : node["children"]) if (!addNode(g, c.get<int>(), world, out, error, depth + 1)) return false;
        return true;
    }

    bool loadGltf(const fs::path& file, bool binary, ModelData& out, std::string* error) {
        std::string data;
        if (!readAll(file, data)) return fail(error, "cannot open the file");
        Gltf g;
        g.folder = file.parent_path();
        std::string bin;
        try {
            if (binary) {
                // GLB: header (magic, version, length) then JSON and BIN chunks.
                if (data.size() < 20 || data.compare(0, 4, "glTF") != 0) return fail(error, "not a GLB file");
                size_t at = 12;
                while (at + 8 <= data.size()) {
                    uint32_t length, type;
                    std::memcpy(&length, data.data() + at, 4);
                    std::memcpy(&type, data.data() + at + 4, 4);
                    if (at + 8 + length > data.size()) break;
                    if (type == 0x4E4F534A) g.j = nlohmann::json::parse(data.begin() + at + 8, data.begin() + at + 8 + length);
                    else if (type == 0x004E4942) bin.assign(data, at + 8, length);
                    at += 8 + length;
                }
            } else {
                g.j = nlohmann::json::parse(data);
            }
            if (!g.j.is_object()) return fail(error, "no glTF data in the file");
            if (g.j.contains("buffers")) {
                for (const auto& b : g.j["buffers"]) {
                    const std::string uri = b.value("uri", std::string());
                    std::string bytes;
                    if (uri.empty()) bytes = bin;
                    else if (uri.rfind("data:", 0) == 0) {
                        const size_t comma = uri.find(',');
                        bytes = comma == std::string::npos ? std::string() : base64Decode(uri.substr(comma + 1));
                    } else if (!readAll(g.folder / Paths::fromUtf8(uri), bytes)) {
                        return fail(error, "missing buffer file " + uri);
                    }
                    g.buffers.push_back(std::move(bytes));
                }
            }
            if (!g.j.contains("meshes")) return fail(error, "the file has no meshes");
            // The default scene's nodes, or every mesh as it is.
            const int sceneIndex = g.j.value("scene", 0);
            if (g.j.contains("scenes") && sceneIndex < static_cast<int>(g.j["scenes"].size()) && g.j.contains("nodes")) {
                for (const auto& n : g.j["scenes"][sceneIndex].value("nodes", nlohmann::json::array()))
                    if (!addNode(g, n.get<int>(), glm::mat4(1.0f), out, error, 0)) return false;
            } else {
                for (const auto& mesh : g.j["meshes"]) {
                    for (const auto& prim : mesh.at("primitives")) if (!addPrimitive(g, prim, glm::mat4(1.0f), out, error)) return false;
                    ++out.meshCount;
                }
            }
        } catch (const std::exception& e) {
            return fail(error, std::string("invalid glTF: ") + e.what());
        }
        if (out.mesh.indices.empty()) return fail(error, "the file has no triangles");
        for (uint32_t i : out.mesh.indices) if (i >= out.mesh.vertices.size()) return fail(error, "a triangle uses a vertex that does not exist");
        return true;
    }
}

bool loadModel(const fs::path& file, ModelData& out, std::string* error) {
    out = ModelData();
    const std::string ext = lower(Paths::toUtf8(file.extension()));
    bool ok = false;
    if (ext == ".obj") ok = loadObj(file, out, error);
    else if (ext == ".gltf") ok = loadGltf(file, false, out, error);
    else if (ext == ".glb") ok = loadGltf(file, true, out, error);
    else return fail(error, "unsupported model format " + ext + " (use .obj, .gltf or .glb)");
    if (!ok) return false;
    fillMissingNormals(out.mesh);
    out.nativeSize = normalise(out.mesh);
    return true;
}

} // namespace rift
