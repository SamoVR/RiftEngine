#include "editor/Theme.h"
#include "core/Paths.h"
#include "core/Log.h"

#include "IconsFontAwesome6.h"
#include <imgui_internal.h>
#include <algorithm>
#include <string>

namespace rift::theme {

namespace {
    Fonts g_fonts;
    float g_scale = 1.0f;
    bool g_fontsLoaded = false;

    void loadFonts() {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        const std::string inter = Paths::toUtf8(Paths::font("Inter-Regular.ttf"));
        const std::string interMedium = Paths::toUtf8(Paths::font("Inter-Medium.ttf"));
        const std::string interSemi = Paths::toUtf8(Paths::font("Inter-SemiBold.ttf"));
        const std::string mono = Paths::toUtf8(Paths::font("JetBrainsMono-Regular.ttf"));
        const std::string icons = Paths::toUtf8(Paths::font("fa-solid-900.ttf"));

        ImFontConfig base;
        base.OversampleH = 2;
        auto add = [&](const std::string& file) -> ImFont* {
            ImFont* f = io.Fonts->AddFontFromFileTTF(file.c_str(), kBaseFontSize, &base);
            if (!f) {
                LOG_WARN("Theme", "Missing font " + file + "; using the built-in one");
                f = io.Fonts->AddFontDefault();
            }
            // Icons merged into every UI font, a touch smaller and centred.
            ImFontConfig ic;
            ic.MergeMode = true;
            ic.PixelSnapH = true;
            ic.GlyphMinAdvanceX = kBaseFontSize * 1.15f;
            ic.GlyphOffset = ImVec2(0.0f, 0.5f);
            io.Fonts->AddFontFromFileTTF(icons.c_str(), kBaseFontSize * 0.9f, &ic);
            return f;
        };
        g_fonts.ui = add(inter);
        g_fonts.medium = add(interMedium);
        g_fonts.heading = add(interSemi);
        ImFontConfig monoConfig;
        g_fonts.mono = io.Fonts->AddFontFromFileTTF(mono.c_str(), kBaseFontSize, &monoConfig);
        if (!g_fonts.mono) g_fonts.mono = g_fonts.ui;
        io.FontDefault = g_fonts.ui;
        g_fontsLoaded = true;
    }
}

Fonts& fonts() { return g_fonts; }
float scale() { return g_scale; }

ImU32 u32(const ImVec4& c, float alphaScale) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alphaScale)); }

void apply(float s) {
    if (!g_fontsLoaded) loadFonts();
    g_scale = s;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);

    st.WindowPadding = ImVec2(10, 10);
    st.FramePadding = ImVec2(8, 5);
    st.CellPadding = ImVec2(6, 4);
    st.ItemSpacing = ImVec2(8, 6);
    st.ItemInnerSpacing = ImVec2(6, 4);
    st.IndentSpacing = 16;
    st.ScrollbarSize = 12;
    st.GrabMinSize = 10;
    st.WindowBorderSize = 1;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.WindowRounding = 7;
    st.ChildRounding = 6;
    st.FrameRounding = 5;
    st.PopupRounding = 7;
    st.ScrollbarRounding = 8;
    st.GrabRounding = 4;
    st.TabRounding = 5;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.WindowMenuButtonPosition = ImGuiDir_None;
    st.SeparatorTextBorderSize = 1;
    st.DockingSeparatorSize = 2;
    st.ScaleAllSizes(s);
    st.FontScaleDpi = s;
    st.FontSizeBase = kBaseFontSize;

    using namespace color;
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = textDim;
    c[ImGuiCol_WindowBg] = panel;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.118f, 0.122f, 0.145f, 0.98f);
    c[ImGuiCol_Border] = border;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = frame;
    c[ImGuiCol_FrameBgHovered] = frameHover;
    c[ImGuiCol_FrameBgActive] = ImVec4(0.231f, 0.243f, 0.282f, 1.0f);
    c[ImGuiCol_TitleBg] = bgDark;
    c[ImGuiCol_TitleBgActive] = bgDark;
    c[ImGuiCol_TitleBgCollapsed] = bgDark;
    c[ImGuiCol_MenuBarBg] = bgDark;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.26f, 0.27f, 0.31f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.33f, 0.34f, 0.39f, 1.0f);
    c[ImGuiCol_ScrollbarGrabActive] = accent;
    c[ImGuiCol_CheckMark] = accentHover;
    c[ImGuiCol_SliderGrab] = accent;
    c[ImGuiCol_SliderGrabActive] = accentHover;
    c[ImGuiCol_Button] = frame;
    c[ImGuiCol_ButtonHovered] = frameHover;
    c[ImGuiCol_ButtonActive] = ImVec4(accent.x, accent.y, accent.z, 0.6f);
    c[ImGuiCol_Header] = ImVec4(accent.x, accent.y, accent.z, 0.30f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 1.0f, 1.0f, 0.06f);
    c[ImGuiCol_HeaderActive] = ImVec4(accent.x, accent.y, accent.z, 0.45f);
    c[ImGuiCol_Separator] = border;
    c[ImGuiCol_SeparatorHovered] = accent;
    c[ImGuiCol_SeparatorActive] = accentHover;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    c[ImGuiCol_ResizeGripActive] = accent;
    c[ImGuiCol_Tab] = bg;
    c[ImGuiCol_TabHovered] = frameHover;
    c[ImGuiCol_TabSelected] = panel;
    c[ImGuiCol_TabSelectedOverline] = accent;
    c[ImGuiCol_TabDimmed] = bg;
    c[ImGuiCol_TabDimmedSelected] = panel;
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(accent.x, accent.y, accent.z, 0.4f);
    c[ImGuiCol_DockingPreview] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DockingEmptyBg] = bgDark;
    c[ImGuiCol_TableHeaderBg] = bg;
    c[ImGuiCol_TableBorderStrong] = border;
    c[ImGuiCol_TableBorderLight] = ImVec4(border.x, border.y, border.z, 0.6f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.02f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(accent.x, accent.y, accent.z, 0.35f);
    c[ImGuiCol_DragDropTarget] = accentHover;
    c[ImGuiCol_NavCursor] = accent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0.02f, 0.02f, 0.04f, 0.55f);
}

const char* classIcon(ClassId cls) {
    switch (cls) {
        case ClassId::Workspace: return ICON_FA_EARTH_AMERICAS;
        case ClassId::Players: return ICON_FA_USERS;
        case ClassId::Lighting: return ICON_FA_SUN;
        case ClassId::ReplicatedStorage: return ICON_FA_BOX_ARCHIVE;
        case ClassId::ScriptService: return ICON_FA_SCROLL;
        case ClassId::StarterGui: return ICON_FA_TABLE_COLUMNS;
        case ClassId::StarterPlayer: return ICON_FA_PERSON;
        case ClassId::SoundService: return ICON_FA_MUSIC;
        case ClassId::Part: return ICON_FA_CUBE;
        case ClassId::SpawnLocation: return ICON_FA_LOCATION_DOT;
        case ClassId::MeshPart: return ICON_FA_SHAPES;
        case ClassId::Model: return ICON_FA_CUBES;
        case ClassId::Folder: return ICON_FA_FOLDER;
        case ClassId::PointLight: return ICON_FA_LIGHTBULB;
        case ClassId::SpotLight: return ICON_FA_LIGHTBULB;
        case ClassId::Sound: return ICON_FA_VOLUME_HIGH;
        case ClassId::Humanoid: return ICON_FA_PERSON_WALKING;
        case ClassId::Player: return ICON_FA_USER;
        case ClassId::Script: return ICON_FA_FILE_CODE;
        case ClassId::ModuleScript: return ICON_FA_CUBES_STACKED;
        case ClassId::ScreenGui: return ICON_FA_DESKTOP;
        case ClassId::Frame: return ICON_FA_SQUARE;
        case ClassId::TextLabel: return ICON_FA_FONT;
        case ClassId::TextButton: return ICON_FA_HAND_POINTER;
        case ClassId::ImageLabel: return ICON_FA_IMAGE;
        case ClassId::ImageButton: return ICON_FA_IMAGE;
        case ClassId::IntValue: case ClassId::NumberValue: return ICON_FA_HASHTAG;
        case ClassId::StringValue: return ICON_FA_QUOTE_LEFT;
        case ClassId::BoolValue: return ICON_FA_TOGGLE_ON;
        case ClassId::Vector3Value: return ICON_FA_UP_DOWN_LEFT_RIGHT;
        case ClassId::Color3Value: return ICON_FA_PALETTE;
        default: return ICON_FA_CIRCLE;
    }
}

ImVec4 classColor(ClassId cls) {
    switch (cls) {
        case ClassId::Workspace: return { 0.40f, 0.70f, 1.00f, 1.0f };
        case ClassId::Lighting: return { 1.00f, 0.80f, 0.30f, 1.0f };
        case ClassId::Part: case ClassId::MeshPart: return { 0.62f, 0.68f, 0.80f, 1.0f };
        case ClassId::SpawnLocation: return { 0.35f, 0.85f, 0.50f, 1.0f };
        case ClassId::Model: return { 0.55f, 0.75f, 1.00f, 1.0f };
        case ClassId::Folder: return { 0.95f, 0.75f, 0.35f, 1.0f };
        case ClassId::PointLight: case ClassId::SpotLight: return { 1.00f, 0.88f, 0.45f, 1.0f };
        case ClassId::Sound: case ClassId::SoundService: return { 0.40f, 0.85f, 0.85f, 1.0f };
        case ClassId::Script: case ClassId::ScriptService: return { 0.62f, 0.55f, 0.98f, 1.0f };
        case ClassId::ModuleScript: return { 0.85f, 0.50f, 0.95f, 1.0f };
        case ClassId::ScreenGui: case ClassId::StarterGui: case ClassId::Frame: case ClassId::TextLabel:
        case ClassId::TextButton: case ClassId::ImageLabel: case ClassId::ImageButton: return { 0.40f, 0.82f, 0.75f, 1.0f };
        case ClassId::Humanoid: case ClassId::Player: case ClassId::Players: case ClassId::StarterPlayer: return { 0.95f, 0.65f, 0.55f, 1.0f };
        case ClassId::ReplicatedStorage: return { 0.75f, 0.65f, 0.55f, 1.0f };
        default: return { 0.70f, 0.72f, 0.78f, 1.0f };
    }
}

bool iconButton(const char* icon, const char* tip, bool active, float size) {
    const float s = size > 0.0f ? size : ImGui::GetFrameHeight();
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(color::accent.x, color::accent.y, color::accent.z, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color::accentHover);
    }
    const bool pressed = ImGui::Button(icon, ImVec2(s, s));
    if (active) ImGui::PopStyleColor(2);
    if (tip) tooltip(tip);
    return pressed;
}

void iconText(const char* icon, const ImVec4& iconColor, const char* text) {
    ImGui::TextColored(iconColor, "%s", icon);
    ImGui::SameLine(0.0f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::TextUnformatted(text);
}

bool menuItem(const char* icon, const ImVec4& iconColor, const char* label, const char* shortcut, bool selected, bool enabled) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float iconWidth = ImGui::GetFontSize() * 1.6f;
    // Spaces leave room for the icon; it is drawn over them afterwards.
    std::string text;
    const float space = std::max(ImGui::CalcTextSize(" ").x, 1.0f);
    text.assign(static_cast<size_t>(iconWidth / space + 0.5f), ' ');
    text += label;
    const bool pressed = ImGui::MenuItem(text.c_str(), shortcut, selected, enabled);
    ImGui::GetWindowDrawList()->AddText(pos, u32(iconColor, enabled ? 1.0f : 0.4f), icon);
    return pressed;
}

void tooltip(const char* text) {
    if (text && *text && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(ImGui::GetFontSize() * 26.0f, FLT_MAX));
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 25.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void heading(const char* text, float width) {
    ImGui::PushFont(g_fonts.heading, 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y), ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(w, ImGui::GetStyle().ItemSpacing.y * 0.5f));
}

void pushHeading(float sizeMultiplier) {
    ImGui::PushFont(g_fonts.heading, kBaseFontSize * sizeMultiplier);
}

void pop() { ImGui::PopFont(); }

} // namespace rift::theme
