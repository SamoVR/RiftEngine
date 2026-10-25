#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/Paths.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace rift {

namespace fs = std::filesystem;

namespace {
    constexpr float kSnap = 0.05f;   // keyframe times snap to this (Shift: free)
    constexpr float kMatch = 1e-3f;  // two times closer than this are the same keyframe

    const std::vector<std::string>& styleNames() {
        static const std::vector<std::string> n = { "Linear", "Constant", "Elastic", "Cubic", "Bounce", "CubicV2" };
        return n;
    }
    const std::vector<std::string>& directionNames() {
        static const std::vector<std::string> n = { "In", "Out", "InOut" };
        return n;
    }
    const std::vector<std::string>& priorityNames() {
        static const std::vector<std::string> n = { "Idle", "Movement", "Action", "Action2", "Action3", "Action4", "Core" };
        return n;
    }

    // Body parts in a sensible order, then anything else alphabetically.
    int jointOrder(const std::string& name) {
        static const char* const order[] = { "Torso", "Head", "Right Arm", "Left Arm", "Right Leg", "Left Leg" };
        for (int i = 0; i < 6; ++i) if (name == order[i]) return i;
        return 100;
    }

    bool sameTransform(const glm::mat4& a, const glm::mat4& b) {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (std::abs(a[c][r] - b[c][r]) > 2e-3f) return false;
        return true;
    }

    // A diamond marker (keyframes on the timeline).
    void diamond(ImDrawList* dl, const ImVec2& c, float r, ImU32 fill, ImU32 outline) {
        const ImVec2 pts[4] = { ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y) };
        dl->AddConvexPolyFilled(pts, 4, fill);
        dl->AddPolyline(pts, 4, outline, ImDrawFlags_Closed, 1.2f);
    }
}

// ============================================================================
//  Rig and clip
// ============================================================================
Instance* AnimationPanel::rig(Editor& editor) const {
    if (!rigId_ || editor.playing()) return nullptr;
    Instance* r = editor.editScene().find(rigId_);
    return r && r->isDescendantOf(&editor.editScene().workspace()) ? r : nullptr;
}

void AnimationPanel::attach(Editor& editor, Instance* model) {
    detach(editor);
    if (!model) return;
    rigId_ = model->id;
    joints_.clear();
    for (const auto& [name, motor] : rigJoints(*model)) joints_.push_back(name);
    std::sort(joints_.begin(), joints_.end(), [](const std::string& a, const std::string& b) {
        const int oa = jointOrder(a), ob = jointOrder(b);
        return oa != ob ? oa < ob : a < b;
    });
    if (clip_.keyframes.empty()) newClip();
    applied_.clear();
    editor.animatingRig = rigId_;
}

void AnimationPanel::detach(Editor& editor) {
    // Back to the rest pose.
    if (Instance* r = rig(editor)) {
        for (auto& [name, motor] : rigJoints(*r)) motor->jointTransform = CFrameValue{};
        solveJoints(editor.editScene(), *r);
    }
    rigId_ = 0;
    playing_ = false;
    applied_.clear();
    editor.animatingRig = 0;
}

void AnimationPanel::newClip() {
    clip_ = AnimClip{};
    clip_.name = "NewAnimation";
    clip_.priority = AnimationPriority::Action;
    AnimKeyframe start;
    for (const std::string& j : joints_) start.poses[j] = AnimPose{};
    clip_.keyframes.push_back(start);
    clip_.prepare();
    name_ = clip_.name;
    time_ = 0.0f;
    selectedTime_ = 0.0f;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
}

void AnimationPanel::changed() {
    clip_.prepare();
    dirty_ = true;
}

void AnimationPanel::snapshot() {
    undo_.push_back(clip_);
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
}

AnimKeyframe* AnimationPanel::keyframeAt(float time, bool create) {
    for (AnimKeyframe& k : clip_.keyframes) if (std::abs(k.time - time) < kMatch) return &k;
    if (!create) return nullptr;
    AnimKeyframe k;
    k.time = time;
    clip_.keyframes.push_back(k);
    clip_.prepare();
    for (AnimKeyframe& x : clip_.keyframes) if (std::abs(x.time - time) < kMatch) return &x;
    return nullptr;
}

// Poses the rig at the current time; a joint the user moved (gizmo,
// Properties) since the last frame becomes a key at the current time.
void AnimationPanel::pose(Editor& editor) {
    Instance* r = rig(editor);
    if (!r) return;
    Scene& scene = editor.editScene();
    auto joints = rigJoints(*r);
    if (!playing_ && !applied_.empty()) {
        bool keyed = false;
        for (auto& [name, motor] : joints) {
            const Instance* p0 = scene.find(motor->attachment0.id);
            const Instance* p1 = scene.find(motor->attachment1.id);
            auto it = applied_.find(name);
            if (!p0 || !p1 || it == applied_.end()) continue;
            if (sameTransform(p1->worldFrame(), it->second)) continue;
            // Moved by hand: the joint transform that puts it there.
            const glm::mat4 t = glm::inverse(motor->c0.matrix()) * glm::inverse(p0->worldFrame()) * p1->worldFrame() * motor->c1.matrix();
            if (!keyed) { snapshot(); keyed = true; }
            AnimKeyframe* k = keyframeAt(time_, true);
            AnimPose pose = poseFromMatrix(t);
            if (auto old = k->poses.find(name); old != k->poses.end()) {
                pose.style = old->second.style;
                pose.direction = old->second.direction;
                pose.weight = old->second.weight;
            }
            k->poses[name] = pose;
            selectedTime_ = time_;
        }
        if (keyed) changed();
    }
    std::unordered_map<std::string, AnimPose> sampled;
    sampleClip(clip_, time_, sampled);
    for (auto& [name, motor] : joints) {
        auto it = sampled.find(name);
        motor->jointTransform = it != sampled.end() ? CFrameValue::fromMatrix(poseMatrix(it->second)) : CFrameValue{};
    }
    solveJoints(scene, *r);
    applied_.clear();
    for (auto& [name, motor] : joints) {
        if (const Instance* p1 = scene.find(motor->attachment1.id)) applied_[name] = p1->worldFrame();
    }
}

fs::path AnimationPanel::fileFor(Editor& editor, const std::string& name) const {
    std::string safe;
    for (char c : name) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') ? c : '_';
    if (safe.empty()) safe = "Animation";
    return editor.project().assetsFolder() / "animations" / Paths::fromUtf8(safe + ".anim");
}

bool AnimationPanel::save(Editor& editor) {
    if (!editor.hasProject()) return false;
    clip_.name = name_;
    const fs::path file = fileFor(editor, name_);
    std::string error;
    if (!saveClip(clip_, file, &error)) {
        editor.notify("Could not save the animation: " + error, NoticeLevel::Error);
        return false;
    }
    dirty_ = false;
    editor.notify("Saved " + Paths::toStored(file) + " (its AnimationId)", NoticeLevel::Success);
    return true;
}

void AnimationPanel::open(Editor& editor, const fs::path& file) {
    std::string error;
    auto loaded = loadClipFile(file, &error);
    if (!loaded) {
        editor.notify("Could not open the animation: " + error, NoticeLevel::Error);
        return;
    }
    clip_ = *loaded;
    clip_.prepare();
    name_ = clip_.name.empty() ? Paths::toUtf8(file.stem()) : clip_.name;
    time_ = 0.0f;
    selectedTime_ = clip_.keyframes.empty() ? -1.0f : clip_.keyframes.front().time;
    dirty_ = false;
    undo_.clear();
    redo_.clear();
    applied_.clear();
}

// ============================================================================
//  Frame
// ============================================================================
void AnimationPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    // The rig deleted: let go of it (while the game runs it just waits).
    if (rigId_ && !editor.playing() && !rig(editor)) {
        rigId_ = 0;
        playing_ = false;
        applied_.clear();
        editor.animatingRig = 0;
    }
    if (playing_ && rig(editor)) {
        const float length = std::max(clip_.length(), 0.01f);
        time_ += ui.dt;
        if (time_ > length) time_ = loopPreview_ ? std::fmod(time_, length) : (playing_ = false, length);
    }
    pose(editor);

    if (!ImGui::Begin(ICON_FA_PERSON_RUNNING "  Animation Editor###Animation", open)) {
        ImGui::End();
        return;
    }
    if (editor.playing()) {
        ImGui::Spacing();
        ImGui::TextColored(theme::color::textDim, ICON_FA_CIRCLE_INFO "  Stop the game to edit animations.");
        ImGui::End();
        return;
    }
    drawToolbar(ui);
    if (!rig(editor)) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, theme::color::textDim);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(ICON_FA_CIRCLE_INFO "  Select a model with a Humanoid (or an AnimationController) and Motor6D joints, then click Edit. "
                               "Insert > Animation > Character Rig makes one shaped like the players' characters.");
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::End();
        return;
    }
    drawTimeline(ui);
    ImGui::End();
}

void AnimationPanel::drawToolbar(UiContext& ui) {
    Editor& editor = ui.editor;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;
    auto separator = [&] {
        ImGui::SameLine(0.0f, spacing * 1.5f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + 3.0f), ImVec2(p.x, p.y + ImGui::GetFrameHeight() - 3.0f), theme::u32(theme::color::border));
        ImGui::Dummy(ImVec2(1.0f, ImGui::GetFrameHeight()));
        ImGui::SameLine(0.0f, spacing * 1.5f);
    };

    // ---- the rig
    Instance* current = rig(editor);
    Instance* selected = nullptr;
    for (Instance* i : editor.selectedInstances()) {
        for (Instance* m = i; m && !selected; m = m->parent) {
            if (m->cls != ClassId::Model) continue;
            for (Instance* c : m->children)
                if (c->cls == ClassId::Humanoid || c->cls == ClassId::AnimationController) { selected = m; break; }
        }
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::classColor(ClassId::Humanoid), ICON_FA_PERSON);
    ImGui::SameLine();
    if (current) ImGui::TextUnformatted(current->name.c_str());
    else ImGui::TextColored(theme::color::textDim, "No rig");
    ImGui::SameLine();
    ImGui::BeginDisabled(!selected || selected == current);
    if (ImGui::Button(ICON_FA_PEN_TO_SQUARE "  Edit")) attach(editor, selected);
    ImGui::EndDisabled();
    theme::tooltip(selected ? "Animate the selected model" : "Select a model with a Humanoid first");
    if (current) {
        ImGui::SameLine(0.0f, 2.0f);
        if (theme::iconButton(ICON_FA_XMARK, "Stop editing (the rig goes back to its rest pose)")) detach(editor);
    }
    separator();

    // ---- the clip: name, files
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::classColor(ClassId::Animation), ICON_FA_FILM);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    ImGui::InputTextWithHint("##name", "Name", &name_);
    if (ImGui::IsItemDeactivatedAfterEdit()) dirty_ = true;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_FOLDER_OPEN, "Open an animation")) ImGui::OpenPopup("##openAnim");
    if (ImGui::BeginPopup("##openAnim")) {
        ImGui::SeparatorText("This game");
        bool any = false;
        std::error_code ec;
        const fs::path folder = editor.hasProject() ? editor.project().assetsFolder() / "animations" : fs::path();
        if (!folder.empty() && fs::exists(folder, ec)) {
            for (const auto& e : fs::directory_iterator(folder, ec)) {
                if (e.path().extension() != ".anim") continue;
                any = true;
                if (ImGui::MenuItem((ICON_FA_FILM "  " + Paths::toUtf8(e.path().stem())).c_str())) open(editor, e.path());
            }
        }
        if (!any) ImGui::TextDisabled("No animations saved yet");
        ImGui::SeparatorText("Built in (open a copy)");
        for (const char* n : { "idle", "walk", "run", "jump", "fall", "swim", "toolnone" }) {
            if (ImGui::MenuItem((std::string(ICON_FA_FILM "  ") + n).c_str())) {
                open(editor, Paths::engineData() / "animations" / (std::string(n) + ".anim"));
                name_ = std::string("My") + static_cast<char>(std::toupper(static_cast<unsigned char>(n[0]))) + (n + 1);
                dirty_ = true;
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_FILE, "New animation")) newClip();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(!editor.hasProject());
    if (theme::iconButton(ICON_FA_FLOPPY_DISK, dirty_ ? "Save (unsaved changes) into assets/animations" : "Save into assets/animations", dirty_)) save(editor);
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_COPY, "Copy its AnimationId (save first)")) {
        const std::string id = Paths::toStored(fileFor(editor, name_));
        ImGui::SetClipboardText(id.c_str());
        editor.notify("Copied " + id);
    }
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_PLUS, "Save, and add an Animation object with this AnimationId to ReplicatedStorage")) {
        if (save(editor)) {
            const std::string id = Paths::toStored(fileFor(editor, name_));
            const std::string name = name_;
            editor.insertWith(ClassId::Animation, editor.editScene().service(ClassId::ReplicatedStorage), [&](Instance& a) {
                a.name = name;
                a.animationId = id;
            }, "Insert Animation");
        }
    }
    ImGui::EndDisabled();
    if (!current) return;

    // ---- second row: transport, clip settings, keys
    const float length = clip_.length();
    if (theme::iconButton(ICON_FA_BACKWARD_STEP, "To the start")) { time_ = 0.0f; playing_ = false; }
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(playing_ ? ICON_FA_PAUSE : ICON_FA_PLAY, playing_ ? "Pause (Space)" : "Play (Space)", playing_)) playing_ = !playing_;
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_FORWARD_STEP, "To the end")) { time_ = length; playing_ = false; }
    ImGui::SameLine(0.0f, 2.0f);
    if (theme::iconButton(ICON_FA_REPEAT, loopPreview_ ? "Preview loops" : "Preview plays once", loopPreview_)) loopPreview_ = !loopPreview_;
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%.2f / %.2f s", static_cast<double>(time_), static_cast<double>(length));
    separator();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Priority");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6.0f);
    int priority = static_cast<int>(clip_.priority);
    if (ImGui::BeginCombo("##priority", priorityNames()[static_cast<size_t>(priority)].c_str())) {
        for (int k = 0; k < static_cast<int>(priorityNames().size()); ++k) {
            if (ImGui::Selectable(priorityNames()[static_cast<size_t>(k)].c_str(), k == priority)) {
                snapshot();
                clip_.priority = static_cast<AnimationPriority>(k);
                changed();
            }
        }
        ImGui::EndCombo();
    }
    theme::tooltip("Higher priorities override lower ones (Core < Idle < Movement < Action): attacks over walking.");
    ImGui::SameLine();
    bool loop = clip_.loop;
    if (ImGui::Checkbox("Loop", &loop)) { snapshot(); clip_.loop = loop; changed(); }
    separator();

    // Keys at the current time.
    if (ImGui::Button(ICON_FA_DIAMOND "  Key All")) {
        snapshot();
        std::unordered_map<std::string, AnimPose> sampled;
        sampleClip(clip_, time_, sampled);
        AnimKeyframe* k = keyframeAt(time_, true);
        for (const std::string& j : joints_) k->poses[j] = sampled.count(j) ? sampled[j] : AnimPose{};
        selectedTime_ = time_;
        changed();
    }
    theme::tooltip("A keyframe at the current time with every joint where it is now. (Moving a limb with the Move or Rotate tool keys it by itself.)");
    ImGui::SameLine(0.0f, 2.0f);
    AnimKeyframe* sel = selectedTime_ >= 0.0f ? keyframeAt(selectedTime_, false) : nullptr;
    ImGui::BeginDisabled(!sel);
    if (theme::iconButton(ICON_FA_TRASH_CAN, "Delete the selected keyframe (Del)")) deleteSelected();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Easing");
    ImGui::SameLine();
    int styleIndex = 0, dirIndex = 0;
    if (sel && !sel->poses.empty()) {
        styleIndex = static_cast<int>(sel->poses.begin()->second.style);
        dirIndex = static_cast<int>(sel->poses.begin()->second.direction);
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 5.5f);
    if (ImGui::BeginCombo("##style", styleNames()[static_cast<size_t>(styleIndex)].c_str())) {
        for (int k = 0; k < static_cast<int>(styleNames().size()); ++k) {
            if (ImGui::Selectable(styleNames()[static_cast<size_t>(k)].c_str(), k == styleIndex) && sel) {
                snapshot();
                for (auto& [n, p] : sel->poses) p.style = static_cast<PoseEasingStyle>(k);
                changed();
            }
        }
        ImGui::EndCombo();
    }
    theme::tooltip("How the selected keyframe moves on to the next one");
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 4.5f);
    if (ImGui::BeginCombo("##dir", directionNames()[static_cast<size_t>(dirIndex)].c_str())) {
        for (int k = 0; k < static_cast<int>(directionNames().size()); ++k) {
            if (ImGui::Selectable(directionNames()[static_cast<size_t>(k)].c_str(), k == dirIndex) && sel) {
                snapshot();
                for (auto& [n, p] : sel->poses) p.direction = static_cast<PoseEasingDirection>(k);
                changed();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    std::string keyName = sel ? sel->name : std::string();
    ImGui::InputTextWithHint("##keyName", "Event name", &keyName);
    if (ImGui::IsItemDeactivatedAfterEdit() && sel) {
        snapshot();
        sel->name = keyName;
        changed();
    }
    theme::tooltip("Name the keyframe to make track.KeyframeReached fire with it");
    ImGui::EndDisabled();
    separator();
    ImGui::BeginDisabled(undo_.empty());
    if (theme::iconButton(ICON_FA_ROTATE_LEFT, "Undo an animation change")) {
        redo_.push_back(clip_);
        clip_ = undo_.back();
        undo_.pop_back();
        changed();
        applied_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::BeginDisabled(redo_.empty());
    if (theme::iconButton(ICON_FA_ROTATE_RIGHT, "Redo")) {
        undo_.push_back(clip_);
        clip_ = redo_.back();
        redo_.pop_back();
        changed();
        applied_.clear();
    }
    ImGui::EndDisabled();
}

void AnimationPanel::deleteSelected() {
    if (selectedTime_ < 0.0f) return;
    snapshot();
    if (!selectedJoint_.empty()) {
        // One joint's key only.
        if (AnimKeyframe* k = keyframeAt(selectedTime_, false)) k->poses.erase(selectedJoint_);
    } else {
        clip_.keyframes.erase(std::remove_if(clip_.keyframes.begin(), clip_.keyframes.end(),
                                             [&](const AnimKeyframe& k) { return std::abs(k.time - selectedTime_) < kMatch; }),
                              clip_.keyframes.end());
    }
    clip_.keyframes.erase(std::remove_if(clip_.keyframes.begin(), clip_.keyframes.end(),
                                         [](const AnimKeyframe& k) { return k.poses.empty() && k.name.empty(); }),
                          clip_.keyframes.end());
    selectedTime_ = -1.0f;
    selectedJoint_.clear();
    changed();
    applied_.clear();
}

void AnimationPanel::drawTimeline(UiContext& ui) {
    (void)ui;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::Spacing();
    const float rowH = ImGui::GetFrameHeight();
    const float nameW = ImGui::GetFontSize() * 8.5f;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float trackW = std::max(width - nameW, 50.0f);
    const float viewLength = std::max({ clip_.length() + 0.5f, 2.0f, time_ + 0.25f });
    const float pxPerSecond = (trackW - ImGui::CalcTextSize("0.00").x - 12.0f) / viewLength; // room for the last label
    const float x0 = origin.x + nameW + 8.0f;
    auto xOf = [&](float t) { return x0 + t * pxPerSecond; };
    auto tOf = [&](float x) { return std::max(0.0f, (x - x0) / pxPerSecond); };
    auto snap = [&](float t) { return ImGui::GetIO().KeyShift ? t : std::round(t / kSnap) * kSnap; };
    const int rows = static_cast<int>(joints_.size()) + 2; // ruler, keyframes, joints
    const float height = rowH * static_cast<float>(rows);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 accent = theme::u32(theme::color::accentHover);

    // Backgrounds.
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height), theme::u32(theme::color::bgDark), style.FrameRounding);
    for (int r = 1; r < rows; ++r) {
        const float y = origin.y + rowH * static_cast<float>(r);
        if (r % 2 == 0) dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + width, y + rowH), IM_COL32(255, 255, 255, 6));
        dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + width, y), theme::u32(theme::color::border));
    }
    dl->AddLine(ImVec2(origin.x + nameW, origin.y), ImVec2(origin.x + nameW, origin.y + height), theme::u32(theme::color::border));

    // Ruler: ticks every 0.1 s, labels every 0.5 s (or 1 s when crowded).
    const float labelStep = pxPerSecond * 0.5f < ImGui::CalcTextSize("0.00").x * 1.6f ? 1.0f : 0.5f;
    for (int i = 0; i <= static_cast<int>(viewLength * 10.0f); ++i) {
        const float t = static_cast<float>(i) * 0.1f;
        const float x = xOf(t);
        const bool label = std::abs(std::fmod(t + 1e-4f, labelStep)) < 2e-3f;
        dl->AddLine(ImVec2(x, origin.y + rowH * (label ? 0.45f : 0.7f)), ImVec2(x, origin.y + rowH), theme::u32(theme::color::textDim, label ? 0.8f : 0.4f));
        if (label) {
            char text[16];
            std::snprintf(text, sizeof(text), "%.1f", static_cast<double>(t));
            dl->AddText(ImVec2(x + 3.0f, origin.y + 1.0f), theme::u32(theme::color::textDim), text);
        }
    }
    // The end of the clip.
    const float endX = xOf(clip_.length());
    dl->AddRectFilled(ImVec2(endX, origin.y + rowH), ImVec2(origin.x + width, origin.y + height), IM_COL32(0, 0, 0, 50));

    // Names.
    auto rowY = [&](int r) { return origin.y + rowH * static_cast<float>(r); };
    dl->AddText(ImVec2(origin.x + style.FramePadding.x, rowY(1) + style.FramePadding.y), theme::u32(theme::color::text), ICON_FA_DIAMOND "  Keyframes");
    for (size_t j = 0; j < joints_.size(); ++j) {
        const bool sel = joints_[j] == selectedJoint_;
        dl->AddText(ImVec2(origin.x + style.FramePadding.x + ImGui::GetFontSize() * 0.6f, rowY(static_cast<int>(j) + 2) + style.FramePadding.y),
                    sel ? accent : theme::u32(theme::color::textDim), joints_[j].c_str());
    }

    // Interaction: one invisible button over the whole area.
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##timeline", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const int mouseRow = static_cast<int>((mouse.y - origin.y) / rowH);
    // What is under the mouse: a keyframe diamond?
    int hitKey = -1;
    std::string hitJoint;
    if (hovered && mouse.x > origin.x + nameW && mouseRow >= 1) {
        for (size_t k = 0; k < clip_.keyframes.size(); ++k) {
            const AnimKeyframe& key = clip_.keyframes[k];
            if (std::abs(mouse.x - xOf(key.time)) > rowH * 0.4f) continue;
            if (mouseRow == 1) { hitKey = static_cast<int>(k); break; }
            const size_t j = static_cast<size_t>(mouseRow - 2);
            if (j < joints_.size() && key.poses.count(joints_[j])) { hitKey = static_cast<int>(k); hitJoint = joints_[j]; break; }
        }
    }
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (mouseRow == 0 || (hitKey < 0 && mouse.x > origin.x + nameW)) {
            scrubbing_ = true;
            playing_ = false;
        } else if (hitKey >= 0) {
            selectedTime_ = clip_.keyframes[static_cast<size_t>(hitKey)].time;
            selectedJoint_ = hitJoint;
            time_ = selectedTime_;
            draggingKey_ = true;
            dragFrom_ = selectedTime_;
            dragSnapshot_ = false;
        } else if (mouse.x <= origin.x + nameW && mouseRow >= 2 && static_cast<size_t>(mouseRow - 2) < joints_.size()) {
            selectedJoint_ = joints_[static_cast<size_t>(mouseRow - 2)];
        }
    }
    if (scrubbing_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) time_ = std::min(snap(tOf(mouse.x)), viewLength);
        else scrubbing_ = false;
    }
    if (draggingKey_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const float target = snap(tOf(mouse.x));
            const bool free = !keyframeAt(target, false) || std::abs(target - selectedTime_) < kMatch;
            if (free && std::abs(target - selectedTime_) >= kMatch) {
                if (!dragSnapshot_) { snapshot(); dragSnapshot_ = true; }
                if (!selectedJoint_.empty()) {
                    // Move one joint's key to another time.
                    AnimKeyframe* from = keyframeAt(selectedTime_, false);
                    if (from && from->poses.count(selectedJoint_)) {
                        const AnimPose p = from->poses[selectedJoint_];
                        from->poses.erase(selectedJoint_);
                        keyframeAt(target, true)->poses[selectedJoint_] = p;
                        clip_.keyframes.erase(std::remove_if(clip_.keyframes.begin(), clip_.keyframes.end(),
                                                             [](const AnimKeyframe& k) { return k.poses.empty() && k.name.empty(); }),
                                              clip_.keyframes.end());
                    }
                } else if (AnimKeyframe* k = keyframeAt(selectedTime_, false)) {
                    k->time = target;
                }
                selectedTime_ = target;
                time_ = target;
                changed();
                applied_.clear();
            }
        } else {
            draggingKey_ = false;
        }
    }
    if (hovered && hitKey >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        selectedTime_ = clip_.keyframes[static_cast<size_t>(hitKey)].time;
        selectedJoint_ = hitJoint;
        ImGui::OpenPopup("##keyMenu");
    }
    if (ImGui::BeginPopup("##keyMenu")) {
        AnimKeyframe* k = keyframeAt(selectedTime_, false);
        if (k) {
            ImGui::TextDisabled("%s at %.2f s", selectedJoint_.empty() ? "Keyframe" : selectedJoint_.c_str(), static_cast<double>(k->time));
            ImGui::Separator();
            if (theme::beginMenu(ICON_FA_WAVE_SQUARE, theme::color::text, "Easing Style")) {
                for (int s = 0; s < static_cast<int>(styleNames().size()); ++s) {
                    if (ImGui::MenuItem(styleNames()[static_cast<size_t>(s)].c_str())) {
                        snapshot();
                        for (auto& [n, p] : k->poses) if (selectedJoint_.empty() || n == selectedJoint_) p.style = static_cast<PoseEasingStyle>(s);
                        changed();
                    }
                }
                ImGui::EndMenu();
            }
            if (theme::beginMenu(ICON_FA_ARROW_RIGHT, theme::color::text, "Easing Direction")) {
                for (int d = 0; d < static_cast<int>(directionNames().size()); ++d) {
                    if (ImGui::MenuItem(directionNames()[static_cast<size_t>(d)].c_str())) {
                        snapshot();
                        for (auto& [n, p] : k->poses) if (selectedJoint_.empty() || n == selectedJoint_) p.direction = static_cast<PoseEasingDirection>(d);
                        changed();
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (theme::menuItem(ICON_FA_TRASH_CAN, theme::color::error, selectedJoint_.empty() ? "Delete Keyframe" : "Delete This Key")) deleteSelected();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelected();
        if (ImGui::IsKeyPressed(ImGuiKey_Space)) playing_ = !playing_;
    }

    // Keyframe diamonds.
    const float r = rowH * 0.28f;
    for (const AnimKeyframe& k : clip_.keyframes) {
        const float x = xOf(k.time);
        const bool sel = std::abs(k.time - selectedTime_) < kMatch;
        diamond(dl, ImVec2(x, rowY(1) + rowH * 0.5f), r * 1.1f, sel && selectedJoint_.empty() ? accent : theme::u32(theme::color::text, 0.85f), IM_COL32(15, 15, 20, 255));
        if (!k.name.empty()) dl->AddText(ImVec2(x + r * 1.6f, rowY(1) + style.FramePadding.y), theme::u32(theme::color::warning), k.name.c_str());
        for (size_t j = 0; j < joints_.size(); ++j) {
            auto it = k.poses.find(joints_[j]);
            if (it == k.poses.end()) continue;
            const bool on = sel && (selectedJoint_.empty() || selectedJoint_ == joints_[j]);
            const bool eased = it->second.style != PoseEasingStyle::Linear;
            diamond(dl, ImVec2(x, rowY(static_cast<int>(j) + 2) + rowH * 0.5f), r, on ? accent : theme::u32(eased ? theme::color::warning : theme::color::textDim),
                    IM_COL32(15, 15, 20, 255));
        }
    }
    // The current time.
    const float tx = xOf(time_);
    dl->AddLine(ImVec2(tx, origin.y), ImVec2(tx, origin.y + height), theme::u32(theme::color::error), 1.5f);
    dl->AddTriangleFilled(ImVec2(tx - 5.0f, origin.y), ImVec2(tx + 5.0f, origin.y), ImVec2(tx, origin.y + 6.0f), theme::u32(theme::color::error));

    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + height + style.ItemSpacing.y));
    ImGui::PushStyleColor(ImGuiCol_Text, theme::color::textDim);
    ImGui::TextUnformatted(ICON_FA_COMPUTER_MOUSE "  Pose with the Move and Rotate tools: changes are keyed at the red line. "
                           "Drag diamonds to move keys, right-click for easing; Shift turns snapping off.");
    ImGui::PopStyleColor();
}

} // namespace rift
