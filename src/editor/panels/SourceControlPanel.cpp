// Source Control: the project's git repository (Git for Windows).
#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <chrono>

namespace rift {

namespace fs = std::filesystem;

namespace {
    constexpr float kRefreshSeconds = 10.0f;

    const char* changeIcon(char kind) {
        switch (kind) {
            case 'A': return ICON_FA_SQUARE_PLUS;
            case 'D': return ICON_FA_SQUARE_MINUS;
            case 'R': return ICON_FA_RIGHT_LEFT;
            case 'C': return ICON_FA_TRIANGLE_EXCLAMATION;
            default: return ICON_FA_PEN_TO_SQUARE;
        }
    }
    ImVec4 changeColor(char kind) {
        switch (kind) {
            case 'A': return theme::color::success;
            case 'D': return theme::color::error;
            case 'C': return theme::color::warning;
            default: return ImVec4(0.45f, 0.66f, 1.0f, 1.0f);
        }
    }
    const char* changeName(char kind) {
        switch (kind) {
            case 'A': return "Added";
            case 'D': return "Deleted";
            case 'R': return "Renamed";
            case 'C': return "Conflict: both sides changed it";
            default: return "Modified";
        }
    }

    // A spinner made of a rotating arc.
    void spinner(float radius) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(radius * 2.0f, radius * 2.0f));
        const float t = static_cast<float>(ImGui::GetTime()) * 6.0f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->PathArcTo(ImVec2(p.x + radius, p.y + radius), radius * 0.8f, t, t + 4.2f, 20);
        draw->PathStroke(theme::u32(theme::color::accentHover), 0, radius * 0.3f);
    }
}

SourceControlPanel::~SourceControlPanel() {
    if (job_.valid()) job_.wait();
}

void SourceControlPanel::start(UiContext& ui, const std::string& label, std::function<Result(const Git&, const fs::path&)> work, bool saveFirst) {
    Editor& editor = ui.editor;
    if (job_.valid() || !editor.hasProject()) return;
    // git sees the files on disk, so unsaved edits are saved first.
    if (saveFirst && editor.isDirty()) {
        if (editor.playing()) { editor.notify("Stop the game first", NoticeLevel::Warning); return; }
        if (!editor.saveScene()) return;
        seenSaves_ = editor.saveCount();
    }
    jobLabel_ = label;
    const fs::path root = editor.project().root();
    const std::string exe = gitPath_;
    job_ = std::async(std::launch::async, [work, root, exe] {
        const Git git(exe);
        Result r = work ? work(git, root) : Result{};
        r.status = git.status(root);
        return r;
    });
}

void SourceControlPanel::poll(UiContext& ui) {
    if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    Result r = job_.get();
    status_ = std::move(r.status);
    haveStatus_ = true;
    if (!jobLabel_.empty()) {
        lastLabel_ = jobLabel_;
        lastOutput_ = r.output;
        lastOk_ = r.ok;
        if (r.ok) ui.editor.notify(jobLabel_ + ": done", NoticeLevel::Success);
        else ui.editor.notify(jobLabel_ + " failed (see Source Control)", NoticeLevel::Error);
    }
    jobLabel_.clear();
    if (r.reload) ui.editor.reloadFromDisk();
    refreshTimer_ = kRefreshSeconds;
}

void SourceControlPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    // Git found again when Preferences point somewhere else.
    if (gitPath_ != editor.settings().gitPath || (gitExe_.empty() && !haveStatus_)) {
        gitPath_ = editor.settings().gitPath;
        gitExe_ = Git::find(gitPath_);
        refreshTimer_ = 0.0f;
    }
    if (editor.hasProject() && root_ != editor.project().root()) {
        root_ = editor.project().root();
        haveStatus_ = false;
        status_ = GitStatus{};
        lastOutput_.clear();
        refreshTimer_ = 0.0f;
    }
    poll(ui);
    // Saving changes the files: look again.
    if (editor.saveCount() != seenSaves_) {
        seenSaves_ = editor.saveCount();
        refreshTimer_ = std::min(refreshTimer_, 0.3f);
    }

    if (!ImGui::Begin(ICON_FA_CODE_BRANCH "  Source Control###SourceControl", open)) {
        ImGui::End();
        return;
    }
    refreshTimer_ -= ui.dt;
    if (refreshTimer_ <= 0.0f && !job_.valid() && editor.hasProject()) {
        refreshTimer_ = kRefreshSeconds;
        start(ui, "", nullptr, false); // a quiet status refresh
    }

    if (!editor.hasProject()) {
        ImGui::TextDisabled("Open a project to use source control.");
        ImGui::End();
        return;
    }
    const float fs_ = ImGui::GetFontSize();
    ImGui::PushTextWrapPos(0.0f);

    if (gitExe_.empty()) {
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION "  Git is not installed");
        ImGui::TextDisabled("Source control uses Git for Windows (free). Install it, then press Check again. "
                            "If git is somewhere unusual, set it in Preferences > Source Control.");
        if (ImGui::Button(ICON_FA_DOWNLOAD "  Download Git")) Platform::openWithShell("https://git-scm.com/download/win");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE "  Check again")) gitPath_ = "\x01"; // forces a new search next frame
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    if (!haveStatus_) {
        spinner(fs_ * 0.6f);
        ImGui::SameLine();
        ImGui::TextDisabled("Reading the repository...");
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    if (!status_.repository) {
        theme::heading("Version control");
        ImGui::TextUnformatted("This project is not under version control yet.");
        ImGui::TextDisabled("Git keeps every version you commit: go back to any of them, try ideas on branches, "
                            "and back the project up to GitHub (or any git server).");
        ImGui::Spacing();
        ImGui::BeginDisabled(job_.valid());
        if (ImGui::Button(ICON_FA_CODE_BRANCH "  Initialize repository", ImVec2(-FLT_MIN, 0.0f))) {
            start(ui, "Initialize repository", [](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.init(root, &r.output);
                if (r.ok && r.output.empty()) r.output = "Repository created with a .gitignore for RIFT projects.";
                return r;
            }, true);
        }
        ImGui::EndDisabled();
        if (job_.valid() && !jobLabel_.empty()) { spinner(fs_ * 0.6f); ImGui::SameLine(); ImGui::TextDisabled("%s...", jobLabel_.c_str()); }
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }

    drawHeader(ui);
    ImGui::Separator();

    // Who commits: git refuses to commit without a name and an email.
    if (status_.userName.empty() || status_.userEmail.empty()) {
        ImGui::TextColored(theme::color::warning, ICON_FA_USER "  Who is committing?");
        ImGui::TextDisabled("Git records a name and an email with every commit.");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##gitName", "Name", &nameDraft_);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##gitEmail", "Email", &emailDraft_);
        ImGui::Checkbox("Use for all my repositories", &identityGlobal_);
        ImGui::BeginDisabled(nameDraft_.empty() || emailDraft_.find('@') == std::string::npos || job_.valid());
        if (ImGui::Button(ICON_FA_CHECK "  Save")) {
            const std::string name = nameDraft_, email = emailDraft_;
            const bool global = identityGlobal_;
            start(ui, "Set name and email", [name, email, global](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.setIdentity(root, name, email, global, &r.output);
                return r;
            }, false);
        }
        ImGui::EndDisabled();
        ImGui::Separator();
    }

    if (status_.merging) {
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION "  A merge is in progress");
        ImGui::TextDisabled("The pull brought changes that clash with yours. Fix the files marked as conflicts and commit, or abort the merge.");
        if (ImGui::Button(ICON_FA_XMARK "  Abort merge")) {
            start(ui, "Abort merge", [](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.abortMerge(root, &r.output);
                r.reload = true;
                return r;
            }, false);
        }
        ImGui::Separator();
    }

    drawChanges(ui);
    drawHistory(ui);

    // The last command's output.
    if (!lastOutput_.empty()) {
        ImGui::Spacing();
        const std::string header = std::string(lastOk_ ? ICON_FA_CIRCLE_CHECK "  " : ICON_FA_CIRCLE_XMARK "  ") + lastLabel_ + "###gitOutput";
        const bool shown = theme::sectionHeader(header.c_str(), !lastOk_, lastOk_ ? &theme::color::textDim : &theme::color::error);
        if (shown) {
            ImGui::PushFont(theme::fonts().mono, 0.0f);
            ImGui::TextUnformatted(lastOutput_.c_str());
            ImGui::PopFont();
            if (ImGui::SmallButton("Clear")) lastOutput_.clear();
        }
    }
    ImGui::PopTextWrapPos();
    drawPopups(ui);
    ImGui::End();
}

void SourceControlPanel::drawHeader(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    const bool busy = job_.valid();

    // Branch
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(theme::color::accentHover, ICON_FA_CODE_BRANCH);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(std::max(fs_ * 6.0f, ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() * 4.0f - ImGui::GetStyle().ItemSpacing.x * 4.0f));
    const std::string branchLabel = status_.branch.empty() ? std::string("(no branch)") : status_.branch;
    ImGui::BeginDisabled(busy);
    if (ImGui::BeginCombo("##branch", branchLabel.c_str())) {
        for (const std::string& b : status_.branches) {
            if (ImGui::Selectable(b.c_str(), b == status_.branch) && b != status_.branch) {
                start(ui, "Switch to " + b, [b](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.switchBranch(root, b, false, &r.output);
                    r.reload = r.ok;
                    return r;
                }, true);
            }
        }
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FA_PLUS "  New branch...")) { branchDraft_.clear(); openBranch_ = true; }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    theme::tooltip("The branch you are on. Switching loads that branch's version of the project.");

    // Sync buttons
    const bool remote = !status_.remoteUrl.empty();
    ImGui::SameLine();
    ImGui::BeginDisabled(busy || !remote);
    char pullTip[96];
    std::snprintf(pullTip, sizeof(pullTip), "Pull: get the changes from the server%s", status_.behind ? " (some are waiting)" : "");
    if (theme::iconButton(ICON_FA_CLOUD_ARROW_DOWN, pullTip, status_.behind > 0)) {
        start(ui, "Pull", [](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.pull(root, &r.output);
            r.reload = true;
            return r;
        }, true);
    }
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_CLOUD_ARROW_UP, "Push: send your commits to the server", status_.ahead > 0)) {
        const GitStatus s = status_;
        start(ui, "Push", [s](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.push(root, s, &r.output);
            return r;
        }, false);
    }
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ARROWS_ROTATE, "Fetch: check the server for new commits")) {
        start(ui, "Fetch", [](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.fetch(root, &r.output);
            return r;
        }, false);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ELLIPSIS_VERTICAL, "More")) ImGui::OpenPopup("##gitMore");
    if (ImGui::BeginPopup("##gitMore")) {
        if (ImGui::MenuItem(ICON_FA_LINK "  Set remote (GitHub URL)...")) { remoteDraft_ = status_.remoteUrl; openRemote_ = true; }
        const std::string web = Git::webUrl(status_.remoteUrl);
        if (ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !web.empty())) Platform::openWithShell(web);
        if (ImGui::MenuItem(ICON_FA_PLUS "  New branch...")) { branchDraft_.clear(); openBranch_ = true; }
        if (ImGui::MenuItem(ICON_FA_ROTATE "  Refresh")) refreshTimer_ = 0.0f;
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show the project folder")) Platform::revealInExplorer(Paths::toUtf8(ui.editor.project().root()));
        ImGui::Separator();
        ImGui::TextDisabled("%s", gitExe_.c_str());
        ImGui::EndPopup();
    }

    // Where the branch stands against the server.
    if (busy) {
        spinner(fs_ * 0.5f);
        ImGui::SameLine();
        ImGui::TextDisabled("%s...", jobLabel_.empty() ? "Refreshing" : jobLabel_.c_str());
    } else if (!remote) {
        ImGui::TextDisabled(ICON_FA_CLOUD "  No server yet.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect to GitHub...")) { remoteDraft_.clear(); openRemote_ = true; }
    } else if (status_.upstream.empty()) {
        ImGui::TextDisabled(ICON_FA_CLOUD "  Not on the server yet: Push to publish this branch.");
    } else if (status_.ahead || status_.behind) {
        ImGui::TextColored(theme::color::warning, ICON_FA_CLOUD "  %d to push, %d to pull", status_.ahead, status_.behind);
    } else {
        ImGui::TextDisabled(ICON_FA_CLOUD "  Up to date with %s", status_.upstream.c_str());
    }
}

void SourceControlPanel::drawChanges(UiContext& ui) {
    Editor& editor = ui.editor;
    const float fs_ = ImGui::GetFontSize();
    const bool busy = job_.valid();
    const int unsavedScene = editor.isDirty() ? 1 : 0;

    // Commit box
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextMultiline("##message", &message_, ImVec2(-FLT_MIN, fs_ * 3.2f));
    if (message_.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y),
                                            theme::u32(theme::color::textDim), "Describe what you changed");
    }
    const bool ctrlEnter = ImGui::IsItemFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter);
    const int count = static_cast<int>(status_.changes.size()) + unsavedScene;
    char label[64];
    std::snprintf(label, sizeof(label), ICON_FA_CHECK "  Commit%s", count ? (" " + std::to_string(count) + (count == 1 ? " change" : " changes")).c_str() : "");
    const bool canCommit = !busy && count > 0 && !message_.empty() && !status_.userName.empty();
    ImGui::BeginDisabled(!canCommit);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::accentHover);
    if ((ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)) || (ctrlEnter && canCommit))) {
        const std::string message = message_;
        const bool push = editor.settings().gitPushAfterCommit && !status_.remoteUrl.empty();
        const GitStatus s = status_;
        start(ui, push ? "Commit and push" : "Commit", [message, push, s](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.commitAll(root, message, &r.output);
            if (r.ok && push) {
                std::string out;
                r.ok = git.push(root, s, &out);
                r.output += "\n" + out;
            }
            return r;
        }, true);
        message_.clear();
    }
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    if (!message_.empty() || count) theme::tooltip("Saves the scene, then records every change as one commit (Ctrl+Enter).");
    ImGui::Checkbox("Push after committing", &editor.settings().gitPushAfterCommit);

    // The files
    ImGui::Spacing();
    char title[64];
    std::snprintf(title, sizeof(title), "Changes (%d)###changes", count);
    if (!theme::sectionHeader(title)) return;
    if (unsavedScene) {
        ImGui::TextColored(theme::color::warning, ICON_FA_CIRCLE "  ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s: unsaved (saved when you commit)", editor.sceneName().c_str());
    }
    if (status_.changes.empty() && !unsavedScene) {
        ImGui::TextDisabled("Nothing changed since the last commit.");
        return;
    }
    const float listHeight = std::min(fs_ * 1.6f * static_cast<float>(status_.changes.size()) + fs_ * 0.5f, fs_ * 14.0f);
    ImGui::BeginChild("##changeList", ImVec2(0.0f, listHeight), ImGuiChildFlags_None);
    for (size_t k = 0; k < status_.changes.size(); ++k) {
        const GitChange& c = status_.changes[k];
        ImGui::PushID(static_cast<int>(k));
        const char kind = c.kind();
        ImGui::TextColored(changeColor(kind), "%s", changeIcon(kind));
        theme::tooltip(changeName(kind));
        ImGui::SameLine();
        // File name first, the folder dimmed after it.
        const size_t slash = c.path.find_last_of('/');
        const std::string name = slash == std::string::npos ? c.path : c.path.substr(slash + 1);
        const std::string folder = slash == std::string::npos ? std::string() : c.path.substr(0, slash);
        ImGui::Selectable(name.c_str(), false, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(ImGui::CalcTextSize(name.c_str()).x, 0.0f));
        const bool open = ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        if (!folder.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", folder.c_str());
        }
        const fs::path file = editor.project().root() / Paths::fromUtf8(c.path);
        if (open && kind != 'D') Platform::revealInExplorer(Paths::toUtf8(file));
        if (ImGui::BeginPopupContextItem("##change")) {
            if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in Explorer", nullptr, false, kind != 'D')) Platform::revealInExplorer(Paths::toUtf8(file));
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy path")) Platform::setClipboard(c.path);
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Discard changes...", nullptr, false, !busy)) {
                discardTarget_ = c;
                openDiscard_ = true;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void SourceControlPanel::drawHistory(UiContext& ui) {
    char title[64];
    std::snprintf(title, sizeof(title), "History (%d)###history", static_cast<int>(status_.history.size()));
    if (!theme::sectionHeader(title)) return;
    if (status_.history.empty()) {
        ImGui::TextDisabled(status_.noCommits ? "No commits yet: commit to save the first version." : "No history.");
        return;
    }
    const std::string web = Git::webUrl(status_.remoteUrl);
    for (size_t k = 0; k < status_.history.size(); ++k) {
        const GitCommit& c = status_.history[k];
        ImGui::PushID(static_cast<int>(k));
        ImGui::TextColored(k == 0 ? theme::color::accentHover : theme::color::textDim, k == 0 ? ICON_FA_CIRCLE_DOT : ICON_FA_CIRCLE);
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Selectable(c.subject.c_str(), false, ImGuiSelectableFlags_AllowOverlap);
        ImGui::TextDisabled("%s  -  %s  -  %s", c.author.c_str(), c.when.c_str(), c.shortHash.c_str());
        ImGui::EndGroup();
        if (ImGui::BeginPopupContextItem("##commit")) {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy commit id")) Platform::setClipboard(c.hash);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy message")) Platform::setClipboard(c.subject);
            if (ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !web.empty())) Platform::openWithShell(web + "/commit/" + c.hash);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    (void)ui;
}

void SourceControlPanel::drawPopups(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    if (openDiscard_) { ImGui::OpenPopup("Discard changes?"); openDiscard_ = false; }
    if (openRemote_) { ImGui::OpenPopup("Remote"); openRemote_ = false; }
    if (openBranch_) { ImGui::OpenPopup("New branch"); openBranch_ = false; }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Discard changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::Text("Throw away your changes to %s?", discardTarget_.path.c_str());
        ImGui::TextDisabled(discardTarget_.untracked() ? "The file is new and will be deleted." : "It goes back to the last committed version. This cannot be undone.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Discard", ImVec2(fs_ * 7.5f, 0.0f))) {
            const GitChange c = discardTarget_;
            start(ui, "Discard " + c.path, [c](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.discard(root, c, &r.output);
                r.reload = true;
                return r;
            }, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 7.5f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Remote", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Repository URL");
        ImGui::TextDisabled("Create an empty repository on GitHub, then paste its address:\nhttps://github.com/you/my-game.git");
        ImGui::SetNextItemWidth(fs_ * 28.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputTextWithHint("##remote", "https://github.com/you/my-game.git", &remoteDraft_);
        if (ImGui::SmallButton(ICON_FA_GLOBE " Create a repository on GitHub")) Platform::openWithShell("https://github.com/new");
        ImGui::Spacing();
        ImGui::BeginDisabled(remoteDraft_.empty());
        if (ImGui::Button(ICON_FA_CHECK "  Save", ImVec2(fs_ * 7.5f, 0.0f))) {
            const std::string url = remoteDraft_;
            start(ui, "Set remote", [url](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.setRemote(root, url, &r.output);
                if (r.ok && r.output.empty()) r.output = "origin is now " + url + ". Push to upload the project.";
                return r;
            }, false);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 7.5f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("New branch", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("Name of the new branch");
        ImGui::TextDisabled("It starts from what you have now; your uncommitted changes come along.");
        ImGui::SetNextItemWidth(fs_ * 20.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##branchName", "new-level", &branchDraft_, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool valid = !branchDraft_.empty() && branchDraft_.find_first_of(" ~^:?*[\\") == std::string::npos;
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(ICON_FA_PLUS "  Create", ImVec2(fs_ * 7.5f, 0.0f)) || (enter && valid)) {
            const std::string name = branchDraft_;
            start(ui, "Create branch " + name, [name](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.switchBranch(root, name, true, &r.output);
                return r;
            }, true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 7.5f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace rift
