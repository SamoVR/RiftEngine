#include "game/GuiSystem.h"

#include <algorithm>

namespace rift {

namespace {
    GuiRect intersect(const GuiRect& a, const GuiRect& b) {
        const glm::vec2 lo = glm::max(a.pos, b.pos);
        const glm::vec2 hi = glm::min(a.pos + a.size, b.pos + b.size);
        return { lo, glm::max(hi - lo, glm::vec2(0.0f)) };
    }
}

void GuiSystem::layout(const glm::vec2& viewSize) {
    items_.clear();
    // The player's PlayerGui while playing; StarterGui for the editor's preview.
    Instance* starterGui = root_ ? scene_.find(root_) : scene_.service(ClassId::StarterGui);
    if (!starterGui) return;

    struct Sortable { Item item; int order; int z; size_t sequence; };
    std::vector<Sortable> sortable;
    size_t sequence = 0;
    const GuiRect screen{ glm::vec2(0.0f), viewSize };

    std::function<void(Instance&, const GuiRect&, const GuiRect&, int)> visit = [&](Instance& node, const GuiRect& parentRect, const GuiRect& clip, int order) {
        for (Instance* child : node.children) {
            if (!child->isGuiObject() || !child->visible) continue;
            const UDim2& s = child->guiSize;
            const UDim2& p = child->guiPosition;
            const glm::vec2 size(s.xScale * parentRect.size.x + s.xOffset, s.yScale * parentRect.size.y + s.yOffset);
            const glm::vec2 pos = parentRect.pos
                + glm::vec2(p.xScale * parentRect.size.x + p.xOffset, p.yScale * parentRect.size.y + p.yOffset)
                - child->anchorPoint * size;
            const GuiRect rect{ pos, glm::max(size, glm::vec2(0.0f)) };
            sortable.push_back({ { child->id, rect, clip }, order, child->zIndex, sequence++ });
            visit(*child, rect, intersect(clip, rect), order);
        }
    };

    for (Instance* gui : starterGui->children) {
        if (gui->cls != ClassId::ScreenGui || !gui->enabled) continue;
        visit(*gui, screen, screen, gui->displayOrder);
    }
    std::stable_sort(sortable.begin(), sortable.end(), [](const Sortable& a, const Sortable& b) {
        if (a.order != b.order) return a.order < b.order;
        if (a.z != b.z) return a.z < b.z;
        return a.sequence < b.sequence;
    });
    items_.reserve(sortable.size());
    for (const Sortable& s : sortable) items_.push_back(s.item);
}

void GuiSystem::handleMouse(const glm::vec2& mouse, bool inView, bool buttonDown) {
    // Topmost element under the mouse (the list is back to front).
    uint64_t topButton = 0;
    overGui_ = false;
    if (inView) {
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (!it->rect.contains(mouse) || !it->clip.contains(mouse)) continue;
            Instance* inst = scene_.find(it->id);
            if (!inst) continue;
            if (isButton(*inst)) { topButton = inst->id; overGui_ = true; break; }
            if (inst->backgroundTransparency < 1.0f) { overGui_ = true; break; } // an opaque frame blocks what is behind it
        }
    }

    auto fire = [&](uint64_t id, GuiEvent e) {
        if (!id || !onEvent) return;
        if (Instance* inst = scene_.find(id)) onEvent(*inst, e);
    };

    if (topButton != hovered_) {
        fire(hovered_, GuiEvent::MouseLeave);
        hovered_ = topButton;
        fire(hovered_, GuiEvent::MouseEnter);
    }
    const bool pressedNow = buttonDown && !wasDown_;
    const bool releasedNow = !buttonDown && wasDown_;
    if (pressedNow && hovered_) {
        pressed_ = hovered_;
        fire(pressed_, GuiEvent::MouseButton1Down);
    }
    if (releasedNow && pressed_) {
        fire(pressed_, GuiEvent::MouseButton1Up);
        if (pressed_ == hovered_) fire(pressed_, GuiEvent::Activated); // a click: released over the same button
        pressed_ = 0;
    }
    wasDown_ = buttonDown;
}

} // namespace rift
