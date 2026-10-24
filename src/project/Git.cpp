#include "project/Git.h"
#include "core/Paths.h"

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

Platform::ProcessResult Git::run(const fs::path& folder, const std::vector<std::string>& args, unsigned timeoutMs) const {
    if (exe_.empty()) {
        Platform::ProcessResult r;
        r.output = "Git is not installed";
        return r;
    }
    // Never wait for a question on a console nobody sees; plain English output.
    return Platform::runProcess(exe_, args, Paths::toUtf8(folder), timeoutMs,
                                { "GIT_TERMINAL_PROMPT=0", "GIT_EDITOR=true", "LC_ALL=C", "LANG=C", "GIT_OPTIONAL_LOCKS=0" });
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
        while (start < s.size() && (s[start] == ' ' || s[start] == '\n')) ++start;
        return s.substr(start);
    }
    bool done(const Platform::ProcessResult& r, std::string* output) {
        if (output) *output = trimmed(r.output);
        if (output && output->empty() && !r.ok()) *output = r.timedOut ? "git took too long and was stopped" : "git failed (exit code " + std::to_string(r.exitCode) + ")";
        return r.ok();
    }
}

void Git::parseStatus(const std::string& text, GitStatus& out) {
    // Entries are NUL-separated; a rename is followed by its original path.
    std::vector<std::string> entries;
    std::string current;
    for (char c : text) {
        if (c == '\0') { entries.push_back(current); current.clear(); }
        else current += c;
    }
    if (!current.empty()) entries.push_back(current);

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
            if (const size_t a = counts.find("ahead "); a != std::string::npos) out.ahead = std::atoi(counts.c_str() + a + 6);
            if (const size_t b = counts.find("behind "); b != std::string::npos) out.behind = std::atoi(counts.c_str() + b + 7);
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

GitStatus Git::status(const fs::path& folder, int historyCount) const {
    GitStatus s;
    s.available = available();
    if (!s.available) { s.error = "Git is not installed"; return s; }
    const auto inside = run(folder, { "rev-parse", "--is-inside-work-tree" }, 15000);
    if (!inside.ok() || trimmed(inside.output) != "true") return s; // not a repository (not an error)
    s.repository = true;

    const auto st = run(folder, { "status", "--porcelain=v1", "-b", "-z", "--untracked-files=all" }, 30000);
    if (!st.ok()) { s.error = trimmed(st.output); return s; }
    parseStatus(st.output, s);

    std::error_code ec;
    const auto gitDir = run(folder, { "rev-parse", "--git-dir" }, 10000);
    if (gitDir.ok()) {
        fs::path dir = Paths::fromUtf8(trimmed(gitDir.output));
        if (dir.is_relative()) dir = folder / dir;
        s.merging = fs::exists(dir / "MERGE_HEAD", ec);
    }

    if (!s.noCommits) {
        const auto log = run(folder, { "log", "-n", std::to_string(historyCount), "--pretty=format:%H%x1f%h%x1f%an%x1f%ar%x1f%s%x1e" }, 30000);
        if (log.ok()) {
            std::stringstream records(log.output);
            std::string record;
            while (std::getline(records, record, '\x1e')) {
                if (!record.empty() && record[0] == '\n') record.erase(0, 1);
                std::vector<std::string> f;
                std::stringstream fields(record);
                std::string field;
                while (std::getline(fields, field, '\x1f')) f.push_back(field);
                if (f.size() < 5) continue;
                s.history.push_back({ f[0], f[1], f[2], f[3], f[4] });
            }
        }
    }
    const auto branches = run(folder, { "branch", "--format=%(refname:short)" }, 15000);
    if (branches.ok()) {
        std::stringstream lines(branches.output);
        std::string line;
        while (std::getline(lines, line)) {
            line = trimmed(line);
            if (!line.empty()) s.branches.push_back(line);
        }
    }
    if (const auto url = run(folder, { "remote", "get-url", "origin" }, 10000); url.ok()) s.remoteUrl = trimmed(url.output);
    if (const auto name = run(folder, { "config", "user.name" }, 10000); name.ok()) s.userName = trimmed(name.output);
    if (const auto email = run(folder, { "config", "user.email" }, 10000); email.ok()) s.userEmail = trimmed(email.output);
    return s;
}

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
    // starts empty and the Source Control panel asks for them.
    const GitStatus s = status(folder, 1);
    if (s.userName.empty() || s.userEmail.empty()) return true;
    if (!commitAll(folder, "Create project", &out) && error) *error = out;
    return true;
}

bool Git::commitAll(const fs::path& folder, const std::string& message, std::string* output) const {
    if (!done(run(folder, { "add", "-A" }), output)) return false;
    return done(run(folder, { "commit", "-m", message }), output);
}

bool Git::push(const fs::path& folder, const GitStatus& status, std::string* output) const {
    if (status.upstream.empty() && !status.branch.empty()) return done(run(folder, { "push", "-u", "origin", status.branch }, 300000), output);
    return done(run(folder, { "push" }, 300000), output);
}

bool Git::pull(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "pull", "--no-rebase", "--no-edit" }, 300000), output);
}

bool Git::fetch(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "fetch", "--prune" }, 300000), output);
}

bool Git::setRemote(const fs::path& folder, const std::string& url, std::string* output) const {
    const auto existing = run(folder, { "remote", "get-url", "origin" }, 10000);
    if (existing.ok()) return done(run(folder, { "remote", "set-url", "origin", url }), output);
    return done(run(folder, { "remote", "add", "origin", url }), output);
}

bool Git::switchBranch(const fs::path& folder, const std::string& branch, bool create, std::string* output) const {
    if (create) return done(run(folder, { "switch", "-c", branch }), output);
    return done(run(folder, { "switch", branch }), output);
}

bool Git::discard(const fs::path& folder, const GitChange& change, std::string* output) const {
    if (change.untracked()) {
        std::error_code ec;
        fs::remove_all(folder / Paths::fromUtf8(change.path), ec);
        if (output) *output = ec ? ec.message() : std::string();
        return !ec;
    }
    if (change.index == 'A') return done(run(folder, { "rm", "--cached", "--force", "--quiet", "--", change.path }), output);
    return done(run(folder, { "restore", "--staged", "--worktree", "--source=HEAD", "--", change.path }), output);
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

bool Git::abortMerge(const fs::path& folder, std::string* output) const {
    return done(run(folder, { "merge", "--abort" }), output);
}

std::string Git::webUrl(const std::string& remote) {
    std::string path;
    if (remote.rfind("https://github.com/", 0) == 0) path = remote.substr(19);
    else if (remote.rfind("git@github.com:", 0) == 0) path = remote.substr(15);
    else return {};
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".git") == 0) path.resize(path.size() - 4);
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path.empty() ? std::string() : "https://github.com/" + path;
}

} // namespace rift
