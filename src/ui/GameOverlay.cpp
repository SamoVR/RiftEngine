#include "ui/GameOverlay.h"
#include "game/GameSession.h"
#include "render/TextureCache.h"

#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

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
}

void drawGameGui(GameSession& session, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts) {
    drawGui(session.scene(), session.gui(), textures, draw, origin, fonts);
}

void drawGui(const Scene& scene, const GuiSystem& gui, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts) {
    ImFont* font = fonts.regular ? fonts.regular : ImGui::GetFont();
    for (const GuiSystem::Item& item : gui.drawList()) {
        const Instance* e = scene.find(item.id);
        if (!e) continue;
        const ImVec2 clipMin(origin.x + item.clip.pos.x, origin.y + item.clip.pos.y);
        const ImVec2 clipMax(clipMin.x + item.clip.size.x, clipMin.y + item.clip.size.y);
        draw->PushClipRect(clipMin, clipMax, true);

        const ImVec2 a(origin.x + item.rect.pos.x, origin.y + item.rect.pos.y);
        const ImVec2 b(a.x + item.rect.size.x, a.y + item.rect.size.y);
        const float rounding = std::min(e->cornerRadius, std::min(item.rect.size.x, item.rect.size.y) * 0.5f);
        const bool isButton = GuiSystem::isButton(*e);
        Color3 background = e->color;
        if (isButton) background = buttonTint(background, gui.hovered() == e->id, gui.pressed() == e->id);

        if (e->backgroundTransparency < 1.0f) draw->AddRectFilled(a, b, rgba(background, 1.0f - e->backgroundTransparency), rounding);
        if (e->borderSize > 0.0f) draw->AddRect(a, b, rgba(e->borderColor, 1.0f - e->backgroundTransparency * 0.5f), rounding, 0, e->borderSize);

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

        if ((e->cls == ClassId::TextLabel || e->cls == ClassId::TextButton) && !e->text.empty()) {
            const float size = std::max(e->textSize, 4.0f);
            const float wrap = e->textWrapped ? std::max(item.rect.size.x - 8.0f, 1.0f) : 0.0f;
            const ImVec2 textSize = font->CalcTextSizeA(size, FLT_MAX, wrap, e->text.c_str());
            float x = a.x + 4.0f, y = a.y;
            if (e->textXAlignment == TextXAlignment::Center) x = a.x + (item.rect.size.x - textSize.x) * 0.5f;
            else if (e->textXAlignment == TextXAlignment::Right) x = b.x - textSize.x - 4.0f;
            if (e->textYAlignment == TextYAlignment::Center) y = a.y + (item.rect.size.y - textSize.y) * 0.5f;
            else if (e->textYAlignment == TextYAlignment::Bottom) y = b.y - textSize.y;
            draw->AddText(font, size, ImVec2(std::floor(x), std::floor(y)), rgba(e->textColor, 1.0f), e->text.c_str(), nullptr, wrap);
        }
        draw->PopClipRect();
    }
}

void drawGameHud(GameSession& session, ImDrawList* draw, const glm::vec2& origin, const glm::vec2& size, const OverlayFonts& fonts) {
    ImFont* font = fonts.regular ? fonts.regular : ImGui::GetFont();
    ImFont* bold = fonts.bold ? fonts.bold : font;
    const float base = ImGui::GetFontSize();

    // ---- leaderboard (top right): the local player's leaderstats
    if (Instance* player = session.player()) {
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
    if (!session.character() && session.outcome() == GameSession::Outcome::None) {
        const char* text = "Respawning...";
        const ImVec2 t = bold->CalcTextSizeA(base * 1.4f, FLT_MAX, 0.0f, text);
        draw->AddText(bold, base * 1.4f, ImVec2(origin.x + (size.x - t.x) * 0.5f, origin.y + size.y * 0.7f), IM_COL32(255, 255, 255, 220), text);
    }
}

} // namespace rift
