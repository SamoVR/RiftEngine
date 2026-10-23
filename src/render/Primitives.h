#pragma once
//
// Primitives.h -- CPU geometry of the built-in part shapes (no OpenGL here, so
// physics and tests use it too). Every shape fits the unit cube [-0.5, 0.5]^3
// and is scaled by the part's Size.
//
//   Block       unit box
//   Ball        sphere of diameter 1
//   Cylinder    along Y: radius 0.5, height 1
//   Wedge       ramp: full bottom and back (+Z) faces, slope rising towards +Z
//   CornerWedge square base, apex above the (+X, +Z) corner
//
#include "render/Vertex.h"
#include "scene/Instance.h"

#include <cstdint>
#include <vector>

namespace rift {

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;  // triangles
    glm::vec3 boundsMin{ 0.0f }, boundsMax{ 0.0f };
    void computeBounds();
};

MeshData makeBlock();
MeshData makeBall(int rings = 24, int segments = 48);
MeshData makeCylinder(int segments = 48);
MeshData makeWedge();
MeshData makeCornerWedge();
const MeshData& primitiveMesh(PartShape shape); // cached

// Corner points of the convex primitives (physics hulls).
std::vector<glm::vec3> hullPoints(PartShape shape);

} // namespace rift
