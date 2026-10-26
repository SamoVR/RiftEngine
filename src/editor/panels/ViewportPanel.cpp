#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/TransformUtil.h"
#include "render/Picking.h"
#include "render/ImageWrite.h"
#include "render/Renderer.h"
#include "game/GuiSystem.h"
#include "platform/Input.h"
#include "ui/GameOverlay.h"

#include "IconsFontAwesome6.h"
#include <GLFW/glfw3.h>
#include <ImGuizmo.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace rift {

namespace {
    constexpr float kClickSlop = 4.0f; // pixels the mouse may move and still count as a click

    // What a click on `part` selects: the outermost Model it belongs to
    // (like Roblox), or the part itself with Alt held.
    Instance* selectionTarget(Instance* part, bool exact) {
        if (!part || exact) return part;
        Instance* target = part;
        for (Instance* p = part->parent; p && !p->isService(); p = p->parent)
            if (p->cls == ClassId::Model) target = p;
        return target;
    }

    void notifyTransform(Scene& scene, Instance& i) {
        for (const char* name : { "Position", "Rotation", "Size" })
            if (const PropertyInfo* p = findProperty(i.cls, name)) scene.notifyPropertyChanged(i, *p);
    }

    // Topmost GUI element under `mouse` (view pixels), or 0.
    uint64_t guiAt(const GuiSystem& gui, const glm::vec2& mouse) {
        const auto& items = gui.drawList();
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if (it->rect.contains(mouse) && it->clip.contains(mouse)) return it->id;
        return 0;
    }

    void styleGizmo() {
        static bool done = false;
        if (done) return;
        done = true;
        ImGuizmo::Style& s = ImGuizmo::GetStyle();
        s.TranslationLineThickness = 3.5f;
        s.TranslationLineArrowSize = 7.0f;
        s.RotationLineThickness = 3.0f;
        s.RotationOuterLineThickness = 3.5f;
        s.ScaleLineThickness = 3.5f;
        s.ScaleLineCircleSize = 7.0f;
        s.CenterCircleSize = 5.0f;
        s.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.93f, 0.33f, 0.36f, 1.0f);
        s.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.45f, 0.82f, 0.36f, 1.0f);
        s.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.33f, 0.56f, 0.96f, 1.0f);
        s.Colors[ImGuizmo::PLANE_X] = ImVec4(0.93f, 0.33f, 0.36f, 0.45f);
        s.Colors[ImGuizmo::PLANE_Y] = ImVec4(0.45f, 0.82f, 0.36f, 0.45f);
        s.Colors[ImGuizmo::PLANE_Z] = ImVec4(0.33f, 0.56f, 0.96f, 0.45f);
        s.Colors[ImGuizmo::SELECTION] = ImVec4(1.0f, 0.82f, 0.30f, 1.0f);
    }
}

ViewportInput ViewportPanel::gameInput(const Editor& editor) const {
    ViewportInput v;
    v.size = size_;
    v.mouse = const_cast<Editor&>(editor).input().mousePosition() - origin_;
    // Looking at the server: the editor has the mouse and keyboard, the game none.
    const bool game = !editor.serverView();
    v.hovered = game && hovered_;
    v.keyboard = game && focused_ && !ImGui::GetIO().WantTextInput;
    return v;
}

void ViewportPanel::draw(UiContext& ui) {
    Editor& editor = ui.editor;
    EditorSettings& settings = editor.settings();
    const ImGuiIO& io = ImGui::GetIO();

    if (focusRequest_) {
        ImGui::SetNextWindowFocus();
        focusRequest_ = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool visible = ImGui::Begin(ICON_FA_CUBE "  Viewport###Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!visible) {
        hovered_ = focused_ = false;
        ImGui::End();
        return;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    origin_ = { pos.x, pos.y };
    size_ = { std::max(avail.x, 1.0f), std::max(avail.y, 1.0f) };
    editor.viewportSize = size_;
    // While the cursor is hidden ImGui sees no mouse, so keep the hover we had.
    hovered_ = ImGui::IsWindowHovered() || capture_;
    if (hovered_ && !capture_ && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle))) ImGui::SetWindowFocus();
    focused_ = ImGui::IsWindowFocused();
    const bool editView = !editor.playing() || editor.serverView(); // editor camera and tools
    if (focused_ && ImGui::IsKeyPressed(ImGuiKey_Escape) && !editView) {
        ImGui::SetWindowFocus(nullptr); // gives the mouse back
        focused_ = false;
    }

    // ---- camera
    bool cameraCapture = false;
    if (editView) {
        EditorCamera::ViewInput vi;
        vi.hovered = hovered_;
        vi.keyboard = !io.WantTextInput;
        vi.mouse = editor.input().mousePosition() - origin_;
        vi.size = size_;
        cameraCapture = editor.camera().update(ui.dt, editor.input(), vi, settings.cameraSpeed, settings.mouseSensitivity);
    }
    bool gameCapture = false;
    if (GameSession* s = editView ? nullptr : editor.session()) {
        gameCapture = focused_ && (s->wantsMouseLock() || (hovered_ && editor.input().mouseDown(GLFW_MOUSE_BUTTON_RIGHT)));
    }
    const bool capture = cameraCapture || gameCapture;
    if (capture != capture_) {
        glfwSetInputMode(ui.window, GLFW_CURSOR, capture ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        capture_ = capture;
    }

    // ---- render the scene
    const float aspect = size_.x / size_.y;
    glm::mat4 view, proj;
    RenderCamera camera;
    if (GameSession* s = editView ? nullptr : editor.session()) {
        view = s->camera().view();
        proj = s->camera().projection(aspect);
        camera.position = s->camera().position;
    } else {
        view = editor.camera().view();
        proj = editor.camera().projection(aspect);
        camera.position = editor.camera().position();
    }
    camera.view = view;
    camera.projection = proj;

    RenderSettings rs;
    rs.editor = editView;
    rs.grid = settings.showGrid;
    rs.dt = editor.session() && editor.session()->paused() ? 0.0f : ui.dt; // effects also play while editing
    rs.gridStep = settings.gridStep;
    rs.wireframe = settings.wireframe;
    rs.shadows = settings.shadows;
    for (uint64_t id : editor.selection()) rs.selected.insert(id);
    rs.hovered = editView ? hoveredId_ : 0;
    framebuffer_.resize(static_cast<int>(size_.x), static_cast<int>(size_.y), settings.msaa);
    ui.renderer.render(editor.scene(), camera, rs, framebuffer_);
    if (editor.thumbnailRequest) {
        editor.thumbnailRequest = false;
        if (!editor.playing()) saveThumbnail(editor);
    }
    ImGui::Image(static_cast<ImTextureID>(framebuffer_.texture()), ImVec2(size_.x, size_.y), ImVec2(0, 1), ImVec2(1, 0));
    handleDrop(ui);

    // ---- 2D GUI over the scene
    const OverlayFonts fonts{ theme::fonts().ui, theme::fonts().heading, theme::fonts().medium, theme::fonts().mono };
    std::unique_ptr<GuiSystem> editGui;
    if (GameSession* s = editor.session()) {
        // The GUI belongs to the player: the server view shows the world only.
        if (!editView) {
            drawGameGui(*s, ui.renderer.textures(), ImGui::GetWindowDrawList(), origin_, fonts);
            drawGameHud(*s, ui.renderer.textures(), ImGui::GetWindowDrawList(), origin_, size_, fonts);
        }
    } else if (settings.showGui) {
        editGui = std::make_unique<GuiSystem>(editor.scene());
        editGui->layout(size_);
        drawGui(editor.scene(), *editGui, ui.renderer.textures(), ImGui::GetWindowDrawList(), origin_, fonts);
        // Selected GUI elements get an outline.
        for (const GuiSystem::Item& item : editGui->drawList()) {
            if (!editor.isSelected(item.id)) continue;
            const ImVec2 a(origin_.x + item.rect.pos.x, origin_.y + item.rect.pos.y);
            ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + item.rect.size.x, a.y + item.rect.size.y), theme::u32(theme::color::accentHover), 0.0f, 0, 2.0f);
        }
    }

    // The Terrain Editor's brush replaces selecting and the gizmo while it is on.
    const bool brush = editor.terrainTool.active && !editor.playing();
    if (!brush) {
        if (brushing_) endStroke(editor);
        brushHit_ = false;
        drawGizmo(ui, view, proj);
    }
    drawOverlays(ui, view, proj);
    if (brush) handleTerrainBrush(ui, view, proj);
    else handleSelection(ui, view, proj, editGui.get());
    ImGui::End();
}

// ============================================================================
//  Terrain brush
// ============================================================================
void ViewportPanel::endStroke(Editor& editor) {
    brushing_ = false;
    editor.endTerrainStroke(std::string("Terrain ") + terrainBrushName(editor.terrainTool.brush));
}

void ViewportPanel::handleTerrainBrush(UiContext& ui, const glm::mat4& view, const glm::mat4& proj) {
    Editor& editor = ui.editor;
    EditorSettings& s = editor.settings();
    Editor::TerrainTool& tool = editor.terrainTool;
    TerrainData& terrain = editor.editScene().terrain();
    const ImGuiIO& io = ImGui::GetIO();
    hoveredId_ = 0;
    pressing_ = marquee_ = guiDragging_ = false;

    // Keys: [ ] brush size, Esc back to selecting.
    if (focused_ && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket)) s.terrainSize = std::max(2.0f, std::round(s.terrainSize / 1.25f));
        if (ImGui::IsKeyPressed(ImGuiKey_RightBracket)) s.terrainSize = std::min(64.0f, std::round(s.terrainSize * 1.25f));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            if (brushing_) endStroke(editor);
            tool.active = false;
            return;
        }
    }

    // The brush this frame: Ctrl reverses it, Shift smooths.
    TerrainData::Brush b = tool.brush;
    if (io.KeyCtrl) {
        if (b == TerrainData::Brush::Add) b = TerrainData::Brush::Subtract;
        else if (b == TerrainData::Brush::Subtract) b = TerrainData::Brush::Add;
        else if (b == TerrainData::Brush::Grow) b = TerrainData::Brush::Erode;
        else if (b == TerrainData::Brush::Erode) b = TerrainData::Brush::Grow;
    }
    if (io.KeyShift && b != TerrainData::Brush::Paint) b = TerrainData::Brush::Smooth;
    const float radius = s.terrainSize * 0.5f;
    const Material material = static_cast<Material>(std::clamp(s.terrainMaterial, 0, kMaterialCount - 1));

    // Where the brush is: on the terrain under the mouse, else on the ground plane.
    if (!capture_) {
        const glm::vec2 mouse = editor.input().mousePosition() - origin_;
        glm::vec3 o, d;
        editor.camera().ray(mouse, size_, o, d);
        const bool water = !s.terrainIgnoreWater || (material == Material::Water && b == TerrainData::Brush::Add);
        brushHit_ = false;
        const float facing = glm::dot(d, strokeNormal_);
        if (hovered_ && brushing_ && b == TerrainData::Brush::Add && facing < -0.05f) {
            // Add piles terrain up towards the camera: during a stroke the brush
            // slides on the surface where the stroke started instead of
            // climbing the terrain it has just added (and ending up in your face).
            const float t = glm::dot(strokeOrigin_ - o, strokeNormal_) / facing;
            if (t > 0.0f && t < 4000.0f) {
                brushHit_ = true;
                brushPoint_ = o + d * t;
                brushNormal_ = strokeNormal_;
            }
        } else if (hovered_) {
            if (auto hit = terrain.raycast(o, d, 4000.0f, water)) {
                brushHit_ = true;
                brushPoint_ = hit->position;
                brushNormal_ = hit->normal;
            } else {
                const float planeY = tool.brush == TerrainData::Brush::Flatten && tool.fixedPlane ? tool.planeHeight : 0.0f;
                if (std::abs(d.y) > 1e-4f) {
                    const float t = (planeY - o.y) / d.y;
                    if (t > 0.0f && t < 2000.0f) {
                        brushHit_ = true;
                        brushPoint_ = o + d * t;
                        brushNormal_ = { 0.0f, 1.0f, 0.0f };
                    }
                }
            }
        }
    }

    // ---- strokes: a dab every 1/30 s while the left button is held
    const bool overUi = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive();
    if (!brushing_ && brushHit_ && hovered_ && !capture_ && !overUi && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        brushing_ = true;
        brushTimer_ = 1.0f; // the first dab right away
        strokePlane_ = tool.fixedPlane ? tool.planeHeight : brushPoint_.y;
        strokeOrigin_ = brushPoint_;
        strokeNormal_ = glm::length(brushNormal_) > 0.5f ? glm::normalize(brushNormal_) : glm::vec3(0.0f, 1.0f, 0.0f);
        editor.beginTerrainStroke();
    }
    if (brushing_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            endStroke(editor);
        } else if (brushHit_) {
            constexpr float kDabInterval = 1.0f / 30.0f;
            brushTimer_ += ui.dt;
            if (brushTimer_ >= kDabInterval) {
                brushTimer_ = std::fmod(brushTimer_, kDabInterval);
                TerrainData::Dab dab;
                dab.brush = b;
                dab.center = brushPoint_;
                dab.radius = radius;
                dab.cube = s.terrainCube;
                dab.strength = s.terrainStrength;
                dab.material = material;
                dab.planeHeight = strokePlane_;
                dab.ignoreWater = s.terrainIgnoreWater;
                terrain.applyDab(dab);
            }
        }
    }

    // ---- the brush outline
    if (!brushHit_) return;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const glm::mat4 viewProj = proj * view;
    auto project = [&](const glm::vec3& p, ImVec2& out) {
        const glm::vec4 c = viewProj * glm::vec4(p, 1.0f);
        if (c.w <= 0.05f) return false;
        out = ImVec2(origin_.x + (c.x / c.w * 0.5f + 0.5f) * size_.x, origin_.y + (0.5f - c.y / c.w * 0.5f) * size_.y);
        return true;
    };
    ImVec4 tint = theme::color::accentHover;
    if (b == TerrainData::Brush::Add || b == TerrainData::Brush::Grow) tint = theme::color::success;
    else if (b == TerrainData::Brush::Subtract || b == TerrainData::Brush::Erode) tint = theme::color::error;
    else if (b == TerrainData::Brush::Flatten) tint = theme::color::warning;
    else if (b == TerrainData::Brush::Paint) {
        const Color3 c = terrain.materialColor(material);
        tint = ImVec4(c.r, c.g, c.b, 1.0f);
    }
    const ImU32 line = theme::u32(tint, brushing_ ? 1.0f : 0.85f);
    const ImU32 fill = theme::u32(tint, 0.12f);
    // A loop of points around `centre` in the plane of u, v.
    auto loop = [&](const glm::vec3& centre, const glm::vec3& u, const glm::vec3& v, float r, float thickness, bool filled) {
        constexpr int kSegments = 64;
        ImVec2 pts[kSegments];
        int count = 0;
        for (int i = 0; i < kSegments; ++i) {
            const float a = static_cast<float>(i) / kSegments * 6.2831853f;
            if (!project(centre + (u * std::cos(a) + v * std::sin(a)) * r, pts[count])) return;
            ++count;
        }
        if (filled) draw->AddConvexPolyFilled(pts, count, fill);
        draw->AddPolyline(pts, count, line, ImDrawFlags_Closed, thickness);
    };
    const glm::vec3 n = glm::length(brushNormal_) > 0.5f ? glm::normalize(brushNormal_) : glm::vec3(0.0f, 1.0f, 0.0f);
    const glm::vec3 helper = std::abs(n.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
    const glm::vec3 u = glm::normalize(glm::cross(n, helper)), v = glm::cross(n, u);
    if (s.terrainCube) {
        // The cube's 12 edges.
        const glm::vec3 c = brushPoint_;
        auto corner = [&](int i) { return c + glm::vec3((i & 1) ? radius : -radius, (i & 2) ? radius : -radius, (i & 4) ? radius : -radius); };
        static const int edges[12][2] = { { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 }, { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 } };
        for (const auto& e : edges) {
            ImVec2 a, bb;
            if (project(corner(e[0]), a) && project(corner(e[1]), bb)) draw->AddLine(a, bb, line, 1.5f);
        }
    } else {
        // The sphere's outline as seen from the camera, and its footprint on the surface.
        const glm::vec3 eye = editor.camera().position();
        const glm::vec3 toEye = glm::length(eye - brushPoint_) > 1e-3f ? glm::normalize(eye - brushPoint_) : n;
        const glm::vec3 h2 = std::abs(toEye.y) < 0.9f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
        const glm::vec3 su = glm::normalize(glm::cross(toEye, h2)), sv = glm::cross(toEye, su);
        loop(brushPoint_, su, sv, radius, 1.0f, false);
    }
    loop(brushPoint_ + n * 0.05f, u, v, radius, 2.0f, true);
    if (b == TerrainData::Brush::Flatten) {
        // The level the stroke flattens to.
        const float y = brushing_ ? strokePlane_ : (tool.fixedPlane ? tool.planeHeight : brushPoint_.y);
        loop(glm::vec3(brushPoint_.x, y, brushPoint_.z), { 1.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 1.0f }, radius * 1.15f, 1.0f, true);
    }
    ImVec2 centre;
    if (project(brushPoint_, centre)) draw->AddCircleFilled(centre, 2.5f, line);

    // What the brush does, by the cursor.
    char label[96];
    std::snprintf(label, sizeof(label), "%s  %s  %.0f", terrainBrushIcon(b), terrainBrushName(b), s.terrainSize);
    const ImVec2 mouse = io.MousePos;
    const ImVec2 t = ImGui::CalcTextSize(label);
    const ImVec2 p(mouse.x + 18.0f, mouse.y + 14.0f);
    draw->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 3.0f), ImVec2(p.x + t.x + 6.0f, p.y + t.y + 3.0f), IM_COL32(12, 12, 16, 190), 4.0f);
    draw->AddText(p, IM_COL32(235, 235, 245, 255), label);
}

void ViewportPanel::saveThumbnail(Editor& editor) {
    if (!editor.hasProject()) return;
    const int w = static_cast<int>(size_.x), h = static_cast<int>(size_.y);
    if (w < 16 || h < 16) return;
    std::vector<unsigned char> pixels(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    glBindTexture(GL_TEXTURE_2D, framebuffer_.texture());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // The middle 16:9 of the view, box-filtered down to 480 x 270, rows flipped
    // (OpenGL's first row is the bottom one).
    constexpr int outW = 480, outH = 270;
    int cw = w, ch = h;
    if (cw * 9 > ch * 16) cw = ch * 16 / 9; else ch = cw * 9 / 16;
    const int x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    std::vector<unsigned char> out(static_cast<size_t>(outW) * outH * 4);
    for (int oy = 0; oy < outH; ++oy) {
        for (int ox = 0; ox < outW; ++ox) {
            const int sx0 = x0 + ox * cw / outW, sx1 = std::max(sx0 + 1, x0 + (ox + 1) * cw / outW);
            const int sy0 = y0 + oy * ch / outH, sy1 = std::max(sy0 + 1, y0 + (oy + 1) * ch / outH);
            unsigned sum[3] = { 0, 0, 0 }, n = 0;
            for (int sy = sy0; sy < sy1; ++sy) {
                for (int sx = sx0; sx < sx1; ++sx) {
                    const unsigned char* p = &pixels[(static_cast<size_t>(sy) * w + sx) * 4];
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    ++n;
                }
            }
            unsigned char* q = &out[(static_cast<size_t>(outH - 1 - oy) * outW + ox) * 4];
            for (int c = 0; c < 3; ++c) q[c] = static_cast<unsigned char>(sum[c] / std::max(n, 1u));
            q[3] = 255;
        }
    }
    writePng(editor.project().root() / ".rift" / "thumbnail.png", outW, outH, out.data());
}

void ViewportPanel::drawGizmo(UiContext& ui, const glm::mat4& view, const glm::mat4& proj) {
    Editor& editor = ui.editor;
    EditorSettings& settings = editor.settings();
    Scene& scene = editor.scene();
    styleGizmo();
    ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
    ImGuizmo::SetRect(origin_.x, origin_.y, size_.x, size_.y);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::Enable(!ImGui::GetIO().KeyAlt && !capture_);

    std::vector<Instance*> roots;
    for (Instance* i : editor.selectionRoots()) if (i->hasTransform()) roots.push_back(i);
    Instance* primary = scene.find(editor.primary());
    if (editor.tool == Tool::Select || roots.empty() || !primary || !primary->hasTransform()) {
        gizmoDragging_ = false;
        return;
    }
    const bool scaling = editor.tool == Tool::Scale;
    if (scaling && !primary->isBasePart()) return; // models and lights have no size

    if (!gizmoDragging_) {
        gizmoMatrix_ = scaling ? primary->worldTransform() : primary->worldFrame();
        // Posing in the Animation Editor: a limb turns around its joint.
        if (!scaling && editor.animatingRig && primary->isBasePart()) {
            if (Instance* rig = scene.find(editor.animatingRig); rig && primary->isDescendantOf(rig)) {
                for (const auto& [name, motor] : rigJoints(*rig)) {
                    if (motor->attachment1.id != primary->id) continue;
                    if (const Instance* part0 = scene.find(motor->attachment0.id))
                        gizmoMatrix_ = part0->worldFrame() * motor->c0.matrix() * motor->jointTransform.matrix();
                    break;
                }
            }
        }
    }
    const glm::mat4 before = gizmoMatrix_;

    ImGuizmo::OPERATION op = editor.tool == Tool::Move ? ImGuizmo::TRANSLATE : editor.tool == Tool::Rotate ? ImGuizmo::ROTATE : ImGuizmo::BOUNDS;
    const ImGuizmo::MODE mode = scaling || settings.localSpace ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    const bool snap = settings.snap != ImGui::GetIO().KeyCtrl; // Ctrl flips snapping while dragging
    const float moveSnap[3] = { settings.snapMove, settings.snapMove, settings.snapMove };
    const float rotateSnap[3] = { settings.snapRotate, settings.snapRotate, settings.snapRotate };
    const float scaleSnap[3] = { settings.snapScale, settings.snapScale, settings.snapScale };
    static const float unitBounds[6] = { -0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f };

    const float* snapValues = nullptr;
    if (snap && op == ImGuizmo::TRANSLATE) snapValues = moveSnap;
    if (snap && op == ImGuizmo::ROTATE) snapValues = rotateSnap;
    // BOUNDS needs a non-bounds operation too; translate handles stay off (scale only).
    const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(proj), op, mode, glm::value_ptr(gizmoMatrix_), nullptr,
                                              snapValues, op == ImGuizmo::BOUNDS ? unitBounds : nullptr, op == ImGuizmo::BOUNDS && snap ? scaleSnap : nullptr);
    const bool using_ = ImGuizmo::IsUsing();

    if (using_ && !gizmoDragging_) {
        gizmoDragging_ = true;
        dragPivotStart_ = before;
        dragChanges_.clear();
        dragStartWorld_.clear();
        for (Instance* r : roots) {
            dragChanges_.push_back({ r->id, TransformCommand::capture(*r), {} });
            dragStartWorld_.push_back(r->worldFrame());
        }
    }
    if (gizmoDragging_ && changed) {
        if (op == ImGuizmo::BOUNDS) {
            primary->setWorldTransform(gizmoMatrix_);
            primary->size = glm::max(primary->size, glm::vec3(0.05f));
            notifyTransform(scene, *primary);
        } else {
            const glm::mat4 delta = gizmoMatrix_ * glm::inverse(dragPivotStart_);
            for (size_t k = 0; k < dragChanges_.size(); ++k) {
                Instance* i = scene.find(dragChanges_[k].id);
                if (!i) continue;
                glm::vec3 p, r, s;
                TransformUtil::decompose(delta * dragStartWorld_[k], p, r, s);
                if (op == ImGuizmo::TRANSLATE) r = i->worldRotation(); // pure moves never touch rotation (no drift)
                i->setWorldFrame(p, r);
                notifyTransform(scene, *i);
            }
        }
    }
    if (gizmoDragging_ && !using_) {
        gizmoDragging_ = false;
        std::vector<TransformCommand::Change> changes;
        for (auto& c : dragChanges_) {
            Instance* i = scene.find(c.id);
            if (!i) continue;
            c.after = TransformCommand::capture(*i);
            if (c.after.position != c.before.position || c.after.rotation != c.before.rotation || c.after.size != c.before.size) changes.push_back(c);
        }
        if (!changes.empty()) {
            const char* verb = op == ImGuizmo::TRANSLATE ? "Move " : op == ImGuizmo::ROTATE ? "Rotate " : "Resize ";
            const Instance* first = scene.find(changes[0].id);
            const std::string what = changes.size() == 1 && first ? first->name : std::to_string(changes.size()) + " objects";
            editor.record(std::make_unique<TransformCommand>(verb + what, std::move(changes)));
        }
        dragChanges_.clear();
    }
}

void ViewportPanel::handleSelection(UiContext& ui, const glm::mat4& view, const glm::mat4& proj, const GuiSystem* gui) {
    Editor& editor = ui.editor;
    if (editor.playing() && !editor.serverView()) {
        hoveredId_ = 0;
        pressing_ = marquee_ = guiDragging_ = false;
        return;
    }
    Scene& scene = editor.scene();
    const ImGuiIO& io = ImGui::GetIO();
    const glm::vec2 mouse = editor.input().mousePosition() - origin_;
    // Gizmo handles, the axis widget and the overlay buttons take clicks first.
    const bool overGizmo = ImGuizmo::IsOver() || ImGuizmo::IsUsing() || gizmoDragging_ || ImGui::IsAnyItemHovered();
    glm::vec3 rayOrigin, rayDir;
    editor.camera().ray(mouse, size_, rayOrigin, rayDir);

    // Hover highlight
    hoveredId_ = 0;
    if (hovered_ && !capture_ && !overGizmo && !pressing_) {
        if (gui && guiAt(*gui, mouse)) hoveredId_ = 0;
        else if (auto hit = pickPart(scene, rayOrigin, rayDir)) hoveredId_ = selectionTarget(hit->part, io.KeyAlt)->id;
    }

    if (hovered_ && !capture_ && !overGizmo && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        pressing_ = true;
        marquee_ = false;
        pressPos_ = mouse;
        // Pressing a selected GUI element starts moving it.
        if (gui) {
            const uint64_t id = guiAt(*gui, mouse);
            Instance* element = id ? scene.find(id) : nullptr;
            if (element && editor.isSelected(id) && element->isGuiObject()) {
                guiDragging_ = true;
                guiDragId_ = id;
                guiDragStart_ = element->guiPosition;
            }
        }
    }
    if (!pressing_) return;

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const glm::vec2 delta = mouse - pressPos_;
        if (guiDragging_) {
            if (glm::length(delta) > kClickSlop || editor.previewing()) {
                UDim2 p = guiDragStart_;
                p.xOffset += std::round(delta.x);
                p.yOffset += std::round(delta.y);
                editor.previewProperty({ guiDragId_ }, "Position", p);
            }
        } else if (!io.KeyAlt && glm::length(delta) > kClickSlop) {
            marquee_ = true;
        }
        if (marquee_) {
            const ImVec2 a(origin_.x + std::min(pressPos_.x, mouse.x), origin_.y + std::min(pressPos_.y, mouse.y));
            const ImVec2 b(origin_.x + std::max(pressPos_.x, mouse.x), origin_.y + std::max(pressPos_.y, mouse.y));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(a, b, theme::u32(theme::color::accent, 0.12f));
            draw->AddRect(a, b, theme::u32(theme::color::accentHover, 0.9f), 0.0f, 0, 1.0f);
        }
        return;
    }

    // Released
    pressing_ = false;
    const bool additive = io.KeyCtrl || io.KeyShift;
    if (guiDragging_) {
        guiDragging_ = false;
        if (editor.previewing()) {
            editor.commitPreview();
            return;
        }
    }
    if (marquee_) {
        marquee_ = false;
        const glm::vec2 lo = glm::min(pressPos_, mouse), hi = glm::max(pressPos_, mouse);
        std::vector<uint64_t> ids = additive ? editor.selection() : std::vector<uint64_t>();
        const glm::mat4 viewProj = proj * view;
        Scene::forEachDescendant(scene.workspace(), [&](Instance& part) {
            if (!part.isBasePart() || !part.visible) return;
            const glm::vec4 clip = viewProj * glm::vec4(part.worldPosition(), 1.0f);
            if (clip.w <= 0.0f) return;
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            const glm::vec2 pixel((ndc.x * 0.5f + 0.5f) * size_.x, (0.5f - ndc.y * 0.5f) * size_.y);
            if (pixel.x < lo.x || pixel.y < lo.y || pixel.x > hi.x || pixel.y > hi.y) return;
            const uint64_t id = selectionTarget(&part, io.KeyAlt)->id;
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        });
        editor.setSelection(ids);
        return;
    }
    if (glm::length(mouse - pressPos_) > kClickSlop) return; // an Alt-orbit, not a click

    Instance* target = nullptr;
    if (gui) {
        if (const uint64_t id = guiAt(*gui, mouse)) target = scene.find(id);
    }
    if (!target) {
        if (auto hit = pickPart(scene, rayOrigin, rayDir)) target = selectionTarget(hit->part, io.KeyAlt);
    }
    if (!target) {
        if (!additive) editor.clearSelection();
        return;
    }
    editor.select(target->id, additive ? SelectMode::Toggle : SelectMode::Replace);
}

void ViewportPanel::handleDrop(UiContext& ui) {
    if (!ImGui::BeginDragDropTarget()) return;
    Editor& editor = ui.editor;
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(kAssetPayload)) {
        const std::string path(static_cast<const char*>(p->Data));
        const glm::vec2 mouse = editor.input().mousePosition() - origin_;
        Instance* target = nullptr;
        if (!editor.playing() && editor.settings().showGui) {
            GuiSystem gui(editor.scene());
            gui.layout(size_);
            if (const uint64_t id = guiAt(gui, mouse)) target = editor.scene().find(id);
        }
        if (!target) {
            glm::vec3 o, d;
            editor.camera().ray(mouse, size_, o, d);
            if (auto hit = pickPart(editor.scene(), o, d)) target = hit->part;
        }
        dropAsset(editor, path, target);
    }
    ImGui::EndDragDropTarget();
}

void ViewportPanel::drawOverlays(UiContext& ui, const glm::mat4& view, const glm::mat4& proj) {
    Editor& editor = ui.editor;
    EditorSettings& settings = editor.settings();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float pad = ImGui::GetStyle().WindowPadding.x;
    const ImVec2 a(origin_.x, origin_.y), b(origin_.x + size_.x, origin_.y + size_.y);

    // Frame rate
    ++frames_;
    fpsTimer_ += ui.dt;
    if (fpsTimer_ >= 0.5f) {
        fps_ = frames_ / fpsTimer_;
        frames_ = 0;
        fpsTimer_ = 0.0f;
    }

    // Playing: a coloured frame around the view.
    if (GameSession* s = editor.session()) {
        const ImVec4 c = s->paused() ? theme::color::warning : editor.serverView() ? theme::color::success : ImVec4(0.30f, 0.60f, 1.0f, 1.0f);
        draw->AddRect(a, b, theme::u32(c, 0.9f), 0.0f, 0, 2.0f);
    }

    // ---- view options (top right)
    const float button = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(b.x - button - pad, a.y + pad));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.08f, 0.08f, 0.10f, 0.65f));
    if (theme::iconButton(ICON_FA_EYE, "View options")) ImGui::OpenPopup("##viewOptions");
    ImGui::PopStyleColor();
    if (ImGui::BeginPopup("##viewOptions")) {
        ImGui::SeparatorText("Show");
        ImGui::Checkbox("Grid", &settings.showGrid);
        ImGui::Checkbox("GUI while editing", &settings.showGui);
        ImGui::Checkbox("Shadows", &settings.shadows);
        ImGui::Checkbox("Wireframe", &settings.wireframe);
        ImGui::Checkbox("Statistics", &settings.showStats);
        ImGui::SeparatorText("Camera");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Speed", &settings.cameraSpeed, 2.0f, 200.0f, "%.0f studs/s", ImGuiSliderFlags_Logarithmic);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::SliderFloat("Field of view", &settings.fieldOfView, 30.0f, 110.0f, "%.0f deg");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        ImGui::DragFloat("Grid step", &settings.gridStep, 0.05f, 0.1f, 64.0f, "%.2f");
        ImGui::EndPopup();
    }

    // ---- axis widget (edit mode): shows the orientation; click an axis to look along it
    if (!editor.playing() || editor.serverView()) {
        const float radius = ImGui::GetFontSize() * 2.6f;
        const float dot = ImGui::GetFontSize() * 0.62f;
        const ImVec2 c(b.x - radius - dot - pad, a.y + button + pad * 2.0f + radius + dot);
        ImGui::SetCursorScreenPos(ImVec2(c.x - radius - dot, c.y - radius - dot));
        ImGui::InvisibleButton("##axes", ImVec2((radius + dot) * 2.0f, (radius + dot) * 2.0f));
        const bool hoveredWidget = ImGui::IsItemHovered();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (hoveredWidget) draw->AddCircleFilled(c, radius + dot, IM_COL32(20, 20, 28, 90));

        struct Axis { glm::vec3 dir; ImVec4 color; const char* label; };
        const Axis axes[6] = {
            { { 1, 0, 0 }, { 0.93f, 0.33f, 0.36f, 1.0f }, "X" }, { { 0, 1, 0 }, { 0.45f, 0.82f, 0.36f, 1.0f }, "Y" }, { { 0, 0, 1 }, { 0.33f, 0.56f, 0.96f, 1.0f }, "Z" },
            { { -1, 0, 0 }, { 0.93f, 0.33f, 0.36f, 1.0f }, nullptr }, { { 0, -1, 0 }, { 0.45f, 0.82f, 0.36f, 1.0f }, nullptr }, { { 0, 0, -1 }, { 0.33f, 0.56f, 0.96f, 1.0f }, nullptr },
        };
        const glm::mat3 toCamera(editor.camera().view());
        int order[6] = { 0, 1, 2, 3, 4, 5 };
        glm::vec3 projected[6];
        for (int k = 0; k < 6; ++k) projected[k] = toCamera * axes[k].dir;
        std::sort(order, order + 6, [&](int l, int r) { return projected[l].z < projected[r].z; }); // far ones first
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        int hit = -1;
        for (int k : order) {
            const ImVec2 end(c.x + projected[k].x * radius, c.y - projected[k].y * radius);
            const bool over = hoveredWidget && (mouse.x - end.x) * (mouse.x - end.x) + (mouse.y - end.y) * (mouse.y - end.y) < dot * dot;
            if (over) hit = k;
            const ImVec4 col = axes[k].color;
            if (axes[k].label) {
                draw->AddLine(c, end, theme::u32(col), 2.0f);
                draw->AddCircleFilled(end, dot, theme::u32(over ? ImVec4(1, 1, 1, 1) : col));
                const ImVec2 t = ImGui::CalcTextSize(axes[k].label);
                draw->AddText(ImVec2(end.x - t.x * 0.5f, end.y - t.y * 0.5f), IM_COL32(20, 20, 26, 255), axes[k].label);
            } else {
                draw->AddCircleFilled(end, dot * 0.8f, theme::u32(col, over ? 0.9f : 0.45f));
                draw->AddCircle(end, dot * 0.8f, theme::u32(col), 0, 1.5f);
            }
        }
        if (hit >= 0) theme::tooltip(hit < 3 ? "Look from this side" : "Look from the opposite side");
        if (clicked && hit >= 0) {
            // Orbit around the point in front of the camera so the view stays on what you were looking at.
            const float distance = 24.0f;
            const glm::vec3 target = editor.camera().position() + editor.camera().forward() * distance;
            glm::vec3 from = axes[hit].dir;
            if (std::abs(from.y) > 0.5f) from += glm::vec3(0.0f, 0.0f, 0.001f); // straight up/down needs a hint of direction
            editor.camera().lookAt(target + glm::normalize(from) * distance, target);
        }
    }

    // ---- statistics (bottom left)
    if (settings.showStats) {
        const RenderStats& st = ui.renderer.stats();
        char text[256];
        char tris[32];
        if (st.triangles < 10000) std::snprintf(tris, sizeof(tris), "%d", st.triangles);
        else std::snprintf(tris, sizeof(tris), "%.1fk", st.triangles / 1000.0f);
        int n = std::snprintf(text, sizeof(text), "%.0f FPS  %.1f ms\n%d parts  %d draws  %s tris", fps_, fps_ > 0.0f ? 1000.0f / fps_ : 0.0f,
                              st.parts, st.drawCalls, tris);
        if (GameSession* s = editor.session()) std::snprintf(text + n, sizeof(text) - n, "\ngame time %.1f s", s->time());
        const ImVec2 t = ImGui::CalcTextSize(text);
        const ImVec2 p(a.x + pad, b.y - t.y - pad * 1.5f);
        draw->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 4.0f), ImVec2(p.x + t.x + 6.0f, p.y + t.y + 4.0f), IM_COL32(10, 10, 14, 150), 5.0f);
        draw->AddText(p, IM_COL32(220, 220, 230, 230), text);
    }

    // ---- hints
    if (!editor.hasScene()) return;
    if (editor.serverView()) {
        const char* text = ICON_FA_SERVER "  Server view: the game gets no input. Switch to Client to play.";
        const ImVec2 t = ImGui::CalcTextSize(text);
        const ImVec2 p(a.x + (size_.x - t.x) * 0.5f, a.y + pad * 2.0f);
        draw->AddRectFilled(ImVec2(p.x - 12.0f, p.y - 6.0f), ImVec2(p.x + t.x + 12.0f, p.y + t.y + 6.0f), IM_COL32(16, 60, 36, 200), 8.0f);
        draw->AddText(p, IM_COL32(235, 245, 238, 255), text);
    } else if (editor.playing()) {
        if (!focused_) {
            const char* text = ICON_FA_GAMEPAD "  Click the view to control the game";
            const ImVec2 t = ImGui::CalcTextSize(text);
            const ImVec2 p(a.x + (size_.x - t.x) * 0.5f, a.y + pad * 2.0f);
            draw->AddRectFilled(ImVec2(p.x - 12.0f, p.y - 6.0f), ImVec2(p.x + t.x + 12.0f, p.y + t.y + 6.0f), IM_COL32(10, 10, 14, 170), 8.0f);
            draw->AddText(p, IM_COL32(235, 235, 245, 255), text);
        }
    } else if (std::all_of(editor.scene().workspace().children.begin(), editor.scene().workspace().children.end(), [](const Instance* i) { return i->isFixedContainer(); })
               && editor.scene().terrain().empty()) {
        const char* text = "The scene is empty. Insert a part from the toolbar, or drag files from the Assets panel.";
        const ImVec2 t = ImGui::CalcTextSize(text);
        draw->AddText(ImVec2(a.x + (size_.x - t.x) * 0.5f, a.y + size_.y * 0.45f), theme::u32(theme::color::textDim), text);
    }
    (void)view;
    (void)proj;
}

} // namespace rift
