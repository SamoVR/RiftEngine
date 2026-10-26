#pragma once
//
// Theme.h -- RIFT's look: graphite panels, a violet accent, Inter for the
// interface and JetBrains Mono for code, Font Awesome icons, and small
// widget helpers shared by the panels.
//
#include "scene/Instance.h"

#include <imgui.h>
#include <string>

namespace rift::theme {

struct Fonts {
    ImFont* ui = nullptr;       // Inter Regular (+ icons)
    ImFont* medium = nullptr;   // Inter Medium (+ icons)
    ImFont* heading = nullptr;  // Inter SemiBold (+ icons)
    ImFont* mono = nullptr;     // JetBrains Mono
};
Fonts& fonts();

constexpr float kBaseFontSize = 15.0f;

// Loads the fonts (once) and applies colours and sizes for the given scale
// (monitor DPI x the user's UI scale). Safe to call again when the scale changes.
void apply(float scale);
float scale();

// ---- palette
namespace color {
    // The accent follows Preferences > Appearance (setAccent).
    inline ImVec4 accent{ 0.545f, 0.361f, 0.965f, 1.0f };       // #8B5CF6
    inline ImVec4 accentHover{ 0.616f, 0.455f, 0.973f, 1.0f };
    inline ImVec4 accentActive{ 0.486f, 0.302f, 0.859f, 1.0f };
    inline const ImVec4 text{ 0.894f, 0.894f, 0.914f, 1.0f };
    inline const ImVec4 textDim{ 0.553f, 0.561f, 0.600f, 1.0f };
    inline const ImVec4 success{ 0.243f, 0.812f, 0.557f, 1.0f };
    inline const ImVec4 warning{ 0.961f, 0.647f, 0.141f, 1.0f };
    inline const ImVec4 error{ 0.941f, 0.322f, 0.322f, 1.0f };
    inline const ImVec4 bgDark{ 0.078f, 0.082f, 0.098f, 1.0f };
    inline const ImVec4 bg{ 0.102f, 0.106f, 0.125f, 1.0f };
    inline const ImVec4 panel{ 0.129f, 0.133f, 0.157f, 1.0f };
    inline const ImVec4 frame{ 0.165f, 0.173f, 0.200f, 1.0f };
    inline const ImVec4 frameHover{ 0.200f, 0.212f, 0.243f, 1.0f };
    inline const ImVec4 border{ 0.173f, 0.180f, 0.208f, 1.0f };
}
ImU32 u32(const ImVec4& c, float alphaScale = 1.0f);

// Accent colour presets (Violet, Blue, Teal, Green, Orange, Pink, Red).
int accentCount();
const char* accentName(int index);
ImVec4 accentColor(int index);
// Switches the accent and restyles (at the current scale).
void setAccent(int index);
// Hover tooltips on / off (Preferences).
void setTooltipsEnabled(bool enabled);

// ---- instance classes
const char* classIcon(ClassId cls);
ImVec4 classColor(ClassId cls);

// ---- widgets
// A square icon button; `active` draws it in the accent colour.
bool iconButton(const char* icon, const char* tooltip, bool active = false, float size = 0.0f);
// Text with a coloured icon in front (Explorer rows, headers).
void iconText(const char* icon, const ImVec4& iconColor, const char* text);
// Menu item with a coloured icon in front of the label.
bool menuItem(const char* icon, const ImVec4& iconColor, const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true);
// Submenu with a coloured icon, lined up with menuItem rows. EndMenu() when true.
bool beginMenu(const char* icon, const ImVec4& iconColor, const char* label, bool enabled = true);
// Tooltip for the last item, after a short hover.
void tooltip(const char* text);
// A collapsible section header in graphite, exactly as wide as the content
// (unlike ImGui's, which pokes into the window padding). "Label###id" works.
bool sectionHeader(const char* label, bool defaultOpen = true, const ImVec4* textColor = nullptr);
// Bold section title with a thin line under it (`width` 0: to the edge of the window).
void heading(const char* text, float width = 0.0f);
// Pushes the heading font at a given size multiplier.
void pushHeading(float sizeMultiplier = 1.15f);
void pop();

} // namespace rift::theme
