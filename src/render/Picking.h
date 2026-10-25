#pragma once
//
// Picking.h -- which part is under the mouse, without the GPU or physics:
// the ray is tested against each part's real shape (box, ball, cylinder,
// wedge triangles) in the part's own space, so rotated and stretched parts
// are hit exactly where they are drawn. Used by the editor for click
// selection and for dropping new parts onto surfaces.
//
#include "scene/Scene.h"

#include <functional>
#include <optional>

namespace rift {

struct PickHit {
    Instance* part = nullptr;
    float distance = 0.0f;
    glm::vec3 position{ 0.0f };
    glm::vec3 normal{ 0.0f, 1.0f, 0.0f };
};

// `accept` filters candidates (nullptr = every visible part in Workspace).
std::optional<PickHit> pickPart(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction,
                                const std::function<bool(const Instance&)>& accept = nullptr);

// The nearest part or terrain surface (the hit's part is then the Terrain
// instance). Terrain also hides the parts behind it from pickPart.
std::optional<PickHit> pickSurface(const Scene& scene, const glm::vec3& origin, const glm::vec3& direction, bool hitWater = false);

// Ray against the ground plane y = 0 (placing parts in an empty scene).
std::optional<glm::vec3> pickGround(const glm::vec3& origin, const glm::vec3& direction);

} // namespace rift
