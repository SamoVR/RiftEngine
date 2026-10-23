#pragma once
//
// GameCamera.h -- the camera while the game runs (workspace.CurrentCamera).
//
//   Custom      follows the character: drag with the right mouse button (or
//               move the mouse in first person) to look around, wheel zooms,
//               walls pull the camera in so it never ends up inside them.
//   Scriptable  scripts set CFrame themselves.
//
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace rift {

enum class CameraType : int { Custom, Scriptable, Fixed };

struct GameCamera {
    CameraType type = CameraType::Custom;
    glm::vec3 position{ 0.0f, 8.0f, 16.0f };
    glm::quat rotation{ 1.0f, 0.0f, 0.0f, 0.0f }; // looks along rotation * -Z
    float fieldOfView = 70.0f;                    // vertical, degrees
    float nearPlane = 0.1f;
    float farPlane = 3000.0f;

    // Custom-mode state
    float yaw = 0.0f;     // degrees, 0 = looking towards -Z
    float pitch = -15.0f; // degrees
    float zoom = 12.0f;   // distance from the focus point

    glm::vec3 lookVector() const { return rotation * glm::vec3(0.0f, 0.0f, -1.0f); }
    glm::vec3 rightVector() const { return rotation * glm::vec3(1.0f, 0.0f, 0.0f); }
    glm::vec3 upVector() const { return rotation * glm::vec3(0.0f, 1.0f, 0.0f); }
    glm::mat4 frame() const;   // world transform (CFrame)
    void setFrame(const glm::mat4& m);
    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
    void lookAt(const glm::vec3& eye, const glm::vec3& target);
};

} // namespace rift
