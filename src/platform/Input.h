#pragma once
//
// Input.h -- keyboard and mouse state for one frame, fed by the window's
// event callbacks (or by tests). Key and button codes are GLFW's.
//
// Call newFrame() once per frame BEFORE processing the window's events, so
// "pressed this frame" / deltas cover exactly the events of that frame.
//
#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace rift {

class Input {
public:
    static constexpr int kMaxKeys = 512;
    static constexpr int kMaxButtons = 8;

    void newFrame();

    // ---- events
    void setKey(int key, bool down);
    void setMouseButton(int button, bool down);
    void setMousePosition(const glm::vec2& position);
    void addScroll(float dy);
    void addText(unsigned int codepoint);
    // Releases everything (window lost focus).
    void releaseAll();

    // ---- queries
    bool keyDown(int key) const;
    bool keyPressed(int key) const;   // went down this frame
    bool keyReleased(int key) const;  // went up this frame
    bool mouseDown(int button) const;
    bool mousePressed(int button) const;
    bool mouseReleased(int button) const;
    glm::vec2 mousePosition() const { return mouse_; }
    glm::vec2 mouseDelta() const { return mouseDelta_; }
    float scroll() const { return scroll_; }
    const std::string& text() const { return text_; }  // UTF-8 typed this frame
    const std::vector<int>& keysPressed() const { return pressed_; }
    const std::vector<int>& keysReleased() const { return released_; }

    bool ctrl() const;
    bool shift() const;
    bool alt() const;

private:
    std::array<bool, kMaxKeys> keys_{};
    std::array<bool, kMaxKeys> prevKeys_{};
    std::array<bool, kMaxButtons> buttons_{};
    std::array<bool, kMaxButtons> prevButtons_{};
    std::vector<int> pressed_, released_;
    glm::vec2 mouse_{ 0.0f }, mouseDelta_{ 0.0f };
    bool hasMouse_ = false;
    float scroll_ = 0.0f;
    std::string text_;
};

// GLFW key code <-> name ("W", "Space", "LeftShift", "F5") used by
// Enum.KeyCode and the keybind editor.
const std::vector<std::pair<std::string, int>>& keyCodeTable();
std::string keyName(int key);
int keyFromName(const std::string& name); // -1 if unknown

} // namespace rift
