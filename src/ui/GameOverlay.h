#pragma once
//
// GameOverlay.h -- draws what a running game shows on top of the 3D view,
// with Dear ImGui's draw lists (used by the editor's Play mode and by the
// player):
//   * the game's GUI (ScreenGuis in StarterGui) laid out by GuiSystem,
//   * the leaderboard (each player's "leaderstats" folder of values),
//   * game:Message() banners and the win / lose screen.
//
#include <glm/glm.hpp>

struct ImDrawList;
struct ImFont;

namespace rift {

class GameSession;
class GuiSystem;
class Scene;
class TextureCache;

struct OverlayFonts {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
};

// `origin`: top-left of the game view in screen pixels (GuiSystem rectangles
// are relative to it).
void drawGameGui(GameSession& session, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts);
// The same for any scene laid out by `gui` (the editor previews StarterGui while editing).
void drawGui(const Scene& scene, const GuiSystem& gui, TextureCache& textures, ImDrawList* draw, const glm::vec2& origin, const OverlayFonts& fonts);
void drawGameHud(GameSession& session, ImDrawList* draw, const glm::vec2& origin, const glm::vec2& size, const OverlayFonts& fonts);

} // namespace rift
