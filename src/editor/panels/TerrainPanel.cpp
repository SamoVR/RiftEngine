#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "scene/Reflection.h"

#include "IconsFontAwesome6.h"
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <random>

namespace rift {

using Brush = TerrainData::Brush;

const char* terrainBrushName(Brush b) {
    switch (b) {
        case Brush::Add: return "Add";
        case Brush::Subtract: return "Subtract";
        case Brush::Grow: return "Grow";
        case Brush::Erode: return "Erode";
        case Brush::Smooth: return "Smooth";
        case Brush::Flatten: return "Flatten";
        case Brush::Paint: return "Paint";
    }
    return "Brush";
}

const char* terrainBrushIcon(Brush b) {
    switch (b) {
        case Brush::Add: return ICON_FA_CIRCLE_PLUS;
        case Brush::Subtract: return ICON_FA_CIRCLE_MINUS;
        case Brush::Grow: return ICON_FA_MOUNTAIN;
        case Brush::Erode: return ICON_FA_HILL_ROCKSLIDE;
        case Brush::Smooth: return ICON_FA_WATER;
        case Brush::Flatten: return ICON_FA_RULER_HORIZONTAL;
        case Brush::Paint: return ICON_FA_PAINTBRUSH;
    }
    return ICON_FA_MOUNTAIN_SUN;
}

const char* materialName(Material m) {
    // "LeafyGrass" -> "Leafy Grass" (built once).
    static const std::vector<std::string> pretty = [] {
        std::vector<std::string> out;
        for (const std::string& name : enumNamesFor("Material")) {
            std::string spaced;
            for (size_t k = 0; k < name.size(); ++k) {
                if (k > 0 && std::isupper(static_cast<unsigned char>(name[k])) && std::islower(static_cast<unsigned char>(name[k - 1]))) spaced += ' ';
                spaced += name[k];
            }
            out.push_back(spaced);
        }
        return out;
    }();
    const size_t i = static_cast<size_t>(m);
    return i < pretty.size() ? pretty[i].c_str() : "?";
}

namespace {
    constexpr Brush kBrushes[] = { Brush::Add, Brush::Subtract, Brush::Grow, Brush::Erode, Brush::Smooth, Brush::Flatten, Brush::Paint };

    const char* brushTip(Brush b) {
        switch (b) {
            case Brush::Add: return "Add terrain of the selected material (Ctrl: subtract)";
            case Brush::Subtract: return "Carve terrain away (Ctrl: add)";
            case Brush::Grow: return "Raise the surface gently (Ctrl: erode)";
            case Brush::Erode: return "Wear the surface down gently (Ctrl: grow)";
            case Brush::Smooth: return "Round off bumps and edges (Shift with any brush)";
            case Brush::Flatten: return "Level the ground to the height where the stroke starts";
            case Brush::Paint: return "Change the material without changing the shape";
        }
        return "";
    }

    bool usesMaterial(Brush b) { return b == Brush::Add || b == Brush::Grow || b == Brush::Flatten || b == Brush::Paint; }
    bool usesWater(Brush b) { return b == Brush::Subtract || b == Brush::Smooth || b == Brush::Flatten; }

    // The materials terrain is made of, in the order the swatches show them.
    const std::vector<Material>& terrainMaterials(bool withWater) {
        static const std::vector<Material> solid = {
            Material::Grass, Material::LeafyGrass, Material::Ground, Material::Mud, Material::Sand, Material::Sandstone,
            Material::Rock, Material::Slate, Material::Basalt, Material::Limestone, Material::CrackedLava, Material::Salt,
            Material::Snow, Material::Glacier, Material::Ice, Material::Asphalt, Material::Pavement, Material::Concrete,
            Material::Brick, Material::Cobblestone, Material::WoodPlanks,
        };
        static const std::vector<Material> all = [] {
            std::vector<Material> v = solid;
            v.push_back(Material::Water);
            return v;
        }();
        return withWater ? all : solid;
    }

    // A label on the left and the next widget filling the rest of the row.
    void row(const char* label, float labelWidth) {
        const float x = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(x + labelWidth);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    // A big tool button: the icon over its name.
    bool toolTile(const char* icon, const char* label, bool active, const ImVec2& size, const char* tip) {
        ImGui::PushID(label);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##tile", size);
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float rounding = ImGui::GetStyle().FrameRounding;
        ImVec4 bg = theme::color::frame;
        if (hovered) bg = theme::color::frameHover;
        if (active) bg = held ? theme::color::accentActive : hovered ? theme::color::accentHover : theme::color::accent;
        dl->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y), theme::u32(bg), rounding);
        const ImU32 fg = theme::u32(active ? ImVec4(1, 1, 1, 1) : theme::color::text);
        ImFont* font = ImGui::GetFont();
        const float iconSize = ImGui::GetFontSize() * 1.35f, textSize = ImGui::GetFontSize() * 0.92f;
        const ImVec2 is = font->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
        const ImVec2 ts = font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, label);
        const float gap = ImGui::GetStyle().ItemInnerSpacing.y;
        const float top = p.y + (size.y - is.y - ts.y - gap) * 0.5f;
        dl->AddText(font, iconSize, ImVec2(std::floor(p.x + (size.x - is.x) * 0.5f), std::floor(top)), fg, icon);
        dl->AddText(font, textSize, ImVec2(std::floor(p.x + (size.x - ts.x) * 0.5f), std::floor(top + is.y + gap)), active ? fg : theme::u32(theme::color::textDim), label);
        ImGui::PopID();
        theme::tooltip(tip);
        return pressed;
    }

    // Two or more buttons side by side, one of them on (Sphere | Cube).
    bool segment(const char* const* labels, int count, int& value) {
        const float spacing = 2.0f;
        const float w = (ImGui::CalcItemWidth() - spacing * (count - 1)) / count;
        bool changed = false;
        for (int i = 0; i < count; ++i) {
            if (i > 0) ImGui::SameLine(0.0f, spacing);
            const bool on = value == i;
            if (on) {
                ImGui::PushStyleColor(ImGuiCol_Button, theme::color::accent);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::accentHover);
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::color::accentActive);
            }
            if (ImGui::Button(labels[i], ImVec2(w, 0.0f)) && !on) { value = i; changed = true; }
            if (on) ImGui::PopStyleColor(3);
        }
        return changed;
    }

    // A material swatch: its colour with a soft highlight; the selected one has
    // an accent ring inside its edge (so the grid lines up with other widgets).
    bool swatch(Material m, const Color3& c, bool selected, float size) {
        ImGui::PushID(static_cast<int>(m));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##swatch", ImVec2(size, size));
        const bool hovered = ImGui::IsItemHovered();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float r = ImGui::GetStyle().FrameRounding;
        const ImVec2 a = p, b(p.x + size, p.y + size);
        dl->AddRectFilled(a, b, theme::u32(ImVec4(c.r, c.g, c.b, 1.0f)), r);
        // Light from above: the top half a little brighter.
        dl->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.45f), IM_COL32(255, 255, 255, 28), r, ImDrawFlags_RoundCornersTop);
        if (selected) {
            dl->AddRect(ImVec2(a.x + 1.0f, a.y + 1.0f), ImVec2(b.x - 1.0f, b.y - 1.0f), theme::u32(theme::color::accentHover), r, 0, 2.5f);
            dl->AddRect(ImVec2(a.x + 3.5f, a.y + 3.5f), ImVec2(b.x - 3.5f, b.y - 3.5f), IM_COL32(255, 255, 255, 150), std::max(r - 2.0f, 0.0f), 0, 1.0f);
        } else if (hovered) {
            dl->AddRect(ImVec2(a.x + 0.5f, a.y + 0.5f), ImVec2(b.x - 0.5f, b.y - 0.5f), IM_COL32(255, 255, 255, 110), r, 0, 1.0f);
        }
        if (m == Material::Water) {
            const float y = (a.y + b.y) * 0.5f;
            for (int k = 0; k < 2; ++k) {
                const float yy = y + (k - 0.5f) * (b.y - a.y) * 0.25f;
                dl->AddBezierCubic(ImVec2(a.x + 4, yy), ImVec2(a.x + (b.x - a.x) * 0.35f, yy - 4), ImVec2(a.x + (b.x - a.x) * 0.65f, yy + 4), ImVec2(b.x - 4, yy), IM_COL32(255, 255, 255, 120), 1.5f);
            }
        }
        ImGui::PopID();
        theme::tooltip(materialName(m));
        return pressed;
    }
}

void TerrainPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    // A colour drag ends without the widget saying so: commit once nothing is held.
    if (colorEdit_ && !ImGui::IsAnyItemActive() && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) commitColor(editor);
    if (!ImGui::Begin(ICON_FA_MOUNTAIN_SUN "  Terrain Editor###Terrain", open)) {
        ImGui::End();
        return;
    }
    const bool editing = editor.hasScene() && !editor.playing();
    if (!editing) {
        ImGui::Spacing();
        ImGui::TextColored(theme::color::textDim, editor.playing() ? ICON_FA_CIRCLE_INFO "  Stop the game to edit terrain." : ICON_FA_CIRCLE_INFO "  Open a scene to edit its terrain.");
        editor.terrainTool.active = false;
        ImGui::End();
        return;
    }

    if (ImGui::BeginTabBar("##terrainTabs")) {
        if (ImGui::BeginTabItem(ICON_FA_WAND_MAGIC_SPARKLES "  Create")) {
            drawCreate(ui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_PAINTBRUSH "  Edit", nullptr, editTabRequest_ ? ImGuiTabItemFlags_SetSelected : 0)) {
            drawEdit(ui);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_FA_PALETTE "  Colours")) {
            drawColors(ui);
            ImGui::EndTabItem();
        }
        editTabRequest_ = false;
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void TerrainPanel::drawCreate(UiContext& ui) {
    Editor& editor = ui.editor;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float labelWidth = ImGui::GetFontSize() * 7.0f;
    TerrainData::GenerateOptions& g = generate_;

    ImGui::Spacing();
    theme::heading("Generate");
    row("Centre", labelWidth);
    float centre[2] = { g.center.x, g.center.z };
    if (ImGui::DragFloat2("##centre", centre, 1.0f, -10000.0f, 10000.0f, "%.0f")) { g.center.x = centre[0]; g.center.z = centre[1]; }
    theme::tooltip("Where the middle of the new land goes (X, Z)");
    row("Size", labelWidth);
    float size[2] = { g.size.x, g.size.y };
    if (ImGui::DragFloat2("##size", size, 2.0f, 32.0f, 4096.0f, "%.0f")) g.size = glm::clamp(glm::vec2(size[0], size[1]), glm::vec2(32.0f), glm::vec2(4096.0f));
    theme::tooltip("Width and length in studs (up to 4096)");
    row("Base height", labelWidth);
    ImGui::DragFloat("##base", &g.baseHeight, 0.5f, -512.0f, 512.0f, "%.0f studs");
    row("Hills", labelWidth);
    ImGui::SliderFloat("##hills", &g.hills, 0.0f, 2.0f, "%.2f");
    row("Mountains", labelWidth);
    ImGui::SliderFloat("##mountains", &g.mountains, 0.0f, 2.0f, "%.2f");
    row("Water", labelWidth);
    ImGui::Checkbox("##water", &g.water);
    ImGui::BeginDisabled(!g.water);
    row("Water level", labelWidth);
    ImGui::DragFloat("##waterLevel", &g.waterLevel, 0.25f, 0.0f, 256.0f, "%.1f studs");
    theme::tooltip("Above the base height");
    ImGui::EndDisabled();
    row("Caves", labelWidth);
    ImGui::Checkbox("##caves", &g.caves);
    row("Seed", labelWidth);
    const float dice = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - dice - style.ItemInnerSpacing.x);
    int seed = static_cast<int>(g.seed);
    if (ImGui::InputInt("##seed", &seed, 0, 0)) g.seed = static_cast<uint32_t>(std::max(seed, 0));
    ImGui::SameLine(0.0f, style.ItemInnerSpacing.x);
    if (theme::iconButton(ICON_FA_DICE, "A random seed")) g.seed = std::random_device{}() % 1000000u;

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, theme::color::accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::color::accentActive);
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES "  Generate", ImVec2(-FLT_MIN, ImGui::GetFrameHeight() * 1.25f))) editor.generateTerrain(g);
    ImGui::PopStyleColor(3);
    theme::tooltip("Adds hills, mountains and lakes inside the area (Ctrl+Z undoes it)");

    ImGui::Spacing();
    ImGui::Spacing();
    theme::heading("Clear");
    const TerrainData& terrain = editor.editScene().terrain();
    size_t chunks = 0;
    for (const auto& [key, c] : terrain.chunks()) if (!c.allAir()) ++chunks;
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::textDim);
    if (chunks == 0) ImGui::TextWrapped("The scene has no terrain yet.");
    else ImGui::TextWrapped("%zu terrain chunk%s (32 studs each). Clearing can be undone.", chunks, chunks == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::BeginDisabled(chunks == 0);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.18f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.24f, 0.26f, 1.0f));
    if (ImGui::Button(ICON_FA_TRASH_CAN "  Clear Terrain", ImVec2(-FLT_MIN, 0.0f))) ImGui::OpenPopup("Clear terrain?");
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (ImGui::BeginPopupModal("Clear terrain?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Remove all terrain from this scene?");
        ImGui::TextColored(theme::color::textDim, "Ctrl+Z brings it back.");
        ImGui::Spacing();
        const float w = ImGui::GetFontSize() * 6.0f;
        if (ImGui::Button("Clear", ImVec2(w, 0.0f))) {
            editor.clearTerrain();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void TerrainPanel::drawEdit(UiContext& ui) {
    Editor& editor = ui.editor;
    EditorSettings& s = editor.settings();
    Editor::TerrainTool& tool = editor.terrainTool;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float labelWidth = ImGui::GetFontSize() * 7.0f;

    // ---- tools
    ImGui::Spacing();
    const float avail = ImGui::GetContentRegionAvail().x;
    const float minTile = ImGui::GetFontSize() * 4.4f;
    const int cols = std::clamp(static_cast<int>((avail + style.ItemSpacing.x) / (minTile + style.ItemSpacing.x)), 2, 4);
    const float tileW = std::floor((avail - style.ItemSpacing.x * (cols - 1)) / cols);
    const ImVec2 tileSize(tileW, std::floor(ImGui::GetFontSize() * 3.6f));
    int n = 0;
    for (Brush b : kBrushes) {
        if (n % cols != 0) ImGui::SameLine();
        const bool on = tool.active && tool.brush == b;
        if (toolTile(terrainBrushIcon(b), terrainBrushName(b), on, tileSize, brushTip(b))) {
            if (on) tool.active = false;
            else { tool.brush = b; tool.active = true; }
        }
        ++n;
    }

    // ---- brush
    ImGui::Spacing();
    ImGui::Spacing();
    theme::heading("Brush");
    row("Shape", labelWidth);
    static const char* shapes[] = { ICON_FA_CIRCLE "  Sphere", ICON_FA_SQUARE "  Cube" };
    int shape = s.terrainCube ? 1 : 0;
    if (segment(shapes, 2, shape)) s.terrainCube = shape == 1;
    row("Size", labelWidth);
    ImGui::SliderFloat("##size", &s.terrainSize, 2.0f, 64.0f, "%.0f studs", ImGuiSliderFlags_Logarithmic);
    theme::tooltip("Brush diameter ([ and ] in the viewport)");
    row("Strength", labelWidth);
    ImGui::SliderFloat("##strength", &s.terrainStrength, 0.05f, 1.0f, "%.2f");
    if (usesWater(tool.brush)) {
        row("Ignore water", labelWidth);
        ImGui::Checkbox("##ignoreWater", &s.terrainIgnoreWater);
        theme::tooltip("Leave water alone (only shape the ground)");
    }
    if (tool.brush == Brush::Flatten) {
        row("Level to", labelWidth);
        static const char* planes[] = { "Stroke start", "Fixed height" };
        int plane = tool.fixedPlane ? 1 : 0;
        if (segment(planes, 2, plane)) tool.fixedPlane = plane == 1;
        if (tool.fixedPlane) {
            row("Height", labelWidth);
            ImGui::DragFloat("##plane", &tool.planeHeight, 0.25f, -1000.0f, 1000.0f, "%.1f studs");
        }
    }

    // ---- material
    if (usesMaterial(tool.brush)) {
        ImGui::Spacing();
        ImGui::Spacing();
        const Material current = static_cast<Material>(std::clamp(s.terrainMaterial, 0, kMaterialCount - 1));
        char title[64];
        std::snprintf(title, sizeof(title), "Material: %s", materialName(current));
        theme::heading(title);
        const TerrainData& terrain = editor.editScene().terrain();
        const float cell = std::floor(ImGui::GetFontSize() * 2.7f);
        const float gap = 3.0f;
        const float width = ImGui::GetContentRegionAvail().x;
        const int perRow = std::max(1, static_cast<int>((width + gap) / (cell + gap)));
        // Spread the leftover width so the grid fills the row edge to edge.
        const float size = std::floor((width - gap * (perRow - 1)) / perRow);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap)); // rows as close as columns
        int k = 0;
        for (Material m : terrainMaterials(tool.brush != Brush::Paint)) {
            if (k % perRow != 0) ImGui::SameLine(0.0f, gap);
            if (swatch(m, terrain.materialColor(m), m == current, size)) s.terrainMaterial = static_cast<int>(m);
            ++k;
        }
        ImGui::PopStyleVar();
    }

    // ---- how to
    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::textDim);
    ImGui::PushTextWrapPos(0.0f);
    if (tool.active) {
        ImGui::TextUnformatted(ICON_FA_COMPUTER_MOUSE "  Drag in the viewport to sculpt. Ctrl reverses the brush, "
                               "Shift smooths, [ and ] change the size, Esc goes back to selecting.");
    } else {
        ImGui::TextUnformatted(ICON_FA_CIRCLE_INFO "  Pick a tool above, then drag in the viewport.");
    }
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void TerrainPanel::drawColors(UiContext& ui) {
    Editor& editor = ui.editor;
    TerrainData& terrain = editor.editScene().terrain();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::Spacing();

    // ---- water: properties of the Terrain object (undoable like any property)
    if (Instance* t = editor.editScene().terrainInstance()) {
        theme::heading("Water");
        const float labelWidth = ImGui::GetFontSize() * 7.0f;
        row("Colour", labelWidth);
        float wc[3] = { t->color.r, t->color.g, t->color.b };
        if (ImGui::ColorEdit3("##waterColor", wc, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
            editor.previewProperty({ t->id }, "WaterColor", Color3(wc[0], wc[1], wc[2]));
        row("Transparency", labelWidth);
        float wt = t->transparency;
        if (ImGui::SliderFloat("##waterTransparency", &wt, 0.0f, 1.0f, "%.2f")) editor.previewProperty({ t->id }, "WaterTransparency", wt);
        ImGui::Spacing();
        ImGui::Spacing();
    }

    // ---- material colours (Terrain:SetMaterialColor)
    theme::heading("Materials");
    const float reset = ImGui::GetFrameHeight();
    for (Material m : terrainMaterials(false)) {
        ImGui::PushID(static_cast<int>(m));
        const Color3 now = terrain.materialColor(m);
        float c[3] = { now.r, now.g, now.b };
        if (ImGui::ColorEdit3("##color", c, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
            if (!colorEdit_ || colorEdit_->material != m) {
                commitColor(editor);
                colorEdit_ = ColorEdit{ m, now };
            }
            terrain.setMaterialColor(m, Color3(c[0], c[1], c[2]));
        }
        ImGui::SameLine(0.0f, style.ItemInnerSpacing.x * 2.0f);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(materialName(m));
        const Color3 def = TerrainData::defaultMaterialColor(m);
        if (now != def) {
            ImGui::SameLine(ImGui::GetContentRegionMax().x - reset);
            if (theme::iconButton(ICON_FA_ROTATE_LEFT, "Back to the default colour")) {
                commitColor(editor);
                editor.execute(std::make_unique<TerrainCommand>(std::string("Reset ") + materialName(m) + " Colour", TerrainData::Snapshot{}, TerrainData::Snapshot{},
                                                                std::vector<TerrainCommand::ColorChange>{ { m, now, def } }));
            }
        }
        ImGui::PopID();
    }
}

void TerrainPanel::commitColor(Editor& editor) {
    if (!colorEdit_) return;
    const ColorEdit e = *colorEdit_;
    colorEdit_.reset();
    const Color3 after = editor.editScene().terrain().materialColor(e.material);
    if (after == e.before) return;
    editor.record(std::make_unique<TerrainCommand>(std::string(materialName(e.material)) + " Colour", TerrainData::Snapshot{}, TerrainData::Snapshot{},
                                                   std::vector<TerrainCommand::ColorChange>{ { e.material, e.before, after } }));
}

} // namespace rift
