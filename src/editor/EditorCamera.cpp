#include "editor/EditorCamera.h"
#include "platform/Input.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

namespace rift {

glm::vec3 EditorCamera::forward() const {
    const float y = glm::radians(yaw_), p = glm::radians(pitch_);
    return glm::normalize(glm::vec3(std::cos(p) * std::cos(y), std::sin(p), std::cos(p) * std::sin(y)));
}

glm::vec3 EditorCamera::right() const {
    return glm::normalize(glm::cross(forward(), glm::vec3(0.0f, 1.0f, 0.0f)));
}

glm::mat4 EditorCamera::view() const {
    return glm::lookAt(position_, position_ + forward(), glm::vec3(0.0f, 1.0f, 0.0f));
}

glm::mat4 EditorCamera::projection(float aspect) const {
    return glm::perspective(glm::radians(fieldOfView), std::max(aspect, 1e-3f), 0.1f, 5000.0f);
}

void EditorCamera::set(const glm::vec3& position, float yaw, float pitch) {
    position_ = position;
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -89.0f, 89.0f);
    framing_ = false;
}

void EditorCamera::lookAt(const glm::vec3& eye, const glm::vec3& target) {
    const glm::vec3 d = target - eye;
    if (glm::length(d) < 1e-5f) return;
    const glm::vec3 n = glm::normalize(d);
    set(eye, glm::degrees(std::atan2(n.z, n.x)), glm::degrees(std::asin(std::clamp(n.y, -1.0f, 1.0f))));
}

void EditorCamera::frame(const glm::vec3& center, float radius) {
    const float distance = std::max(radius, 0.5f) / std::tan(glm::radians(fieldOfView * 0.5f)) * 1.2f;
    frameTarget_ = center - forward() * distance;
    framing_ = true;
}

void EditorCamera::ray(const glm::vec2& pixel, const glm::vec2& size, glm::vec3& origin, glm::vec3& direction) const {
    const glm::vec2 ndc(2.0f * pixel.x / std::max(size.x, 1.0f) - 1.0f, 1.0f - 2.0f * pixel.y / std::max(size.y, 1.0f));
    const glm::mat4 inv = glm::inverse(projection(size.x / std::max(size.y, 1.0f)) * view());
    glm::vec4 n = inv * glm::vec4(ndc, -1.0f, 1.0f);
    glm::vec4 f = inv * glm::vec4(ndc, 1.0f, 1.0f);
    n /= n.w;
    f /= f.w;
    origin = glm::vec3(n);
    direction = glm::normalize(glm::vec3(f - n));
}

bool EditorCamera::update(float dt, const Input& input, const ViewInput& view, float speed, float sensitivity) {
    const glm::vec2 delta = input.mouseDelta();

    // Drags start only inside the viewport but continue until the button is released.
    if (view.hovered && input.mousePressed(GLFW_MOUSE_BUTTON_RIGHT)) looking_ = true;
    if (!input.mouseDown(GLFW_MOUSE_BUTTON_RIGHT)) looking_ = false;
    if (view.hovered && input.mousePressed(GLFW_MOUSE_BUTTON_MIDDLE)) panning_ = true;
    if (!input.mouseDown(GLFW_MOUSE_BUTTON_MIDDLE)) panning_ = false;
    if (view.hovered && input.alt() && input.mousePressed(GLFW_MOUSE_BUTTON_LEFT)) {
        orbiting_ = true;
        orbitPivot_ = position_ + forward() * 15.0f;
    }
    if (!input.mouseDown(GLFW_MOUSE_BUTTON_LEFT) || !input.alt()) orbiting_ = false;

    if (looking_ || orbiting_) {
        yaw_ += delta.x * sensitivity;
        pitch_ = std::clamp(pitch_ - delta.y * sensitivity, -89.0f, 89.0f);
        framing_ = false;
        if (orbiting_) position_ = orbitPivot_ - forward() * glm::length(orbitPivot_ - position_);
    }
    if (panning_) {
        const glm::vec3 up = glm::normalize(glm::cross(right(), forward()));
        const float scale = speed * 0.0025f + 0.01f;
        position_ += (-right() * delta.x + up * delta.y) * scale;
        framing_ = false;
    }

    // Fly with WASD/QE while looking (keys are free for shortcuts otherwise).
    glm::vec3 wish(0.0f);
    if (looking_ && view.keyboard) {
        if (input.keyDown(GLFW_KEY_W)) wish += forward();
        if (input.keyDown(GLFW_KEY_S)) wish -= forward();
        if (input.keyDown(GLFW_KEY_D)) wish += right();
        if (input.keyDown(GLFW_KEY_A)) wish -= right();
        if (input.keyDown(GLFW_KEY_E)) wish += glm::vec3(0.0f, 1.0f, 0.0f);
        if (input.keyDown(GLFW_KEY_Q)) wish -= glm::vec3(0.0f, 1.0f, 0.0f);
    }
    float targetSpeed = speed;
    if (input.shift()) targetSpeed *= 3.0f;
    if (input.ctrl()) targetSpeed *= 0.25f;
    const glm::vec3 targetVelocity = glm::length(wish) > 1e-4f ? glm::normalize(wish) * targetSpeed : glm::vec3(0.0f);
    velocity_ += (targetVelocity - velocity_) * std::min(1.0f, dt * 12.0f);
    position_ += velocity_ * dt;
    if (glm::length(velocity_) > 0.01f) framing_ = false;

    // Wheel: dolly towards where the mouse points.
    if (view.hovered && input.scroll() != 0.0f && !looking_) {
        glm::vec3 origin, dir;
        ray(view.mouse, view.size, origin, dir);
        position_ += dir * input.scroll() * std::max(2.0f, speed * 0.25f);
        framing_ = false;
    }

    if (framing_) {
        position_ += (frameTarget_ - position_) * std::min(1.0f, dt * 10.0f);
        if (glm::length(frameTarget_ - position_) < 0.01f) framing_ = false;
    }
    return looking_ || orbiting_ || panning_;
}

} // namespace rift
