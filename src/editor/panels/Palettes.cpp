#include "editor/panels/Palettes.h"
#include "editor/Theme.h"
#include "scene/BrickColor.h"
#include "scene/Reflection.h"
#include "scene/Terrain.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace rift {

namespace {
    ImU32 col(const Color3& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a)); }
    ImU32 col(int r, int g, int b, int a = 255) { return IM_COL32(r, g, b, a); }
    Color3 scaled(const Color3& c, float k) { return { std::min(c.r * k, 1.0f), std::min(c.g * k, 1.0f), std::min(c.b * k, 1.0f) }; }

    std::string hexOf(const Color3& c) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", static_cast<int>(std::lround(c.r * 255)), static_cast<int>(std::lround(c.g * 255)),
                      static_cast<int>(std::lround(c.b * 255)));
        return buf;
    }
    bool parseHex(const std::string& s, Color3& out) {
        std::string h = s;
        if (!h.empty() && h[0] == '#') h.erase(0, 1);
        if (h.size() != 6 || !std::all_of(h.begin(), h.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) return false;
        const unsigned long v = std::strtoul(h.c_str(), nullptr, 16);
        out = Color3::fromRGB(static_cast<int>((v >> 16) & 255), static_cast<int>((v >> 8) & 255), static_cast<int>(v & 255));
        return true;
    }

    // A colour cell; the current colour gets a ring.
    bool colorCell(const char* id, const Color3& c, bool selected, float size, const char* tip) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 q(p.x + size, p.y + size);
        dl->AddRectFilled(p, q, col(c), 3.0f);
        if (selected) {
            dl->AddRect(ImVec2(p.x - 1.5f, p.y - 1.5f), ImVec2(q.x + 1.5f, q.y + 1.5f), theme::u32(theme::color::accentHover), 4.0f, 0, 2.0f);
        } else if (ImGui::IsItemHovered()) {
            dl->AddRect(p, q, IM_COL32(255, 255, 255, 170), 3.0f, 0, 1.5f);
        }
        if (tip && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        return pressed;
    }

    // The materials in the order the grid shows them, in groups.
    struct Group { const char* name; std::vector<Material> items; };
    const std::vector<Group>& groups() {
        static const std::vector<Group> g = {
            { "Basic", { Material::Plastic, Material::SmoothPlastic, Material::Neon, Material::Glass, Material::ForceField, Material::Fabric } },
            { "Wood and stone", { Material::Wood, Material::WoodPlanks, Material::Brick, Material::Cobblestone, Material::Concrete, Material::Granite,
                                  Material::Marble, Material::Slate, Material::Pebble } },
            { "Metal", { Material::Metal, Material::DiamondPlate, Material::CorrodedMetal, Material::Foil } },
            { "Nature", { Material::Grass, Material::LeafyGrass, Material::Ground, Material::Mud, Material::Sand, Material::Sandstone, Material::Rock,
                          Material::Basalt, Material::Limestone, Material::CrackedLava, Material::Salt, Material::Snow, Material::Glacier, Material::Ice,
                          Material::Asphalt, Material::Pavement } },
        };
        return g;
    }

    std::string prettyName(Material m) {
        const auto& names = enumNamesFor("Material");
        const size_t i = static_cast<size_t>(m);
        if (i >= names.size()) return "?";
        std::string out;
        for (size_t k = 0; k < names[i].size(); ++k) {
            const char ch = names[i][k];
            if (k > 0 && std::isupper(static_cast<unsigned char>(ch)) && std::islower(static_cast<unsigned char>(names[i][k - 1]))) out += ' ';
            out += ch;
        }
        return out;
    }

    // A small deterministic noise for speckled swatches.
    float hash(int x, int y, int seed) {
        unsigned h = static_cast<unsigned>(x * 374761393 + y * 668265263 + seed * 2147483647u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<float>((h ^ (h >> 16)) & 1023) / 1023.0f;
    }
}

// ============================================================================
//  Colours
// ============================================================================
void rememberColor(EditorSettings& settings, const Color3& c) {
    const std::string hex = hexOf(c);
    auto& recent = settings.recentColors;
    recent.erase(std::remove(recent.begin(), recent.end(), hex), recent.end());
    recent.insert(recent.begin(), hex);
    if (recent.size() > 16) recent.resize(16);
}

int colorPalette(Color3& color, EditorSettings& settings) {
    int picked = 0;
    const float cell = std::floor(ImGui::GetFontSize() * 1.45f);
    const float gap = 3.0f;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
    const int current = nearestBrickColor(color);
    const bool exact = brickColors()[static_cast<size_t>(current)].color == color;
    ImGui::TextColored(theme::color::textDim, "BrickColors");
    const auto& table = brickColors();
    for (size_t i = 0; i < table.size(); ++i) {
        if (i % 8 != 0) ImGui::SameLine();
        ImGui::PushID(static_cast<int>(i));
        if (colorCell("##c", table[i].color, exact && static_cast<int>(i) == current, cell, table[i].name)) {
            color = table[i].color;
            picked = 1;
        }
        ImGui::PopID();
    }
    if (!settings.recentColors.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(theme::color::textDim, "Recent");
        int n = 0;
        for (const std::string& hex : settings.recentColors) {
            Color3 c;
            if (!parseHex(hex, c)) continue;
            if (n % 8 != 0) ImGui::SameLine();
            ImGui::PushID(1000 + n);
            if (colorCell("##r", c, c == color, cell, hex.c_str())) { color = c; picked = 1; }
            ImGui::PopID();
            if (++n >= 16) break;
        }
    }
    ImGui::PopStyleVar();
    // Anything else: a picker and a hex field.
    ImGui::Spacing();
    ImGui::Separator();
    float rgb[3] = { color.r, color.g, color.b };
    const float width = (cell + gap) * 8.0f - gap;
    ImGui::SetNextItemWidth(width);
    if (ImGui::ColorPicker3("##custom", rgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_NoInputs)) {
        color = Color3(rgb[0], rgb[1], rgb[2]);
        picked = 2;
    }
    std::string hex = hexOf(color);
    ImGui::SetNextItemWidth(width * 0.5f);
    if (ImGui::InputText("##hex", &hex, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase)) {
        Color3 c;
        if (parseHex(hex, c)) { color = c; picked = 1; }
    }
    ImGui::SameLine();
    ImGui::TextColored(theme::color::textDim, exact ? "%s" : "near %s", table[static_cast<size_t>(current)].name);
    return picked;
}

// ============================================================================
//  Materials
// ============================================================================
void drawMaterialSwatch(ImDrawList* dl, Material m, const ImVec2& a, const ImVec2& b, float rounding) {
    const float w = b.x - a.x, h = b.y - a.y;
    Color3 base(0.64f, 0.64f, 0.64f);
    switch (m) {
        case Material::Plastic: base = Color3::fromRGB(163, 162, 165); break;
        case Material::SmoothPlastic: base = Color3::fromRGB(175, 178, 184); break;
        case Material::Metal: base = Color3::fromRGB(150, 156, 166); break;
        case Material::Neon: base = Color3::fromRGB(255, 70, 200); break;
        case Material::Glass: base = Color3::fromRGB(150, 200, 230); break;
        case Material::Wood: base = Color3::fromRGB(150, 103, 60); break;
        case Material::WoodPlanks: base = Color3::fromRGB(166, 116, 72); break;
        case Material::Concrete: base = Color3::fromRGB(150, 150, 146); break;
        case Material::Brick: base = Color3::fromRGB(158, 72, 52); break;
        case Material::Cobblestone: base = Color3::fromRGB(128, 126, 120); break;
        case Material::Granite: base = Color3::fromRGB(160, 140, 136); break;
        case Material::Marble: base = Color3::fromRGB(228, 226, 220); break;
        case Material::Slate: base = Color3::fromRGB(86, 90, 96); break;
        case Material::Fabric: base = Color3::fromRGB(120, 110, 150); break;
        case Material::Foil: base = Color3::fromRGB(196, 198, 205); break;
        case Material::DiamondPlate: base = Color3::fromRGB(140, 146, 154); break;
        case Material::CorrodedMetal: base = Color3::fromRGB(110, 120, 96); break;
        case Material::Pebble: base = Color3::fromRGB(150, 140, 124); break;
        case Material::Ice: base = Color3::fromRGB(196, 228, 245); break;
        case Material::ForceField: base = Color3::fromRGB(70, 200, 255); break;
        case Material::Water: base = Color3::fromRGB(40, 120, 190); break;
        default: base = TerrainData::defaultMaterialColor(m); break;
    }
    dl->AddRectFilled(a, b, col(base), rounding);
    dl->PushClipRect(a, b, true);
    const ImU32 dark = col(scaled(base, 0.72f)), light = col(scaled(base, 1.25f));
    switch (m) {
        case Material::SmoothPlastic:
        case Material::Glass:
        case Material::Ice:
            dl->AddRectFilledMultiColor(a, ImVec2(b.x, a.y + h * 0.5f), IM_COL32(255, 255, 255, 70), IM_COL32(255, 255, 255, 40), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 0));
            dl->AddLine(ImVec2(a.x + w * 0.2f, b.y - h * 0.15f), ImVec2(b.x - w * 0.15f, a.y + h * 0.2f), IM_COL32(255, 255, 255, 90), 1.5f);
            break;
        case Material::Metal:
        case Material::Foil:
            dl->AddRectFilledMultiColor(a, b, light, dark, light, dark);
            if (m == Material::Foil)
                for (int k = 0; k < 6; ++k) dl->AddLine(ImVec2(a.x + w * hash(k, 1, 3), a.y), ImVec2(a.x + w * hash(k, 2, 3), b.y), IM_COL32(255, 255, 255, 50), 1.0f);
            break;
        case Material::Neon:
            dl->AddRectFilled(ImVec2(a.x + w * 0.2f, a.y + h * 0.2f), ImVec2(b.x - w * 0.2f, b.y - h * 0.2f), IM_COL32(255, 230, 250, 200), rounding);
            break;
        case Material::Wood:
        case Material::WoodPlanks:
            for (int k = 1; k < 7; ++k) {
                const float y = a.y + h * k / 7.0f + (hash(k, 0, 7) - 0.5f) * 2.0f;
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y + (hash(k, 1, 7) - 0.5f) * 3.0f), dark, m == Material::WoodPlanks && k % 2 == 0 ? 1.6f : 0.8f);
            }
            if (m == Material::WoodPlanks)
                for (int k = 0; k < 3; ++k) dl->AddLine(ImVec2(a.x + w * (0.25f + 0.3f * k), a.y + h * (k / 3.0f)), ImVec2(a.x + w * (0.25f + 0.3f * k), a.y + h * ((k + 1) / 3.0f)), dark, 1.2f);
            break;
        case Material::Brick:
            for (int row = 0; row < 4; ++row) {
                const float y = a.y + h * row / 4.0f;
                dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col(230, 220, 205, 160), 1.2f);
                for (int k = 0; k < 3; ++k) {
                    const float x = a.x + w * (k / 2.0f) + (row % 2 ? w * 0.25f : 0.0f);
                    dl->AddLine(ImVec2(x, y), ImVec2(x, y + h / 4.0f), col(230, 220, 205, 160), 1.2f);
                }
            }
            break;
        case Material::Cobblestone:
        case Material::Pebble: {
            const int n = m == Material::Pebble ? 9 : 5;
            for (int k = 0; k < n; ++k) {
                const ImVec2 c(a.x + w * hash(k, 3, 11), a.y + h * hash(k, 4, 11));
                dl->AddCircleFilled(c, w * (m == Material::Pebble ? 0.08f : 0.16f), hash(k, 5, 11) > 0.5f ? light : dark);
            }
            break;
        }
        case Material::Concrete:
        case Material::Granite:
        case Material::Asphalt:
        case Material::Pavement:
        case Material::Sand:
        case Material::Salt:
            for (int k = 0; k < 28; ++k) {
                const ImVec2 c(a.x + w * hash(k, 6, 13), a.y + h * hash(k, 7, 13));
                dl->AddRectFilled(c, ImVec2(c.x + 1.5f, c.y + 1.5f), hash(k, 8, 13) > 0.5f ? light : dark);
            }
            break;
        case Material::Marble:
            dl->AddBezierCubic(ImVec2(a.x, a.y + h * 0.3f), ImVec2(a.x + w * 0.4f, a.y + h * 0.1f), ImVec2(a.x + w * 0.5f, a.y + h * 0.8f), ImVec2(b.x, a.y + h * 0.6f), col(150, 150, 160, 160), 1.2f);
            break;
        case Material::Slate:
        case Material::Limestone:
        case Material::Sandstone:
            for (int k = 1; k < 5; ++k) dl->AddLine(ImVec2(a.x, a.y + h * k / 5.0f), ImVec2(b.x, a.y + h * k / 5.0f + 2.0f), dark, 1.2f);
            break;
        case Material::Fabric:
            for (int k = 0; k < 8; ++k) {
                dl->AddLine(ImVec2(a.x + w * k / 8.0f, a.y), ImVec2(a.x + w * k / 8.0f, b.y), dark, 0.7f);
                dl->AddLine(ImVec2(a.x, a.y + h * k / 8.0f), ImVec2(b.x, a.y + h * k / 8.0f), light, 0.7f);
            }
            break;
        case Material::DiamondPlate:
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x) {
                    const ImVec2 c(a.x + w * (x + 0.5f + (y % 2) * 0.5f) / 3.0f, a.y + h * (y + 0.5f) / 3.0f);
                    dl->AddLine(ImVec2(c.x - w * 0.08f, c.y + h * 0.06f), ImVec2(c.x + w * 0.08f, c.y - h * 0.06f), light, 2.0f);
                }
            break;
        case Material::CorrodedMetal:
            for (int k = 0; k < 5; ++k) dl->AddCircleFilled(ImVec2(a.x + w * hash(k, 9, 17), a.y + h * hash(k, 10, 17)), w * 0.14f, col(150, 95, 55, 170));
            break;
        case Material::ForceField:
            for (int k = 0; k < 3; ++k) dl->AddNgon(ImVec2(a.x + w * (0.25f + 0.25f * k), a.y + h * 0.5f), w * 0.14f, IM_COL32(220, 250, 255, 200), 6, 1.2f);
            break;
        case Material::Grass:
        case Material::LeafyGrass:
            for (int k = 0; k < 14; ++k) {
                const float x = a.x + w * hash(k, 11, 19), y = a.y + h * (0.3f + 0.7f * hash(k, 12, 19));
                dl->AddLine(ImVec2(x, y), ImVec2(x + 1.5f, y - h * 0.18f), light, 1.0f);
            }
            break;
        case Material::CrackedLava:
            dl->AddLine(ImVec2(a.x, a.y + h * 0.4f), ImVec2(a.x + w * 0.5f, a.y + h * 0.55f), col(255, 140, 40), 1.8f);
            dl->AddLine(ImVec2(a.x + w * 0.5f, a.y + h * 0.55f), ImVec2(b.x, a.y + h * 0.3f), col(255, 140, 40), 1.8f);
            dl->AddLine(ImVec2(a.x + w * 0.5f, a.y + h * 0.55f), ImVec2(a.x + w * 0.6f, b.y), col(255, 140, 40), 1.8f);
            break;
        case Material::Snow:
        case Material::Glacier:
            dl->AddRectFilledMultiColor(a, b, IM_COL32(255, 255, 255, 60), IM_COL32(255, 255, 255, 20), IM_COL32(255, 255, 255, 0), IM_COL32(255, 255, 255, 40));
            break;
        default:
            break;
    }
    dl->PopClipRect();
    dl->AddRect(a, b, IM_COL32(0, 0, 0, 60), rounding);
}

bool materialButton(const char* id, Material material, bool mixed, float width) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, h));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), r);
    const float pad = 3.0f;
    if (!mixed) drawMaterialSwatch(dl, material, ImVec2(p.x + pad, p.y + pad), ImVec2(p.x + h - pad, p.y + h - pad), 2.0f);
    const std::string name = mixed ? std::string("--") : prettyName(material);
    const ImGuiStyle& style = ImGui::GetStyle();
    dl->AddText(ImVec2(p.x + h + style.ItemInnerSpacing.x, p.y + style.FramePadding.y), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
    // A combo-style arrow.
    const float arrow = ImGui::GetFontSize() * 0.3f;
    const ImVec2 c(p.x + width - h * 0.5f, p.y + h * 0.5f);
    dl->AddTriangleFilled(ImVec2(c.x - arrow, c.y - arrow * 0.5f), ImVec2(c.x + arrow, c.y - arrow * 0.5f), ImVec2(c.x, c.y + arrow * 0.6f), ImGui::GetColorU32(ImGuiCol_Text));
    return pressed;
}

bool materialPalette(Material& material) {
    static std::string filter;
    bool picked = false;
    const float tile = std::floor(ImGui::GetFontSize() * 3.4f);
    const int perRow = 6;
    const float gap = 4.0f;
    const float width = (tile + gap) * perRow - gap;
    ImGui::SetNextItemWidth(width);
    if (ImGui::IsWindowAppearing()) {
        filter.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Search materials", &filter);
    auto matches = [&](Material m) {
        if (filter.empty()) return true;
        std::string a = prettyName(m), f = filter;
        std::transform(a.begin(), a.end(), a.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        std::transform(f.begin(), f.end(), f.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return a.find(f) != std::string::npos;
    };
    ImFont* font = ImGui::GetFont();
    const float small = ImGui::GetFontSize() * 0.8f;
    for (const Group& g : groups()) {
        std::vector<Material> shown;
        for (Material m : g.items) if (matches(m)) shown.push_back(m);
        if (shown.empty()) continue;
        ImGui::Spacing();
        ImGui::TextColored(theme::color::textDim, "%s", g.name);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, gap));
        for (size_t k = 0; k < shown.size(); ++k) {
            const Material m = shown[k];
            if (k % perRow != 0) ImGui::SameLine();
            ImGui::PushID(static_cast<int>(m));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float labelH = small + 4.0f;
            if (ImGui::InvisibleButton("##m", ImVec2(tile, tile + labelH))) { material = m; picked = true; }
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            drawMaterialSwatch(dl, m, p, ImVec2(p.x + tile, p.y + tile), 4.0f);
            if (m == material) dl->AddRect(ImVec2(p.x - 2.0f, p.y - 2.0f), ImVec2(p.x + tile + 2.0f, p.y + tile + 2.0f), theme::u32(theme::color::accentHover), 5.0f, 0, 2.0f);
            else if (hovered) dl->AddRect(p, ImVec2(p.x + tile, p.y + tile), IM_COL32(255, 255, 255, 170), 4.0f, 0, 1.5f);
            // The name under it, cut to fit.
            std::string name = prettyName(m);
            while (name.size() > 3 && font->CalcTextSizeA(small, FLT_MAX, 0.0f, name.c_str()).x > tile) name = name.substr(0, name.size() - 2) + ".";
            const ImVec2 ts = font->CalcTextSizeA(small, FLT_MAX, 0.0f, name.c_str());
            dl->AddText(font, small, ImVec2(p.x + (tile - ts.x) * 0.5f, p.y + tile + 2.0f), theme::u32(m == material ? theme::color::text : theme::color::textDim), name.c_str());
            if (hovered) ImGui::SetTooltip("%s", prettyName(m).c_str());
            ImGui::PopID();
        }
        ImGui::PopStyleVar();
    }
    return picked;
}

} // namespace rift
