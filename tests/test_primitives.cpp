// Built-in part shapes: closed, fit the unit cube, faces wound outwards.
#include "doctest.h"
#include "render/Primitives.h"

using namespace rift;

TEST_CASE("primitive meshes face outwards and fit the unit cube") {
    for (PartShape shape : { PartShape::Block, PartShape::Ball, PartShape::Cylinder, PartShape::Wedge, PartShape::CornerWedge }) {
        const MeshData& m = primitiveMesh(shape);
        INFO("shape " << static_cast<int>(shape));
        REQUIRE(m.indices.size() % 3 == 0);
        REQUIRE(!m.indices.empty());
        CHECK(m.boundsMin.x == doctest::Approx(-0.5f));
        CHECK(m.boundsMax.y == doctest::Approx(0.5f));

        // A point inside every convex shape: the average of its hull points.
        glm::vec3 inside(0.0f);
        const auto hull = hullPoints(shape);
        for (const glm::vec3& p : hull) inside += p;
        inside /= static_cast<float>(hull.size());

        int wrong = 0;
        for (size_t i = 0; i < m.indices.size(); i += 3) {
            const glm::vec3 a = m.vertices[m.indices[i]].position;
            const glm::vec3 b = m.vertices[m.indices[i + 1]].position;
            const glm::vec3 c = m.vertices[m.indices[i + 2]].position;
            const glm::vec3 n = glm::cross(b - a, c - a);
            if (glm::length(n) < 1e-9f) continue; // degenerate (sphere poles)
            const glm::vec3 centroid = (a + b + c) / 3.0f;
            if (glm::dot(n, centroid - inside) <= 0.0f) ++wrong;
            // Stored normals agree with the winding.
            const glm::vec3 stored = m.vertices[m.indices[i]].normal;
            if (glm::dot(stored, n) <= 0.0f) ++wrong;
        }
        CHECK(wrong == 0);
    }
}
