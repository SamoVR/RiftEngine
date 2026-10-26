#pragma once
//
// Git.h -- version control for projects through the git command line
// (Git for Windows). Every call runs git and waits, so the editor calls these
// from a worker thread. git never prompts: credentials come from Git's own
// credential manager window, anything else fails with a message.
//
// Paths in GitStatus, GitChange and GitFileChange are relative to the
// repository's root (which may be above the project folder), '/' separated.
//
#include "platform/Process.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rift {

struct GitChange {
    std::string path;        // relative to the repository root
    std::string oldPath;     // renames: where it came from
    char index = ' ';        // staged state (porcelain v1 X)
    char worktree = ' ';     // unstaged state (porcelain v1 Y)
    bool untracked() const { return index == '?'; }
    bool conflicted() const { return index == 'U' || worktree == 'U' || (index == 'A' && worktree == 'A') || (index == 'D' && worktree == 'D'); }
    bool staged() const { return !untracked() && !conflicted() && index != ' '; }
    bool unstaged() const { return untracked() || conflicted() || worktree != ' '; }
    // 'M' modified, 'A' added (or untracked), 'D' deleted, 'R' renamed, 'C' conflict
    char kind() const;
    char stagedKind() const;   // the staged half ('M', 'A', 'D', 'R')
    char unstagedKind() const; // the working-tree half
};

// A branch or tag.
struct GitRef {
    enum Kind { Local, Remote, Tag } kind = Local;
    std::string name;      // "main", "origin/main", "v1.0"
    std::string hash;      // the commit it points at
    std::string upstream;  // locals: "origin/main" or ""
    int ahead = 0, behind = 0;
    bool head = false;     // the checked-out branch
    bool gone = false;     // the upstream was deleted on the server
};

struct GitStash {
    std::string ref;      // "stash@{0}"
    std::string message;
};

struct GitStatus {
    bool available = false;  // git was found
    bool repository = false; // the folder is inside a repository
    std::string error;       // what went wrong, if anything
    std::filesystem::path root;  // the repository's top folder
    std::string projectPrefix;   // the project folder inside the repository ("" = the root)
    std::string branch;      // "" when detached or before the first commit
    std::string headHash;    // the commit checked out ("" before the first commit)
    bool noCommits = false;  // a fresh repository
    std::string upstream;    // "origin/main", "" if none
    int ahead = 0, behind = 0;
    std::string remoteUrl;   // origin
    std::vector<GitChange> changes;
    std::vector<GitRef> refs;      // local branches, remote branches, tags
    std::vector<GitStash> stashes;
    std::string userName, userEmail;
    bool merging = false;    // a merge with conflicts is in progress
    bool rebasing = false;
    bool cherryPicking = false, reverting = false;

    std::vector<std::string> localBranches() const;
    int conflictCount() const;
};

// One commit of the history graph, with the lanes to draw it.
struct GitGraphCommit {
    std::string hash, shortHash, author, email, subject;
    int64_t time = 0;                  // unix seconds
    std::vector<std::string> parents;
    std::vector<std::string> refs;     // "HEAD -> main", "origin/main", "tag: v1.0"
    // Layout (layoutGraph): the commit sits in `lane`; `lanesAbove` are the
    // lanes that come in from the row above (each with its colour), `lines`
    // join the top of this row to its bottom.
    int lane = 0;
    int color = 0;
    struct Line { int fromLane, toLane, color; bool fromCommit, toCommit; };
    std::vector<Line> lines;           // drawn in this row: top half and bottom half
    int width = 1;                     // lanes used in this row
};

struct GitFileChange {
    char kind = 'M';         // 'A', 'M', 'D', 'R'
    std::string path, oldPath;
};

struct GitCommitDetails {
    std::string hash, author, email, committer, message;
    int64_t time = 0;
    std::vector<std::string> parents;
    std::vector<GitFileChange> files;
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

    // Runs `git args...` in `folder`. `data`: stdout only (stderr apart in .errors),
    // for output that is parsed.
    Platform::ProcessResult run(const std::filesystem::path& folder, const std::vector<std::string>& args, unsigned timeoutMs = 60000,
                                bool data = false) const;

    GitStatus status(const std::filesystem::path& folder) const;

    // ---- history
    // The newest `count` commits of every branch, newest first, laid out as a graph.
    std::vector<GitGraphCommit> graph(const std::filesystem::path& folder, int count = 400) const;
    // Lanes and lines for commits given newest first (exposed for tests).
    static void layoutGraph(std::vector<GitGraphCommit>& commits);
    bool commitDetails(const std::filesystem::path& folder, const std::string& hash, GitCommitDetails& out) const;
    // A unified diff: of a commit's file (against its first parent), or of a
    // working file (staged: what is staged; otherwise what is not).
    std::string commitFileDiff(const std::filesystem::path& folder, const std::string& hash, const GitFileChange& file) const;
    std::string workingDiff(const std::filesystem::path& folder, const GitChange& change, bool staged) const;

    // ---- making a repository
    // Makes `folder` a repository with a RIFT .gitignore and (if the user's
    // name and email are set) a first commit of everything in it.
    bool init(const std::filesystem::path& folder, std::string* error) const;
    bool setIdentity(const std::filesystem::path& folder, const std::string& name, const std::string& email, bool global, std::string* output) const;
    bool setRemote(const std::filesystem::path& folder, const std::string& url, std::string* output) const;

    // ---- changes
    bool stage(const std::filesystem::path& folder, const std::vector<std::string>& paths, std::string* output) const;   // empty: all
    bool unstage(const std::filesystem::path& folder, const std::vector<std::string>& paths, std::string* output) const; // empty: all
    // Throws away the changes to one file (an untracked file is deleted).
    bool discard(const std::filesystem::path& folder, const GitChange& change, std::string* output) const;
    // Stages everything and commits it (amend: replaces the last commit).
    bool commitAll(const std::filesystem::path& folder, const std::string& message, std::string* output, bool amend = false) const;
    bool commitStaged(const std::filesystem::path& folder, const std::string& message, std::string* output, bool amend = false) const;
    // Conflicts: keep this side's version, take the other side's, or mark as fixed by hand.
    bool resolve(const std::filesystem::path& folder, const std::string& path, char how, std::string* output) const; // 'o' ours, 't' theirs, 'f' fixed

    // ---- server
    bool push(const std::filesystem::path& folder, const GitStatus& status, std::string* output) const;
    bool pull(const std::filesystem::path& folder, std::string* output) const;
    bool fetch(const std::filesystem::path& folder, std::string* output) const;
    bool sync(const std::filesystem::path& folder, const GitStatus& status, std::string* output) const; // pull, then push

    // ---- branches and commits
    bool switchBranch(const std::filesystem::path& folder, const std::string& branch, bool create, std::string* output) const;
    bool createBranch(const std::filesystem::path& folder, const std::string& name, const std::string& startPoint, bool checkout, std::string* output) const;
    bool checkoutRemote(const std::filesystem::path& folder, const std::string& remoteBranch, std::string* output) const; // origin/x -> local x tracking it
    bool checkoutCommit(const std::filesystem::path& folder, const std::string& hash, std::string* output) const;        // detached
    bool deleteBranch(const std::filesystem::path& folder, const std::string& name, bool force, std::string* output) const;
    bool deleteRemoteBranch(const std::filesystem::path& folder, const std::string& remoteBranch, std::string* output) const;
    bool renameBranch(const std::filesystem::path& folder, const std::string& from, const std::string& to, std::string* output) const;
    bool merge(const std::filesystem::path& folder, const std::string& what, std::string* output) const;
    bool abortMerge(const std::filesystem::path& folder, std::string* output) const;
    bool revert(const std::filesystem::path& folder, const std::string& hash, std::string* output) const;
    bool cherryPick(const std::filesystem::path& folder, const std::string& hash, std::string* output) const;
    bool abortOperation(const std::filesystem::path& folder, const GitStatus& status, std::string* output) const; // merge / revert / cherry-pick / rebase
    bool reset(const std::filesystem::path& folder, const std::string& hash, char mode, std::string* output) const; // 's'oft 'm'ixed 'h'ard
    bool createTag(const std::filesystem::path& folder, const std::string& name, const std::string& hash, std::string* output) const;
    bool deleteTag(const std::filesystem::path& folder, const std::string& name, std::string* output) const;

    // ---- stashes
    bool stashPush(const std::filesystem::path& folder, const std::string& message, std::string* output) const;
    bool stashApply(const std::filesystem::path& folder, const std::string& ref, bool pop, std::string* output) const;
    bool stashDrop(const std::filesystem::path& folder, const std::string& ref, std::string* output) const;

    // What .gitignore gets for a new project.
    static const char* defaultIgnore();
    // Parses `git status --porcelain=v1 -b -z` (exposed for tests).
    static void parseStatus(const std::string& text, GitStatus& out);
    // A GitHub page for a remote URL (https://github.com/owner/repo), or "".
    static std::string webUrl(const std::string& remoteUrl);
    // A valid branch / tag name?
    static bool validRefName(const std::string& name);

private:
    std::string exe_;
};

} // namespace rift
