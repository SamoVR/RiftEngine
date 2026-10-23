#pragma once
//
// EditorCamera.h -- the editor's fly camera (Roblox Studio controls):
//   right mouse held   look around; WASD / QE fly, Shift faster, Ctrl slower
//   mouse wheel        move towards / away from where the mouse points
//   middle mouse drag  pan
//   Alt + left drag    orbit around the point in the middle of the view
//   F                  frame the selection
// Motion is eased so the view glides instead of jumping.
//
#include <glm/glm.hpp>

namespace rift {

class Input;

class EditorCamera {
public:
    struct ViewInput {
        bool hovered = false;         // the mouse is over the viewport
        bool keyboard = true;         // keys go to the viewport (no text field focused)
        glm::vec2 mouse{ 0.0f };      // relative to the viewport
        glm::vec2 size{ 1.0f };       // viewport size in pixels
    };

    // Returns true while the camera owns the mouse (hide the cursor).
    bool update(float dt, const Input& input, const ViewInput& view, float speed, float sensitivity);

    glm::mat4 view() const;
    glm::mat4 projection(float aspect) const;
    glm::vec3 position() const { return position_; }
    glm::vec3 forward() const;
    glm::vec3 right() const;
    float yaw() const { return yaw_; }
    float pitch() const { return pitch_; }

    void set(const glm::vec3& position, float yaw, float pitch);
    void lookAt(const glm::vec3& eye, const glm::vec3& target);
    // Smoothly moves so `center` with the given radius fills the view.
    void frame(const glm::vec3& center, float radius);
    // Ray through a viewport pixel.
    void ray(const glm::vec2& pixel, const glm::vec2& size, glm::vec3& origin, glm::vec3& direction) const;

    float fieldOfView = 70.0f;

private:
    glm::vec3 position_{ 24.0f, 18.0f, 24.0f };
    float yaw_ = -135.0f;  // degrees; -90 looks along -Z
    float pitch_ = -28.0f;
    glm::vec3 velocity_{ 0.0f };
    // Smooth framing
    bool framing_ = false;
    glm::vec3 frameTarget_{ 0.0f };
    // Drag states
    bool looking_ = false, panning_ = false, orbiting_ = false;
    glm::vec3 orbitPivot_{ 0.0f };
};

} // namespace rift
