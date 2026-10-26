// Source control, laid out like Visual Studio's: the Git Changes window
// (stage, commit, push / pull, stashes, conflicts), the Git Repository window
// (branches, the commit graph, what each commit changed), a diff tab and the
// Git menu -- over the project's git repository (Git for Windows).
#include "editor/panels/Panels.h"
#include "editor/Theme.h"
#include "core/Paths.h"
#include "platform/Platform.h"

#include "IconsFontAwesome6.h"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace rift {

namespace fs = std::filesystem;

namespace {
    constexpr float kRefreshSeconds = 10.0f;
    constexpr int kGraphCommits = 600;

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

    // "5 minutes ago", "yesterday", "12 Mar 2026".
    std::string timeAgo(int64_t when) {
        if (when <= 0) return {};
        const int64_t s = static_cast<int64_t>(std::time(nullptr)) - when;
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
    std::string dateTime(int64_t when) {
        const std::time_t t = static_cast<std::time_t>(when);
        std::tm tm{};
        localtime_s(&tm, &t);
        char text[48];
        std::strftime(text, sizeof(text), "%d %b %Y, %H:%M", &tm);
        return text;
    }

    // Branch colours of the graph.
    ImVec4 laneColor(int index) {
        static const ImVec4 palette[] = {
            { 0.55f, 0.42f, 0.98f, 1.0f }, { 0.25f, 0.70f, 0.98f, 1.0f }, { 0.97f, 0.62f, 0.18f, 1.0f }, { 0.28f, 0.82f, 0.56f, 1.0f },
            { 0.95f, 0.38f, 0.60f, 1.0f }, { 0.96f, 0.84f, 0.28f, 1.0f }, { 0.30f, 0.86f, 0.86f, 1.0f }, { 0.95f, 0.45f, 0.35f, 1.0f },
        };
        return palette[((index % 8) + 8) % 8];
    }

    bool containsNoCase(const std::string& text, const std::string& needle) {
        if (needle.empty()) return true;
        auto it = std::search(text.begin(), text.end(), needle.begin(), needle.end(),
                              [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        return it != text.end();
    }
    std::string baseName(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    std::string folderOf(const std::string& path) {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    // A small rounded label (branch / tag names in the graph); returns its width.
    float badge(ImDrawList* dl, ImVec2 p, float height, const std::string& text, const ImVec4& color, bool strong) {
        const float fs_ = ImGui::GetFontSize();
        const ImVec2 size = ImGui::CalcTextSize(text.c_str());
        const float w = size.x + fs_ * 0.7f;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + height), theme::u32(color, strong ? 0.85f : 0.25f), height * 0.35f);
        if (!strong) dl->AddRect(p, ImVec2(p.x + w, p.y + height), theme::u32(color, 0.8f), height * 0.35f);
        dl->AddText(ImVec2(p.x + fs_ * 0.35f, p.y + (height - size.y) * 0.5f), strong ? IM_COL32(255, 255, 255, 255) : theme::u32(theme::color::text), text.c_str());
        return w;
    }

    SourceControlPanel::Result ran(bool ok, std::string output, bool reload = false) {
        SourceControlPanel::Result r;
        r.ok = ok;
        r.output = std::move(output);
        r.reload = reload;
        return r;
    }
}

SourceControlPanel::~SourceControlPanel() {
    if (action_.valid()) action_.wait();
    if (refresh_.valid()) refresh_.wait();
    if (lookup_.valid()) lookup_.wait();
}

// ============================================================================
// git work

void SourceControlPanel::start(UiContext& ui, const std::string& label, Work work, bool saveFirst) {
    Editor& editor = ui.editor;
    if (!editor.hasProject()) return;
    const bool quiet = label.empty();
    if (quiet ? (refresh_.valid() || action_.valid()) : action_.valid()) {
        if (!quiet) editor.notify("Git is still busy: " + actionLabel_, NoticeLevel::Warning);
        return;
    }
    // git sees the files on disk, so unsaved edits are saved first.
    if (saveFirst && editor.isDirty()) {
        if (editor.playing()) { editor.notify("Stop the game first", NoticeLevel::Warning); return; }
        if (!editor.saveScene()) return;
        seenSaves_ = editor.saveCount();
    }
    const fs::path project = editor.project().root();
    const std::string exe = gitPath_;
    const bool graph = ImGui::GetFrameCount() - repoShownFrame_ <= 2;
    auto job = std::async(std::launch::async, [work, project, exe, graph] {
        const Git git(exe);
        Result r = work ? work(git, project) : Result{};
        r.status = git.status(project);
        if (graph && r.status.repository) {
            if (!r.status.noCommits) r.graph = git.graph(project, kGraphCommits);
            r.haveGraph = true;
        }
        return r;
    });
    if (quiet) {
        refresh_ = std::move(job);
        refreshGeneration_ = generation_;
    } else {
        action_ = std::move(job);
        actionLabel_ = label;
        ++generation_;
    }
}

void SourceControlPanel::apply(Result& r) {
    status_ = std::move(r.status);
    haveStatus_ = true;
    if (!r.haveGraph) return;
    graph_ = std::move(r.graph);
    graphLoaded_ = true;
    graphIndex_.clear();
    graphLanes_ = 1;
    for (size_t k = 0; k < graph_.size(); ++k) {
        graphIndex_[graph_[k].hash] = k;
        graphLanes_ = std::max(graphLanes_, graph_[k].width);
    }
    // Outgoing: on this branch, not yet on the server's; incoming: the other way round.
    auto ancestors = [&](const std::string& from) {
        std::unordered_set<std::string> seen;
        std::vector<std::string> stack = { from };
        while (!stack.empty()) {
            const std::string h = stack.back();
            stack.pop_back();
            if (h.empty() || !seen.insert(h).second) continue;
            auto it = graphIndex_.find(h);
            if (it == graphIndex_.end()) continue;
            for (const std::string& p : graph_[it->second].parents) stack.push_back(p);
        }
        return seen;
    };
    outgoing_.clear();
    incoming_.clear();
    std::string upstreamHash;
    for (const GitRef& ref : status_.refs)
        if (ref.kind == GitRef::Remote && ref.name == status_.upstream) upstreamHash = ref.hash;
    if (!status_.headHash.empty() && !upstreamHash.empty() && upstreamHash != status_.headHash) {
        const auto mine = ancestors(status_.headHash), theirs = ancestors(upstreamHash);
        for (const std::string& h : mine)
            if (!theirs.count(h)) outgoing_.insert(h);
        for (const std::string& h : theirs)
            if (!mine.count(h)) incoming_.insert(h);
    }
}

void SourceControlPanel::poll(UiContext& ui) {
    if (action_.valid() && action_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Result r = action_.get();
        const bool ok = r.ok;
        const bool reload = r.reload;
        lastLabel_ = actionLabel_;
        lastOutput_ = r.output;
        lastOk_ = ok;
        apply(r);
        if (ok) ui.editor.notify(actionLabel_ + ": done", NoticeLevel::Success);
        else ui.editor.notify(actionLabel_ + " failed (see Git Changes)", NoticeLevel::Error);
        actionLabel_.clear();
        if (reload) ui.editor.reloadFromDisk();
        refreshTimer_ = kRefreshSeconds;
        // Whatever details were showing may have changed (amend, reset).
        if (!selectedCommit_.empty() && !graphIndex_.count(selectedCommit_)) { selectedCommit_.clear(); details_ = GitCommitDetails{}; }
    }
    if (refresh_.valid() && refresh_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Result r = refresh_.get();
        // A refresh that started before the last action may show the old state.
        if (!action_.valid() && refreshGeneration_ == generation_) apply(r);
    }
}

void SourceControlPanel::lookup(LookupWork work) {
    if (lookup_.valid()) { pendingLookup_ = std::move(work); return; }
    const fs::path project = project_;
    const std::string exe = gitPath_;
    lookup_ = std::async(std::launch::async, [work, project, exe] {
        const Git git(exe);
        return work(git, project);
    });
}

void SourceControlPanel::pollLookup() {
    if (!lookup_.valid() || lookup_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    Lookup l = lookup_.get();
    if (l.haveDetails && l.details.hash == selectedCommit_) details_ = std::move(l.details);
    if (l.haveDiff) {
        diffTitle_ = l.diffTitle;
        diffLines_.clear();
        diffAdded_ = diffRemoved_ = 0;
        size_t start = 0;
        while (start < l.diff.size()) {
            size_t end = l.diff.find('\n', start);
            if (end == std::string::npos) end = l.diff.size();
            std::string line = l.diff.substr(start, end - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            for (size_t t = line.find('\t'); t != std::string::npos; t = line.find('\t', t)) line.replace(t, 1, "    ");
            if (line.rfind("+++", 0) != 0 && line.rfind("+", 0) == 0) ++diffAdded_;
            if (line.rfind("---", 0) != 0 && line.rfind("-", 0) == 0) ++diffRemoved_;
            diffLines_.push_back(std::move(line));
            start = end + 1;
        }
        showDiff_ = true;
        focusDiff_ = true;
    }
    if (pendingLookup_) {
        LookupWork next = std::move(pendingLookup_);
        pendingLookup_ = nullptr;
        lookup(std::move(next));
    }
}

void SourceControlPanel::selectCommit(const std::string& hash) {
    if (selectedCommit_ == hash) return;
    selectedCommit_ = hash;
    lookup([hash](const Git& git, const fs::path& root) {
        Lookup l;
        l.haveDetails = true;
        if (!git.commitDetails(root, hash, l.details)) {
            l.details = GitCommitDetails{};
            l.details.hash = hash;
            l.details.message = "(could not read this commit)";
        }
        return l;
    });
}

void SourceControlPanel::openCommitDiff(const std::string& hash, const GitFileChange& file) {
    diffKey_ = hash + ":" + file.path;
    lookup([hash, file](const Git& git, const fs::path& root) {
        Lookup l;
        l.haveDiff = true;
        l.diff = git.commitFileDiff(root, hash, file);
        l.diffTitle = baseName(file.path) + " @ " + hash.substr(0, 7);
        return l;
    });
}

void SourceControlPanel::openWorkingDiff(const GitChange& change, bool staged) {
    diffKey_ = std::string(staged ? "S:" : "W:") + change.path;
    lookup([change, staged](const Git& git, const fs::path& root) {
        Lookup l;
        l.haveDiff = true;
        l.diff = git.workingDiff(root, change, staged);
        l.diffTitle = baseName(change.path) + (staged ? " (staged)" : " (changes)");
        return l;
    });
}

void SourceControlPanel::ask(const std::string& title, const std::string& text, const std::string& button, bool danger, std::function<void(UiContext&)> action) {
    confirm_ = Confirm{ title, text, button, danger, std::move(action) };
    openConfirm_ = true;
}

void SourceControlPanel::promptFor(const std::string& title, const std::string& text, const std::string& hint, const std::string& value, const std::string& button,
                                   std::function<void(UiContext&, const std::string&, bool)> action, const std::string& checkboxLabel, bool refName) {
    prompt_ = Prompt{};
    prompt_.title = title;
    prompt_.text = text;
    prompt_.hint = hint;
    prompt_.value = value;
    prompt_.button = button;
    prompt_.refName = refName;
    prompt_.showCheckbox = !checkboxLabel.empty();
    prompt_.checkboxLabel = checkboxLabel;
    prompt_.action = std::move(action);
    openPrompt_ = true;
}

// ============================================================================
// Actions

void SourceControlPanel::doFetch(UiContext& ui) {
    start(ui, "Fetch", [](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.fetch(root, &r.output);
        return r;
    }, false);
}

void SourceControlPanel::doPull(UiContext& ui) {
    start(ui, "Pull", [](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.pull(root, &r.output);
        r.reload = true;
        return r;
    }, true);
}

void SourceControlPanel::doPush(UiContext& ui) {
    const GitStatus s = status_;
    start(ui, "Push", [s](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.push(root, s, &r.output);
        return r;
    }, false);
}

void SourceControlPanel::doSync(UiContext& ui) {
    const GitStatus s = status_;
    start(ui, "Sync", [s](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.sync(root, s, &r.output);
        r.reload = true;
        return r;
    }, true);
}

void SourceControlPanel::doSwitch(UiContext& ui, const std::string& branch) {
    start(ui, "Switch to " + branch, [branch](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.switchBranch(root, branch, false, &r.output);
        r.reload = r.ok;
        return r;
    }, true);
}

void SourceControlPanel::doCheckoutRemote(UiContext& ui, const std::string& remoteBranch) {
    start(ui, "Check out " + remoteBranch, [remoteBranch](const Git& git, const fs::path& root) {
        Result r;
        r.ok = git.checkoutRemote(root, remoteBranch, &r.output);
        r.reload = r.ok;
        return r;
    }, true);
}

void SourceControlPanel::doMerge(UiContext& ui, const std::string& what) {
    const std::string into = status_.branch.empty() ? std::string("HEAD") : status_.branch;
    ask("Merge", "Merge " + what + " into " + into + "?\nIf both changed the same lines, you choose which version to keep.", "Merge", false,
        [this, what](UiContext& u) {
            start(u, "Merge " + what, [what](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.merge(root, what, &r.output);
                r.reload = true;
                return r;
            }, true);
        });
}

void SourceControlPanel::newBranch(UiContext&, const std::string& startPoint, const std::string& startLabel) {
    promptFor("New branch", "It starts from " + startLabel + ". Your uncommitted changes come along when you switch to it.", "new-level", "", "Create",
              [this, startPoint](UiContext& u, const std::string& name, bool checkout) {
                  start(u, "Create branch " + name, [name, startPoint, checkout](const Git& git, const fs::path& root) {
                      Result r;
                      r.ok = git.createBranch(root, name, startPoint, checkout, &r.output);
                      r.reload = r.ok && checkout && !startPoint.empty();
                      return r;
                  }, checkout);
              }, "Switch to the new branch");
}

void SourceControlPanel::setRemote(UiContext&) {
    promptFor("Connect to a server", "Create an empty repository on GitHub (github.com/new), then paste its address.",
              "https://github.com/you/my-game.git", status_.remoteUrl, "Save",
              [this](UiContext& u, const std::string& url, bool) {
                  start(u, "Set remote", [url](const Git& git, const fs::path& root) {
                      Result r;
                      r.ok = git.setRemote(root, url, &r.output);
                      if (r.ok && r.output.empty()) r.output = "origin is now " + url + ". Push to upload the project.";
                      return r;
                  }, false);
              }, "", false);
}

// Right-click menu of a commit (graph rows and the details pane).
void SourceControlPanel::commitActions(UiContext& ui, const std::string& hash) {
    const std::string shortHash = hash.substr(0, 7);
    const std::string branch = status_.branch.empty() ? std::string("HEAD") : status_.branch;
    const bool b = busy();
    if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  New Branch From Here...", nullptr, false, !b)) newBranch(ui, hash, "commit " + shortHash);
    if (ImGui::MenuItem(ICON_FA_TAG "  New Tag Here...", nullptr, false, !b)) {
        promptFor("New tag", "Marks commit " + shortHash + " with a name, like a version number.", "v1.0", "", "Create",
                  [this, hash](UiContext& u, const std::string& name, bool) {
                      start(u, "Tag " + name, [name, hash](const Git& git, const fs::path& root) {
                          Result r;
                          r.ok = git.createTag(root, name, hash, &r.output);
                          return r;
                      }, false);
                  });
    }
    if (ImGui::MenuItem(ICON_FA_CODE_COMMIT "  Check Out (Detached)", nullptr, false, !b && hash != status_.headHash)) {
        ask("Check out a commit", "Load the project as it was at " + shortHash + "?\nYou are then on no branch: make a branch there to keep new commits.",
            "Check Out", false, [this, hash, shortHash](UiContext& u) {
                start(u, "Check out " + shortHash, [hash](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.checkoutCommit(root, hash, &r.output);
                    r.reload = r.ok;
                    return r;
                }, true);
            });
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Revert", nullptr, false, !b)) {
        ask("Revert", "Make a new commit that undoes " + shortHash + "?", "Revert", false, [this, hash, shortHash](UiContext& u) {
            start(u, "Revert " + shortHash, [hash](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.revert(root, hash, &r.output);
                r.reload = true;
                return r;
            }, true);
        });
    }
    if (ImGui::MenuItem((ICON_FA_CODE_PULL_REQUEST "  Cherry-Pick Into " + branch).c_str(), nullptr, false, !b && hash != status_.headHash)) {
        start(ui, "Cherry-pick " + shortHash, [hash](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.cherryPick(root, hash, &r.output);
            r.reload = true;
            return r;
        }, true);
    }
    if (ImGui::BeginMenu((ICON_FA_CLOCK_ROTATE_LEFT "  Reset " + branch + " To Here").c_str(), !b && hash != status_.headHash)) {
        if (ImGui::MenuItem("Keep my changes (--mixed)")) {
            ask("Reset", "Move " + branch + " back to " + shortHash + "?\nThe commits after it are undone; their changes stay in your files.", "Reset", false,
                [this, hash, shortHash](UiContext& u) {
                    start(u, "Reset to " + shortHash, [hash](const Git& git, const fs::path& root) {
                        Result r;
                        r.ok = git.reset(root, hash, 'm', &r.output);
                        return r;
                    }, true);
                });
        }
        if (ImGui::MenuItem("Delete my changes (--hard)")) {
            ask("Reset and delete changes", "Move " + branch + " back to " + shortHash + " and throw away every change after it?\nThis cannot be undone.",
                "Delete Changes", true, [this, hash, shortHash](UiContext& u) {
                    start(u, "Hard reset to " + shortHash, [hash](const Git& git, const fs::path& root) {
                        Result r;
                        r.ok = git.reset(root, hash, 'h', &r.output);
                        r.reload = true;
                        return r;
                    }, false);
                });
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Commit Id")) Platform::setClipboard(hash);
    auto it = graphIndex_.find(hash);
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Message", nullptr, false, it != graphIndex_.end())) Platform::setClipboard(graph_[it->second].subject);
    const std::string web = Git::webUrl(status_.remoteUrl);
    if (ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !web.empty())) Platform::openWithShell(web + "/commit/" + hash);
}

// Right-click menu of a branch or tag.
void SourceControlPanel::branchActions(UiContext& ui, const GitRef& ref) {
    const bool b = busy();
    const std::string current = status_.branch.empty() ? std::string("HEAD") : status_.branch;
    const std::string web = Git::webUrl(status_.remoteUrl);
    const std::string name = ref.name;
    if (ref.kind == GitRef::Local) {
        if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Check Out", nullptr, false, !b && !ref.head)) doSwitch(ui, name);
        if (ImGui::MenuItem(ICON_FA_PLUS "  New Branch From Here...", nullptr, false, !b)) newBranch(ui, name, name);
        if (ImGui::MenuItem((ICON_FA_CODE_MERGE "  Merge Into " + current).c_str(), nullptr, false, !b && !ref.head)) doMerge(ui, name);
        ImGui::Separator();
        if (ref.head) {
            if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Push", nullptr, false, !b && !status_.remoteUrl.empty())) doPush(ui);
            if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Pull", nullptr, false, !b && !status_.upstream.empty())) doPull(ui);
        }
        if (ImGui::MenuItem(ICON_FA_I_CURSOR "  Rename...", nullptr, false, !b)) {
            promptFor("Rename branch", "A new name for " + name + ".", name, name, "Rename", [this, name](UiContext& u, const std::string& to, bool) {
                start(u, "Rename " + name, [name, to](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.renameBranch(root, name, to, &r.output);
                    return r;
                }, false);
            });
        }
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete...", nullptr, false, !b && !ref.head)) {
            ask("Delete branch", "Delete the branch " + name + "?\nCommits that are on no other branch are lost.", "Delete", true, [this, name](UiContext& u) {
                start(u, "Delete " + name, [name](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.deleteBranch(root, name, true, &r.output);
                    return r;
                }, false);
            });
        }
        if (!web.empty() && ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !ref.upstream.empty())) Platform::openWithShell(web + "/tree/" + name);
    } else if (ref.kind == GitRef::Remote) {
        if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Check Out", nullptr, false, !b)) doCheckoutRemote(ui, name);
        if (ImGui::MenuItem(ICON_FA_PLUS "  New Branch From Here...", nullptr, false, !b)) newBranch(ui, name, name);
        if (ImGui::MenuItem((ICON_FA_CODE_MERGE "  Merge Into " + current).c_str(), nullptr, false, !b)) doMerge(ui, name);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete From Server...", nullptr, false, !b)) {
            ask("Delete from the server", "Delete " + name + " on the server for everyone?", "Delete", true, [this, name](UiContext& u) {
                start(u, "Delete " + name, [name](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.deleteRemoteBranch(root, name, &r.output);
                    return r;
                }, false);
            });
        }
        const size_t slash = name.find('/');
        if (!web.empty() && slash != std::string::npos && ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub")) Platform::openWithShell(web + "/tree/" + name.substr(slash + 1));
    } else {
        if (ImGui::MenuItem(ICON_FA_CODE_COMMIT "  Check Out (Detached)", nullptr, false, !b)) {
            const std::string hash = ref.hash;
            start(ui, "Check out " + name, [hash](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.checkoutCommit(root, hash, &r.output);
                r.reload = r.ok;
                return r;
            }, true);
        }
        if (ImGui::MenuItem(ICON_FA_PLUS "  New Branch From Here...", nullptr, false, !b)) newBranch(ui, ref.hash, "tag " + name);
        if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Push Tag", nullptr, false, !b && !status_.remoteUrl.empty())) {
            start(ui, "Push tag " + name, [name](const Git& git, const fs::path& root) {
                const auto run = git.run(root, { "push", "origin", "refs/tags/" + name }, 300000);
                return ran(run.ok(), run.output);
            }, false);
        }
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete Tag...", nullptr, false, !b)) {
            ask("Delete tag", "Delete the tag " + name + " (here, not on the server)?", "Delete", true, [this, name](UiContext& u) {
                start(u, "Delete tag " + name, [name](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.deleteTag(root, name, &r.output);
                    return r;
                }, false);
            });
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Name")) Platform::setClipboard(name);
}

// ============================================================================
// Every frame

void SourceControlPanel::update(UiContext& ui, bool visible) {
    Editor& editor = ui.editor;
    // Git found again when Preferences point somewhere else.
    if (gitPath_ != editor.settings().gitPath || (gitExe_.empty() && !haveStatus_)) {
        gitPath_ = editor.settings().gitPath;
        gitExe_ = Git::find(gitPath_);
        refreshTimer_ = 0.0f;
    }
    if (editor.hasProject() && project_ != editor.project().root()) {
        project_ = editor.project().root();
        haveStatus_ = graphLoaded_ = false;
        status_ = GitStatus{};
        graph_.clear();
        graphIndex_.clear();
        outgoing_.clear();
        incoming_.clear();
        selectedCommit_.clear();
        details_ = GitCommitDetails{};
        lastOutput_.clear();
        showDiff_ = false;
        refreshTimer_ = 0.0f;
    }
    poll(ui);
    pollLookup();
    // Saving changes the files: look again.
    if (editor.saveCount() != seenSaves_) {
        seenSaves_ = editor.saveCount();
        refreshTimer_ = std::min(refreshTimer_, 0.3f);
    }
    // The Git Repository window just opened: it needs the history.
    const bool repoVisible = ImGui::GetFrameCount() - repoShownFrame_ <= 2;
    if (repoVisible && !graphLoaded_ && haveStatus_ && status_.repository && !refresh_.valid() && !action_.valid()) refreshTimer_ = 0.0f;
    if (!visible || !editor.hasProject() || gitExe_.empty()) return;
    refreshTimer_ -= ui.dt;
    if (refreshTimer_ <= 0.0f && !refresh_.valid() && !action_.valid()) {
        refreshTimer_ = kRefreshSeconds;
        start(ui, "", nullptr, false);
    }
}

// ============================================================================
// Git Changes

void SourceControlPanel::draw(UiContext& ui, bool* open) {
    Editor& editor = ui.editor;
    if (!ImGui::Begin(ICON_FA_CODE_BRANCH "  Git Changes###SourceControl", open)) {
        ImGui::End();
        return;
    }
    if (!editor.hasProject()) {
        ImGui::TextDisabled("Open a project to use source control.");
        ImGui::End();
        return;
    }
    ImGui::PushTextWrapPos(0.0f);
    drawSetup(ui);
    if (haveStatus_ && status_.repository) {
        drawToolbar(ui, true);
        ImGui::Separator();
        drawOperation(ui);
        drawCommitBox(ui);
        drawChangeList(ui);
        drawStashes(ui);

        // The last command's output.
        if (!lastOutput_.empty()) {
            ImGui::Spacing();
            const std::string header = std::string(lastOk_ ? ICON_FA_CIRCLE_CHECK "  " : ICON_FA_CIRCLE_XMARK "  ") + lastLabel_ + "###gitOutput";
            if (theme::sectionHeader(header.c_str(), !lastOk_, lastOk_ ? &theme::color::textDim : &theme::color::error)) {
                ImGui::PushFont(theme::fonts().mono, 0.0f);
                ImGui::TextUnformatted(lastOutput_.c_str());
                ImGui::PopFont();
                if (ImGui::SmallButton("Clear")) lastOutput_.clear();
            }
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
}

// Git missing, still reading, not a repository yet, or no name to commit with.
void SourceControlPanel::drawSetup(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    if (gitExe_.empty()) {
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION "  Git is not installed");
        ImGui::TextDisabled("Source control uses Git for Windows (free). Install it, then press Check again. "
                            "If git is somewhere unusual, set it in Preferences > Source Control.");
        if (ImGui::Button(ICON_FA_DOWNLOAD "  Download Git")) Platform::openWithShell("https://git-scm.com/download/win");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ROTATE "  Check again")) gitPath_ = "\x01"; // forces a new search next frame
        return;
    }
    if (!haveStatus_) {
        spinner(fs_ * 0.6f);
        ImGui::SameLine();
        ImGui::TextDisabled("Reading the repository...");
        return;
    }
    if (!status_.repository) {
        theme::heading("Version control");
        ImGui::TextUnformatted("This project is not under version control yet.");
        ImGui::TextDisabled("Git keeps every version you commit: go back to any of them, try ideas on branches, "
                            "and back the project up to GitHub (or any git server).");
        if (!status_.error.empty()) ImGui::TextColored(theme::color::error, "%s", status_.error.c_str());
        ImGui::Spacing();
        ImGui::BeginDisabled(busy());
        if (ImGui::Button(ICON_FA_CODE_BRANCH "  Create Git Repository", ImVec2(-FLT_MIN, 0.0f))) {
            start(ui, "Create repository", [](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.init(root, &r.output);
                if (r.ok && r.output.empty()) r.output = "Repository created with a .gitignore for RIFT projects.";
                return r;
            }, true);
        }
        ImGui::EndDisabled();
        if (busy()) { spinner(fs_ * 0.6f); ImGui::SameLine(); ImGui::TextDisabled("%s...", actionLabel_.c_str()); }
        return;
    }
    // Who commits: git refuses to commit without a name and an email.
    if (status_.userName.empty() || status_.userEmail.empty()) {
        ImGui::TextColored(theme::color::warning, ICON_FA_USER "  Who is committing?");
        ImGui::TextDisabled("Git records a name and an email with every commit.");
        if (nameDraft_.empty() && !status_.userName.empty()) nameDraft_ = status_.userName;
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##gitName", "Name", &nameDraft_);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##gitEmail", "Email", &emailDraft_);
        ImGui::Checkbox("Use for all my repositories", &identityGlobal_);
        ImGui::BeginDisabled(nameDraft_.empty() || emailDraft_.find('@') == std::string::npos || busy());
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
}

// Branch picker, fetch / pull / push / sync and where the branch stands.
void SourceControlPanel::drawToolbar(UiContext& ui, bool compact) {
    const float fs_ = ImGui::GetFontSize();
    const bool b = busy();
    const bool remote = !status_.remoteUrl.empty();
    const ImGuiStyle& style = ImGui::GetStyle();

    // Branch
    std::string branchLabel = status_.branch.empty() ? (status_.headHash.empty() ? std::string("(no branch)") : "(detached at " + status_.headHash.substr(0, 7) + ")") : status_.branch;
    char pullText[32], pushText[32];
    std::snprintf(pullText, sizeof(pullText), ICON_FA_ARROW_DOWN " %d", status_.behind);
    std::snprintf(pushText, sizeof(pushText), ICON_FA_ARROW_UP " %d", status_.ahead);
    const float buttons = ImGui::CalcTextSize(pullText).x + ImGui::CalcTextSize(pushText).x + style.FramePadding.x * 4.0f + ImGui::GetFrameHeight() * 3.0f +
                          style.ItemSpacing.x * 5.0f;
    ImGui::SetNextItemWidth(std::max(fs_ * 6.0f, compact ? ImGui::GetContentRegionAvail().x - buttons : fs_ * 14.0f));
    ImGui::BeginDisabled(b);
    if (ImGui::BeginCombo("##branch", (ICON_FA_CODE_BRANCH "  " + branchLabel).c_str(), ImGuiComboFlags_HeightLarge)) {
        ImGui::TextDisabled("Branches");
        for (const GitRef& r : status_.refs) {
            if (r.kind != GitRef::Local) continue;
            if (ImGui::Selectable(r.name.c_str(), r.head) && !r.head) doSwitch(ui, r.name);
        }
        bool anyRemote = false;
        for (const GitRef& r : status_.refs) {
            if (r.kind != GitRef::Remote) continue;
            // Remote branches that have a local branch already are left out.
            const size_t slash = r.name.find('/');
            const std::string local = slash == std::string::npos ? r.name : r.name.substr(slash + 1);
            bool have = false;
            for (const GitRef& l : status_.refs) have |= l.kind == GitRef::Local && l.name == local;
            if (have) continue;
            if (!anyRemote) { ImGui::Separator(); ImGui::TextDisabled("On the server"); anyRemote = true; }
            if (ImGui::Selectable((ICON_FA_CLOUD "  " + r.name).c_str())) doCheckoutRemote(ui, r.name);
        }
        ImGui::Separator();
        if (ImGui::Selectable(ICON_FA_PLUS "  New Branch...")) newBranch(ui, "", branchLabel);
        if (ImGui::Selectable(ICON_FA_SITEMAP "  Manage Branches")) openRepository_ = true;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    theme::tooltip("The branch you are on. Switching loads that branch's version of the project.");

    ImGui::SameLine();
    ImGui::BeginDisabled(b || !remote);
    if (theme::iconButton(ICON_FA_ARROWS_ROTATE, "Fetch: see what is new on the server (changes nothing here)")) doFetch(ui);
    ImGui::SameLine();
    ImGui::BeginDisabled(status_.upstream.empty());
    if (ImGui::Button(pullText)) doPull(ui);
    ImGui::EndDisabled();
    theme::tooltip(status_.upstream.empty() ? "Pull: this branch is not on the server yet" : "Pull: bring the server's new commits into your branch");
    ImGui::SameLine();
    if (ImGui::Button(pushText)) doPush(ui);
    theme::tooltip(status_.upstream.empty() ? "Push: publish this branch to the server" : "Push: send your commits to the server");
    ImGui::SameLine();
    ImGui::BeginDisabled(status_.upstream.empty());
    if (theme::iconButton(ICON_FA_ARROWS_UP_DOWN, "Sync: pull, then push")) doSync(ui);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (theme::iconButton(ICON_FA_ELLIPSIS_VERTICAL, "More")) ImGui::OpenPopup("##gitMore");
    if (ImGui::BeginPopup("##gitMore")) {
        if (ImGui::MenuItem(ICON_FA_SITEMAP "  Git Repository (branches and history)")) openRepository_ = true;
        if (ImGui::MenuItem(ICON_FA_LINK "  Connect to a Server (GitHub)...", nullptr, false, !b)) setRemote(ui);
        const std::string web = Git::webUrl(status_.remoteUrl);
        if (ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !web.empty())) Platform::openWithShell(web);
        if (ImGui::MenuItem(ICON_FA_ROTATE "  Refresh")) refreshTimer_ = 0.0f;
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show the Repository Folder")) Platform::openWithShell(Paths::toUtf8(status_.root));
        ImGui::Separator();
        ImGui::TextDisabled("%s", gitExe_.c_str());
        ImGui::EndPopup();
    }

    // Where the branch stands against the server.
    if (b) {
        spinner(fs_ * 0.5f);
        ImGui::SameLine();
        ImGui::TextDisabled("%s...", actionLabel_.c_str());
    } else if (!remote) {
        ImGui::TextDisabled(ICON_FA_CLOUD "  No server yet.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Connect to GitHub...")) setRemote(ui);
    } else if (status_.upstream.empty()) {
        ImGui::TextDisabled(ICON_FA_CLOUD "  %s is not on the server yet: Push publishes it.", branchLabel.c_str());
    } else if (status_.ahead || status_.behind) {
        ImGui::TextColored(theme::color::warning, ICON_FA_CLOUD "  %d outgoing, %d incoming (%s)", status_.ahead, status_.behind, status_.upstream.c_str());
    } else {
        ImGui::TextDisabled(ICON_FA_CLOUD "  Up to date with %s", status_.upstream.c_str());
    }
}

// A merge / rebase / cherry-pick / revert that stopped, and its conflicts.
void SourceControlPanel::drawOperation(UiContext& ui) {
    const bool any = status_.merging || status_.rebasing || status_.cherryPicking || status_.reverting;
    const int conflicts = status_.conflictCount();
    if (!any && conflicts == 0) return;
    const char* what = status_.rebasing ? "A rebase" : status_.cherryPicking ? "A cherry-pick" : status_.reverting ? "A revert" : "A merge";
    ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION "  %s is in progress", what);
    if (conflicts) {
        ImGui::TextDisabled("Both sides changed the same lines of %d file%s. Pick a version for each, or edit the file and mark it resolved. "
                            "Then commit to finish.", conflicts, conflicts == 1 ? "" : "s");
    } else {
        ImGui::TextDisabled("Every conflict is resolved: commit to finish.");
    }
    const bool b = busy();
    for (size_t k = 0; k < status_.changes.size(); ++k) {
        const GitChange& c = status_.changes[k];
        if (!c.conflicted()) continue;
        ImGui::PushID(static_cast<int>(k));
        ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION);
        ImGui::SameLine();
        ImGui::TextUnformatted(c.path.c_str());
        ImGui::BeginDisabled(b);
        const std::string path = c.path;
        auto resolveWith = [&](char how, const char* label) {
            start(ui, std::string(label) + " " + baseName(path), [path, how](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.resolve(root, path, how, &r.output);
                r.reload = how != 'f';
                return r;
            }, false);
        };
        if (ImGui::SmallButton("Keep Mine")) resolveWith('o', "Keep mine:");
        theme::tooltip("Use your branch's version of the file.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Take Theirs")) resolveWith('t', "Take theirs:");
        theme::tooltip("Use the incoming version of the file.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Mark Resolved")) resolveWith('f', "Resolved:");
        theme::tooltip("You fixed the file by hand (removed the <<<<<<< ======= >>>>>>> markers).");
        ImGui::SameLine();
        if (ImGui::SmallButton("Diff")) openWorkingDiff(c, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("Open")) Platform::openWithShell(Paths::toUtf8(status_.root / Paths::fromUtf8(c.path)));
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::BeginDisabled(b || !any);
    if (ImGui::Button(ICON_FA_XMARK "  Abort")) {
        const GitStatus s = status_;
        ask("Abort", std::string("Stop ") + what + " and go back to how things were before it?", "Abort", true, [this, s](UiContext& u) {
            start(u, "Abort", [s](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.abortOperation(root, s, &r.output);
                r.reload = true;
                return r;
            }, false);
        });
    }
    ImGui::EndDisabled();
    ImGui::Separator();
}

void SourceControlPanel::drawCommitBox(UiContext& ui) {
    Editor& editor = ui.editor;
    const float fs_ = ImGui::GetFontSize();
    const bool b = busy();
    int staged = 0, unstaged = 0;
    for (const GitChange& c : status_.changes) {
        staged += c.staged() ? 1 : 0;
        unstaged += c.unstaged() ? 1 : 0;
    }
    const int unsavedScene = editor.isDirty() ? 1 : 0;

    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextMultiline("##message", &message_, ImVec2(-FLT_MIN, fs_ * 3.2f));
    if (message_.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y),
                                            theme::u32(theme::color::textDim), amend_ ? "New message (empty keeps the old one)" : "Describe what you changed");
    }
    const bool ctrlEnter = ImGui::IsItemFocused() && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter);

    // Commit Staged when something is staged (like Visual Studio), else Commit All.
    const bool stagedOnly = staged > 0;
    const int count = stagedOnly ? staged : unstaged + staged + unsavedScene;
    const bool canCommit = !b && !status_.userName.empty() && (amend_ || (count > 0 && !message_.empty()));
    auto commit = [&](bool all, int then) { // then: 0 nothing, 1 push, 2 sync
        const std::string message = message_;
        const bool amend = amend_;
        const GitStatus s = status_;
        const char* label = then == 2 ? (all ? "Commit all and sync" : "Commit staged and sync")
                          : then == 1 ? (all ? "Commit all and push" : "Commit staged and push")
                          : amend ? "Amend commit" : (all ? "Commit all" : "Commit staged");
        start(ui, label, [message, amend, all, then, s](const Git& git, const fs::path& root) {
            Result r;
            r.ok = all ? git.commitAll(root, message, &r.output, amend) : git.commitStaged(root, message, &r.output, amend);
            if (r.ok && then) {
                std::string out;
                r.ok = then == 2 ? git.sync(root, s, &out) : git.push(root, s, &out);
                r.output += "\n" + out;
                r.reload = then == 2;
            }
            return r;
        }, all);
        message_.clear();
        amend_ = false;
    };
    const bool pushAfter = editor.settings().gitPushAfterCommit && !status_.remoteUrl.empty();
    char label[80];
    if (amend_) std::snprintf(label, sizeof(label), ICON_FA_CHECK "  Amend%s", stagedOnly ? " (with staged)" : "");
    else std::snprintf(label, sizeof(label), ICON_FA_CHECK "  Commit %s%s", stagedOnly ? "Staged" : "All", pushAfter ? " and Push" : "");
    ImGui::BeginDisabled(!canCommit);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::accentHover);
    if (ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x, 0.0f)) || (ctrlEnter && canCommit))
        commit(!stagedOnly, pushAfter ? 1 : 0);
    ImGui::PopStyleColor(2);
    ImGui::EndDisabled();
    theme::tooltip(stagedOnly ? "Records the staged files as one commit (Ctrl+Enter)." : "Saves the scene, then records every change as one commit (Ctrl+Enter).");
    ImGui::SameLine();
    ImGui::BeginDisabled(b);
    if (ImGui::ArrowButton("##commitMore", ImGuiDir_Down)) ImGui::OpenPopup("##commitMenu");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##commitMenu")) {
        const bool remote = !status_.remoteUrl.empty();
        const bool okAll = !status_.userName.empty() && (amend_ || (unstaged + staged + unsavedScene > 0 && !message_.empty()));
        const bool okStaged = !status_.userName.empty() && staged > 0 && (amend_ || !message_.empty());
        if (ImGui::MenuItem("Commit All", nullptr, false, okAll)) commit(true, 0);
        if (ImGui::MenuItem("Commit All and Push", nullptr, false, okAll && remote)) commit(true, 1);
        if (ImGui::MenuItem("Commit All and Sync", nullptr, false, okAll && !status_.upstream.empty())) commit(true, 2);
        ImGui::Separator();
        if (ImGui::MenuItem("Commit Staged", nullptr, false, okStaged)) commit(false, 0);
        if (ImGui::MenuItem("Commit Staged and Push", nullptr, false, okStaged && remote)) commit(false, 1);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_BOX_ARCHIVE "  Stash All", nullptr, false, unstaged + staged > 0)) {
            const std::string message = message_;
            start(ui, "Stash", [message](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.stashPush(root, message, &r.output);
                r.reload = true;
                return r;
            }, true);
            message_.clear();
        }
        ImGui::Separator();
        ImGui::MenuItem("Push after every commit", nullptr, &editor.settings().gitPushAfterCommit);
        ImGui::EndPopup();
    }
    ImGui::BeginDisabled(status_.noCommits);
    ImGui::Checkbox("Amend previous commit", &amend_);
    ImGui::EndDisabled();
    theme::tooltip("Replace the last commit with one that also has these changes (and the new message, if any). "
                   "Don't amend a commit you already pushed.");
}

void SourceControlPanel::drawChangeRow(UiContext& ui, const GitChange& c, bool staged, int id) {
    const float fs_ = ImGui::GetFontSize();
    const bool b = busy();
    ImGui::PushID(id);
    const char kind = staged ? c.stagedKind() : c.unstagedKind();
    const std::string key = std::string(staged ? "S:" : "W:") + c.path;
    const bool selected = showDiff_ && diffKey_ == key;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    // The buttons get their own space at the end of the row (shown on hover), so they never fight the row for clicks.
    const float h = ImGui::GetTextLineHeight();
    const float gap = fs_ * 0.15f;
    const int buttonCount = staged ? 1 : 2;
    const float buttonsWidth = h * buttonCount + gap * static_cast<float>(buttonCount);
    if (ImGui::Selectable("##row", selected, 0, ImVec2(std::max(1.0f, rowWidth - buttonsWidth), 0.0f))) openWorkingDiff(c, staged);
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    const bool hovered = ImGui::IsMouseHoveringRect(rowMin, ImVec2(rowMin.x + rowWidth, rowMax.y)) &&
                         ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    const fs::path file = status_.root / Paths::fromUtf8(c.path);
    if (ImGui::BeginPopupContextItem("##change")) {
        if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare With Unmodified")) openWorkingDiff(c, staged);
        if (staged) {
            if (ImGui::MenuItem(ICON_FA_MINUS "  Unstage", nullptr, false, !b)) {
                std::vector<std::string> paths = { c.path };
                if (!c.oldPath.empty()) paths.push_back(c.oldPath);
                start(ui, "Unstage " + baseName(c.path), [paths](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.unstage(root, paths, &r.output);
                    return r;
                }, false);
            }
        } else if (ImGui::MenuItem(ICON_FA_PLUS "  Stage", nullptr, false, !b)) {
            const std::string path = c.path;
            start(ui, "Stage " + baseName(path), [path](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.stage(root, { path }, &r.output);
                return r;
            }, true);
        }
        if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Undo Changes...", nullptr, false, !b)) {
            const GitChange change = c;
            ask("Undo changes", "Throw away your changes to " + c.path + "?\n" +
                (c.untracked() || c.index == 'A' ? "The file is new and will be deleted." : "It goes back to the last committed version. This cannot be undone."),
                "Undo Changes", true, [this, change](UiContext& u) {
                    start(u, "Undo " + baseName(change.path), [change](const Git& git, const fs::path& root) {
                        Result r;
                        r.ok = git.discard(root, change, &r.output);
                        r.reload = true;
                        return r;
                    }, false);
                });
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FILE "  Open", nullptr, false, kind != 'D')) Platform::openWithShell(Paths::toUtf8(file));
        if (ImGui::MenuItem(ICON_FA_FOLDER_OPEN "  Show in Explorer", nullptr, false, kind != 'D')) Platform::revealInExplorer(Paths::toUtf8(file));
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) Platform::setClipboard(c.path);
        ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s\n%s%s", c.path.c_str(), changeName(kind),
                                                                                  c.oldPath.empty() ? "" : (" from " + c.oldPath).c_str());

    // Icon, file name, folder dimmed after it; the stage / undo buttons on hover.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float y = rowStart.y;
    float x = rowStart.x + fs_ * 0.2f;
    dl->AddText(ImVec2(x, y), theme::u32(changeColor(kind)), changeIcon(kind));
    x += fs_ * 1.5f;
    const std::string name = baseName(c.path), folder = folderOf(c.path);
    dl->PushClipRect(ImVec2(x, rowMin.y), rowMax, true);
    dl->AddText(ImVec2(x, y), theme::u32(kind == 'D' ? theme::color::textDim : theme::color::text), name.c_str());
    x += ImGui::CalcTextSize(name.c_str()).x + fs_ * 0.5f;
    if (!folder.empty()) dl->AddText(ImVec2(x, y), theme::u32(theme::color::textDim), folder.c_str());
    dl->PopClipRect();
    ImGui::SameLine(0.0f, gap);
    if (!hovered) {
        ImGui::Dummy(ImVec2(buttonsWidth - gap, h));
        ImGui::PopID();
        return;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginDisabled(b);
    if (staged) {
        if (theme::iconButton(ICON_FA_MINUS, "Unstage", false, h)) {
            std::vector<std::string> paths = { c.path };
            if (!c.oldPath.empty()) paths.push_back(c.oldPath);
            start(ui, "Unstage " + name, [paths](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.unstage(root, paths, &r.output);
                return r;
            }, false);
        }
    } else {
        if (theme::iconButton(ICON_FA_ROTATE_LEFT, "Undo changes", false, h)) {
            const GitChange change = c;
            ask("Undo changes", "Throw away your changes to " + c.path + "?\n" +
                (c.untracked() || c.index == 'A' ? "The file is new and will be deleted." : "It goes back to the last committed version. This cannot be undone."),
                "Undo Changes", true, [this, change](UiContext& u) {
                    start(u, "Undo " + baseName(change.path), [change](const Git& git, const fs::path& root) {
                        Result r;
                        r.ok = git.discard(root, change, &r.output);
                        r.reload = true;
                        return r;
                    }, false);
                });
        }
        ImGui::SameLine(0.0f, gap);
        if (theme::iconButton(ICON_FA_PLUS, "Stage", false, h)) {
            const std::string path = c.path;
            start(ui, "Stage " + name, [path](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.stage(root, { path }, &r.output);
                return r;
            }, true);
        }
    }
    ImGui::EndDisabled();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

void SourceControlPanel::drawChangeList(UiContext& ui) {
    Editor& editor = ui.editor;
    const bool b = busy();
    std::vector<int> staged, unstaged;
    for (size_t k = 0; k < status_.changes.size(); ++k) {
        const GitChange& c = status_.changes[k];
        if (c.conflicted()) continue; // listed with the merge
        if (c.staged()) staged.push_back(static_cast<int>(k));
        if (c.unstaged()) unstaged.push_back(static_cast<int>(k));
    }
    const int unsavedScene = editor.isDirty() ? 1 : 0;
    ImGui::Spacing();

    if (!staged.empty()) {
        char title[64];
        std::snprintf(title, sizeof(title), "Staged Changes (%d)###staged", static_cast<int>(staged.size()));
        if (theme::sectionHeader(title)) {
            ImGui::BeginDisabled(b);
            if (ImGui::SmallButton(ICON_FA_MINUS "  Unstage All")) {
                start(ui, "Unstage all", [](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.unstage(root, {}, &r.output);
                    return r;
                }, false);
            }
            ImGui::EndDisabled();
            for (int k : staged) drawChangeRow(ui, status_.changes[k], true, k);
        }
    }

    char title[64];
    std::snprintf(title, sizeof(title), "Changes (%d)###changes", static_cast<int>(unstaged.size()) + unsavedScene);
    if (!theme::sectionHeader(title)) return;
    if (unsavedScene) {
        ImGui::TextColored(theme::color::warning, ICON_FA_CIRCLE "  ");
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextDisabled("%s: unsaved (saved when you commit all or stage)", editor.sceneName().c_str());
    }
    if (unstaged.empty()) {
        if (!unsavedScene) ImGui::TextDisabled(staged.empty() ? "Nothing changed since the last commit." : "Everything else is committed.");
        return;
    }
    ImGui::BeginDisabled(b);
    if (ImGui::SmallButton(ICON_FA_PLUS "  Stage All")) {
        start(ui, "Stage all", [](const Git& git, const fs::path& root) {
            Result r;
            r.ok = git.stage(root, {}, &r.output);
            return r;
        }, true);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT "  Undo All...")) {
        std::vector<GitChange> changes;
        for (int k : unstaged) changes.push_back(status_.changes[k]);
        ask("Undo all changes", "Throw away the changes to " + std::to_string(changes.size()) + " file" + (changes.size() == 1 ? "" : "s") +
            "?\nNew files are deleted, the others go back to the last commit. This cannot be undone.", "Undo All", true, [this, changes](UiContext& u) {
                start(u, "Undo all changes", [changes](const Git& git, const fs::path& root) {
                    Result r;
                    for (const GitChange& c : changes) {
                        std::string out;
                        if (!git.discard(root, c, &out)) { r.ok = false; r.output += out + "\n"; }
                    }
                    r.reload = true;
                    return r;
                }, false);
            });
    }
    ImGui::EndDisabled();
    for (int k : unstaged) drawChangeRow(ui, status_.changes[k], false, 100000 + k);
}

void SourceControlPanel::drawStashes(UiContext& ui) {
    if (status_.stashes.empty()) return;
    char title[64];
    std::snprintf(title, sizeof(title), "Stashes (%d)###stashes", static_cast<int>(status_.stashes.size()));
    if (!theme::sectionHeader(title, false)) return;
    ImGui::TextDisabled("Changes put aside. Apply one to bring it back.");
    const bool b = busy();
    for (size_t k = 0; k < status_.stashes.size(); ++k) {
        const GitStash& st = status_.stashes[k];
        ImGui::PushID(static_cast<int>(k));
        ImGui::TextColored(theme::color::textDim, ICON_FA_BOX_ARCHIVE);
        ImGui::SameLine();
        ImGui::TextUnformatted(st.message.c_str());
        ImGui::BeginDisabled(b);
        const std::string ref = st.ref;
        if (ImGui::SmallButton("Apply")) {
            start(ui, "Apply " + ref, [ref](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.stashApply(root, ref, false, &r.output);
                r.reload = true;
                return r;
            }, true);
        }
        theme::tooltip("Bring the changes back and keep the stash.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Pop")) {
            start(ui, "Pop " + ref, [ref](const Git& git, const fs::path& root) {
                Result r;
                r.ok = git.stashApply(root, ref, true, &r.output);
                r.reload = true;
                return r;
            }, true);
        }
        theme::tooltip("Bring the changes back and delete the stash.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Drop")) {
            ask("Drop stash", "Delete \"" + st.message + "\" and the changes in it?", "Drop", true, [this, ref](UiContext& u) {
                start(u, "Drop " + ref, [ref](const Git& git, const fs::path& root) {
                    Result r;
                    r.ok = git.stashDrop(root, ref, &r.output);
                    return r;
                }, false);
            });
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

// ============================================================================
// Git Repository

void SourceControlPanel::drawRepository(UiContext& ui, bool* open, ImGuiID dock) {
    Editor& editor = ui.editor;
    const float fs_ = ImGui::GetFontSize();
    if (dock) ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
    else ImGui::SetNextWindowSize(ImVec2(fs_ * 60.0f, fs_ * 34.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(ICON_FA_SITEMAP "  Git Repository###GitRepository", open)) {
        ImGui::End();
        return;
    }
    repoShownFrame_ = ImGui::GetFrameCount();
    if (!editor.hasProject() || gitExe_.empty() || !haveStatus_ || !status_.repository) {
        ImGui::PushTextWrapPos(0.0f);
        drawSetup(ui);
        ImGui::PopTextWrapPos();
        ImGui::End();
        return;
    }
    drawToolbar(ui, false);
    ImGui::Separator();
    if (ImGui::BeginTable("##repoLayout", 3, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Branches", ImGuiTableColumnFlags_WidthStretch, 0.2f);
        ImGui::TableSetupColumn("History", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 0.25f);
        ImGui::TableNextRow();
        const float height = ImGui::GetContentRegionAvail().y;
        ImGui::TableSetColumnIndex(0);
        if (ImGui::BeginChild("##branches", ImVec2(0.0f, height))) drawBranches(ui);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(1);
        if (ImGui::BeginChild("##history", ImVec2(0.0f, height))) drawGraph(ui);
        ImGui::EndChild();
        ImGui::TableSetColumnIndex(2);
        if (ImGui::BeginChild("##details", ImVec2(0.0f, height))) {
            ImGui::PushTextWrapPos(0.0f);
            drawDetails(ui);
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::End();
}

void SourceControlPanel::drawBranches(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    auto row = [&](const GitRef& r, const std::string& label, const char* icon) {
        ImGui::PushID(r.name.c_str());
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (r.hash == selectedCommit_ && !selectedCommit_.empty()) flags |= ImGuiTreeNodeFlags_Selected;
        if (r.head) ImGui::PushFont(theme::fonts().medium, 0.0f);
        ImGui::TreeNodeEx("##ref", flags, "%s  %s", icon, label.c_str());
        if (r.head) ImGui::PopFont();
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selectCommit(r.hash);
            scrollToCommit_ = r.hash;
        }
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !busy()) {
            if (r.kind == GitRef::Local && !r.head) doSwitch(ui, r.name);
            else if (r.kind == GitRef::Remote) doCheckoutRemote(ui, r.name);
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            if (r.kind == GitRef::Local)
                ImGui::SetTooltip("%s%s\n%s%s", r.name.c_str(), r.head ? " (checked out)" : "",
                                  r.upstream.empty() ? "Not on the server" : ("Tracks " + r.upstream).c_str(), r.gone ? " (deleted on the server)" : "");
            else ImGui::SetTooltip("%s\nDouble-click to check out", r.name.c_str());
        }
        if (ImGui::BeginPopupContextItem("##refMenu")) {
            branchActions(ui, r);
            ImGui::EndPopup();
        }
        // Outgoing / incoming counts.
        if (r.kind == GitRef::Local && (r.ahead || r.behind)) {
            char counts[32];
            std::snprintf(counts, sizeof(counts), "%s%d %s%d", r.ahead ? ICON_FA_ARROW_UP : "", r.ahead, r.behind ? ICON_FA_ARROW_DOWN : "", r.behind);
            if (!r.ahead) std::snprintf(counts, sizeof(counts), ICON_FA_ARROW_DOWN "%d", r.behind);
            else if (!r.behind) std::snprintf(counts, sizeof(counts), ICON_FA_ARROW_UP "%d", r.ahead);
            const float w = ImGui::CalcTextSize(counts).x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - w - fs_ * 0.2f);
            ImGui::TextColored(theme::color::warning, "%s", counts);
        }
        ImGui::PopID();
    };

    ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::TreeNodeEx("##local", ImGuiTreeNodeFlags_SpanAvailWidth, ICON_FA_CODE_BRANCH "  Branches")) {
        for (const GitRef& r : status_.refs)
            if (r.kind == GitRef::Local) row(r, r.name, r.head ? ICON_FA_CIRCLE_DOT : ICON_FA_CODE_BRANCH);
        if (status_.noCommits) ImGui::TextDisabled("No commits yet.");
        ImGui::TreePop();
    }
    // Remote branches, under their server's name.
    std::vector<std::string> remotes;
    for (const GitRef& r : status_.refs) {
        if (r.kind != GitRef::Remote) continue;
        const std::string server = r.name.substr(0, r.name.find('/'));
        if (std::find(remotes.begin(), remotes.end(), server) == remotes.end()) remotes.push_back(server);
    }
    if (!remotes.empty()) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        if (ImGui::TreeNodeEx("##remotes", ImGuiTreeNodeFlags_SpanAvailWidth, ICON_FA_CLOUD "  Remotes")) {
            for (const std::string& server : remotes) {
                ImGui::SetNextItemOpen(true, ImGuiCond_Once);
                if (ImGui::TreeNodeEx(server.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth, ICON_FA_SERVER "  %s", server.c_str())) {
                    for (const GitRef& r : status_.refs)
                        if (r.kind == GitRef::Remote && r.name.compare(0, server.size() + 1, server + "/") == 0)
                            row(r, r.name.substr(server.size() + 1), ICON_FA_CODE_BRANCH);
                    ImGui::TreePop();
                }
            }
            ImGui::TreePop();
        }
    } else {
        ImGui::TextDisabled(ICON_FA_CLOUD "  No server yet.");
        if (ImGui::SmallButton("Connect to GitHub...")) setRemote(ui);
    }
    bool anyTag = false;
    for (const GitRef& r : status_.refs) anyTag |= r.kind == GitRef::Tag;
    if (anyTag && ImGui::TreeNodeEx("##tags", ImGuiTreeNodeFlags_SpanAvailWidth, ICON_FA_TAG "  Tags")) {
        for (const GitRef& r : status_.refs)
            if (r.kind == GitRef::Tag) row(r, r.name, ICON_FA_TAG);
        ImGui::TreePop();
    }
    if (!status_.stashes.empty()) ImGui::TextDisabled(ICON_FA_BOX_ARCHIVE "  %d stash%s (see Git Changes)", static_cast<int>(status_.stashes.size()),
                                                      status_.stashes.size() == 1 ? "" : "es");
}

void SourceControlPanel::drawGraph(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint("##graphFilter", ICON_FA_MAGNIFYING_GLASS "  Filter by message, author or commit id", &graphFilter_);
    if (status_.noCommits) {
        ImGui::TextDisabled("No commits yet: commit in Git Changes to save the first version.");
        return;
    }
    if (!graphLoaded_) {
        spinner(fs_ * 0.5f);
        ImGui::SameLine();
        ImGui::TextDisabled("Reading the history...");
        return;
    }
    const bool filtering = !graphFilter_.empty();
    std::vector<int> rows;
    rows.reserve(graph_.size());
    for (size_t k = 0; k < graph_.size(); ++k) {
        const GitGraphCommit& c = graph_[k];
        if (!filtering || containsNoCase(c.subject, graphFilter_) || containsNoCase(c.author, graphFilter_) || c.hash.rfind(graphFilter_, 0) == 0)
            rows.push_back(static_cast<int>(k));
    }
    int scrollRow = -1;
    if (!scrollToCommit_.empty()) {
        for (size_t i = 0; i < rows.size(); ++i)
            if (graph_[rows[i]].hash == scrollToCommit_) scrollRow = static_cast<int>(i);
        scrollToCommit_.clear();
    }

    const float laneW = std::floor(fs_ * 0.9f);
    const float rowH = std::floor(fs_ * 1.65f);
    const int lanes = std::min(graphLanes_, 16);
    const float graphW = filtering ? laneW : laneW * static_cast<float>(lanes) + fs_ * 0.2f;
    const float thick = std::max(1.5f, fs_ * 0.12f);

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(fs_ * 0.3f, 0.0f));
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_Hideable | ImGuiTableFlags_SizingFixedFit;
    if (ImGui::BeginTable("##graph", 5, flags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, graphW);
        ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide);
        ImGui::TableSetupColumn("Author", ImGuiTableColumnFlags_WidthFixed, fs_ * 7.5f);
        ImGui::TableSetupColumn("Date", ImGuiTableColumnFlags_WidthFixed, fs_ * 6.5f);
        ImGui::TableSetupColumn("Commit", ImGuiTableColumnFlags_WidthFixed, fs_ * 4.0f);
        ImGui::TableHeadersRow();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(rows.size()), rowH);
        if (scrollRow >= 0) clipper.IncludeItemByIndex(scrollRow);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const GitGraphCommit& c = graph_[rows[i]];
                ImGui::TableNextRow(0, rowH);
                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(i);
                const ImVec2 cell = ImGui::GetCursorScreenPos();
                const bool selected = c.hash == selectedCommit_;
                if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, rowH)))
                    selectCommit(c.hash);
                if (i == scrollRow) ImGui::SetScrollHereY(0.4f);
                if (ImGui::BeginPopupContextItem("##commitMenu")) {
                    selectCommit(c.hash);
                    commitActions(ui, c.hash);
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
                    ImGui::SetTooltip("%s\n%s, %s\n%s", c.subject.c_str(), c.author.c_str(), dateTime(c.time).c_str(), c.hash.c_str());

                // The lanes: lines through the row and the commit's dot.
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float y0 = cell.y, y1 = cell.y + rowH, yc = std::floor((y0 + y1) * 0.5f);
                auto laneX = [&](int lane) { return std::floor(cell.x + laneW * (static_cast<float>(lane) + 0.5f)); };
                if (!filtering) {
                    for (const GitGraphCommit::Line& l : c.lines) {
                        if (l.fromLane >= lanes || l.toLane >= lanes) continue;
                        const ImVec2 a = l.fromCommit ? ImVec2(laneX(c.lane), yc) : ImVec2(laneX(l.fromLane), y0);
                        const ImVec2 bb = l.toCommit ? ImVec2(laneX(c.lane), yc) : ImVec2(laneX(l.toLane), y1);
                        const ImU32 col = theme::u32(laneColor(l.color));
                        if (a.x == bb.x) dl->AddLine(a, bb, col, thick);
                        else dl->AddBezierCubic(a, ImVec2(a.x, (a.y + bb.y) * 0.5f), ImVec2(bb.x, (a.y + bb.y) * 0.5f), bb, col, thick);
                    }
                }
                const ImVec2 dot(filtering ? laneX(0) : laneX(std::min(c.lane, lanes - 1)), yc);
                const ImU32 dotColor = theme::u32(laneColor(c.color));
                const float radius = laneW * 0.27f;
                if (c.hash == status_.headHash) {
                    dl->AddCircleFilled(dot, radius * 1.45f, theme::u32(theme::color::bg));
                    dl->AddCircle(dot, radius * 1.45f, dotColor, 0, thick * 1.2f);
                    dl->AddCircleFilled(dot, radius * 0.7f, dotColor);
                } else if (c.parents.size() > 1) {
                    dl->AddCircleFilled(dot, radius * 0.85f, theme::u32(theme::color::bg));
                    dl->AddCircle(dot, radius * 0.85f, dotColor, 0, thick);
                } else {
                    dl->AddCircleFilled(dot, radius, dotColor);
                }

                // Message, with the branches and tags that point here.
                ImGui::TableSetColumnIndex(1);
                const float textY = cell.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
                float x = ImGui::GetCursorScreenPos().x;
                const float badgeH = std::floor(fs_ * 1.25f), badgeY = cell.y + (rowH - badgeH) * 0.5f;
                for (const std::string& ref : c.refs) {
                    std::string text = ref;
                    ImVec4 color = theme::color::accent;
                    bool strong = false;
                    if (ref.rfind("HEAD -> ", 0) == 0) { text = ICON_FA_CODE_BRANCH " " + ref.substr(8); strong = true; }
                    else if (ref == "HEAD") { text = "HEAD"; strong = true; color = theme::color::warning; }
                    else if (ref.rfind("tag: ", 0) == 0) { text = ICON_FA_TAG " " + ref.substr(5); color = theme::color::warning; }
                    else {
                        bool remote = false;
                        for (const GitRef& r : status_.refs) remote |= r.kind == GitRef::Remote && r.name == ref;
                        if (remote) { text = ICON_FA_CLOUD " " + ref; color = ImVec4(0.45f, 0.66f, 1.0f, 1.0f); }
                        else text = ICON_FA_CODE_BRANCH " " + ref;
                    }
                    x += badge(dl, ImVec2(x, badgeY), badgeH, text, color, strong) + fs_ * 0.3f;
                }
                if (outgoing_.count(c.hash) || incoming_.count(c.hash)) {
                    const bool out = outgoing_.count(c.hash) > 0;
                    dl->AddText(ImVec2(x, textY), theme::u32(out ? theme::color::success : ImVec4(0.45f, 0.66f, 1.0f, 1.0f)), out ? ICON_FA_ARROW_UP : ICON_FA_ARROW_DOWN);
                    x += fs_ * 1.2f;
                }
                ImGui::SetCursorScreenPos(ImVec2(x, textY));
                if (c.hash == status_.headHash) ImGui::PushFont(theme::fonts().medium, 0.0f);
                ImGui::TextUnformatted(c.subject.c_str());
                if (c.hash == status_.headHash) ImGui::PopFont();

                ImGui::TableSetColumnIndex(2);
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, textY));
                ImGui::TextUnformatted(c.author.c_str());
                ImGui::TableSetColumnIndex(3);
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, textY));
                ImGui::TextDisabled("%s", timeAgo(c.time).c_str());
                ImGui::TableSetColumnIndex(4);
                ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, textY));
                ImGui::TextDisabled("%s", c.shortHash.c_str());
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
}

void SourceControlPanel::drawDetails(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    if (selectedCommit_.empty()) {
        ImGui::TextDisabled("Select a commit to see what it changed. Right-click one for branch, revert, reset and more.");
        return;
    }
    if (details_.hash != selectedCommit_) {
        spinner(fs_ * 0.5f);
        ImGui::SameLine();
        ImGui::TextDisabled("Reading the commit...");
        return;
    }
    const GitCommitDetails& d = details_;
    // The message: first line as a heading.
    const size_t newline = d.message.find('\n');
    theme::pushHeading(1.1f);
    ImGui::TextUnformatted(d.message.substr(0, newline).c_str());
    theme::pop();
    if (newline != std::string::npos) {
        std::string body = d.message.substr(newline + 1);
        while (!body.empty() && body.front() == '\n') body.erase(0, 1);
        if (!body.empty()) ImGui::TextUnformatted(body.c_str());
    }
    ImGui::Spacing();
    ImGui::TextColored(theme::color::textDim, ICON_FA_USER);
    ImGui::SameLine();
    ImGui::Text("%s", d.author.c_str());
    theme::tooltip(d.email.c_str());
    ImGui::TextColored(theme::color::textDim, ICON_FA_CLOCK);
    ImGui::SameLine();
    ImGui::Text("%s (%s)", dateTime(d.time).c_str(), timeAgo(d.time).c_str());
    ImGui::TextColored(theme::color::textDim, ICON_FA_CODE_COMMIT);
    ImGui::SameLine();
    ImGui::TextUnformatted(d.hash.substr(0, 10).c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_COPY "##copyHash")) Platform::setClipboard(d.hash);
    theme::tooltip("Copy the full commit id");
    if (!d.parents.empty()) {
        ImGui::TextDisabled(d.parents.size() > 1 ? "Parents" : "Parent");
        for (const std::string& p : d.parents) {
            ImGui::SameLine();
            if (ImGui::SmallButton(p.substr(0, 7).c_str())) {
                const std::string hash = p;
                selectCommit(hash);
                scrollToCommit_ = hash;
                return;
            }
        }
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(busy());
    if (ImGui::Button(ICON_FA_CODE_BRANCH "  New Branch...")) newBranch(ui, d.hash, "commit " + d.hash.substr(0, 7));
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_ELLIPSIS "  Actions")) ImGui::OpenPopup("##detailsActions");
    ImGui::EndDisabled();
    if (ImGui::BeginPopup("##detailsActions")) {
        commitActions(ui, d.hash);
        ImGui::EndPopup();
    }
    ImGui::Spacing();
    char title[64];
    std::snprintf(title, sizeof(title), "Changed files (%d)###files", static_cast<int>(d.files.size()));
    if (!theme::sectionHeader(title)) return;
    if (d.files.empty()) ImGui::TextDisabled(d.parents.size() > 1 ? "The merge itself changed nothing more." : "No files changed.");
    for (size_t k = 0; k < d.files.size(); ++k) {
        const GitFileChange& f = d.files[k];
        ImGui::PushID(static_cast<int>(k));
        const bool selected = showDiff_ && diffKey_ == d.hash + ":" + f.path;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##file", selected, 0, ImVec2(ImGui::GetContentRegionAvail().x, 0.0f))) openCommitDiff(d.hash, f);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
            ImGui::SetTooltip("%s\n%s%s", f.path.c_str(), changeName(f.kind), f.oldPath.empty() ? "" : (" from " + f.oldPath).c_str());
        if (ImGui::BeginPopupContextItem("##fileMenu")) {
            if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare With Previous")) openCommitDiff(d.hash, f);
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy Path")) Platform::setClipboard(f.path);
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 max = ImGui::GetItemRectMax();
        dl->PushClipRect(p, max, true);
        dl->AddText(ImVec2(p.x + fs_ * 0.2f, p.y), theme::u32(changeColor(f.kind)), changeIcon(f.kind));
        const std::string name = baseName(f.path), folder = folderOf(f.path);
        float x = p.x + fs_ * 1.7f;
        dl->AddText(ImVec2(x, p.y), theme::u32(theme::color::text), name.c_str());
        x += ImGui::CalcTextSize(name.c_str()).x + fs_ * 0.5f;
        if (!folder.empty()) dl->AddText(ImVec2(x, p.y), theme::u32(theme::color::textDim), folder.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
}

// ============================================================================
// The diff tab

void SourceControlPanel::drawDiff(UiContext&, ImGuiID dock) {
    if (!showDiff_) return;
    const float fs_ = ImGui::GetFontSize();
    if (dock) ImGui::SetNextWindowDockID(dock, ImGuiCond_FirstUseEver);
    else ImGui::SetNextWindowSize(ImVec2(fs_ * 44.0f, fs_ * 30.0f), ImGuiCond_FirstUseEver);
    if (focusDiff_) { ImGui::SetNextWindowFocus(); focusDiff_ = false; }
    const std::string title = std::string(ICON_FA_CODE_COMPARE "  ") + diffTitle_ + "###GitDiff";
    if (!ImGui::Begin(title.c_str(), &showDiff_)) {
        ImGui::End();
        return;
    }
    ImGui::TextColored(theme::color::success, "+%d", diffAdded_);
    ImGui::SameLine();
    ImGui::TextColored(theme::color::error, "-%d", diffRemoved_);
    ImGui::SameLine();
    ImGui::TextDisabled("lines");
    if (ImGui::BeginChild("##diffText", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (diffLines_.empty() || (diffLines_.size() == 1 && diffLines_[0].empty())) {
            ImGui::TextDisabled("No differences (perhaps only line endings changed).");
        }
        ImGui::PushFont(theme::fonts().mono, 0.0f);
        const float lineH = ImGui::GetTextLineHeightWithSpacing();
        const float width = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionMax().x);
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(diffLines_.size()), lineH);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& line = diffLines_[i];
                ImVec4 color = theme::color::text;
                ImVec4 back(0, 0, 0, 0);
                const bool header = line.rfind("diff ", 0) == 0 || line.rfind("index ", 0) == 0 || line.rfind("--- ", 0) == 0 || line.rfind("+++ ", 0) == 0 ||
                                    line.rfind("new file", 0) == 0 || line.rfind("deleted file", 0) == 0 || line.rfind("similarity", 0) == 0 ||
                                    line.rfind("rename ", 0) == 0 || line.rfind("old mode", 0) == 0 || line.rfind("new mode", 0) == 0;
                if (header) color = theme::color::textDim;
                else if (line.rfind("@@", 0) == 0) { color = theme::color::accentHover; back = ImVec4(theme::color::accent.x, theme::color::accent.y, theme::color::accent.z, 0.10f); }
                else if (!line.empty() && line[0] == '+') { color = theme::color::success; back = ImVec4(0.24f, 0.81f, 0.56f, 0.10f); }
                else if (!line.empty() && line[0] == '-') { color = theme::color::error; back = ImVec4(0.94f, 0.32f, 0.32f, 0.10f); }
                const ImVec2 p = ImGui::GetCursorScreenPos();
                if (back.w > 0.0f) ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width + ImGui::GetScrollX(), p.y + lineH), theme::u32(back));
                ImGui::TextColored(color, "%s", line.empty() ? " " : line.c_str());
            }
        }
        ImGui::PopFont();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ============================================================================
// The Git menu and dialogs

void SourceControlPanel::drawMenu(UiContext& ui, bool* showChanges, bool* showRepository) {
    Editor& editor = ui.editor;
    if (!ImGui::BeginMenu("Git", editor.hasProject())) return;
    const bool b = busy();
    const bool repo = haveStatus_ && status_.repository;
    const bool remote = !status_.remoteUrl.empty();
    ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Git Changes", nullptr, showChanges);
    ImGui::MenuItem(ICON_FA_SITEMAP "  Git Repository", nullptr, showRepository);
    ImGui::Separator();
    if (!repo) {
        if (ImGui::MenuItem(ICON_FA_PLUS "  Create Git Repository...", nullptr, false, !gitExe_.empty() && haveStatus_ && !b)) *showChanges = true;
        ImGui::EndMenu();
        return;
    }
    if (ImGui::MenuItem(ICON_FA_CHECK "  Commit or Stash...")) *showChanges = true;
    if (ImGui::MenuItem(ICON_FA_ARROWS_ROTATE "  Fetch", nullptr, false, !b && remote)) doFetch(ui);
    if (ImGui::MenuItem(ICON_FA_ARROW_DOWN "  Pull", nullptr, false, !b && !status_.upstream.empty())) doPull(ui);
    if (ImGui::MenuItem(ICON_FA_ARROW_UP "  Push", nullptr, false, !b && remote)) doPush(ui);
    if (ImGui::MenuItem(ICON_FA_ARROWS_UP_DOWN "  Sync (Pull, then Push)", nullptr, false, !b && !status_.upstream.empty())) doSync(ui);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_PLUS "  New Branch...", nullptr, false, !b)) newBranch(ui, "", status_.branch.empty() ? std::string("HEAD") : status_.branch);
    if (ImGui::BeginMenu(ICON_FA_CODE_BRANCH "  Switch Branch", !b)) {
        for (const GitRef& r : status_.refs)
            if (r.kind == GitRef::Local && ImGui::MenuItem(r.name.c_str(), nullptr, r.head, !r.head)) doSwitch(ui, r.name);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(ICON_FA_CODE_MERGE "  Merge Into Current Branch", !b && !status_.branch.empty())) {
        bool any = false;
        for (const GitRef& r : status_.refs) {
            if (r.kind == GitRef::Tag || r.head) continue;
            if (ImGui::MenuItem((r.kind == GitRef::Remote ? ICON_FA_CLOUD "  " + r.name : ICON_FA_CODE_BRANCH "  " + r.name).c_str())) doMerge(ui, r.name);
            any = true;
        }
        if (!any) ImGui::TextDisabled("No other branches");
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem(ICON_FA_SITEMAP "  Manage Branches")) *showRepository = true;
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_LINK "  Connect to a Server (GitHub)...", nullptr, false, !b)) setRemote(ui);
    const std::string web = Git::webUrl(status_.remoteUrl);
    if (ImGui::MenuItem(ICON_FA_GLOBE "  Open on GitHub", nullptr, false, !web.empty())) Platform::openWithShell(web);
    if (ImGui::MenuItem(ICON_FA_ROTATE "  Refresh")) refreshTimer_ = 0.0f;
    ImGui::EndMenu();
}

void SourceControlPanel::drawPopups(UiContext& ui) {
    const float fs_ = ImGui::GetFontSize();
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    if (openConfirm_) { ImGui::OpenPopup("###gitConfirm"); openConfirm_ = false; }
    if (openPrompt_) { ImGui::OpenPopup("###gitPrompt"); openPrompt_ = false; }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const std::string confirmTitle = confirm_.title + "###gitConfirm";
    if (ImGui::BeginPopupModal(confirmTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        if (confirm_.danger) {
            ImGui::TextColored(theme::color::warning, ICON_FA_TRIANGLE_EXCLAMATION);
            ImGui::SameLine();
        }
        ImGui::PushTextWrapPos(fs_ * 30.0f);
        ImGui::TextUnformatted(confirm_.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (confirm_.danger) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(theme::color::error.x, theme::color::error.y, theme::color::error.z, 0.75f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme::color::error);
        }
        const bool go = ImGui::Button(confirm_.button.c_str(), ImVec2(fs_ * 8.0f, 0.0f));
        if (confirm_.danger) ImGui::PopStyleColor(2);
        if (go) {
            ImGui::CloseCurrentPopup();
            if (confirm_.action) confirm_.action(ui);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    const std::string promptTitle = prompt_.title + "###gitPrompt";
    if (ImGui::BeginPopupModal(promptTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::PushTextWrapPos(fs_ * 26.0f);
        ImGui::TextDisabled("%s", prompt_.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetNextItemWidth(fs_ * 26.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputTextWithHint("##value", prompt_.hint.c_str(), &prompt_.value, ImGuiInputTextFlags_EnterReturnsTrue);
        std::string value = prompt_.value;
        while (!value.empty() && value.back() == ' ') value.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        const bool valid = prompt_.refName ? Git::validRefName(value) : !value.empty();
        if (!valid && !value.empty() && prompt_.refName) ImGui::TextColored(theme::color::warning, "Names can't have spaces or ~ ^ : ? * [ \\ .. and can't end in / or .");
        if (prompt_.showCheckbox) ImGui::Checkbox(prompt_.checkboxLabel.c_str(), &prompt_.checkbox);
        ImGui::Spacing();
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button(prompt_.button.c_str(), ImVec2(fs_ * 8.0f, 0.0f)) || (enter && valid)) {
            ImGui::CloseCurrentPopup();
            if (prompt_.action) prompt_.action(ui, value, prompt_.checkbox);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(fs_ * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace rift
