#pragma once
//
// GuiSystem.h -- layout and mouse interaction of the game's 2D GUI
// (ScreenGuis in StarterGui and their Frames, TextLabels, buttons, images),
// and of GUI in the 3D world: BillboardGuis float over parts facing the
// camera (laid out on the screen with the rest), SurfaceGuis cover a face of
// a part (laid out on their own canvas, which the host draws into a texture).
// Drawing is done by the host (editor viewport or player window) from the
// draw list, so this file has no rendering dependency and is tested headless.
//
// Layout follows Roblox: an element's Size and Position are UDim2s relative
// to its parent's rectangle; AnchorPoint picks the point that sits at
// Position. Elements are drawn by ScreenGui.DisplayOrder, then ZIndex, then
// tree order; the topmost button under the mouse receives clicks.
//
#include "scene/Scene.h"

#include <array>
#include <functional>
#include <vector>

namespace rift {

struct GuiRect {
    glm::vec2 pos{ 0.0f };
    glm::vec2 size{ 0.0f };
    bool contains(const glm::vec2& p) const { return p.x >= pos.x && p.y >= pos.y && p.x < pos.x + size.x && p.y < pos.y + size.y; }
};

enum class GuiEvent { MouseEnter, MouseLeave, MouseButton1Down, MouseButton1Up, Activated };

// Where a BillboardGui goes on the screen this frame (the host knows the camera).
struct BillboardPlacement {
    bool visible = false;
    glm::vec2 center{ 0.0f };   // view pixels
    float pixelsPerStud = 0.0f; // at its distance (Size's Scale is in studs)
    float depth = 0.0f;         // distance from the camera
    glm::vec3 anchor{ 0.0f };   // the world point it floats at
};

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

    // ScreenGuis are laid out (off: only the GUI in the world, e.g. the editor's View > GUI is off).
    bool screenGuis = true;
    // Places BillboardGuis; without it they are not shown.
    std::function<BillboardPlacement(const Instance& gui, const Instance& part)> placeBillboard;
    // A placement from a camera (no occlusion test): what placeBillboard usually does.
    static BillboardPlacement placeWithCamera(const Instance& gui, const Instance& part, const glm::mat4& view, const glm::mat4& projection,
                                              const glm::vec2& viewSize);

    // Recomputes every visible element's rectangle for a view of `size` pixels.
    void layout(const glm::vec2& viewSize);
    const std::vector<Item>& drawList() const { return items_; } // back to front (BillboardGuis first, far to near)

    // A SurfaceGui: its elements in canvas pixels, and the face of the part the canvas covers.
    struct Surface {
        uint64_t gui = 0, part = 0;
        glm::vec2 canvas{ 1.0f };
        std::array<glm::vec3, 4> corners{}; // world: the canvas's top-left, top-right, bottom-right, bottom-left
        glm::vec3 normal{ 0.0f, 0.0f, -1.0f };
        bool alwaysOnTop = false;
        std::vector<Item> items;            // back to front
    };
    const std::vector<Surface>& surfaces() const { return surfaces_; }
    // The face of `part` a SurfaceGui covers (corners in that order, a hair off the face).
    static void faceCorners(const Instance& part, NormalId face, std::array<glm::vec3, 4>& corners, glm::vec3& normal, glm::vec2& studs);

    // The mouse on a SurfaceGui: which one and where, in canvas pixels.
    struct SurfacePoint { uint64_t gui = 0; glm::vec2 point{ 0.0f }; };
    // The SurfaceGui a ray hits first, closer than `maxDistance`.
    bool surfaceUnder(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, SurfacePoint& out) const;

    // Mouse in view pixels (and on a SurfaceGui, if the host's ray found one). Fires events through `onEvent`.
    void handleMouse(const glm::vec2& mouse, bool inView, bool buttonDown, const SurfacePoint* surface = nullptr);
    uint64_t hovered() const { return hovered_; }
    uint64_t pressed() const { return pressed_; }
    // True if the mouse is over a visible GUI element (input "processed by the GUI").
    bool mouseOverGui() const { return overGui_; }

    std::function<void(Instance&, GuiEvent)> onEvent;

    static bool isButton(const Instance& i) { return i.cls == ClassId::TextButton || i.cls == ClassId::ImageButton; }

private:
    Scene& scene_;
    std::vector<Item> items_;
    std::vector<Surface> surfaces_;
    uint64_t root_ = 0;
    uint64_t hovered_ = 0, pressed_ = 0;
    bool overGui_ = false;
    bool wasDown_ = false;
};

} // namespace rift
