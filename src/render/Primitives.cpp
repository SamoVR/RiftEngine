#include "render/Primitives.h"

#include <array>
#include <cmath>

namespace rift {

namespace {
    constexpr float kPi = 3.14159265358979f;

    // Adds a flat quad (a, b, c, d counter-clockwise seen from outside) with
    // its own vertices, so faces get hard edges. UVs map the face's two
    // in-plane directions; Renderer scales them by the part size (studs).
    void quad(MeshData& m, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
        const glm::vec3 n = glm::normalize(glm::cross(b - a, d - a));
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        const glm::vec2 uv[4] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
        const glm::vec3 p[4] = { a, b, c, d };
        for (int i = 0; i < 4; ++i) m.vertices.push_back({ p[i], uv[i], n });
        for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) m.indices.push_back(base + i);
    }

    void tri(MeshData& m, glm::vec3 a, glm::vec3 b, glm::vec3 c) {
        const glm::vec3 n = glm::normalize(glm::cross(b - a, c - a));
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back({ a, { 0, 0 }, n });
        m.vertices.push_back({ b, { 1, 0 }, n });
        m.vertices.push_back({ c, { 0.5f, 1 }, n });
        for (uint32_t i : { 0u, 1u, 2u }) m.indices.push_back(base + i);
    }
}

void MeshData::computeBounds() {
    if (vertices.empty()) { boundsMin = boundsMax = glm::vec3(0.0f); return; }
    boundsMin = boundsMax = vertices[0].position;
    for (const Vertex& v : vertices) {
        boundsMin = glm::min(boundsMin, v.position);
        boundsMax = glm::max(boundsMax, v.position);
    }
}

MeshData makeBlock() {
    MeshData m;
    const float h = 0.5f;
    quad(m, { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }); // +Z
    quad(m, {  h, -h, -h }, { -h, -h, -h }, { -h,  h, -h }, {  h,  h, -h }); // -Z
    quad(m, { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h }, { -h,  h, -h }); // -X
    quad(m, {  h, -h,  h }, {  h, -h, -h }, {  h,  h, -h }, {  h,  h,  h }); // +X
    quad(m, { -h,  h,  h }, {  h,  h,  h }, {  h,  h, -h }, { -h,  h, -h }); // +Y
    quad(m, { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h }); // -Y
    m.computeBounds();
    return m;
}

MeshData makeBall(int rings, int segments) {
    MeshData m;
    for (int i = 0; i <= rings; ++i) {
        const float v = static_cast<float>(i) / rings;
        const float phi = kPi * v;
        for (int j = 0; j <= segments; ++j) {
            const float u = static_cast<float>(j) / segments;
            const float theta = 2.0f * kPi * u;
            const glm::vec3 n(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
            m.vertices.push_back({ n * 0.5f, { u, 1.0f - v }, n });
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            const uint32_t a = i * (segments + 1) + j, b = a + segments + 1;
            m.indices.insert(m.indices.end(), { a, a + 1, b, b, a + 1, b + 1 });
        }
    }
    m.computeBounds();
    return m;
}

MeshData makeCylinder(int segments) {
    MeshData m;
    const float h = 0.5f, r = 0.5f;
    // Side: smooth normals, own vertices so the caps keep hard edges.
    for (int j = 0; j <= segments; ++j) {
        const float u = static_cast<float>(j) / segments;
        const float a = 2.0f * kPi * u;
        const glm::vec3 n(std::cos(a), 0.0f, std::sin(a));
        m.vertices.push_back({ glm::vec3(n.x * r, -h, n.z * r), { u * 3.0f, 0.0f }, n });
        m.vertices.push_back({ glm::vec3(n.x * r,  h, n.z * r), { u * 3.0f, 1.0f }, n });
    }
    for (int j = 0; j < segments; ++j) {
        const uint32_t a = j * 2;
        m.indices.insert(m.indices.end(), { a, a + 1, a + 2, a + 2, a + 1, a + 3 });
    }
    // Caps.
    for (int cap = 0; cap < 2; ++cap) {
        const float y = cap == 0 ? h : -h;
        const glm::vec3 n(0.0f, cap == 0 ? 1.0f : -1.0f, 0.0f);
        const uint32_t center = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back({ glm::vec3(0.0f, y, 0.0f), { 0.5f, 0.5f }, n });
        for (int j = 0; j <= segments; ++j) {
            const float a = 2.0f * kPi * j / segments;
            m.vertices.push_back({ glm::vec3(std::cos(a) * r, y, std::sin(a) * r), { 0.5f + std::cos(a) * 0.5f, 0.5f + std::sin(a) * 0.5f }, n });
        }
        for (int j = 0; j < segments; ++j) {
            const uint32_t a = center + 1 + j, b = a + 1;
            if (cap == 0) m.indices.insert(m.indices.end(), { center, b, a });
            else m.indices.insert(m.indices.end(), { center, a, b });
        }
    }
    m.computeBounds();
    return m;
}

// Every face below is listed counter-clockwise as seen from outside, so the
// winding and the computed normal both point outwards (tests check this).
MeshData makeWedge() {
    MeshData m;
    const float h = 0.5f;
    quad(m, { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h }); // bottom (-Y)
    quad(m, { -h, -h,  h }, {  h, -h,  h }, {  h,  h,  h }, { -h,  h,  h }); // back (+Z)
    quad(m, {  h, -h, -h }, { -h, -h, -h }, { -h,  h,  h }, {  h,  h,  h }); // slope (up and -Z)
    tri(m, { -h, -h, -h }, { -h, -h,  h }, { -h,  h,  h });                   // left (-X)
    tri(m, {  h, -h, -h }, {  h,  h,  h }, {  h, -h,  h });                   // right (+X)
    m.computeBounds();
    return m;
}

MeshData makeCornerWedge() {
    MeshData m;
    const float h = 0.5f;
    const glm::vec3 apex(h, h, h);
    quad(m, { -h, -h, -h }, {  h, -h, -h }, {  h, -h,  h }, { -h, -h,  h }); // bottom (-Y)
    tri(m, {  h, -h, -h }, { -h, -h, -h }, apex);  // front slope
    tri(m, {  h, -h,  h }, {  h, -h, -h }, apex);  // right (+X)
    tri(m, { -h, -h,  h }, {  h, -h,  h }, apex);  // back (+Z)
    tri(m, { -h, -h, -h }, { -h, -h,  h }, apex);  // left slope
    m.computeBounds();
    return m;
}

const MeshData& primitiveMesh(PartShape shape) {
    static const std::array<MeshData, 5> meshes = { makeBlock(), makeBall(), makeCylinder(), makeWedge(), makeCornerWedge() };
    const int i = static_cast<int>(shape);
    return meshes[i >= 0 && i < 5 ? i : 0];
}

std::vector<glm::vec3> hullPoints(PartShape shape) {
    const float h = 0.5f;
    switch (shape) {
        case PartShape::Wedge:
            return { { -h, -h, -h }, { h, -h, -h }, { -h, -h, h }, { h, -h, h }, { -h, h, h }, { h, h, h } };
        case PartShape::CornerWedge:
            return { { -h, -h, -h }, { h, -h, -h }, { -h, -h, h }, { h, -h, h }, { h, h, h } };
        case PartShape::Ball: {
            std::vector<glm::vec3> pts;
            for (const Vertex& v : makeBall(8, 16).vertices) pts.push_back(v.position);
            return pts;
        }
        case PartShape::Cylinder: {
            std::vector<glm::vec3> pts;
            for (int j = 0; j < 24; ++j) {
                const float a = 2.0f * kPi * j / 24;
                pts.push_back({ std::cos(a) * h, -h, std::sin(a) * h });
                pts.push_back({ std::cos(a) * h,  h, std::sin(a) * h });
            }
            return pts;
        }
        case PartShape::Block:
        default:
            return { { -h, -h, -h }, { h, -h, -h }, { -h, h, -h }, { h, h, -h }, { -h, -h, h }, { h, -h, h }, { -h, h, h }, { h, h, h } };
    }
}

} // namespace rift
