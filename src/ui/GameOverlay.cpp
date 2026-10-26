#include "ui/GameOverlay.h"
#include "game/GameSession.h"
#include "game/GuiSystem.h"
#include "render/TextureCache.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unordered_map>

namespace rift {

namespace {
    ImU32 rgba(const Color3& c, float alpha) {
        return IM_COL32(static_cast<int>(std::clamp(c.r, 0.0f, 1.0f) * 255), static_cast<int>(std::clamp(c.g, 0.0f, 1.0f) * 255),
                        static_cast<int>(std::clamp(c.b, 0.0f, 1.0f) * 255), static_cast<int>(std::clamp(alpha, 0.0f, 1.0f) * 255));
    }

    // Buttons react to the mouse a little, like Roblox's AutoButtonColor.
    Color3 buttonTint(const Color3& c, bool hovered, bool pressed) {
        const float f = pressed ? 0.78f : hovered ? 0.9f : 1.0f;
        return Color3(c.r * f, c.g * f, c.b * f);
    }

    std::string valueText(const Instance& v) {
        char buf[64];
        switch (v.cls) {
            case ClassId::IntValue: return std::to_string(v.intValue);
            case ClassId::NumberValue:
                std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v.numberValue));
                return buf;
            case ClassId::StringValue: return v.text;
            case ClassId::BoolValue: return v.boolValue ? "true" : "false";
            default: return "";
        }
    }

    ImFont* fontFor(GuiFont f, const OverlayFonts& fonts) {
        ImFont* regular = fonts.regular ? fonts.regular : ImGui::GetFont();
        switch (f) {
            case GuiFont::GothamMedium: return fonts.medium ? fonts.medium : regular;
            case GuiFont::GothamBold: case GuiFont::GothamBlack: case GuiFont::SourceSansSemibold:
            case GuiFont::SourceSansBold: case GuiFont::ArialBold: return fonts.bold ? fonts.bold : regular;
            case GuiFont::Code: return fonts.mono ? fonts.mono : regular;
            default: return regular;
        }
    }

    // TextScaled: the largest size (up to 100, like Roblox) at which the text fits the box.
    float scaledTextSize(ImFont* font, const std::string& text, const ImVec2& box, bool wrapped) {
        const float width = std::max(box.x - 8.0f, 1.0f);
        if (!wrapped) {
            const ImVec2 t = font->CalcTextSizeA(100.0f, FLT_MAX, 0.0f, text.c_str());
            const float s = std::min(100.0f * width / std::max(t.x, 1.0f), 100.0f * box.y / std::max(t.y, 1.0f));
            return std::clamp(std::floor(s), 1.0f, 100.0f);
        }
        float lo = 1.0f, hi = std::clamp(box.y, 1.0f, 100.0f);
        for (int k = 0; k < 12; ++k) {
            const float mid = (lo + hi) * 0.5f;
            const ImVec2 t = font->CalcTextSizeA(mid, FLT_MAX, width, text.c_str());
            if (t.x <= width + 0.5f && t.y <= box.y) lo = mid;
            else hi = mid;
        }
        return std::floor(lo);
    }

    // Multiplies the vertices drawn since `first` by the gradient across a..b.
    void applyGradient(ImDrawList* draw, int first, const ImVec2& a, const ImVec2& b, const Instance& g) {
        const float angle = g.guiRotation * 3.14159265f / 180.0f;
        const ImVec2 dir(std::cos(angle), std::sin(angle));
        const ImVec2 size(b.x - a.x, b.y - a.y);
        const ImVec2 center(a.x + size.x * (0.5f + g.gradientOffset.x), a.y + size.y * (0.5f + g.gradientOffset.y));
        const float extent = std::max(std::abs(dir.x) * size.x + std::abs(dir.y) * size.y, 1.0f);
        for (int k = first; k < draw->VtxBuffer.Size; ++k) {
            ImDrawVert& v = draw->VtxBuffer[k];
            const float t = std::clamp(((v.pos.x - center.x) * dir.x + (v.pos.y - center.y) * dir.y) / extent + 0.5f, 0.0f, 1.0f);
            const Color3 c = g.colorSequence.at(t);
            const float alpha = 1.0f - std::clamp(g.transparencySequence.at(t), 0.0f, 1.0f);
            auto channel = [&](int shift, float f) { return static_cast<ImU32>(std::clamp(((v.col >> shift) & 0xFF) * f + 0.5f, 0.0f, 255.0f)); };
            v.col = (channel(IM_COL32_R_SHIFT, c.r) << IM_COL32_R_SHIFT) | (channel(IM_COL32_G_SHIFT, c.g) << IM_COL32_G_SHIFT)
                  | (channel(IM_COL32_B_SHIFT, c.b) << IM_COL32_B_SHIFT) | (channel(IM_COL32_A_SHIFT, alpha) << IM_COL32_A_SHIFT);
        }
    }

    // GuiObject.Rotation: turns what was drawn since `first` around `center` (clockwise on screen).
    void rotateVertices(ImDrawList* draw, int first, const ImVec2& center, float degrees) {
        const float r = degrees * 3.14159265f / 180.0f;
        const float c = std::cos(r), s = std::sin(r);
        for (int k = first; k < draw->VtxBuffer.Size; ++k) {
            ImDrawVert& v = draw->VtxBuffer[k];
            const ImVec2 p(v.pos.x - center.x, v.pos.y - center.y);
            v.pos = ImVec2(center.x + p.x * c - p.y * s, center.y + p.x * s + p.y * c);
        }
    }
}

void drawGameGui(GameSession& session, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts) {
    drawGui(session.clientScene(), session.gui(), textures, draw, origin, fonts);
}

void drawGui(const Scene& scene, const GuiSystem& gui, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts) {
    // Where each element is, for turning children with a rotated parent.
    std::unordered_map<uint64_t, GuiRect> rects;
    for (const GuiSystem::Item& item : gui.drawList()) rects.emplace(item.id, item.rect);
    auto centerOf = [&](const GuiRect& r) { return ImVec2(origin.x + r.pos.x + r.size.x * 0.5f, origin.y + r.pos.y + r.size.y * 0.5f); };

    for (const GuiSystem::Item& item : gui.drawList()) {
        const Instance* e = scene.find(item.id);
        if (!e) continue;
        const ImVec2 clipMin(origin.x + item.clip.pos.x, origin.y + item.clip.pos.y);
        const ImVec2 clipMax(clipMin.x + item.clip.size.x, clipMin.y + item.clip.size.y);
        draw->PushClipRect(clipMin, clipMax, true);
        const int firstVertex = draw->VtxBuffer.Size;

        const ImVec2 a(origin.x + item.rect.pos.x, origin.y + item.rect.pos.y);
        const ImVec2 b(a.x + item.rect.size.x, a.y + item.rect.size.y);
        const float rounding = std::min(e->cornerRadius, std::min(item.rect.size.x, item.rect.size.y) * 0.5f);
        const bool isButton = GuiSystem::isButton(*e);
        Color3 background = e->color;
        if (isButton) background = buttonTint(background, gui.hovered() == e->id, gui.pressed() == e->id);

        if (e->backgroundTransparency < 1.0f) draw->AddRectFilled(a, b, rgba(background, 1.0f - e->backgroundTransparency), rounding);

        if (e->cls == ClassId::ImageLabel || e->cls == ClassId::ImageButton) {
            if (auto tex = textures.get(e->texture.path)) {
                ImVec2 ia = a, ib = b;
                if (e->scaleType != ScaleType::Stretch && tex->width() > 0 && tex->height() > 0) {
                    const float imageAspect = static_cast<float>(tex->width()) / tex->height();
                    const float boxAspect = item.rect.size.x / std::max(item.rect.size.y, 1.0f);
                    const bool fitWidth = (e->scaleType == ScaleType::Fit) == (imageAspect > boxAspect);
                    const ImVec2 size = fitWidth ? ImVec2(item.rect.size.x, item.rect.size.x / imageAspect)
                                                 : ImVec2(item.rect.size.y * imageAspect, item.rect.size.y);
                    const ImVec2 center((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
                    ia = ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f);
                    ib = ImVec2(center.x + size.x * 0.5f, center.y + size.y * 0.5f);
                }
                // Textures are stored bottom-up (OpenGL), so flip V.
                draw->AddImageRounded(static_cast<ImTextureID>(tex->id()), ia, ib, ImVec2(0, 1), ImVec2(1, 0),
                                      rgba(e->imageColor, 1.0f - e->imageTransparency), rounding);
            }
        }

        const float textAlpha = 1.0f - std::clamp(e->textTransparency, 0.0f, 1.0f);
        if ((e->cls == ClassId::TextLabel || e->cls == ClassId::TextButton) && !e->text.empty() && textAlpha > 0.0f) {
            ImFont* font = fontFor(e->font, fonts);
            const float wrap = e->textWrapped ? std::max(item.rect.size.x - 8.0f, 1.0f) : 0.0f;
            const float size = e->textScaled ? scaledTextSize(font, e->text, ImVec2(item.rect.size.x, item.rect.size.y), e->textWrapped)
                                             : std::max(e->textSize, 4.0f);
            const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, wrap, e->text.c_str());
            float x = a.x + 4.0f, y = a.y;
            if (e->textXAlignment == TextXAlignment::Center) x = a.x + (item.rect.size.x - textSize.x) * 0.5f;
            else if (e->textXAlignment == TextXAlignment::Right) x = b.x - textSize.x - 4.0f;
            if (e->textYAlignment == TextYAlignment::Center) y = a.y + (item.rect.size.y - textSize.y) * 0.5f;
            else if (e->textYAlignment == TextYAlignment::Bottom) y = b.y - textSize.y;
            const ImVec2 pos(std::floor(x), std::floor(y));
            // Black is the bold face drawn twice, a little apart.
            const float heavy = e->font == GuiFont::GothamBlack ? std::max(1.0f, std::round(size * 0.035f)) : 0.0f;
            if (e->textStrokeTransparency < 1.0f) {
                const ImU32 stroke = rgba(e->textStrokeColor, (1.0f - e->textStrokeTransparency) * textAlpha);
                const float t = std::max(1.0f, std::round(size / 22.0f));
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        const float k = dx && dy ? 0.75f : 1.0f; // rounder corners
                        const ImVec2 at(pos.x + dx * t * k, pos.y + dy * t * k);
                        draw->AddText(font, size, at, stroke, e->text.c_str(), nullptr, wrap);
                        if (heavy > 0.0f) draw->AddText(font, size, ImVec2(at.x + heavy, at.y), stroke, e->text.c_str(), nullptr, wrap);
                    }
            }
            const ImU32 color = rgba(e->textColor, textAlpha);
            draw->AddText(font, size, pos, color, e->text.c_str(), nullptr, wrap);
            if (heavy > 0.0f) draw->AddText(font, size, ImVec2(pos.x + heavy, pos.y), color, e->text.c_str(), nullptr, wrap);
        }

        // A UIGradient inside shades the background, image and text.
        for (const Instance* c : e->children) {
            if (c->cls == ClassId::UIGradient && c->enabled) {
                applyGradient(draw, firstVertex, a, b, *c);
                break;
            }
        }
        if (e->borderSize > 0.0f) draw->AddRect(a, b, rgba(e->borderColor, 1.0f - e->backgroundTransparency * 0.5f), rounding, 0, e->borderSize);
        // Its own rotation, then each rotated ancestor's (children turn with their parent).
        if (std::fmod(e->guiRotation, 360.0f) != 0.0f) rotateVertices(draw, firstVertex, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), e->guiRotation);
        for (const Instance* p = e->parent; p && p->isGuiObject(); p = p->parent) {
            if (std::fmod(p->guiRotation, 360.0f) == 0.0f) continue;
            if (auto it = rects.find(p->id); it != rects.end()) rotateVertices(draw, firstVertex, centerOf(it->second), p->guiRotation);
        }
        draw->PopClipRect();
    }
}

void drawGameHud(GameSession& session, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const glm::vec2& size, const OverlayFonts& fonts) {
    ImFont* font = fonts.regular ? fonts.regular : ImGui::GetFont();
    ImFont* bold = fonts.bold ? fonts.bold : font;
    const float base = ImGui::GetFontSize();

    // ---- Backpack: the bar (bottom centre), the button beside it and the inventory above it
    {
        const Scene& world = session.clientScene();
        const GameSession::Client& me = session.focused();
        const glm::vec2 mouse = session.viewport().mouse;
        const bool hoveredView = session.viewport().hovered;
        auto over = [&](const glm::vec2& p, const glm::vec2& s) {
            return hoveredView && mouse.x >= p.x && mouse.y >= p.y && mouse.x <= p.x + s.x && mouse.y <= p.y + s.y;
        };
        const Instance* hoveredTool = nullptr;
        glm::vec2 hoveredPos(0.0f);
        const uint64_t dragged = me.dragging ? me.dragTool : 0;

        // One tool's tile: its icon, or its name wrapped to the tile.
        auto tile = [&](const Instance* tool, const ImVec2& a, const ImVec2& b, bool equipped, bool hot, float alpha) {
            const float w = b.x - a.x;
            const float r = w * 0.14f;
            const int fill = static_cast<int>((equipped ? 220 : hot ? 210 : 190) * alpha);
            draw->AddRectFilled(a, b, equipped ? IM_COL32(58, 52, 86, fill) : hot ? IM_COL32(40, 40, 52, fill) : IM_COL32(20, 20, 28, fill), r);
            draw->AddRect(a, b, equipped ? IM_COL32(167, 139, 250, static_cast<int>(255 * alpha)) : IM_COL32(255, 255, 255, static_cast<int>(40 * alpha)), r, 0,
                          equipped ? 2.5f : 1.0f);
            if (!tool) return;
            const float pad = w * 0.12f;
            const ImU32 tint = IM_COL32(255, 255, 255, static_cast<int>(255 * alpha));
            if (auto tex = tool->texture.path.empty() ? nullptr : textures.get(tool->texture.path)) {
                draw->AddImage(static_cast<ImTextureID>(tex->id()), ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, b.y - pad), ImVec2(0, 1), ImVec2(1, 0), tint);
            } else {
                // Smaller text for long words, so none breaks in the middle.
                const float wrap = w - pad;
                float textSize = base * 0.8f;
                float widest = 0.0f;
                for (size_t start = 0; start < tool->name.size();) {
                    size_t end = tool->name.find(' ', start);
                    if (end == std::string::npos) end = tool->name.size();
                    widest = std::max(widest, font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, tool->name.c_str() + start, tool->name.c_str() + end).x);
                    start = end + 1;
                }
                if (widest > wrap) textSize = std::max(base * 0.5f, std::floor(textSize * wrap / widest));
                const ImVec2 t = font->CalcTextSizeA(textSize, FLT_MAX, wrap, tool->name.c_str());
                draw->PushClipRect(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(b.x - 2.0f, b.y - 2.0f), true);
                draw->AddText(font, textSize, ImVec2(a.x + (w - std::min(t.x, wrap)) * 0.5f, a.y + (b.y - a.y - t.y) * 0.5f),
                              IM_COL32(240, 240, 248, static_cast<int>(255 * alpha)), tool->name.c_str(), nullptr, wrap);
                draw->PopClipRect();
            }
        };
        auto number = [&](int n, const ImVec2& a, float w, int alpha) {
            char text[4];
            std::snprintf(text, sizeof(text), "%d", n);
            draw->AddText(bold, base * 0.72f, ImVec2(a.x + w * 0.072f, a.y + w * 0.042f), IM_COL32(255, 255, 255, alpha), text);
        };
        auto at = [&](const glm::vec2& p) { return ImVec2(origin.x + p.x, origin.y + p.y); };

        // The inventory: every tool, a search box, scrolled with the wheel.
        const GameSession::InventoryView inv = session.inventory(session.focus());
        if (inv.open) {
            const ImVec2 a = at(inv.pos), b = at(inv.pos + inv.size);
            const float r = base * 0.6f;
            draw->AddRectFilled(a, b, IM_COL32(16, 16, 22, 205), r);
            draw->AddRect(a, b, IM_COL32(255, 255, 255, 30), r);
            const ImVec2 sa = at(inv.searchPos), sb = at(inv.searchPos + inv.searchSize);
            const float titleSize = base * 1.05f;
            const float titleY = sa.y + (inv.searchSize.y - titleSize) * 0.5f;
            draw->AddText(bold, titleSize, ImVec2(at(inv.gridPos).x, titleY), IM_COL32(255, 255, 255, 235), "Backpack");
            char count[48];
            std::snprintf(count, sizeof(count), "%d tool%s", inv.toolCount, inv.toolCount == 1 ? "" : "s");
            const float titleWidth = bold->CalcTextSizeA(titleSize, FLT_MAX, 0.0f, "Backpack").x;
            draw->AddText(font, base * 0.85f, ImVec2(at(inv.gridPos).x + titleWidth + base * 0.7f, titleY + (titleSize - base * 0.85f) * 0.6f),
                          IM_COL32(190, 190, 205, 200), count);
            // Search box
            draw->AddRectFilled(sa, sb, me.searchFocused ? IM_COL32(44, 44, 58, 240) : IM_COL32(32, 32, 42, 220), inv.searchSize.y * 0.5f);
            if (me.searchFocused) draw->AddRect(sa, sb, IM_COL32(167, 139, 250, 255), inv.searchSize.y * 0.5f, 0, 1.5f);
            const float textSize = base * 0.9f;
            const float textY = sa.y + (inv.searchSize.y - textSize) * 0.5f;
            const float textX = sa.x + inv.searchSize.y * 0.5f;
            // A small magnifier.
            const ImVec2 lens(textX + textSize * 0.33f, textY + textSize * 0.45f);
            draw->AddCircle(lens, textSize * 0.27f, IM_COL32(200, 200, 215, 200), 12, 1.6f);
            draw->AddLine(ImVec2(lens.x + textSize * 0.2f, lens.y + textSize * 0.2f), ImVec2(lens.x + textSize * 0.42f, lens.y + textSize * 0.42f), IM_COL32(200, 200, 215, 200), 1.8f);
            const float queryX = textX + textSize * 1.1f;
            draw->PushClipRect(sa, sb, true);
            if (me.inventorySearch.empty() && !me.searchFocused) {
                draw->AddText(font, textSize, ImVec2(queryX, textY), IM_COL32(160, 160, 175, 200), "Search");
            } else {
                draw->AddText(font, textSize, ImVec2(queryX, textY), IM_COL32(245, 245, 250, 255), me.inventorySearch.c_str());
                if (me.searchFocused && std::fmod(ImGui::GetTime(), 1.0) < 0.55) {
                    const float caret = queryX + font->CalcTextSizeA(textSize, FLT_MAX, 0.0f, me.inventorySearch.c_str()).x + 1.0f;
                    draw->AddLine(ImVec2(caret, textY), ImVec2(caret, textY + textSize), IM_COL32(245, 245, 250, 255), 1.5f);
                }
            }
            draw->PopClipRect();
            // Tools
            if (inv.cells.empty()) {
                const char* none = inv.toolCount ? "No tools match the search." : "No tools yet. Tools you pick up show up here.";
                const ImVec2 t = font->CalcTextSizeA(base, FLT_MAX, 0.0f, none);
                draw->AddText(font, base, ImVec2(at(inv.gridPos).x + (inv.gridSize.x - t.x) * 0.5f, at(inv.gridPos).y + (inv.gridSize.y - t.y) * 0.5f),
                              IM_COL32(180, 180, 195, 220), none);
            }
            for (const GameSession::InventoryView::Cell& cell : inv.cells) {
                const Instance* tool = world.find(cell.tool);
                if (!tool) continue;
                const bool hot = over(cell.pos, cell.size);
                if (hot && !dragged) { hoveredTool = tool; hoveredPos = cell.pos; }
                tile(tool, at(cell.pos), at(cell.pos + cell.size), cell.equipped, hot, cell.tool == dragged ? 0.35f : 1.0f);
                if (cell.hotbarIndex >= 0) number((cell.hotbarIndex + 1) % 10, at(cell.pos), cell.size.x, 150);
            }
            // More rows below or above: a thin scroll bar.
            if (inv.maxScroll > 0) {
                const float cell = inv.cells.empty() ? inv.gridSize.y : inv.cells[0].size.y * 1.1f;
                const float visible = std::max(1.0f, std::round((inv.gridSize.y + cell * 0.1f) / cell));
                const float track = inv.gridSize.y;
                const float thumb = std::max(base, track * visible / (visible + static_cast<float>(inv.maxScroll)));
                const float t = std::clamp(static_cast<float>(me.inventoryScroll) / inv.maxScroll, 0.0f, 1.0f);
                const float x = at(inv.gridPos).x + inv.gridSize.x + base * 0.35f;
                const float y = at(inv.gridPos).y + (track - thumb) * t;
                draw->AddRectFilled(ImVec2(x, y), ImVec2(x + base * 0.22f, y + thumb), IM_COL32(255, 255, 255, 90), base * 0.11f);
            }
        }

        // The bar: numbered slots (empty ones only while the inventory is open).
        for (const GameSession::HotbarSlot& slot : session.hotbar(session.focus())) {
            const Instance* tool = slot.tool ? world.find(slot.tool) : nullptr;
            const bool hot = over(slot.pos, slot.size);
            const ImVec2 a = at(slot.pos), b = at(slot.pos + slot.size);
            if (!tool) {
                // An empty slot to drop a tool on.
                const float r = slot.size.x * 0.14f;
                draw->AddRectFilled(a, b, hot && dragged ? IM_COL32(58, 52, 86, 170) : IM_COL32(20, 20, 28, 110), r);
                draw->AddRect(a, b, IM_COL32(255, 255, 255, hot && dragged ? 120 : 45), r, 0, 1.0f);
                number(slot.number, a, slot.size.x, 110);
                continue;
            }
            if (hot && !dragged) { hoveredTool = tool; hoveredPos = slot.pos; }
            tile(tool, a, b, slot.equipped, hot, slot.tool == dragged ? 0.35f : 1.0f);
            if (hot && dragged && dragged != slot.tool) draw->AddRect(a, b, IM_COL32(167, 139, 250, 200), slot.size.x * 0.14f, 0, 2.0f);
            number(slot.number, a, slot.size.x, 170);
        }

        // The button that opens the inventory: a little backpack.
        if (inv.button) {
            const bool hot = over(inv.buttonPos, inv.buttonSize);
            const ImVec2 a = at(inv.buttonPos), b = at(inv.buttonPos + inv.buttonSize);
            const float w = inv.buttonSize.x;
            draw->AddRectFilled(a, b, inv.open ? IM_COL32(58, 52, 86, 225) : hot ? IM_COL32(40, 40, 52, 215) : IM_COL32(20, 20, 28, 190), w * 0.18f);
            draw->AddRect(a, b, inv.open ? IM_COL32(167, 139, 250, 255) : IM_COL32(255, 255, 255, 45), w * 0.18f, 0, inv.open ? 2.0f : 1.0f);
            const ImU32 ink = IM_COL32(235, 235, 245, 235);
            const ImVec2 c(a.x + w * 0.5f, a.y + w * 0.54f);
            const float bw = w * 0.42f, bh = w * 0.44f;
            draw->AddRect(ImVec2(c.x - bw * 0.5f, c.y - bh * 0.5f), ImVec2(c.x + bw * 0.5f, c.y + bh * 0.5f), ink, w * 0.08f, 0, 2.0f);   // the bag
            draw->AddLine(ImVec2(c.x - bw * 0.5f, c.y - bh * 0.05f), ImVec2(c.x + bw * 0.5f, c.y - bh * 0.05f), ink, 1.6f);           // its flap
            draw->AddRectFilled(ImVec2(c.x - w * 0.045f, c.y - bh * 0.12f), ImVec2(c.x + w * 0.045f, c.y + bh * 0.08f), ink, 1.0f);   // the buckle
            draw->PathArcTo(ImVec2(c.x, c.y - bh * 0.5f), bw * 0.24f, 3.14159f, 6.28318f, 10);                                        // the handle
            draw->PathStroke(ink, 0, 2.0f);
            if (hot && !dragged) {
                const char* tip = inv.open ? "Close the backpack (`)" : "Backpack (`)";
                const ImVec2 t = font->CalcTextSizeA(base * 0.9f, FLT_MAX, 0.0f, tip);
                const ImVec2 p(b.x - t.x, a.y - t.y - base * 0.9f);
                draw->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 4.0f), ImVec2(p.x + t.x + 6.0f, p.y + t.y + 4.0f), IM_COL32(14, 14, 20, 230), 5.0f);
                draw->AddText(font, base * 0.9f, p, IM_COL32(240, 240, 248, 255), tip);
            }
        }

        // The tool being dragged follows the mouse.
        if (const Instance* tool = dragged ? world.find(dragged) : nullptr) {
            const float w = inv.cells.empty() ? std::clamp(session.viewport().size.y * 0.058f, 40.0f, 84.0f) : inv.cells[0].size.x;
            const ImVec2 a(origin.x + mouse.x - w * 0.5f, origin.y + mouse.y - w * 0.5f);
            tile(tool, a, ImVec2(a.x + w, a.y + w), false, true, 0.85f);
        }

        if (hoveredTool) {
            const std::string& tip = hoveredTool->text.empty() ? hoveredTool->name : hoveredTool->text;
            const ImVec2 t = font->CalcTextSizeA(base * 0.9f, FLT_MAX, 0.0f, tip.c_str());
            const ImVec2 p(origin.x + hoveredPos.x, origin.y + hoveredPos.y - t.y - base * 0.9f);
            draw->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 4.0f), ImVec2(p.x + t.x + 6.0f, p.y + t.y + 4.0f), IM_COL32(14, 14, 20, 230), 5.0f);
            draw->AddText(font, base * 0.9f, p, IM_COL32(240, 240, 248, 255), tip.c_str());
        }
    }

    // ---- leaderboard (top right): the local player's leaderstats
    // What the client sees: its copy of the player (leaderstats replicate from the server).
    if (Instance* player = session.focused().playerList ? session.clientScene().find(session.playerId()) : nullptr) {
        if (Instance* stats = player->findFirstChild("leaderstats")) {
            std::vector<const Instance*> values;
            for (const Instance* v : stats->children) if (v->isValueObject() && v->cls != ClassId::Vector3Value && v->cls != ClassId::Color3Value) values.push_back(v);
            if (!values.empty()) {
                const float column = base * 6.0f;
                const float width = column * (values.size() + 1) + base;
                const ImVec2 a(origin.x + size.x - width - base, origin.y + base);
                const float rowHeight = base * 1.7f;
                draw->AddRectFilled(a, ImVec2(a.x + width, a.y + rowHeight * 2.0f), IM_COL32(18, 18, 24, 170), base * 0.4f);
                draw->AddText(bold, base, ImVec2(a.x + base * 0.6f, a.y + base * 0.35f), IM_COL32(200, 200, 215, 255), "Player");
                draw->AddText(font, base, ImVec2(a.x + base * 0.6f, a.y + rowHeight + base * 0.35f), IM_COL32(255, 255, 255, 255), player->name.c_str());
                for (size_t k = 0; k < values.size(); ++k) {
                    const float x = a.x + column * (k + 1) + base * 0.6f;
                    draw->AddText(bold, base, ImVec2(x, a.y + base * 0.35f), IM_COL32(200, 200, 215, 255), values[k]->name.c_str());
                    draw->AddText(font, base, ImVec2(x, a.y + rowHeight + base * 0.35f), IM_COL32(255, 255, 255, 255), valueText(*values[k]).c_str());
                }
            }
        }
    }

    // ---- message banner / win and lose screen
    const std::string& message = session.message();
    if (!message.empty()) {
        const bool final = session.outcome() != GameSession::Outcome::None;
        const float textSize = base * (final ? 2.6f : 1.8f);
        const ImVec2 t = bold->CalcTextSizeA(textSize, FLT_MAX, size.x * 0.8f, message.c_str());
        const ImVec2 center(origin.x + size.x * 0.5f, origin.y + size.y * (final ? 0.4f : 0.2f));
        const ImVec2 pos(center.x - t.x * 0.5f, center.y - t.y * 0.5f);
        if (final) draw->AddRectFilled(ImVec2(origin.x, origin.y), ImVec2(origin.x + size.x, origin.y + size.y), IM_COL32(8, 8, 14, 120));
        draw->AddRectFilled(ImVec2(pos.x - base * 1.4f, pos.y - base * 0.8f), ImVec2(pos.x + t.x + base * 1.4f, pos.y + t.y + base * 0.8f),
                            IM_COL32(14, 14, 22, 200), base * 0.6f);
        ImU32 color = IM_COL32(255, 255, 255, 255);
        if (session.outcome() == GameSession::Outcome::Won) color = IM_COL32(255, 214, 90, 255);
        if (session.outcome() == GameSession::Outcome::Lost) color = IM_COL32(255, 110, 100, 255);
        draw->AddText(bold, textSize, pos, color, message.c_str(), nullptr, size.x * 0.8f);
    }

    // ---- respawn notice
    if (!session.characterId() && session.outcome() == GameSession::Outcome::None) {
        const char* text = "Respawning...";
        const ImVec2 t = bold->CalcTextSizeA(base * 1.4f, FLT_MAX, 0.0f, text);
        draw->AddText(bold, base * 1.4f, ImVec2(origin.x + (size.x - t.x) * 0.5f, origin.y + size.y * 0.7f), IM_COL32(255, 255, 255, 220), text);
    }
}

} // namespace rift
