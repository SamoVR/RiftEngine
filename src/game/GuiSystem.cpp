#include "game/GuiSystem.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace rift {

namespace {
    GuiRect intersect(const GuiRect& a, const GuiRect& b) {
        const glm::vec2 lo = glm::max(a.pos, b.pos);
        const glm::vec2 hi = glm::min(a.pos + a.size, b.pos + b.size);
        return { lo, glm::max(hi - lo, glm::vec2(0.0f)) };
    }
}

BillboardPlacement GuiSystem::placeWithCamera(const Instance& gui, const Instance& part, const glm::mat4& view, const glm::mat4& projection,
                                              const glm::vec2& viewSize) {
    BillboardPlacement p;
    // StudsOffset is along the camera's right, up and back; StudsOffsetWorldSpace along the world's axes.
    const glm::mat4 camera = glm::inverse(view);
    p.anchor = part.worldPosition() + gui.studsOffsetWorldSpace + glm::vec3(camera[0]) * gui.studsOffset.x + glm::vec3(camera[1]) * gui.studsOffset.y +
               glm::vec3(camera[2]) * gui.studsOffset.z;
    const glm::vec4 clip = projection * view * glm::vec4(p.anchor, 1.0f);
    if (clip.w <= 0.05f) return p; // behind the camera
    p.depth = glm::length(p.anchor - glm::vec3(camera[3]));
    if (gui.maxDistance > 0.0f && p.depth > gui.maxDistance) return p;
    const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
    p.center = glm::vec2((ndc.x * 0.5f + 0.5f) * viewSize.x, (0.5f - ndc.y * 0.5f) * viewSize.y);
    p.pixelsPerStud = viewSize.y * 0.5f * projection[1][1] / clip.w;
    p.visible = true;
    return p;
}

void GuiSystem::faceCorners(const Instance& part, NormalId face, std::array<glm::vec3, 4>& corners, glm::vec3& normal, glm::vec2& studs) {
    // The face's outward normal and the canvas's up, in the part's space; right follows for a reader facing the face.
    glm::vec3 n(0.0f, 0.0f, -1.0f), up(0.0f, 1.0f, 0.0f);
    switch (face) {
        case NormalId::Right: n = { 1, 0, 0 }; break;
        case NormalId::Left: n = { -1, 0, 0 }; break;
        case NormalId::Back: n = { 0, 0, 1 }; break;
        case NormalId::Front: n = { 0, 0, -1 }; break;
        case NormalId::Top: n = { 0, 1, 0 }; up = { 0, 0, -1 }; break;
        case NormalId::Bottom: n = { 0, -1, 0 }; up = { 0, 0, 1 }; break;
    }
    const glm::vec3 right = glm::cross(-n, up);
    const glm::vec3 size = part.size;
    const float halfW = std::abs(glm::dot(right, size)) * 0.5f, halfH = std::abs(glm::dot(up, size)) * 0.5f;
    const glm::vec3 center = n * (std::abs(glm::dot(n, size)) * 0.5f + 0.01f); // a hair off the face: no flicker
    const glm::mat4 world = part.worldFrame();
    auto at = [&](const glm::vec3& local) { return glm::vec3(world * glm::vec4(local, 1.0f)); };
    corners[0] = at(center - right * halfW + up * halfH);
    corners[1] = at(center + right * halfW + up * halfH);
    corners[2] = at(center + right * halfW - up * halfH);
    corners[3] = at(center - right * halfW - up * halfH);
    normal = glm::normalize(glm::vec3(world * glm::vec4(n, 0.0f)));
    studs = glm::vec2(halfW * 2.0f, halfH * 2.0f);
}

bool GuiSystem::surfaceUnder(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, SurfacePoint& out) const {
    bool found = false;
    float best = maxDistance;
    for (const Surface& s : surfaces_) {
        const float facing = glm::dot(direction, s.normal);
        if (facing >= -1e-4f) continue; // from behind
        const float t = glm::dot(s.corners[0] - origin, s.normal) / facing;
        if (t < 0.0f || t > best) continue;
        const glm::vec3 p = origin + direction * t;
        const glm::vec3 across = s.corners[1] - s.corners[0], down = s.corners[3] - s.corners[0];
        const float u = glm::dot(p - s.corners[0], across) / std::max(glm::dot(across, across), 1e-6f);
        const float v = glm::dot(p - s.corners[0], down) / std::max(glm::dot(down, down), 1e-6f);
        if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) continue;
        best = t;
        out.gui = s.gui;
        out.point = glm::vec2(u * s.canvas.x, v * s.canvas.y);
        found = true;
    }
    return found;
}

void GuiSystem::layout(const glm::vec2& viewSize) {
    items_.clear();
    surfaces_.clear();
    // The player's PlayerGui while playing; StarterGui for the editor's preview.
    Instance* starterGui = root_ ? scene_.find(root_) : scene_.service(ClassId::StarterGui);

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

    auto sorted = [](std::vector<Sortable>& list) {
        std::stable_sort(list.begin(), list.end(), [](const Sortable& a, const Sortable& b) {
            if (a.order != b.order) return a.order < b.order;
            if (a.z != b.z) return a.z < b.z;
            return a.sequence < b.sequence;
        });
    };

    // GUI in the world: in the Workspace on its parent part (or Adornee), or in the player's GUI with an Adornee.
    std::vector<std::pair<Instance*, Instance*>> world; // gui, part
    auto consider = [&](Instance& gui, bool inPlayerGui) {
        if ((gui.cls != ClassId::BillboardGui && gui.cls != ClassId::SurfaceGui) || !gui.enabled) return;
        Instance* part = gui.adornee.id ? scene_.find(gui.adornee.id) : nullptr;
        if (!part && !inPlayerGui && gui.parent && gui.parent->isBasePart()) part = gui.parent;
        if (part && part->isBasePart() && part->isDescendantOf(&scene_.workspace())) world.push_back({ &gui, part });
    };
    Scene::forEachDescendant(scene_.workspace(), [&](Instance& i) { consider(i, false); });
    if (starterGui) for (Instance* gui : starterGui->children) consider(*gui, true);

    // BillboardGuis: on the screen, far ones first, under the ScreenGuis.
    struct Billboard { Instance* gui; BillboardPlacement place; };
    std::vector<Billboard> billboards;
    for (auto& [gui, part] : world) {
        if (gui->cls == ClassId::SurfaceGui) {
            Surface s;
            s.gui = gui->id;
            s.part = part->id;
            s.alwaysOnTop = gui->alwaysOnTop;
            glm::vec2 studs;
            faceCorners(*part, gui->face, s.corners, s.normal, studs);
            s.canvas = gui->sizingMode == SurfaceGuiSizingMode::PixelsPerStud ? studs * std::max(gui->pixelsPerStud, 1.0f) : gui->canvasSize;
            s.canvas = glm::clamp(s.canvas, glm::vec2(1.0f), glm::vec2(4096.0f));
            const GuiRect canvas{ glm::vec2(0.0f), s.canvas };
            sortable.clear();
            sequence = 0;
            visit(*gui, canvas, canvas, 0);
            sorted(sortable);
            for (const Sortable& x : sortable) s.items.push_back(x.item);
            surfaces_.push_back(std::move(s));
        } else if (placeBillboard) {
            const BillboardPlacement p = placeBillboard(*gui, *part);
            if (p.visible) billboards.push_back({ gui, p });
        }
    }
    std::sort(billboards.begin(), billboards.end(), [](const Billboard& a, const Billboard& b) { return a.place.depth > b.place.depth; });
    for (const Billboard& b : billboards) {
        const UDim2& s = b.gui->guiSize;
        const glm::vec2 size = glm::max(glm::vec2(s.xScale, s.yScale) * b.place.pixelsPerStud + glm::vec2(s.xOffset, s.yOffset), glm::vec2(0.0f));
        const GuiRect rect{ b.place.center - size * 0.5f, size };
        sortable.clear();
        sequence = 0;
        visit(*b.gui, rect, screen, 0);
        sorted(sortable);
        for (const Sortable& x : sortable) items_.push_back(x.item);
    }

    // ScreenGuis on top.
    sortable.clear();
    sequence = 0;
    if (starterGui && screenGuis) {
        for (Instance* gui : starterGui->children) {
            if (gui->cls != ClassId::ScreenGui || !gui->enabled) continue;
            visit(*gui, screen, screen, gui->displayOrder);
        }
    }
    sorted(sortable);
    items_.reserve(items_.size() + sortable.size());
    for (const Sortable& s : sortable) items_.push_back(s.item);
}

void GuiSystem::handleMouse(const glm::vec2& mouse, bool inView, bool buttonDown, const SurfacePoint* surface) {
    // Topmost element under the mouse (the list is back to front).
    uint64_t topButton = 0;
    overGui_ = false;
    // True when the search is over: a button, or an opaque frame that blocks what is behind it.
    auto search = [&](const std::vector<Item>& items, const glm::vec2& point) {
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!it->rect.contains(point) || !it->clip.contains(point)) continue;
            Instance* inst = scene_.find(it->id);
            if (!inst) continue;
            // What is inside a button (its label, an icon, a gradient fill) is part of it.
            Instance* button = nullptr;
            for (Instance* p = inst; p && p->isGuiObject() && !button; p = p->parent) if (isButton(*p)) button = p;
            if (button) { topButton = button->id; overGui_ = true; return true; }
            if (inst->backgroundTransparency < 1.0f) { overGui_ = true; return true; }
        }
        return false;
    };
    if (inView && !search(items_, mouse) && surface) {
        // Then the SurfaceGui the mouse points at (its buttons only: a sign does not block the world).
        for (const Surface& s : surfaces_) {
            if (s.gui != surface->gui) continue;
            if (search(s.items, surface->point) && !topButton) overGui_ = false;
            break;
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
