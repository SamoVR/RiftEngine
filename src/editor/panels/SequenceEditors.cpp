// Editors for the effect property types: NumberRange (two fields),
// NumberSequence (a curve with draggable points), ColorSequence (a gradient
// with colour stops) and instance references (an Attachment picker).
#include "editor/panels/SequenceEditors.h"
#include "editor/Theme.h"

#include "IconsFontAwesome6.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rift {

namespace {
    ImU32 colorU32(const Color3& c, float a = 1.0f) { return ImGui::GetColorU32(ImVec4(c.r, c.g, c.b, a)); }

    // A gradient over a rectangle, one band per pair of colour stops.
    void drawGradient(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const ColorSequence& seq, float rounding) {
        // Checkerboard-free: colours are opaque. Rounded ends by clipping a rounded rect behind.
        dl->AddRectFilled(a, b, colorU32(seq.at(0.0f)), rounding);
        const float w = b.x - a.x;
        for (size_t k = 0; k + 1 < seq.keys.size(); ++k) {
            const float x0 = a.x + seq.keys[k].time * w, x1 = a.x + seq.keys[k + 1].time * w;
            if (x1 - x0 < 0.5f) continue;
            const float inset0 = k == 0 ? rounding : 0.0f, inset1 = k + 2 == seq.keys.size() ? rounding : 0.0f;
            const ImU32 c0 = colorU32(seq.keys[k].color), c1 = colorU32(seq.keys[k + 1].color);
            dl->AddRectFilledMultiColor(ImVec2(std::max(x0, a.x + inset0 * 0.5f), a.y), ImVec2(std::min(x1, b.x - inset1 * 0.5f), b.y), c0, c1, c1, c0);
        }
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), rounding);
    }

    // The value range a curve is drawn over.
    void curveRange(const NumberSequence& seq, float lo, float hi, bool bounded, float& bottom, float& top) {
        if (bounded && hi - lo <= 2.0f) { bottom = lo; top = hi; return; } // Transparency: always 0..1
        float mn = 0.0f, mx = 1.0f;
        for (const NumberKeypoint& k : seq.keys) {
            mn = std::min(mn, k.value - k.envelope);
            mx = std::max(mx, k.value + k.envelope);
        }
        bottom = mn;
        top = mx + (mx - mn) * 0.2f;
        if (bounded) { bottom = std::max(bottom, lo); top = std::min(top, hi); }
        if (top - bottom < 1e-3f) top = bottom + 1.0f;
    }

    // The curve (and its envelope) inside a rectangle.
    void drawCurve(ImDrawList* dl, const ImVec2& a, const ImVec2& b, const NumberSequence& seq, float bottom, float top, bool handles, int hot, int active) {
        auto px = [&](float t, float v) {
            return ImVec2(a.x + t * (b.x - a.x), b.y - (v - bottom) / (top - bottom) * (b.y - a.y));
        };
        const ImU32 line = theme::u32(theme::color::accentHover);
        // Envelope band.
        bool anyEnvelope = false;
        for (const NumberKeypoint& k : seq.keys) anyEnvelope |= k.envelope > 0.0f;
        if (anyEnvelope) {
            for (size_t k = 0; k + 1 < seq.keys.size(); ++k) {
                const NumberKeypoint& p = seq.keys[k];
                const NumberKeypoint& q = seq.keys[k + 1];
                const ImVec2 quad[4] = { px(p.time, p.value + p.envelope), px(q.time, q.value + q.envelope), px(q.time, q.value - q.envelope), px(p.time, p.value - p.envelope) };
                dl->AddConvexPolyFilled(quad, 4, theme::u32(theme::color::accent, 0.18f));
            }
        }
        std::vector<ImVec2> pts;
        for (const NumberKeypoint& k : seq.keys) pts.push_back(px(k.time, k.value));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), line, 0, handles ? 2.0f : 1.5f);
        if (!handles) return;
        for (size_t k = 0; k < pts.size(); ++k) {
            const bool on = static_cast<int>(k) == active || static_cast<int>(k) == hot;
            dl->AddCircleFilled(pts[k], on ? 6.0f : 4.5f, on ? IM_COL32(255, 255, 255, 255) : line);
            dl->AddCircle(pts[k], on ? 6.0f : 4.5f, IM_COL32(20, 20, 26, 255), 0, 1.5f);
        }
    }

    // Shared by both sequence editors: which keypoint is dragged.
    int g_dragKey = -1;
    int g_selectedStop = 0;
}

// ============================================================================
//  NumberRange
// ============================================================================
bool numberRangeField(const char* id, NumberRange& r, float speed, float lo, float hi) {
    ImGui::PushID(id);
    const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
    const float width = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
    bool changed = false;
    ImGui::SetNextItemWidth(width);
    if (ImGui::DragFloat("##min", &r.min, speed, lo, hi, "Min %.3g", ImGuiSliderFlags_NoRoundToFormat)) {
        r.max = std::max(r.max, r.min);
        changed = true;
    }
    ImGui::SameLine(0.0f, spacing);
    ImGui::SetNextItemWidth(width);
    if (ImGui::DragFloat("##max", &r.max, speed, lo, hi, "Max %.3g", ImGuiSliderFlags_NoRoundToFormat)) {
        r.min = std::min(r.min, r.max);
        changed = true;
    }
    ImGui::PopID();
    return changed;
}

// ============================================================================
//  NumberSequence
// ============================================================================
bool numberSequencePreview(const char* id, const NumberSequence& seq, float lo, float hi, bool bounded) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const ImVec2 b(a.x + size.x, a.y + size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float r = ImGui::GetStyle().FrameRounding;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), r);
    float bottom, top;
    curveRange(seq, lo, hi, bounded, bottom, top);
    const float pad = 4.0f;
    // The curve on the left, the values as text on the right.
    char text[64];
    const bool constant = std::all_of(seq.keys.begin(), seq.keys.end(), [&](const NumberKeypoint& k) { return k.value == seq.keys.front().value && k.envelope == 0.0f; });
    if (constant) std::snprintf(text, sizeof(text), "%g", static_cast<double>(seq.keys.front().value));
    else std::snprintf(text, sizeof(text), "%g " ICON_FA_ARROW_RIGHT " %g", static_cast<double>(seq.keys.front().value), static_cast<double>(seq.keys.back().value));
    const ImVec2 ts = ImGui::CalcTextSize(text);
    const float curveRight = std::max(a.x + pad + 24.0f, b.x - ts.x - pad * 3.0f);
    drawCurve(dl, ImVec2(a.x + pad, a.y + pad), ImVec2(curveRight, b.y - pad), seq, bottom, top, false, -1, -1);
    dl->AddText(ImVec2(b.x - ts.x - pad * 1.5f, a.y + (size.y - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), text);
    theme::tooltip("Click to edit the curve");
    return pressed;
}

bool numberSequenceEditor(NumberSequence& seq, float lo, float hi, bool bounded, bool& discrete) {
    discrete = false;
    bool changed = false;
    float bottom, top;
    curveRange(seq, lo, hi, bounded, bottom, top);
    const float font = ImGui::GetFontSize();
    const ImVec2 size(font * 22.0f, font * 10.0f);
    const float margin = ImGui::CalcTextSize("0000.0").x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 a(origin.x + margin, origin.y + 4.0f), b(a.x + size.x, a.y + size.y);
    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##curve", size);
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(a, b, theme::u32(theme::color::bgDark), 4.0f);
    for (int g = 1; g < 4; ++g) {
        const float x = a.x + (b.x - a.x) * g / 4.0f, y = a.y + (b.y - a.y) * g / 4.0f;
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), theme::u32(theme::color::border));
        dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), theme::u32(theme::color::border));
    }
    // Axis labels.
    char label[32];
    std::snprintf(label, sizeof(label), "%g", static_cast<double>(top));
    dl->AddText(ImVec2(origin.x, a.y - 2.0f), theme::u32(theme::color::textDim), label);
    std::snprintf(label, sizeof(label), "%g", static_cast<double>(bottom));
    dl->AddText(ImVec2(origin.x, b.y - font), theme::u32(theme::color::textDim), label);

    auto px = [&](const NumberKeypoint& k) { return ImVec2(a.x + k.time * size.x, b.y - (k.value - bottom) / (top - bottom) * size.y); };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    for (size_t k = 0; k < seq.keys.size(); ++k) {
        const ImVec2 p = px(seq.keys[k]);
        if ((mouse.x - p.x) * (mouse.x - p.x) + (mouse.y - p.y) * (mouse.y - p.y) < 64.0f) hot = static_cast<int>(k);
    }
    const float mt = std::clamp((mouse.x - a.x) / size.x, 0.0f, 1.0f);
    const float mv = top - std::clamp((mouse.y - a.y) / size.y, 0.0f, 1.0f) * (top - bottom);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot >= 0) {
            g_dragKey = hot;
        } else {
            // A click on the curve's area adds a point there.
            NumberKeypoint k{ mt, mv, 0.0f };
            auto it = std::upper_bound(seq.keys.begin(), seq.keys.end(), k, [](const NumberKeypoint& x, const NumberKeypoint& y) { return x.time < y.time; });
            if (seq.keys.size() < 20 && it != seq.keys.begin() && it != seq.keys.end()) {
                g_dragKey = static_cast<int>(seq.keys.insert(it, k) - seq.keys.begin());
                changed = true;
            }
        }
    }
    if (hovered && hot > 0 && hot + 1 < static_cast<int>(seq.keys.size()) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        seq.keys.erase(seq.keys.begin() + hot);
        hot = -1;
        changed = discrete = true;
    }
    if (g_dragKey >= static_cast<int>(seq.keys.size())) g_dragKey = -1;
    if (g_dragKey >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_dragKey = -1;
        } else {
            NumberKeypoint& k = seq.keys[static_cast<size_t>(g_dragKey)];
            const bool end = g_dragKey == 0 || g_dragKey + 1 == static_cast<int>(seq.keys.size());
            if (!end) {
                const float before = seq.keys[static_cast<size_t>(g_dragKey) - 1].time + 0.001f;
                const float after = seq.keys[static_cast<size_t>(g_dragKey) + 1].time - 0.001f;
                k.time = std::clamp(mt, before, std::max(before, after));
            }
            float v = mv;
            if (bounded) v = std::clamp(v, lo, hi);
            if (ImGui::GetIO().KeyCtrl) v = std::round(v * 10.0f) / 10.0f; // Ctrl: snap to 0.1
            if (v != k.value) k.value = v;
            changed = true;
        }
    }
    drawCurve(dl, a, b, seq, bottom, top, true, hot, g_dragKey);
    if (hot >= 0 && g_dragKey < 0) {
        const NumberKeypoint& k = seq.keys[static_cast<size_t>(hot)];
        ImGui::SetTooltip("time %.2f  value %.3g%s", static_cast<double>(k.time), static_cast<double>(k.value), hot > 0 && hot + 1 < static_cast<int>(seq.keys.size()) ? "\nRight-click to remove" : "");
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, b.y + 6.0f));
    ImGui::TextColored(theme::color::textDim, "Click to add a point, drag to move it, right-click to remove it.");

    // The points as numbers.
    if (ImGui::BeginTable("##keys", 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Envelope", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        int remove = -1;
        for (size_t k = 0; k < seq.keys.size(); ++k) {
            NumberKeypoint& key = seq.keys[k];
            const bool end = k == 0 || k + 1 == seq.keys.size();
            ImGui::PushID(static_cast<int>(k));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::BeginDisabled(end);
            const float before = k > 0 ? seq.keys[k - 1].time + 0.001f : 0.0f, after = k + 1 < seq.keys.size() ? seq.keys[k + 1].time - 0.001f : 1.0f;
            if (ImGui::DragFloat("##t", &key.time, 0.005f, before, after, "%.3f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
            ImGui::EndDisabled();
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##v", &key.value, bounded && hi - lo <= 2.0f ? 0.005f : 0.05f, bounded ? lo : 0.0f, bounded ? hi : 0.0f, "%.3g")) changed = true;
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##e", &key.envelope, 0.005f, 0.0f, bounded ? (hi - lo) * 0.5f : 1000.0f, "%.3g")) changed = true;
            ImGui::TableSetColumnIndex(3);
            ImGui::BeginDisabled(end);
            if (ImGui::Button(ICON_FA_XMARK, ImVec2(ImGui::GetFrameHeight(), 0.0f))) remove = static_cast<int>(k);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (remove > 0) {
            seq.keys.erase(seq.keys.begin() + remove);
            changed = discrete = true;
        }
    }

    // Common shapes.
    ImGui::Spacing();
    ImGui::TextColored(theme::color::textDim, "Presets");
    ImGui::SameLine();
    const float v0 = seq.keys.front().value, v1 = seq.keys.back().value;
    const float big = bounded ? hi : std::max({ v0, v1, 1.0f });
    const float small = bounded ? lo : 0.0f;
    struct Preset { const char* name; NumberSequence seq; };
    const Preset presets[] = {
        { "Constant", NumberSequence(v0) },
        { "Rise", NumberSequence(small, big) },
        { "Fall", NumberSequence(big, small) },
        { "Pulse", NumberSequence(std::vector<NumberKeypoint>{ { 0.0f, small, 0.0f }, { 0.5f, big, 0.0f }, { 1.0f, small, 0.0f } }) },
    };
    for (const Preset& p : presets) {
        if (ImGui::SmallButton(p.name)) { seq = p.seq; changed = discrete = true; }
        ImGui::SameLine();
    }
    ImGui::NewLine();
    return changed;
}

// ============================================================================
//  ColorSequence
// ============================================================================
bool colorSequencePreview(const char* id, const ColorSequence& seq) {
    const ImVec2 size(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton(id, size);
    const float r = ImGui::GetStyle().FrameRounding;
    drawGradient(ImGui::GetWindowDrawList(), a, ImVec2(a.x + size.x, a.y + size.y), seq, r);
    if (ImGui::IsItemHovered()) ImGui::GetWindowDrawList()->AddRect(a, ImVec2(a.x + size.x, a.y + size.y), theme::u32(theme::color::accentHover), r, 0, 1.5f);
    theme::tooltip("Click to edit the colours");
    return pressed;
}

bool colorSequenceEditor(ColorSequence& seq, bool& discrete) {
    discrete = false;
    bool changed = false;
    const float font = ImGui::GetFontSize();
    const float width = font * 22.0f;
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float barH = font * 1.6f, markerH = font * 0.9f;
    const ImVec2 b(a.x + width, a.y + barH);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("##bar", ImVec2(width, barH + markerH + 4.0f));
    const bool hovered = ImGui::IsItemHovered();
    drawGradient(dl, a, b, seq, 4.0f);
    if (g_selectedStop >= static_cast<int>(seq.keys.size())) g_selectedStop = 0;

    // Stops: little markers under the bar.
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    int hot = -1;
    for (size_t k = 0; k < seq.keys.size(); ++k) {
        const float x = a.x + seq.keys[k].time * width;
        if (std::abs(mouse.x - x) < 7.0f && mouse.y > b.y - 2.0f && mouse.y < b.y + markerH + 6.0f) hot = static_cast<int>(k);
    }
    const float mt = std::clamp((mouse.x - a.x) / width, 0.0f, 1.0f);
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hot >= 0) {
            g_selectedStop = g_dragKey = hot;
        } else if (seq.keys.size() < 20) {
            // A click on the bar adds a stop with the colour that is there.
            ColorKeypoint k{ mt, seq.at(mt) };
            auto it = std::upper_bound(seq.keys.begin(), seq.keys.end(), k, [](const ColorKeypoint& x, const ColorKeypoint& y) { return x.time < y.time; });
            if (it != seq.keys.begin() && it != seq.keys.end()) {
                g_selectedStop = g_dragKey = static_cast<int>(seq.keys.insert(it, k) - seq.keys.begin());
                changed = discrete = true;
            }
        }
    }
    if (g_dragKey >= static_cast<int>(seq.keys.size())) g_dragKey = -1;
    if (g_dragKey >= 0) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_dragKey = -1;
        } else if (g_dragKey > 0 && g_dragKey + 1 < static_cast<int>(seq.keys.size())) {
            const float before = seq.keys[static_cast<size_t>(g_dragKey) - 1].time + 0.001f;
            const float after = seq.keys[static_cast<size_t>(g_dragKey) + 1].time - 0.001f;
            const float t = std::clamp(mt, before, std::max(before, after));
            if (t != seq.keys[static_cast<size_t>(g_dragKey)].time) {
                seq.keys[static_cast<size_t>(g_dragKey)].time = t;
                changed = true;
            }
        }
    }
    for (size_t k = 0; k < seq.keys.size(); ++k) {
        const float x = a.x + seq.keys[k].time * width;
        const bool selected = static_cast<int>(k) == g_selectedStop;
        const float half = 5.5f;
        const ImVec2 tip(x, b.y + 1.0f);
        const ImVec2 pts[5] = { tip, ImVec2(x + half, tip.y + half), ImVec2(x + half, tip.y + markerH), ImVec2(x - half, tip.y + markerH), ImVec2(x - half, tip.y + half) };
        dl->AddConvexPolyFilled(pts, 5, colorU32(seq.keys[k].color));
        dl->AddPolyline(pts, 5, selected ? theme::u32(theme::color::accentHover) : (static_cast<int>(k) == hot ? IM_COL32(255, 255, 255, 220) : IM_COL32(20, 20, 26, 255)), ImDrawFlags_Closed, selected ? 2.0f : 1.2f);
    }

    // The selected stop.
    ImGui::Spacing();
    ColorKeypoint& stop = seq.keys[static_cast<size_t>(g_selectedStop)];
    const bool end = g_selectedStop == 0 || g_selectedStop + 1 == static_cast<int>(seq.keys.size());
    float rgb[3] = { stop.color.r, stop.color.g, stop.color.b };
    ImGui::SetNextItemWidth(width * 0.62f);
    if (ImGui::ColorPicker3("##pick", rgb, ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayRGB | ImGuiColorEditFlags_Uint8)) {
        stop.color = Color3(rgb[0], rgb[1], rgb[2]);
        changed = true;
    }
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::TextColored(theme::color::textDim, "Stop %d of %d", g_selectedStop + 1, static_cast<int>(seq.keys.size()));
    ImGui::BeginDisabled(end);
    ImGui::SetNextItemWidth(font * 6.0f);
    const float before = g_selectedStop > 0 ? seq.keys[static_cast<size_t>(g_selectedStop) - 1].time + 0.001f : 0.0f;
    const float after = g_selectedStop + 1 < static_cast<int>(seq.keys.size()) ? seq.keys[static_cast<size_t>(g_selectedStop) + 1].time - 0.001f : 1.0f;
    if (ImGui::DragFloat("Time", &stop.time, 0.005f, before, after, "%.3f", ImGuiSliderFlags_AlwaysClamp)) changed = true;
    if (ImGui::Button(ICON_FA_TRASH_CAN "  Remove stop")) {
        seq.keys.erase(seq.keys.begin() + g_selectedStop);
        g_selectedStop = std::max(0, g_selectedStop - 1);
        changed = discrete = true;
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::TextColored(theme::color::textDim, "Presets");
    struct Preset { const char* name; ColorSequence seq; };
    const Preset presets[] = {
        { "White", ColorSequence(Color3(1, 1, 1)) },
        { "Fire", ColorSequence(std::vector<ColorKeypoint>{ { 0.0f, Color3(1.0f, 0.95f, 0.55f) }, { 0.4f, Color3(1.0f, 0.55f, 0.1f) }, { 1.0f, Color3(0.75f, 0.12f, 0.05f) } }) },
        { "Ice", ColorSequence(Color3(0.85f, 0.97f, 1.0f), Color3(0.25f, 0.55f, 1.0f)) },
        { "Rainbow", ColorSequence(std::vector<ColorKeypoint>{ { 0.0f, Color3(1, 0.2f, 0.2f) }, { 0.25f, Color3(1, 0.85f, 0.2f) }, { 0.5f, Color3(0.3f, 1, 0.4f) }, { 0.75f, Color3(0.3f, 0.6f, 1) }, { 1.0f, Color3(0.75f, 0.35f, 1) } }) },
    };
    for (const Preset& p : presets) {
        if (ImGui::Button(p.name, ImVec2(font * 6.0f, 0.0f))) { seq = p.seq; g_selectedStop = 0; changed = discrete = true; }
    }
    ImGui::EndGroup();
    ImGui::TextColored(theme::color::textDim, "Click the bar to add a stop; drag the markers to move them.");
    return changed;
}

// ============================================================================
//  Instance references
// ============================================================================
bool attachmentPicker(const char* id, const Scene& scene, InstanceRef& ref, uint64_t near) {
    const Instance* current = ref.id ? scene.find(ref.id) : nullptr;
    auto labelOf = [](const Instance& a) {
        std::string s = a.name;
        if (a.parent) s = a.parent->name + " " ICON_FA_ANGLE_RIGHT " " + s;
        return s;
    };
    const std::string preview = current ? std::string(ICON_FA_LOCATION_DOT "  ") + labelOf(*current) : std::string("(none)");
    bool changed = false;
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::BeginCombo(id, preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        if (ImGui::Selectable("(none)", ref.id == 0)) { ref.id = 0; changed = true; }
        // Attachments on the same part (or model) first: the usual choice.
        std::vector<const Instance*> list;
        Scene::forEachDescendant(const_cast<Instance&>(scene.workspace()), [&](Instance& i) { if (i.cls == ClassId::Attachment) list.push_back(&i); });
        const Instance* home = near ? scene.find(near) : nullptr;
        const Instance* homeRoot = home && home->parent ? home->parent : nullptr;
        std::stable_sort(list.begin(), list.end(), [&](const Instance* x, const Instance* y) {
            const bool nx = homeRoot && x->isDescendantOf(homeRoot), ny = homeRoot && y->isDescendantOf(homeRoot);
            return nx > ny;
        });
        for (const Instance* a : list) {
            ImGui::PushID(static_cast<int>(a->id ^ (a->id >> 32)));
            const std::string label = std::string(ICON_FA_LOCATION_DOT "  ") + labelOf(*a);
            if (ImGui::Selectable(label.c_str(), a->id == ref.id)) { ref.id = a->id; changed = true; }
            ImGui::PopID();
        }
        if (list.empty()) ImGui::TextDisabled("No Attachments yet: insert one into a part.");
        ImGui::EndCombo();
    }
    // Dropping an Attachment from the Explorer.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("RIFT_INSTANCES")) {
            const uint64_t* ids = static_cast<const uint64_t*>(p->Data);
            const size_t count = static_cast<size_t>(p->DataSize) / sizeof(uint64_t);
            for (size_t k = 0; k < count; ++k) {
                const Instance* i = scene.find(ids[k]);
                if (i && i->cls == ClassId::Attachment) { ref.id = i->id; changed = true; break; }
            }
        }
        ImGui::EndDragDropTarget();
    }
    return changed;
}

} // namespace rift
