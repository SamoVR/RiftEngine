#pragma once
//
// Palettes.h -- the colour and material pickers: Roblox's BrickColor palette
// with the colours used last and a custom picker, and a grid of materials
// with small previews. Used by the Properties panel and the toolbar's Color
// and Material buttons. Each draws popup content and returns true on a pick.
//
#include "editor/EditorSettings.h"
#include "scene/Instance.h"

#include <imgui.h>

namespace rift {

// 0: nothing, 1: a colour was clicked (close the popup), 2: the custom picker is being dragged.
int colorPalette(Color3& color, EditorSettings& settings);
void rememberColor(EditorSettings& settings, const Color3& color);

bool materialPalette(Material& material);
// A material's little preview (its usual colour and a hint of its pattern).
void drawMaterialSwatch(ImDrawList* dl, Material material, const ImVec2& a, const ImVec2& b, float rounding);
// A combo-like button: the material's swatch and name. True when clicked.
bool materialButton(const char* id, Material material, bool mixed, float width);

} // namespace rift
