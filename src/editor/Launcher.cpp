// The home screen shown while no project is open: recent projects with
// thumbnails, templates and examples, learning links and update news.
#include "editor/EditorApp.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "render/Texture.h"

#include "IconsFontAwesome6.h"
#include <imgui_internal.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>

namespace rift {

namespace fs = std::filesystem;

namespace {
    // "5 minutes ago", "yesterday", "12 Mar 2026".
    std::string timeAgo(std::int64_t when) {
        if (when <= 0) return {};
        const std::int64_t s = static_cast<std::int64_t>(std::time(nullptr)) - when;
        if (s < 60) return "just now";
        if (s < 3600) return std::to_string(s / 60) + (s < 120 ? " minute ago" : " minutes ago");
        if (s < 86400) return std::to_string(s / 3600) + (s < 7200 ? " hour ago" : " hours ago");
        if (s < 2 * 86400) return "yesterday";
        if (s < 30 * 86400) return std::to_string(s / 86400) + " days ago";
        const std::time_t t = static_cast<std::time_t>(when);
        std::tm tm{};
        localtime_s(&tm, &t);
        char text[32];
        std::strftime(text, sizeof(text), "%d %b %Y", &tm);
        return text;
    }

    bool containsNoCase(const std::string& text, const char* needle) {
        if (!*needle) return true;
        const std::string n = needle;
        auto it = std::search(text.begin(), text.end(), n.begin(), n.end(),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        return it != text.end();
    }

    // `text` cut to fit `width`: "Long na..." or, with `front`, "...end of a path".
    std::string fitText(ImFont* font, float size, const std::string& text, float width, bool front = false) {
        auto w = [&](const std::string& t) { return font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.c_str()).x; };
        if (text.empty() || w(text) <= width) return text;
        std::string t = text;
        while (!t.empty() && w(front ? "..." + t : t + "...") > width) {
            if (front) t.erase(0, 1);
            else t.pop_back();
        }
        return front ? "..." + t : t + "...";
    }

    // An example's name and one line about it, from its project.json (read once).
    const std::pair<std::string, std::string>& exampleInfo(const fs::path& folder) {
        static std::map<fs::path, std::pair<std::string, std::string>> cache;
        if (auto it = cache.find(folder); it != cache.end()) return it->second;
        std::pair<std::string, std::string> info{ Paths::toUtf8(folder.filename()), "Example: open a copy" };
        std::ifstream in(folder / "project.json");
        const nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_object()) {
            if (j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty()) info.first = j["name"].get<std::string>();
            if (j.contains("description") && j["description"].is_string()) info.second = j["description"].get<std::string>();
        }
        return cache[folder] = info;
    }

    // A stable colour per project name for cards without a thumbnail.
    ImVec4 nameColor(const std::string& name) {
        unsigned h = 2166136261u;
        for (unsigned char c : name) h = (h ^ c) * 16777619u;
        float r, g, b;
        ImGui::ColorConvertHSVtoRGB(static_cast<float>(h % 360) / 360.0f, 0.55f, 0.42f, r, g, b);
        return ImVec4(r, g, b, 1.0f);
    }

    // Big primary / secondary buttons of the home screen.
    bool bigButton(const char* label, bool primary, float height) {
        if (primary) {
            ImGui::PushStyleColor(ImGuiCol_Button, theme::color::accent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::accentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme::color::accentActive);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetFontSize() * 1.1f, (height - ImGui::GetFontSize()) * 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize() * 0.45f);
        const bool pressed = ImGui::Button(label);
        ImGui::PopStyleVar(2);
        if (primary) ImGui::PopStyleColor(4);
        return pressed;
    }

    // A side-bar entry: icon + label, highlighted when selected.
    bool navItem(const char* icon, const char* label, bool selected, float width) {
        const float fs_ = ImGui::GetFontSize();
        const float h = fs_ * 2.3f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(label);
        const bool pressed = ImGui::InvisibleButton("##nav", ImVec2(width, h));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (selected || hovered) {
            draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), theme::u32(selected ? ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.18f) : theme::color::frame), fs_ * 0.45f);
        }
        if (selected) draw->AddRectFilled(ImVec2(p.x, p.y + h * 0.25f), ImVec2(p.x + 3.0f, p.y + h * 0.75f), theme::u32(theme::color::accentHover), 2.0f);
        const ImVec4 iconColor = selected ? theme::color::accentHover : theme::color::textDim;
        draw->AddText(ImVec2(p.x + fs_ * 0.9f, p.y + (h - fs_) * 0.5f), theme::u32(iconColor), icon);
        draw->AddText(theme::fonts().medium, fs_, ImVec2(p.x + fs_ * 2.6f, p.y + (h - fs_) * 0.5f), theme::u32(selected ? theme::color::text : theme::color::textDim), label);
        return pressed;
    }

    // A tile with a coloured top (icon) and a title + line of text under it.
    bool tile(const char* id, const char* icon, const ImVec4& color, const char* title, const char* text, float width) {
        const float fs_ = ImGui::GetFontSize();
        const float top = fs_ * 4.2f, bottom = fs_ * 3.3f;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(id);
        const bool pressed = ImGui::InvisibleButton("##tile", ImVec2(width, top + bottom));
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float r = fs_ * 0.6f;
        const ImVec2 b(p.x + width, p.y + top + bottom);
        draw->AddRectFilled(p, b, theme::u32(hovered ? theme::color::frameHover : theme::color::panel), r);
        // Coloured band with a soft vertical fade: one rounded shape, shaded
        // (layered shapes leave a seam where they overlap).
        const ImVec4 base = hovered ? theme::color::frameHover : theme::color::panel;
        auto over = [&](float a) { return theme::u32(ImVec4(base.x + (color.x - base.x) * a, base.y + (color.y - base.y) * a, base.z + (color.z - base.z) * a, 1.0f)); };
        const int firstVertex = draw->VtxBuffer.Size;
        draw->AddRectFilled(p, ImVec2(b.x, p.y + top), IM_COL32_WHITE, r, ImDrawFlags_RoundCornersTop);
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, firstVertex, draw->VtxBuffer.Size, p, ImVec2(p.x, p.y + top), over(hovered ? 0.55f : 0.40f), over(0.10f));
        const float iconSize = fs_ * 2.0f;
        const ImVec2 t = theme::fonts().ui->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, icon);
        draw->AddText(theme::fonts().ui, iconSize, ImVec2(p.x + (width - t.x) * 0.5f, p.y + (top - t.y) * 0.5f), theme::u32(ImVec4(1, 1, 1, 0.92f)), icon);
        const float textWidth = width - fs_ * 1.6f;
        draw->AddText(theme::fonts().heading, fs_ * 1.02f, ImVec2(p.x + fs_ * 0.8f, p.y + top + fs_ * 0.55f), theme::u32(theme::color::text),
                      fitText(theme::fonts().heading, fs_ * 1.02f, title, textWidth).c_str());
        draw->AddText(theme::fonts().ui, fs_ * 0.88f, ImVec2(p.x + fs_ * 0.8f, p.y + top + fs_ * 1.85f), theme::u32(theme::color::textDim),
                      fitText(theme::fonts().ui, fs_ * 0.88f, text, textWidth).c_str());
        if (hovered) draw->AddRect(p, b, theme::u32(theme::color::accent, 0.8f), r, 0, 1.5f);
        return pressed;
    }
}

Texture* EditorApp::thumbnailFor(const std::string& folder) {
    Thumbnail& t = thumbnails_[folder];
    // Reloaded when the file changes (checked every couple of seconds).
    if (t.stamp == 0 || thumbnailTimer_ <= 0.0f) {
        std::error_code ec;
        const fs::path file = Paths::fromUtf8(folder) / ".rift" / "thumbnail.png";
        const auto time = fs::last_write_time(file, ec);
        const long long stamp = ec ? -1 : static_cast<long long>(time.time_since_epoch().count());
        if (stamp != t.stamp) {
            t.stamp = stamp;
            t.texture.reset();
            if (stamp != -1) {
                auto tex = std::make_unique<Texture>(Paths::toUtf8(file));
                if (tex->isValid()) t.texture = std::move(tex);
            }
        }
    }
    return t.texture.get();
}

void EditorApp::drawWelcome(ImGuiID dockspace) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme::color::bgDark);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##welcome", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    // The panels' dock layout must stay alive while it is not shown.
    ImGui::DockSpace(dockspace, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_KeepAliveOnly);
    thumbnailTimer_ -= ImGui::GetIO().DeltaTime;

    const float fs_ = ImGui::GetFontSize();
    const float sideWidth = fs_ * 14.5f;

    // ---- side bar
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.063f, 0.067f, 0.082f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs_ * 0.9f, fs_ * 1.2f));
    ImGui::BeginChild("##side", ImVec2(sideWidth, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
    ImGui::PopStyleVar();
    const float navWidth = ImGui::GetContentRegionAvail().x;
    if (logo_ && logo_->isValid()) {
        ImGui::Image(static_cast<ImTextureID>(logo_->id()), ImVec2(fs_ * 2.6f, fs_ * 2.6f), ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine(0.0f, fs_ * 0.7f);
    }
    ImGui::BeginGroup();
    theme::pushHeading(1.55f);
    ImGui::TextUnformatted("RIFT");
    theme::pop();
    ImGui::TextDisabled("Engine " RIFT_VERSION_DISPLAY);
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, fs_ * 1.2f));
    if (navItem(ICON_FA_HOUSE, "Projects", homePage_ == 0, navWidth)) homePage_ = 0;
    if (navItem(ICON_FA_GRADUATION_CAP, "Learn", homePage_ == 1, navWidth)) homePage_ = 1;

    // Bottom of the side bar
    const float bottomItems = 3.0f + (updater_.state() == Updater::State::Available || updater_.state() == Updater::State::Ready ? 1.6f : 0.0f);
    ImGui::SetCursorPosY(std::max(ImGui::GetCursorPosY() + fs_, ImGui::GetWindowHeight() - fs_ * 1.2f - bottomItems * fs_ * 2.3f));
    if (updater_.state() == Updater::State::Available || updater_.state() == Updater::State::Ready) {
        const std::string text = ICON_FA_CIRCLE_ARROW_UP "  Update to " + updater_.release().version;
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::success.x, theme::color::success.y, theme::color::success.z, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(theme::color::success.x, theme::color::success.y, theme::color::success.z, 0.35f));
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::success);
        if (ImGui::Button(text.c_str(), ImVec2(navWidth, fs_ * 2.2f))) showUpdate_ = true;
        ImGui::PopStyleColor(3);
        ImGui::Dummy(ImVec2(0.0f, fs_ * 0.4f));
    }
    if (navItem(ICON_FA_SLIDERS, "Preferences", false, navWidth)) showPreferences_ = true;
    if (navItem(ICON_FA_CIRCLE_ARROW_UP, "Check for updates", false, navWidth)) {
        showUpdate_ = true;
        if (!updater_.busy() && updater_.state() != Updater::State::Ready) updater_.check(settings_.includePrereleases);
    }
    if (navItem(ICON_FA_GLOBE, "Website", false, navWidth)) Platform::openWithShell(RIFT_WEBSITE);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // ---- main area
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(fs_ * 2.4f, fs_ * 1.8f));
    ImGui::BeginChild("##home", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float width = std::min(ImGui::GetContentRegionAvail().x, fs_ * 78.0f);

    // Update news
    if (updater_.state() == Updater::State::Available && settings_.skippedVersion != updater_.release().version) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = fs_ * 3.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(p, ImVec2(p.x + width, p.y + h), theme::u32(ImVec4(theme::color::success.x, theme::color::success.y, theme::color::success.z, 0.12f)), fs_ * 0.5f);
        draw->AddRect(p, ImVec2(p.x + width, p.y + h), theme::u32(theme::color::success, 0.4f), fs_ * 0.5f);
        ImGui::SetCursorScreenPos(ImVec2(p.x + fs_, p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(theme::color::success, ICON_FA_GIFT);
        ImGui::SameLine();
        ImGui::Text("RIFT %s is available", updater_.release().version.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("(you have %s)", RIFT_VERSION_DISPLAY);
        ImGui::SameLine(width - fs_ * 13.0f);
        if (ImGui::Button("What's new")) showUpdate_ = true;
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::success.x * 0.8f, theme::color::success.y * 0.8f, theme::color::success.z * 0.8f, 1.0f));
        if (ImGui::Button(ICON_FA_DOWNLOAD "  Update")) {
            showUpdate_ = true;
            updater_.download();
        }
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + fs_ * 1.2f));
    }

    if (homePage_ == 1) drawHomeLearn(width);
    else drawHomeProjects(width);
    if (thumbnailTimer_ <= 0.0f) thumbnailTimer_ = 2.0f; // every card looked at its file this frame
    ImGui::EndChild();
    ImGui::End();
}

void EditorApp::drawHomeProjects(float width) {
    const float fs_ = ImGui::GetFontSize();
    std::error_code ec;

    // ---- header: greeting + the two main actions
    const bool first = settings_.recentProjects.empty();
    const float headerX = ImGui::GetCursorPosX();
    ImGui::BeginGroup();
    theme::pushHeading(2.0f);
    ImGui::TextUnformatted(first ? "Make your first game" : "Welcome back");
    theme::pop();
    ImGui::TextDisabled(first ? "Start from a template below, or open one of the examples to see how things work."
                              : "Pick up where you left off, or start something new.");
    ImGui::EndGroup();
    const float buttonsWidth = ImGui::CalcTextSize(ICON_FA_FOLDER_OPEN "  Open" ICON_FA_PLUS "  New project").x + fs_ * 4.4f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + fs_, headerX + width - buttonsWidth));
    const float buttonsY = ImGui::GetCursorPosY() + fs_ * 0.5f;
    ImGui::SetCursorPosY(buttonsY);
    if (bigButton(ICON_FA_FOLDER_OPEN "  Open", false, fs_ * 2.4f)) openProjectDialog();
    theme::tooltip("Open a project folder (the one with project.json)");
    ImGui::SameLine();
    ImGui::SetCursorPosY(buttonsY); // SameLine goes back to the top of the line
    if (bigButton(ICON_FA_PLUS "  New project", true, fs_ * 2.4f)) openNewProject();
    ImGui::Dummy(ImVec2(0.0f, fs_ * 1.2f));

    // ---- start something new: templates and examples
    theme::heading("Start something new", width);
    struct Start { const char* id; const char* icon; ImVec4 color; const char* title; const char* text; int templ; fs::path example; };
    std::vector<Start> starts = {
        { "empty", ICON_FA_FILE, ImVec4(0.45f, 0.48f, 0.56f, 1.0f), "Empty", "Just a spawn point", 0, {} },
        { "baseplate", ICON_FA_TABLE_CELLS_LARGE, ImVec4(0.23f, 0.51f, 0.96f, 1.0f), "Baseplate", "A big floor to build on", 1, {} },
        { "obby", ICON_FA_PERSON_RUNNING, ImVec4(0.55f, 0.36f, 0.96f, 1.0f), "Obby", "Obstacle course with scripts", 2, {} },
    };
    const fs::path examples = Paths::engineData().parent_path() / "examples";
    if (fs::is_directory(examples, ec)) {
        static const ImVec4 exampleColors[] = { { 0.13f, 0.77f, 0.37f, 1.0f }, { 0.98f, 0.45f, 0.09f, 1.0f }, { 0.93f, 0.28f, 0.60f, 1.0f } };
        int k = 0;
        for (const auto& item : fs::directory_iterator(examples, ec)) {
            if (!fs::exists(item.path() / "project.json", ec)) continue;
            starts.push_back({ nullptr, ICON_FA_GAMEPAD, exampleColors[k++ % 3], nullptr, nullptr, -1, item.path() });
        }
    }
    const float tileWidth = fs_ * 11.5f;
    const float gap = fs_ * 0.9f;
    const int perRow = std::max(1, static_cast<int>((width + gap) / (tileWidth + gap)));
    for (size_t k = 0; k < starts.size(); ++k) {
        const Start& s = starts[k];
        if (k % static_cast<size_t>(perRow) != 0) ImGui::SameLine(0.0f, gap);
        const std::string name = s.example.empty() ? std::string() : Paths::toUtf8(s.example.filename());
        const std::string id = s.id ? s.id : "example:" + name;
        const std::pair<std::string, std::string> info = s.example.empty() ? std::pair<std::string, std::string>() : exampleInfo(s.example);
        if (tile(id.c_str(), s.icon, s.color, s.title ? s.title : info.first.c_str(), s.text ? s.text : info.second.c_str(), tileWidth)) {
            if (s.templ >= 0) {
                openNewProject();
                newProjectTemplate_ = s.templ;
            } else {
                openExample(s.example);
            }
        }
    }
    ImGui::Dummy(ImVec2(0.0f, fs_ * 1.2f));

    // ---- recent projects
    const ImVec2 headingPos = ImGui::GetCursorPos();
    theme::heading("Recent projects", width);
    if (!first) {
        // Search and sort sit on the heading's line, at the right.
        const ImVec2 after = ImGui::GetCursorPos();
        const float searchWidth = fs_ * 13.0f, sortWidth = fs_ * 8.5f;
        ImGui::SetCursorPos(ImVec2(headingPos.x + width - searchWidth - sortWidth - ImGui::GetStyle().ItemSpacing.x, headingPos.y - ImGui::GetStyle().FramePadding.y));
        ImGui::SetNextItemWidth(searchWidth);
        ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Search", homeSearch_, sizeof(homeSearch_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(sortWidth);
        const char* sorts[] = { "Last opened", "Name" };
        ImGui::Combo("##sort", &homeSort_, sorts, 2);
        ImGui::SetCursorPos(after);
    }

    std::vector<std::string> list;
    for (const std::string& path : settings_.recentProjects) {
        if (containsNoCase(Paths::toUtf8(Paths::fromUtf8(path).filename()), homeSearch_) || containsNoCase(path, homeSearch_)) list.push_back(path);
    }
    if (homeSort_ == 1) {
        std::sort(list.begin(), list.end(), [](const std::string& a, const std::string& b) {
            std::string x = Paths::toUtf8(Paths::fromUtf8(a).filename()), y = Paths::toUtf8(Paths::fromUtf8(b).filename());
            std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return x < y;
        });
    }
    if (first) {
        ImGui::Dummy(ImVec2(0.0f, fs_ * 0.6f));
        ImGui::TextDisabled(ICON_FA_FOLDER_OPEN "  Projects you open show up here, with a picture of the scene.");
    } else if (list.empty()) {
        ImGui::TextDisabled("No project matches \"%s\".", homeSearch_);
    }

    const float cardWidth = fs_ * 15.0f;
    const float thumbHeight = cardWidth * 9.0f / 16.0f;
    const float textHeight = fs_ * 3.6f;
    const int columns = std::max(1, static_cast<int>((width + gap) / (cardWidth + gap)));
    std::string remove;
    fs::path openNow; // opened after the loop: opening reorders the recent list
    for (size_t k = 0; k < list.size(); ++k) {
        const std::string& path = list[k];
        const fs::path p = Paths::fromUtf8(path);
        const bool exists = fs::exists(p / "project.json", ec);
        const std::string name = Paths::toUtf8(p.filename());
        if (k % static_cast<size_t>(columns) != 0) ImGui::SameLine(0.0f, gap);
        else if (k) ImGui::Dummy(ImVec2(0.0f, gap * 0.3f));

        ImGui::PushID(path.c_str());
        const ImVec2 a = ImGui::GetCursorScreenPos();
        const ImVec2 b(a.x + cardWidth, a.y + thumbHeight + textHeight);
        if (ImGui::InvisibleButton("##card", ImVec2(cardWidth, thumbHeight + textHeight)) && exists) openNow = p;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && exists) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float r = fs_ * 0.6f;
        draw->AddRectFilled(a, b, theme::u32(hovered ? theme::color::frameHover : theme::color::panel), r);

        // Picture: the scene as last saved, or a coloured placeholder.
        const ImVec2 ta = a, tb(b.x, a.y + thumbHeight);
        Texture* thumb = exists ? thumbnailFor(path) : nullptr;
        if (thumb) {
            draw->AddImageRounded(static_cast<ImTextureID>(thumb->id()), ta, tb, ImVec2(0, 1), ImVec2(1, 0), IM_COL32_WHITE, r, ImDrawFlags_RoundCornersTop);
        } else {
            const ImVec4 c = exists ? nameColor(name) : ImVec4(0.2f, 0.2f, 0.22f, 1.0f);
            draw->AddRectFilled(ta, tb, theme::u32(c), r, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilledMultiColor(ImVec2(ta.x, ta.y + thumbHeight * 0.45f), tb, IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 90), IM_COL32(0, 0, 0, 90));
            const std::string letter = name.empty() ? std::string("?") : std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
            const float size = fs_ * 3.2f;
            const ImVec2 t = theme::fonts().heading->CalcTextSizeA(size, FLT_MAX, 0.0f, letter.c_str());
            draw->AddText(theme::fonts().heading, size, ImVec2(ta.x + (cardWidth - t.x) * 0.5f, ta.y + (thumbHeight - t.y) * 0.5f), IM_COL32(255, 255, 255, 200), letter.c_str());
        }
        // Git badge
        std::error_code gec;
        if (exists && fs::exists(p / ".git", gec)) {
            const ImVec2 bp(tb.x - fs_ * 1.9f, ta.y + fs_ * 0.5f);
            draw->AddRectFilled(bp, ImVec2(bp.x + fs_ * 1.4f, bp.y + fs_ * 1.4f), IM_COL32(10, 10, 14, 170), fs_ * 0.3f);
            draw->AddText(ImVec2(bp.x + fs_ * 0.28f, bp.y + fs_ * 0.18f), theme::u32(theme::color::text), ICON_FA_CODE_BRANCH);
        }
        // Name, folder and when
        const float textWidth = cardWidth - fs_ * 1.6f;
        const std::string shownName = fitText(theme::fonts().heading, fs_ * 1.02f, name, textWidth);
        draw->AddText(theme::fonts().heading, fs_ * 1.02f, ImVec2(a.x + fs_ * 0.8f, tb.y + fs_ * 0.55f), theme::u32(exists ? theme::color::text : theme::color::textDim), shownName.c_str());
        auto opened = settings_.recentOpened.find(path);
        const bool known = opened != settings_.recentOpened.end() && opened->second > 0;
        const std::string when = !exists ? std::string("Missing: moved or deleted") : known ? "Opened " + timeAgo(opened->second) : fitText(theme::fonts().ui, fs_ * 0.86f, path, textWidth, true);
        draw->AddText(theme::fonts().ui, fs_ * 0.86f, ImVec2(a.x + fs_ * 0.8f, tb.y + fs_ * 1.95f), theme::u32(exists ? theme::color::textDim : theme::color::warning), when.c_str());
        if (hovered) {
            draw->AddRect(a, b, theme::u32(theme::color::accent, 0.85f), r, 0, 1.5f);
            ImGui::SetTooltip("%s", path.c_str());
        }

        if (ImGui::BeginPopupContextItem("##cardMenu")) {
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Open", nullptr, false, exists)) openNow = p;
            if (ImGui::MenuItem(ICON_FA_FOLDER "  Show in Explorer", nullptr, false, exists)) Platform::revealInExplorer(path);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy path")) Platform::setClipboard(path);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_XMARK "  Remove from the list")) remove = path;
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(0.0f, fs_ * 2.0f));

    if (!remove.empty()) {
        settings_.recentProjects.erase(std::remove(settings_.recentProjects.begin(), settings_.recentProjects.end(), remove), settings_.recentProjects.end());
        settings_.recentOpened.erase(remove);
        settings_.save();
    }
    if (!openNow.empty()) editor_->openProject(openNow);
}

void EditorApp::drawHomeLearn(float width) {
    const float fs_ = ImGui::GetFontSize();
    theme::pushHeading(2.0f);
    ImGui::TextUnformatted("Learn RIFT");
    theme::pop();
    ImGui::TextDisabled("Guides, the scripting reference and help. If you know Roblox Studio, you already know most of it.");
    ImGui::Dummy(ImVec2(0.0f, fs_ * 1.2f));

    struct Link { const char* icon; ImVec4 color; const char* title; const char* text; std::string url; };
    const std::string docs = RIFT_DOCS_URL;
    const Link links[] = {
        { ICON_FA_BOOK_OPEN, { 0.55f, 0.36f, 0.96f, 1.0f }, "Editor guide", "Projects, building, playing, shipping", docs + "GUIDE.md" },
        { ICON_FA_CODE, { 0.23f, 0.51f, 0.96f, 1.0f }, "Scripting reference", "The Lua API, Roblox style", docs + "SCRIPTING.md" },
        { ICON_FA_TOWER_BROADCAST, { 0.98f, 0.45f, 0.09f, 1.0f }, "Client and server", "LocalScripts and RemoteEvents", docs + "SCRIPTING.md#client-and-server" },
        { ICON_FA_GEARS, { 0.08f, 0.72f, 0.65f, 1.0f }, "Engine internals", "How RIFT works inside", docs + "ENGINE.md" },
        { ICON_FA_KEYBOARD, { 0.45f, 0.48f, 0.56f, 1.0f }, "Keyboard shortcuts", "Every key in one list", std::string() },
        { ICON_FA_GLOBE, { 0.13f, 0.77f, 0.37f, 1.0f }, "Website", "News and downloads", RIFT_WEBSITE },
        { ICON_FA_BUG, { 0.94f, 0.27f, 0.27f, 1.0f }, "Report a problem", "Tell us what went wrong", std::string("https://github.com/") + RIFT_GITHUB_REPO + "/issues" },
        { ICON_FA_CODE_BRANCH, { 0.93f, 0.28f, 0.60f, 1.0f }, "Source code", "RIFT on GitHub", std::string("https://github.com/") + RIFT_GITHUB_REPO },
    };
    const float tileWidth = fs_ * 13.5f, gap = fs_ * 0.9f;
    const int perRow = std::max(1, static_cast<int>((width + gap) / (tileWidth + gap)));
    int k = 0;
    for (const Link& l : links) {
        if (k % perRow != 0) ImGui::SameLine(0.0f, gap);
        if (tile(l.title, l.icon, l.color, l.title, l.text, tileWidth)) {
            if (l.url.empty()) showShortcuts_ = true;
            else Platform::openWithShell(l.url);
        }
        ++k;
    }
}

} // namespace rift
