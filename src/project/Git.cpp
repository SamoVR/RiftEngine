#include "project/Git.h"
#include "core/Paths.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rift {

namespace fs = std::filesystem;

char GitChange::kind() const {
    if (conflicted()) return 'C';
    if (untracked()) return 'A';
    const char c = index != ' ' ? index : worktree;
    if (c == 'R' || c == 'C') return 'R';
    if (c == 'A' || c == 'D') return c;
    return 'M';
}

char GitChange::stagedKind() const {
    if (index == 'R' || index == 'C') return 'R';
    if (index == 'A' || index == 'D') return index;
    return 'M';
}

char GitChange::unstagedKind() const {
    if (conflicted()) return 'C';
    if (untracked()) return 'A';
    if (worktree == 'D') return 'D';
    return 'M';
}

std::vector<std::string> GitStatus::localBranches() const {
    std::vector<std::string> out;
    for (const GitRef& r : refs)
        if (r.kind == GitRef::Local) out.push_back(r.name);
    return out;
}

int GitStatus::conflictCount() const {
    int n = 0;
    for (const GitChange& c : changes) n += c.conflicted() ? 1 : 0;
    return n;
}

Git::Git(const std::string& exeOverride) : exe_(find(exeOverride)) {}

std::string Git::find(const std::string& exeOverride) {
    std::error_code ec;
    if (!exeOverride.empty() && fs::exists(Paths::fromUtf8(exeOverride), ec)) return exeOverride;
    std::string onPath = Platform::findOnPath("git");
    if (!onPath.empty()) return onPath;
    // Git for Windows installs here when it is not added to PATH.
    std::vector<fs::path> candidates;
    for (const char* var : { "ProgramFiles", "ProgramW6432", "ProgramFiles(x86)" }) {
        if (const char* v = std::getenv(var)) candidates.push_back(fs::path(v) / "Git" / "cmd" / "git.exe");
    }
    const std::string local = Platform::localAppDataFolder();
    if (!local.empty()) candidates.push_back(Paths::fromUtf8(local) / "Programs" / "Git" / "cmd" / "git.exe");
    for (const fs::path& c : candidates) {
        if (fs::exists(c, ec)) return Paths::toUtf8(c);
    }
    return {};
}

Platform::ProcessResult Git::run(const fs::path& folder, const std::vector<std::string>& args, unsigned timeoutMs, bool data) const {
    if (exe_.empty()) {
        Platform::ProcessResult r;
        r.output = "Git is not installed";
        return r;
    }
    // Paths as they are (not \octal escaped), no signatures in log output.
    std::vector<std::string> full = { "-c", "core.quotepath=off", "-c", "log.showSignature=false", "-c", "color.ui=never" };
    full.insert(full.end(), args.begin(), args.end());
    // Never wait for a question on a console nobody sees; plain English output.
    return Platform::runProcess(exe_, full, Paths::toUtf8(folder), timeoutMs,
                                { "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=true", "GIT_MERGE_AUTOEDIT=no", "LC_ALL=C", "LANG=C", "GIT_OPTIONAL_LOCKS=0" },
                                data);
}

std::string Git::version() const {
    const auto r = Platform::runProcess(exe_, { "--version" }, "", 10000);
    std::string v = r.output;
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
    return r.ok() ? v : std::string();
}

namespace {
    std::string trimmed(std::string s) {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        size_t start = 0;
        while (start < s.size() && (s[start] == ' ' || s[start] == '\n' || s[start] == '\r')) ++start;
        return s.substr(start);
    }
    bool done(const Platform::ProcessResult& r, std::string* output) {
        if (output) *output = trimmed(r.output + (r.errors.empty() ? "" : "\n" + r.errors));
        if (output && output->empty() && !r.ok()) *output = r.timedOut ? "git took too long and was stopped" : "git failed (exit code " + std::to_string(r.exitCode) + ")";
        return r.ok();
    }
    std::vector<std::string> split(const std::string& text, char sep) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : text) {
            if (c == sep) { out.push_back(cur); cur.clear(); }
            else cur += c;
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }
    std::vector<std::string> lines(const std::string& text) {
        std::vector<std::string> out;
        for (std::string l : split(text, '\n')) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            out.push_back(l);
        }
        return out;
    }
    // A path from the repository's root, whatever folder git runs in; no wildcards.
    std::string top(const std::string& path) { return ":(top,literal)" + path; }
    void addPaths(std::vector<std::string>& args, const std::vector<std::string>& paths) {
        args.push_back("--");
        for (const std::string& p : paths) args.push_back(top(p));
    }
    // "[ahead 2, behind 1]", "[gone]"
    void parseTrack(const std::string& track, int& ahead, int& behind, bool& gone) {
        if (const size_t a = track.find("ahead "); a != std::string::npos) ahead = std::atoi(track.c_str() + a + 6);
        if (const size_t b = track.find("behind "); b != std::string::npos) behind = std::atoi(track.c_str() + b + 7);
        gone = track.find("gone") != std::string::npos;
    }
    constexpr size_t kMaxDiff = 2 * 1024 * 1024;
    std::string capped(std::string text) {
        if (text.size() > kMaxDiff) {
            text.resize(kMaxDiff);
            text += "\n... (the rest of the diff is not shown)\n";
        }
        return text;
    }
}

void Git::parseStatus(const std::string& text, GitStatus& out) {
    // Entries are NUL-separated; a rename is followed by its original path.
    const std::vector<std::string> entries = split(text, '\0');
    for (size_t k = 0; k < entries.size(); ++k) {
        const std::string& e = entries[k];
        if (e.rfind("## ", 0) == 0) {
            // "## main...origin/main [ahead 1, behind 2]", "## No commits yet on main", "## HEAD (no branch)"
            std::string head = e.substr(3);
            if (head.rfind("No commits yet on ", 0) == 0) {
                out.noCommits = true;
                out.branch = head.substr(18);
                continue;
            }
            if (head.rfind("Initial commit on ", 0) == 0) { // git before 2.15
                out.noCommits = true;
                out.branch = head.substr(18);
                continue;
            }
            if (head.rfind("HEAD (no branch)", 0) == 0) { out.branch.clear(); continue; }
            std::string counts;
            if (const size_t bracket = head.find(" ["); bracket != std::string::npos) {
                counts = head.substr(bracket + 2);
                head = head.substr(0, bracket);
            }
            if (const size_t dots = head.find("..."); dots != std::string::npos) {
                out.branch = head.substr(0, dots);
                out.upstream = head.substr(dots + 3);
            } else {
                out.branch = head;
            }
            bool gone = false;
            parseTrack(counts, out.ahead, out.behind, gone);
            continue;
        }
        if (e.size() < 4) continue;
        GitChange c;
        c.index = e[0];
        c.worktree = e[1];
        c.path = e.substr(3);
        if ((c.index == 'R' || c.index == 'C') && k + 1 < entries.size()) c.oldPath = entries[++k];
        if (c.index == '!') continue; // ignored files
        out.changes.push_back(std::move(c));
    }
}

GitStatus Git::status(const fs::path& folder) const {
    GitStatus s;
    s.available = available();
    if (!s.available) { s.error = "Git is not installed"; return s; }

    // Where the repository is, in one call. Outside a repository this fails (not an error).
    const auto where = run(folder, { "rev-parse", "--show-toplevel", "--absolute-git-dir", "--show-prefix" }, 15000, true);
    if (!where.ok()) return s;
    const std::vector<std::string> w = lines(where.output);
    if (w.size() < 2 || w[0].empty()) return s;
    s.repository = true;
    s.root = Paths::fromUtf8(w[0]).make_preferred();
    const fs::path gitDir = Paths::fromUtf8(w[1]);
    s.projectPrefix = w.size() > 2 ? w[2] : std::string();
    while (!s.projectPrefix.empty() && s.projectPrefix.back() == '/') s.projectPrefix.pop_back();

    const auto st = run(s.root, { "status", "--porcelain=v1", "-b", "-z", "--untracked-files=all" }, 30000, true);
    if (!st.ok()) { s.error = trimmed(st.errors.empty() ? st.output : st.errors); return s; }
    parseStatus(st.output, s);

    std::error_code ec;
    s.merging = fs::exists(gitDir / "MERGE_HEAD", ec);
    s.rebasing = fs::exists(gitDir / "rebase-merge", ec) || fs::exists(gitDir / "rebase-apply", ec);
    s.cherryPicking = fs::exists(gitDir / "CHERRY_PICK_HEAD", ec);
    s.reverting = fs::exists(gitDir / "REVERT_HEAD", ec);

    // Branches and tags (tags point at their commit, not the tag object).
    const auto refs = run(s.root, { "for-each-ref",
        "--format=%(refname)%1f%(if)%(*objectname)%(then)%(*objectname)%(else)%(objectname)%(end)%1f%(upstream:short)%1f%(upstream:track)%1f%(HEAD)",
        "refs/heads", "refs/remotes", "refs/tags" }, 30000, true);
    if (refs.ok()) {
        for (const std::string& line : lines(refs.output)) {
            const std::vector<std::string> f = split(line, '\x1f');
            if (f.size() < 2) continue;
            GitRef r;
            const std::string& full = f[0];
            if (full.rfind("refs/heads/", 0) == 0) { r.kind = GitRef::Local; r.name = full.substr(11); }
            else if (full.rfind("refs/remotes/", 0) == 0) {
                r.kind = GitRef::Remote;
                r.name = full.substr(13);
                if (r.name.size() >= 5 && r.name.compare(r.name.size() - 5, 5, "/HEAD") == 0) continue; // origin/HEAD
            } else if (full.rfind("refs/tags/", 0) == 0) { r.kind = GitRef::Tag; r.name = full.substr(10); }
            else continue;
            r.hash = f[1];
            if (f.size() > 2) r.upstream = f[2];
            if (f.size() > 3) parseTrack(f[3], r.ahead, r.behind, r.gone);
            r.head = f.size() > 4 && f[4] == "*";
            if (r.head) s.headHash = r.hash;
            s.refs.push_back(std::move(r));
        }
    }
    if (s.headHash.empty() && !s.noCommits) {
        if (const auto head = run(s.root, { "rev-parse", "-q", "--verify", "HEAD" }, 10000, true); head.ok()) s.headHash = trimmed(head.output);
    }

    if (const auto stashes = run(s.root, { "stash", "list", "--format=%gd%x1f%s" }, 15000, true); stashes.ok()) {
        for (const std::string& line : lines(stashes.output)) {
            const std::vector<std::string> f = split(line, '\x1f');
            if (f.size() >= 2) s.stashes.push_back({ f[0], f[1] });
        }
    }
    // Name, email and server in one call (keys come back in lower case).
    const auto config = run(s.root, { "config", "--get-regexp", "^(user\\.(name|email)|remote\\.origin\\.url)$" }, 10000, true);
    for (const std::string& line : lines(config.output)) {
        const size_t space = line.find(' ');
        if (space == std::string::npos) continue;
        const std::string key = line.substr(0, space), value = line.substr(space + 1);
        if (key == "user.name") s.userName = value;
        else if (key == "user.email") s.userEmail = value;
        else if (key == "remote.origin.url") s.remoteUrl = value;
    }
    return s;
}

// ---- history

std::vector<GitGraphCommit> Git::graph(const fs::path& folder, int count) const {
    std::vector<GitGraphCommit> out;
    const auto log = run(folder, { "log", "--exclude=refs/stash", "--all", "--date-order", "-n", std::to_string(count),
                                   "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%D%x1f%s%x1e" }, 60000, true);
    if (!log.ok()) return out;
    for (std::string record : split(log.output, '\x1e')) {
        while (!record.empty() && (record[0] == '\n' || record[0] == '\r')) record.erase(0, 1);
        const std::vector<std::string> f = split(record, '\x1f');
        if (f.size() < 5 || f[0].size() < 7) continue;
        GitGraphCommit c;
        c.hash = f[0];
        c.shortHash = f[0].substr(0, 7);
        for (const std::string& p : split(f[1], ' '))
            if (!p.empty()) c.parents.push_back(p);
        c.author = f[2];
        c.email = f[3];
        c.time = std::atoll(f[4].c_str());
        if (f.size() > 5) {
            for (std::string r : split(f[5], ',')) {
                r = trimmed(r);
                if (!r.empty() && r != "refs/stash") c.refs.push_back(r);
            }
        }
        if (f.size() > 6) c.subject = f[6];
        out.push_back(std::move(c));
    }
    layoutGraph(out);
    return out;
}

void Git::layoutGraph(std::vector<GitGraphCommit>& commits) {
    // Each lane waits for one commit (its hash); "" is a free lane.
    std::vector<std::string> lanes;
    std::vector<int> colors;
    int nextColor = 0;
    auto freeLane = [&](int avoid) {
        for (int k = 0; k < static_cast<int>(lanes.size()); ++k)
            if (lanes[k].empty() && k != avoid) return k;
        lanes.emplace_back();
        colors.push_back(0);
        return static_cast<int>(lanes.size()) - 1;
    };
    for (GitGraphCommit& c : commits) {
        c.lines.clear();
        int widthAbove = static_cast<int>(lanes.size());
        // The lanes that were waiting for this commit join into it; the first is its own.
        int mine = -1;
        for (int k = 0; k < static_cast<int>(lanes.size()); ++k) {
            if (lanes[k] != c.hash) continue;
            if (mine < 0) mine = k;
        }
        if (mine < 0) { // a branch tip nothing below the rows above leads to
            mine = freeLane(-1);
            colors[mine] = nextColor++;
        }
        c.lane = mine;
        c.color = colors[mine];
        for (int k = 0; k < widthAbove; ++k) {
            if (lanes[k].empty()) continue;
            if (lanes[k] == c.hash) {
                c.lines.push_back({ k, mine, colors[k], false, true });
                if (k != mine) lanes[k].clear();
            } else {
                c.lines.push_back({ k, k, colors[k], false, false }); // passes by
            }
        }
        // Below: the first parent continues this lane, the others get lanes of their own.
        lanes[mine] = c.parents.empty() ? std::string() : c.parents[0];
        if (!c.parents.empty()) c.lines.push_back({ mine, mine, colors[mine], true, false });
        for (size_t p = 1; p < c.parents.size(); ++p) {
            int target = -1;
            for (int k = 0; k < static_cast<int>(lanes.size()); ++k)
                if (lanes[k] == c.parents[p]) { target = k; break; }
            if (target < 0) {
                target = freeLane(mine);
                lanes[target] = c.parents[p];
                colors[target] = nextColor++;
            }
            c.lines.push_back({ mine, target, colors[target], true, false });
        }
        while (!lanes.empty() && lanes.back().empty()) { lanes.pop_back(); colors.pop_back(); }
        c.width = std::max({ widthAbove, static_cast<int>(lanes.size()), mine + 1 });
    }
}

bool Git::commitDetails(const fs::path& folder, const std::string& hash, GitCommitDetails& out) const {
    out = GitCommitDetails{};
    const auto show = run(folder, { "show", "-s", "--format=%H%x1f%an%x1f%ae%x1f%cn%x1f%at%x1f%P%x1f%B", hash, "--" }, 30000, true);
    if (!show.ok()) return false;
    const std::vector<std::string> f = split(show.output, '\x1f');
    if (f.size() < 6) return false;
    out.hash = f[0];
    out.author = f[1];
    out.email = f[2];
    out.committer = f[3];
    out.time = std::atoll(f[4].c_str());
    for (const std::string& p : split(f[5], ' '))
        if (!p.empty()) out.parents.push_back(p);
    out.message = f.size() > 6 ? trimmed(f[6]) : std::string();

    // Files changed, against the first parent (what the commit brought to its branch).
    std::vector<std::string> args = { "diff-tree", "-r", "--no-commit-id", "--name-status", "-M", "-z" };
    if (out.parents.empty()) { args.push_back("--root"); args.push_back(hash); }
    else { args.push_back(out.parents[0]); args.push_back(hash); }
    const auto files = run(folder, args, 30000, true);
    if (files.ok()) {
        const std::vector<std::string> e = split(files.output, '\0');
        for (size_t k = 0; k < e.size(); ++k) {
            if (e[k].empty()) continue;
            GitFileChange fc;
            const char status = e[k][0];
            fc.kind = status == 'R' || status == 'C' ? 'R' : status == 'A' || status == 'D' ? status : 'M';
            if (status == 'R' || status == 'C') {
                if (k + 2 >= e.size()) break;
                fc.oldPath = e[++k];
                fc.path = e[++k];
            } else if (k + 1 < e.size()) {
                fc.path = e[++k];
            }
            out.files.push_back(std::move(fc));
        }
    }
    return true;
}

std::string Git::commitFileDiff(const fs::path& folder, const std::string& hash, const GitFileChange& file) const {
    std::vector<std::string> paths = { file.path };
    if (!file.oldPath.empty()) paths.push_back(file.oldPath);
    std::vector<std::string> args = { "diff", "--no-ext-diff", "-M", hash + "^1", hash };
    addPaths(args, paths);
    auto r = run(folder, args, 60000, true);
    if (!r.ok()) { // the first commit has no parent
        args = { "show", "--no-ext-diff", "--format=", "-M", hash };
        addPaths(args, paths);
        r = run(folder, args, 60000, true);
    }
    return capped(r.ok() ? r.output : trimmed(r.errors));
}

std::string Git::workingDiff(const fs::path& folder, const GitChange& change, bool staged) const {
    if (change.untracked() && !staged) {
        // A new file: all of it is added.
        const auto root = run(folder, { "rev-parse", "--show-toplevel" }, 10000, true);
        const fs::path file = Paths::fromUtf8(trimmed(root.output)) / Paths::fromUtf8(change.path);
        std::ifstream in(file, std::ios::binary);
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (text.find('\0') != std::string::npos) return "Binary file " + change.path + " (new)\n";
        std::string out = "new file\n--- /dev/null\n+++ b/" + change.path + "\n";
        std::istringstream ls(capped(text));
        std::string line;
        int n = 0;
        while (std::getline(ls, line)) ++n;
        out += "@@ -0,0 +1," + std::to_string(n) + " @@\n";
        ls.clear();
        ls.str(capped(text));
        while (std::getline(ls, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            out += "+" + line + "\n";
        }
        return out;
    }
    std::vector<std::string> args = { "diff", "--no-ext-diff", "-M" };
    if (staged) args.push_back("--cached");
    std::vector<std::string> paths = { change.path };
    if (!change.oldPath.empty()) paths.push_back(change.oldPath);
    addPaths(args, paths);
    const auto r = run(folder, args, 60000, true);
    return capped(r.ok() ? r.output : trimmed(r.errors));
}

// ---- making a repository

const char* Git::defaultIgnore() {
    return
        "# RIFT\n"
        "Builds/\n"
        "*.autosave.json\n"
        "*.tmp\n"
        "*.part\n"
        ".rift/\n"
        "\n"
        "# Windows\n"
        "Thumbs.db\n"
        "desktop.ini\n";
}

bool Git::init(const fs::path& folder, std::string* error) const {
    if (!available()) { if (error) *error = "Git is not installed"; return false; }
    std::string out;
    // -b needs git 2.28+; older versions get the default branch name.
    if (!done(run(folder, { "init", "-b", "main" }), &out) && !done(run(folder, { "init" }), &out)) {
        if (error) *error = out;
        return false;
    }
    std::error_code ec;
    if (!fs::exists(folder / ".gitignore", ec)) {
        std::ofstream ignore(folder / ".gitignore", std::ios::binary);
        ignore << defaultIgnore();
    }
    // The first commit needs a name and email; without them the repository
    // starts empty and the Git Changes panel asks for them.
    const GitStatus s = status(folder);
    if (s.userName.empty() || s.userEmail.empty() || !s.noCommits) return true;
    if (!commitAll(folder, "Create project", &out) && error) *error = out;
    return true;
}

bool Git::setIdentity(const fs::path& folder, const std::string& name, const std::string& email, bool global, std::string* output) const {
    std::vector<std::string> base = { "config" };
    if (global) base.push_back("--global");
    auto withValue = [&](const char* key, const std::string& value) {
        std::vector<std::string> a = base;
        a.push_back(key);
        a.push_back(value);
        return a;
    };
    return done(run(folder, withValue("user.name", name)), output) && done(run(folder, withValue("user.email", email)), output);
}

bool Git::setRemote(const fs::path& folder, const std::string& url, std::string* output) const {
    const auto existing = run(folder, { "remote", "get-url", "origin" }, 10000, true);
    if (existing.ok()) return done(run(folder, { "remote", "set-url", "origin", url }), output);
    return done(run(folder, { "remote", "add", "origin", url }), output);
}

// ---- changes

bool Git::stage(const fs::path& folder, const std::vector<std::string>& paths, std::string* output) const {
    std::vector<std::string> args = { "add", "-A" };
    if (!paths.empty()) addPaths(args, paths);
    return done(run(folder, args), output);
}

bool Git::unstage(const fs::path& folder, const std::vector<std::string>& paths, std::string* output) const {
    std::vector<std::string> args = { "reset", "-q" };
    if (!paths.empty()) addPaths(args, paths);
    if (done(run(folder, args), output)) return true;
    // Before the first commit there is nothing to reset to: take the files out of the index.
    args = { "rm", "--cached", "-r", "-q", "--ignore-unmatch" };
    if (paths.empty()) args.insert(args.end(), { "--", ":(top)" });
    else addPaths(args, paths);
    return done(run(folder, args), output);
}

bool Git::discard(const fs::path& folder, const GitChange& change, std::string* output) const {
    const auto rootOut = run(folder, { "rev-parse", "--show-toplevel" }, 10000, true);
    if (!rootOut.ok()) return done(rootOut, output);
    const fs::path root = Paths::fromUtf8(trimmed(rootOut.output));
    std::error_code ec;
    auto removeFile = [&](const std::string& path) {
        fs::remove_all(root / Paths::fromUtf8(path), ec);
        if (output) *output = ec ? ec.message() : std::string();
        return !ec;
    };
    if (change.untracked()) return removeFile(change.path);
    // A new or renamed-to file goes away; the original comes back.
    if (change.index == 'A' || change.index == 'R' || change.index == 'C') {
        if (!done(run(folder, { "rm", "--cached", "-f", "-q", "--", top(change.path) }), output)) return false;
        if (!removeFile(change.path)) return false;
        if (change.oldPath.empty() || change.index == 'C') return true;
        return done(run(folder, { "restore", "--staged", "--worktree", "--source=HEAD", "--", top(change.oldPath) }), output);
    }
    return done(run(folder, { "restore", "--staged", "--worktree", "--source=HEAD", "--", top(change.path) }), output);
}

bool Git::commitAll(const fs::path& folder, const std::string& message, std::string* output, bool amend) const {
    if (!done(run(folder, { "add", "-A" }), output)) return false;
    return commitStaged(folder, message, output, amend);
}

bool Git::commitStaged(const fs::path& folder, const std::string& message, std::string* output, bool amend) const {
    std::vector<std::string> args = { "commit" };
    if (amend) args.push_back("--amend");
    if (message.empty() && amend) args.push_back("--no-edit");
    else { args.push_back("-m"); args.push_back(message); }
    return done(run(folder, args), output);
}

bool Git::resolve(const fs::path& folder, const std::string& path, char how, std::string* output) const {
    if (how == 'o' || how == 't') {
        // A file deleted on that side: take the deletion.
        if (!done(run(folder, { "checkout", how == 'o' ? "--ours" : "--theirs", "--", top(path) }), output)) {
            return done(run(folder, { "rm", "-q", "--", top(path) }), output);
        }
    }
    return done(run(folder, { "add", "--", top(path) }), output);
}

// ---- server

bool Git::push(const fs::path& folder, const GitStatus& status, std::string* output) const {
    if (status.upstream.empty() && !status.branch.empty()) return done(run(folder, { "push", "-u", "origin", status.branch }, 300000), output);
    return done(run(folder, { "push" }, 300000), output);
}

bool Git::pull(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "pull", "--no-rebase", "--no-edit" }, 300000), output);
}

bool Git::fetch(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "fetch", "--all", "--prune", "--tags" }, 300000), output);
}

bool Git::sync(const fs::path& folder, const GitStatus& status, std::string* output) const {
    std::string pulled;
    if (!status.upstream.empty() && !pull(folder, &pulled)) {
        if (output) *output = pulled;
        return false;
    }
    std::string pushed;
    const bool ok = push(folder, status, &pushed);
    if (output) *output = trimmed(pulled + "\n" + pushed);
    return ok;
}

// ---- branches and commits

bool Git::switchBranch(const fs::path& folder, const std::string& branch, bool create, std::string* output) const {
    if (create) return done(run(folder, { "switch", "-c", branch }), output);
    return done(run(folder, { "switch", branch }), output);
}

bool Git::createBranch(const fs::path& folder, const std::string& name, const std::string& startPoint, bool checkout, std::string* output) const {
    std::vector<std::string> args = checkout ? std::vector<std::string>{ "switch", "-c", name } : std::vector<std::string>{ "branch", name };
    if (!startPoint.empty()) args.push_back(startPoint);
    return done(run(folder, args), output);
}

bool Git::checkoutRemote(const fs::path& folder, const std::string& remoteBranch, std::string* output) const {
    const size_t slash = remoteBranch.find('/');
    const std::string local = slash == std::string::npos ? remoteBranch : remoteBranch.substr(slash + 1);
    // Already have it: switch to it.
    if (run(folder, { "rev-parse", "-q", "--verify", "refs/heads/" + local }, 10000, true).ok())
        return done(run(folder, { "switch", local }), output);
    return done(run(folder, { "switch", "-c", local, "--track", remoteBranch }), output);
}

bool Git::checkoutCommit(const fs::path& folder, const std::string& hash, std::string* output) const {
    return done(run(folder, { "switch", "--detach", hash }), output);
}

bool Git::deleteBranch(const fs::path& folder, const std::string& name, bool force, std::string* output) const {
    return done(run(folder, { "branch", force ? "-D" : "-d", name }), output);
}

bool Git::deleteRemoteBranch(const fs::path& folder, const std::string& remoteBranch, std::string* output) const {
    const size_t slash = remoteBranch.find('/');
    if (slash == std::string::npos) { if (output) *output = "not a remote branch: " + remoteBranch; return false; }
    return done(run(folder, { "push", remoteBranch.substr(0, slash), "--delete", remoteBranch.substr(slash + 1) }, 300000), output);
}

bool Git::renameBranch(const fs::path& folder, const std::string& from, const std::string& to, std::string* output) const {
    return done(run(folder, { "branch", "-m", from, to }), output);
}

bool Git::merge(const fs::path& folder, const std::string& what, std::string* output) const {
    return done(run(folder, { "merge", "--no-edit", what }), output);
}

bool Git::abortMerge(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "merge", "--abort" }), output);
}

bool Git::revert(const fs::path& folder, const std::string& hash, std::string* output) const {
    if (done(run(folder, { "revert", "--no-edit", hash }), output)) return true;
    // A merge commit: undo what it brought into its first parent.
    if (output && output->find("is a merge") != std::string::npos) return done(run(folder, { "revert", "--no-edit", "-m", "1", hash }), output);
    return false;
}

bool Git::cherryPick(const fs::path& folder, const std::string& hash, std::string* output) const {
    if (done(run(folder, { "cherry-pick", hash }), output)) return true;
    if (output && output->find("is a merge") != std::string::npos) return done(run(folder, { "cherry-pick", "-m", "1", hash }), output);
    return false;
}

bool Git::abortOperation(const fs::path& folder, const GitStatus& status, std::string* output) const {
    if (status.rebasing) return done(run(folder, { "rebase", "--abort" }), output);
    if (status.cherryPicking) return done(run(folder, { "cherry-pick", "--abort" }), output);
    if (status.reverting) return done(run(folder, { "revert", "--abort" }), output);
    return abortMerge(folder, output);
}

bool Git::reset(const fs::path& folder, const std::string& hash, char mode, std::string* output) const {
    const char* flag = mode == 's' ? "--soft" : mode == 'h' ? "--hard" : "--mixed";
    return done(run(folder, { "reset", "-q", flag, hash }), output);
}

bool Git::createTag(const fs::path& folder, const std::string& name, const std::string& hash, std::string* output) const {
    std::vector<std::string> args = { "tag", name };
    if (!hash.empty()) args.push_back(hash);
    return done(run(folder, args), output);
}

bool Git::deleteTag(const fs::path& folder, const std::string& name, std::string* output) const {
    return done(run(folder, { "tag", "-d", name }), output);
}

// ---- stashes

bool Git::stashPush(const fs::path& folder, const std::string& message, std::string* output) const {
    std::vector<std::string> args = { "stash", "push", "--include-untracked" };
    if (!message.empty()) { args.push_back("-m"); args.push_back(message); }
    return done(run(folder, args), output);
}

bool Git::stashApply(const fs::path& folder, const std::string& ref, bool pop, std::string* output) const {
    return done(run(folder, { "stash", pop ? "pop" : "apply", ref }), output);
}

bool Git::stashDrop(const fs::path& folder, const std::string& ref, std::string* output) const {
    return done(run(folder, { "stash", "drop", ref }), output);
}

// ---- names

std::string Git::webUrl(const std::string& remote) {
    std::string path;
    if (remote.rfind("https://github.com/", 0) == 0) path = remote.substr(19);
    else if (remote.rfind("git@github.com:", 0) == 0) path = remote.substr(15);
    else if (remote.rfind("ssh://git@github.com/", 0) == 0) path = remote.substr(21);
    else return {};
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".git") == 0) path.resize(path.size() - 4);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path.empty() ? std::string() : "https://github.com/" + path;
}

bool Git::validRefName(const std::string& name) {
    // The rules of git check-ref-format, for one branch or tag name.
    if (name.empty() || name == "@" || name == "HEAD") return false;
    if (name.front() == '-' || name.front() == '/' || name.front() == '.' || name.back() == '/' || name.back() == '.') return false;
    if (name.size() >= 5 && name.compare(name.size() - 5, 5, ".lock") == 0) return false;
    if (name.find("..") != std::string::npos || name.find("@{") != std::string::npos || name.find("//") != std::string::npos ||
        name.find("/.") != std::string::npos)
        return false;
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || c == ' ' || c == '~' || c == '^' || c == ':' || c == '?' || c == '*' || c == '[' || c == '\\') return false;
    }
    return true;
}

} // namespace rift
