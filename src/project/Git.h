#pragma once
//
// Git.h -- version control for projects through the git command line
// (Git for Windows). Every call runs git and waits, so the editor calls these
// from a worker thread. git never prompts: credentials come from Git's own
// credential manager window, anything else fails with a message.
//
#include "platform/Process.h"

#include <filesystem>
#include <string>
#include <vector>

namespace rift {

struct GitChange {
    std::string path;        // relative to the repository, '/' separators
    std::string oldPath;     // renames: where it came from
    char index = ' ';        // staged state (porcelain v1 X)
    char worktree = ' ';     // unstaged state (porcelain v1 Y)
    bool untracked() const { return index == '?'; }
    bool conflicted() const { return index == 'U' || worktree == 'U' || (index == 'A' && worktree == 'A') || (index == 'D' && worktree == 'D'); }
    // 'M' modified, 'A' added (or untracked), 'D' deleted, 'R' renamed, 'C' conflict
    char kind() const;
};

struct GitCommit {
    std::string hash, shortHash, author, when, subject;
};

struct GitStatus {
    bool available = false;  // git was found
    bool repository = false; // the folder is inside a repository
    std::string error;       // what went wrong, if anything
    std::string branch;      // "" when detached or before the first commit
    bool noCommits = false;  // a fresh repository
    std::string upstream;    // "origin/main", "" if none
    int ahead = 0, behind = 0;
    std::string remoteUrl;   // origin
    std::vector<GitChange> changes;
    std::vector<GitCommit> history;
    std::vector<std::string> branches;
    std::string userName, userEmail;
    bool merging = false;    // a merge with conflicts is in progress
};

class Git {
public:
    // `exeOverride`: a git.exe chosen in Preferences ("" = find it).
    explicit Git(const std::string& exeOverride = "");

    // git.exe on PATH or in the usual install folders ("" if not installed).
    static std::string find(const std::string& exeOverride = "");
    bool available() const { return !exe_.empty(); }
    const std::string& exe() const { return exe_; }
    std::string version() const;

    // Runs `git args...` in `folder`.
    Platform::ProcessResult run(const std::filesystem::path& folder, const std::vector<std::string>& args, unsigned timeoutMs = 60000) const;

    GitStatus status(const std::filesystem::path& folder, int historyCount = 60) const;

    // Makes `folder` a repository with a RIFT .gitignore and (if the user's
    // name and email are set) a first commit of everything in it.
    bool init(const std::filesystem::path& folder, std::string* error) const;
    // Stages everything and commits it.
    bool commitAll(const std::filesystem::path& folder, const std::string& message, std::string* output) const;
    bool push(const std::filesystem::path& folder, const GitStatus& status, std::string* output) const;
    bool pull(const std::filesystem::path& folder, std::string* output) const;
    bool fetch(const std::filesystem::path& folder, std::string* output) const;
    bool setRemote(const std::filesystem::path& folder, const std::string& url, std::string* output) const;
    bool switchBranch(const std::filesystem::path& folder, const std::string& branch, bool create, std::string* output) const;
    // Throws away the changes to one file (an untracked file is deleted).
    bool discard(const std::filesystem::path& folder, const GitChange& change, std::string* output) const;
    bool setIdentity(const std::filesystem::path& folder, const std::string& name, const std::string& email, bool global, std::string* output) const;
    bool abortMerge(const std::filesystem::path& folder, std::string* output) const;

    // What .gitignore gets for a new project.
    static const char* defaultIgnore();
    // Parses `git status --porcelain=v1 -b -z` (exposed for tests).
    static void parseStatus(const std::string& text, GitStatus& out);
    // A GitHub page for a remote URL (https://github.com/owner/repo), or "".
    static std::string webUrl(const std::string& remoteUrl);

private:
    std::string exe_;
};

} // namespace rift
