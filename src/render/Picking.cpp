#include "render/Picking.h"
#include "render/Primitives.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rift {

namespace {
    // Slab test against the unit box [-0.5, 0.5]^3 (rejects most parts cheaply).
    bool rayUnitBox(const glm::vec3& o, const glm::vec3& d, float& tNear) {
        float t0 = 0.0f, t1 = std::numeric_limits<float>::max();
        for (int a = 0; a < 3; ++a) {
            if (std::abs(d[a]) < 1e-9f) {
                if (o[a] < -0.5f || o[a] > 0.5f) return false;
                continue;
            }
            float ta = (-0.5f - o[a]) / d[a], tb = (0.5f - o[a]) / d[a];
            if (ta > tb) std::swap(ta, tb);
            t0 = std::max(t0, ta);
            t1 = std::min(t1, tb);
            if (t0 > t1) return false;
        }
        tNear = t0;
        return true;
    }

    // Moller-Trumbore; t in the ray's own parameter.
    bool rayTriangle(const glm::vec3& o, const glm::vec3& d, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c, float& t) {
        const glm::vec3 e1 = b - a, e2 = c - a;
        const glm::vec3 p = glm::cross(d, e2);
        const float det = glm::dot(e1, p);
        if (std::abs(det) < 1e-12f) return false;
        const float inv = 1.0f / det;
        const glm::vec3 s = o - a;
        const float u = glm::dot(s, p) * inv;
        if (u < 0.0f || u > 1.0f) return false;
        const glm::vec3 q = glm::cross(s, e1);
        const float v = glm::dot(d, q) * inv;
        if (v < 0.0f || u + v > 1.0f) return false;
        t = glm::dot(e2, q) * inv;
        return t >= 0.0f;
    }
}

std::optional<PickHit> pickPart(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction,
                                const std::function<bool(const Instance&)>& accept) {
    const glm::vec3 dir = glm::normalize(direction);
    std::optional<PickHit> best;
    float bestDistance = std::numeric_limits<float>::max();

    Scene::forEachDescendant(scene.workspace(), [&](Instance& part) {
        if (!part.isBasePart()) return;
        if (accept ? !accept(part) : (!part.visible || part.transparency >= 0.999f)) return;
        const glm::mat4 world = part.worldTransform();
        if (std::abs(glm::determinant(world)) < 1e-12f) return;
        const glm::mat4 inv = glm::inverse(world);
        const glm::vec3 o = glm::vec3(inv * glm::vec4(origin, 1.0f));
        const glm::vec3 d = glm::vec3(inv * glm::vec4(dir, 0.0f)); // not normalised: t stays in world units
        float tBox = 0.0f;
        if (!rayUnitBox(o, d, tBox) || tBox > bestDistance) return;

        // Exact shape: blocks are their box; others are tested per triangle.
        const PartShape shape = part.cls == ClassId::Part ? part.shape : PartShape::Block;
        float tHit = tBox;
        glm::vec3 localNormal(0.0f);
        if (shape == PartShape::Block) {
            const glm::vec3 p = o + d * tBox;
            int axis = 0;
            for (int a = 1; a < 3; ++a) if (std::abs(p[a]) > std::abs(p[axis])) axis = a;
            localNormal[axis] = p[axis] > 0.0f ? 1.0f : -1.0f;
        } else {
            const MeshData& mesh = primitiveMesh(shape);
            bool hit = false;
            float closest = std::numeric_limits<float>::max();
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
                const glm::vec3& a = mesh.vertices[mesh.indices[i]].position;
                const glm::vec3& b = mesh.vertices[mesh.indices[i + 1]].position;
                const glm::vec3& c = mesh.vertices[mesh.indices[i + 2]].position;
                float t;
                if (rayTriangle(o, d, a, b, c, t) && t < closest) {
                    closest = t;
                    localNormal = glm::cross(b - a, c - a);
                    hit = true;
                }
            }
            if (!hit) return;
            tHit = closest;
        }
        if (tHit >= bestDistance) return;
        bestDistance = tHit;
        PickHit h;
        h.part = &part;
        h.distance = tHit;
        h.position = origin + dir * tHit;
        h.normal = glm::normalize(glm::transpose(glm::mat3(inv)) * localNormal);
        if (glm::dot(h.normal, dir) > 0.0f) h.normal = -h.normal;
        best = h;
    });
    return best;
}

std::optional<glm::vec3> pickGround(const glm::vec3& origin, const glm::vec3& direction) {
    if (std::abs(direction.y) < 1e-6f) return std::nullopt;
    const float t = -origin.y / direction.y;
    if (t < 0.0f) return std::nullopt;
    return origin + direction * t;
}

} // namespace rift
