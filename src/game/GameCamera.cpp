#include "game/GameCamera.h"

#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

namespace rift {

glm::mat4 GameCamera::frame() const {
    glm::mat4 m = glm::mat4_cast(rotation);
    m[3] = glm::vec4(position, 1.0f);
    return m;
}

void GameCamera::setFrame(const glm::mat4& m) {
    position = glm::vec3(m[3]);
    glm::mat3 r(m);
    for (int i = 0; i < 3; ++i) { const float l = glm::length(r[i]); if (l > 1e-8f) r[i] /= l; }
    rotation = glm::normalize(glm::quat_cast(r));
}

glm::mat4 GameCamera::view() const {
    return glm::inverse(frame());
}

glm::mat4 GameCamera::projection(float aspect) const {
    return glm::perspective(glm::radians(fieldOfView), aspect > 1e-4f ? aspect : 1.0f, nearPlane, farPlane);
}

void GameCamera::lookAt(const glm::vec3& eye, const glm::vec3& target) {
    position = eye;
    glm::vec3 forward = target - eye;
    if (glm::length(forward) < 1e-6f) return;
    forward = glm::normalize(forward);
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(forward, up)) > 0.999f) up = glm::vec3(0.0f, 0.0f, -1.0f);
    // glm::lookAt builds a view matrix; its inverse is the camera frame.
    setFrame(glm::inverse(glm::lookAt(eye, target, up)));
}

} // namespace rift
