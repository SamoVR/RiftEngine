#pragma once
//
// GuiSystem.h -- layout and mouse interaction of the game's 2D GUI
// (ScreenGuis in StarterGui and their Frames, TextLabels, buttons, images).
// Drawing is done by the host (editor viewport or player window) from the
// draw list, so this file has no rendering dependency and is tested headless.
//
// Layout follows Roblox: an element's Size and Position are UDim2s relative
// to its parent's rectangle; AnchorPoint picks the point that sits at
// Position. Elements are drawn by ScreenGui.DisplayOrder, then ZIndex, then
// tree order; the topmost button under the mouse receives clicks.
//
#include "scene/Scene.h"

#include <functional>
#include <vector>

namespace rift {

struct GuiRect {
    glm::vec2 pos{ 0.0f };
    glm::vec2 size{ 0.0f };
    bool contains(const glm::vec2& p) const { return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y; }
};

enum class GuiEvent { MouseEnter, MouseLeave, MouseButton1Down, MouseButton1Up, Activated };

class GuiSystem {
public:
    struct Item {
        uint64_t id = 0;
        GuiRect rect;
        GuiRect clip;   // the parent's rectangle chain (children are clipped to it)
    };

    explicit GuiSystem(Scene& scene) : scene_(scene) {}
    // The instance whose ScreenGuis are shown (PlayerGui); 0 = StarterGui.
    void setRoot(uint64_t id) { root_ = id; }

    // Recomputes every visible element's rectangle for a view of `size` pixels.
    void layout(const glm::vec2& viewSize);
    const std::vector<Item>& drawList() const { return items_; } // back to front

    // Mouse in view pixels. Fires events through `onEvent`.
    void handleMouse(const glm::vec2& mouse, bool inView, bool buttonDown);
    uint64_t hovered() const { return hovered_; }
    uint64_t pressed() const { return pressed_; }
    // True if the mouse is over a visible GUI element (input "processed by the GUI").
    bool mouseOverGui() const { return overGui_; }

    std::function<void(Instance&, GuiEvent)> onEvent;

    static bool isButton(const Instance& i) { return i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton; }

private:
    Scene& scene_;
    std::vector<Item> items_;
    uint64_t root_ = 0;
    uint64_t hovered_ = 0, pressed_ = 0;
    bool overGui_ = false;
    bool wasDown_ = false;
};

} // namespace rift
