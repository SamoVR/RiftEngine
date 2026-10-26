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

    // ---- Backpack bar (bottom centre): the player's tools
    {
        const Scene& world = session.clientScene();
        const glm::vec2 mouse = session.viewport().mouse;
        const bool hoveredView = session.viewport().hovered;
        const Instance* hoveredTool = nullptr;
        glm::vec2 hoveredPos(0.0f);
        for (const GameSession::HotbarSlot& slot : session.hotbar(session.focus())) {
            const Instance* tool = world.find(slot.tool);
            if (!tool) continue;
            const ImVec2 a(origin.x + slot.pos.x, origin.y + slot.pos.y);
            const ImVec2 b(a.x + slot.size.x, a.y + slot.size.y);
            const bool over = hoveredView && mouse.x >= slot.pos.x && mouse.y >= slot.pos.y && mouse.x <= slot.pos.x + slot.size.x && mouse.y <= slot.pos.y + slot.size.y;
            if (over) { hoveredTool = tool; hoveredPos = slot.pos; }
            const float r = slot.size.x * 0.14f;
            draw->AddRectFilled(a, b, slot.equipped ? IM_COL32(58, 52, 86, 220) : over ? IM_COL32(40, 40, 52, 210) : IM_COL32(20, 20, 28, 190), r);
            draw->AddRect(a, b, slot.equipped ? IM_COL32(167, 139, 250, 255) : IM_COL32(255, 255, 255, 40), r, 0, slot.equipped ? 2.5f : 1.0f);
            const float pad = slot.size.x * 0.12f;
            if (auto tex = tool->texture.path.empty() ? nullptr : textures.get(tool->texture.path)) {
                draw->AddImage(static_cast<ImTextureID>(tex->id()), ImVec2(a.x + pad, a.y + pad), ImVec2(b.x - pad, b.y - pad), ImVec2(0, 1), ImVec2(1, 0));
            } else {
                // No icon: the tool's name, wrapped to the slot.
                const float textSize = base * 0.8f;
                const float wrap = slot.size.x - pad;
                const ImVec2 t = font->CalcTextSizeA(textSize, FLT_MAX, wrap, tool->name.c_str());
                draw->PushClipRect(ImVec2(a.x + 2.0f, a.y + 2.0f), ImVec2(b.x - 2.0f, b.y - 2.0f), true);
                draw->AddText(font, textSize, ImVec2(a.x + (slot.size.x - std::min(t.x, wrap)) * 0.5f, a.y + (slot.size.y - t.y) * 0.5f),
                              IM_COL32(240, 240, 248, 255), tool->name.c_str(), nullptr, wrap);
                draw->PopClipRect();
            }
            char number[4];
            std::snprintf(number, sizeof(number), "%d", slot.number);
            draw->AddText(bold, base * 0.72f, ImVec2(a.x + pad * 0.6f, a.y + pad * 0.35f), IM_COL32(255, 255, 255, 170), number);
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
