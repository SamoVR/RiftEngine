// Source control: the commit graph's lanes, and a whole day of git work against
// a local "server" (a bare repository): staging, diffs, branches, a merge
// conflict, push / fetch / pull, stashes and tags.
#include "doctest.h"
#include "project/Git.h"

#include <filesystem>
#include <fstream>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    struct TempRepoFolder {
        fs::path path;
        explicit TempRepoFolder(const char* name) {
            path = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            std::error_code ec;
            fs::remove_all(path, ec);
            fs::create_directories(path);
        }
        ~TempRepoFolder() {
            std::error_code ec;
            // git makes its object files read-only.
            for (auto it = fs::recursive_directory_iterator(path, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
                fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
            fs::remove_all(path, ec);
        }
    };
    void put(const fs::path& file, const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
    std::string get(const fs::path& file) {
        std::ifstream in(file, std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    GitGraphCommit commit(const char* hash, std::vector<std::string> parents) {
        GitGraphCommit c;
        c.hash = hash;
        c.parents = std::move(parents);
        return c;
    }
    // A repository with its own name and email, and no signing whatever the user's settings say.
    void identity(const Git& git, const fs::path& folder) {
        std::string out;
        REQUIRE(git.setIdentity(folder, "RIFT Test", "test@example.com", false, &out));
        REQUIRE(git.run(folder, { "config", "commit.gpgsign", "false" }).ok());
        REQUIRE(git.run(folder, { "config", "core.autocrlf", "false" }).ok());
    }
    const GitRef* findRef(const GitStatus& s, GitRef::Kind kind, const std::string& name) {
        for (const GitRef& r : s.refs)
            if (r.kind == kind && r.name == name) return &r;
        return nullptr;
    }
}

TEST_CASE("the commit graph puts branches in lanes and joins them at merges") {
    // Newest first:  M merges A and B, which both grew from base.
    std::vector<GitGraphCommit> g = { commit("M", { "A", "B" }), commit("B", { "base" }), commit("A", { "base" }), commit("base", {}) };
    Git::layoutGraph(g);
    CHECK(g[0].lane == 0);
    CHECK(g[1].lane == 1);
    CHECK(g[2].lane == 0);
    CHECK(g[3].lane == 0);
    CHECK(g[1].color != g[2].color);
    CHECK(g[0].width == 2);
    // M goes down its own lane and out to B's.
    bool toB = false;
    for (const auto& l : g[0].lines) toB |= l.fromCommit && l.toLane == 1;
    CHECK(toB);
    // B's row: lane 0 passes by.
    bool passes = false;
    for (const auto& l : g[1].lines) passes |= !l.fromCommit && !l.toCommit && l.fromLane == 0 && l.toLane == 0;
    CHECK(passes);
    // base: both lanes come in, nothing goes out.
    int incoming = 0, outgoing = 0;
    for (const auto& l : g[3].lines) {
        incoming += l.toCommit ? 1 : 0;
        outgoing += l.fromCommit ? 1 : 0;
    }
    CHECK(incoming == 2);
    CHECK(outgoing == 0);

    // Two unrelated tips side by side.
    std::vector<GitGraphCommit> two = { commit("x2", { "x1" }), commit("y1", {}), commit("x1", {}) };
    Git::layoutGraph(two);
    CHECK(two[0].lane == 0);
    CHECK(two[1].lane == 1);
    CHECK(two[2].lane == 0);
}

TEST_CASE("branch and tag names are checked like git does") {
    CHECK(Git::validRefName("main"));
    CHECK(Git::validRefName("feature/new-map"));
    CHECK(Git::validRefName("v1.0"));
    CHECK_FALSE(Git::validRefName(""));
    CHECK_FALSE(Git::validRefName("has space"));
    CHECK_FALSE(Git::validRefName("-x"));
    CHECK_FALSE(Git::validRefName("a..b"));
    CHECK_FALSE(Git::validRefName("end/"));
    CHECK_FALSE(Git::validRefName("x.lock"));
    CHECK_FALSE(Git::validRefName("what?"));
    CHECK_FALSE(Git::validRefName("a@{1}"));
}

TEST_CASE("source control: stage, commit, branch, merge conflicts, push, pull, stash") {
    const Git git;
    if (!git.available()) {
        MESSAGE("git is not installed; skipped");
        return;
    }
    TempRepoFolder temp("rift-git-flow");
    std::string out;
    const fs::path server = temp.path / "server.git";
    const fs::path work = temp.path / "work";
    fs::create_directories(work);
    REQUIRE(git.run(temp.path, { "init", "--bare", "-b", "main", "server.git" }).ok());
    REQUIRE(git.run(work, { "init", "-b", "main" }).ok());
    identity(git, work);

    // A fresh repository: no commits, an untracked file.
    put(work / "a.txt", "one\ntwo\nthree\n");
    GitStatus s = git.status(work);
    REQUIRE(s.repository);
    CHECK(s.noCommits);
    CHECK(s.branch == "main");
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0].untracked());
    CHECK(git.workingDiff(work, s.changes[0], false).find("+two") != std::string::npos);

    // Stage, unstage (before any commit), stage again, commit what is staged.
    REQUIRE(git.stage(work, { "a.txt" }, &out));
    s = git.status(work);
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0].staged());
    CHECK_FALSE(s.changes[0].unstaged());
    REQUIRE(git.unstage(work, { "a.txt" }, &out));
    CHECK(git.status(work).changes[0].untracked());
    REQUIRE(git.stage(work, {}, &out));
    REQUIRE(git.commitStaged(work, "First", &out));
    s = git.status(work);
    CHECK_FALSE(s.noCommits);
    CHECK(s.changes.empty());
    CHECK(s.headHash.size() == 40);

    // To the server: the first push sets the upstream.
    REQUIRE(git.setRemote(work, server.string(), &out));
    s = git.status(work);
    CHECK(s.remoteUrl == server.string());
    REQUIRE(git.push(work, s, &out));
    s = git.status(work);
    CHECK(s.upstream == "origin/main");
    CHECK(s.ahead == 0);
    REQUIRE(findRef(s, GitRef::Remote, "origin/main"));
    CHECK(findRef(s, GitRef::Local, "main")->head);

    // A change: unstaged diff, then staged diff, commit; ahead by one.
    put(work / "a.txt", "one\nTWO\nthree\n");
    s = git.status(work);
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0].unstagedKind() == 'M');
    CHECK(git.workingDiff(work, s.changes[0], false).find("+TWO") != std::string::npos);
    CHECK(git.workingDiff(work, s.changes[0], true).empty());
    REQUIRE(git.stage(work, { "a.txt" }, &out));
    s = git.status(work);
    CHECK(git.workingDiff(work, s.changes[0], true).find("+TWO") != std::string::npos);
    REQUIRE(git.commitStaged(work, "Shout two", &out));
    s = git.status(work);
    CHECK(s.ahead == 1);

    // History and what each commit changed (the first commit has no parent).
    auto history = git.graph(work);
    REQUIRE(history.size() == 2);
    CHECK(history[0].subject == "Shout two");
    CHECK(history[1].parents.empty());
    bool headLabel = false;
    for (const std::string& r : history[0].refs) headLabel |= r.find("main") != std::string::npos;
    CHECK(headLabel);
    GitCommitDetails details;
    REQUIRE(git.commitDetails(work, history[0].hash, details));
    CHECK(details.message == "Shout two");
    REQUIRE(details.files.size() == 1);
    CHECK(details.files[0].kind == 'M');
    CHECK(git.commitFileDiff(work, history[0].hash, details.files[0]).find("+TWO") != std::string::npos);
    REQUIRE(git.commitDetails(work, history[1].hash, details));
    REQUIRE(details.files.size() == 1);
    CHECK(details.files[0].kind == 'A');
    CHECK(git.commitFileDiff(work, history[1].hash, details.files[0]).find("+one") != std::string::npos);

    // Amend: same number of commits, new message.
    REQUIRE(git.commitAll(work, "Shout TWO", &out, true));
    history = git.graph(work);
    REQUIRE(history.size() == 2);
    CHECK(history[0].subject == "Shout TWO");

    // Two branches change the same line: the merge stops with a conflict.
    REQUIRE(git.createBranch(work, "feature", "", true, &out));
    put(work / "a.txt", "one\nfeature two\nthree\n");
    REQUIRE(git.commitAll(work, "Feature two", &out));
    REQUIRE(git.switchBranch(work, "main", false, &out));
    put(work / "a.txt", "one\nmain two\nthree\n");
    REQUIRE(git.commitAll(work, "Main two", &out));
    CHECK_FALSE(git.merge(work, "feature", &out));
    s = git.status(work);
    CHECK(s.merging);
    CHECK(s.conflictCount() == 1);
    REQUIRE(git.resolve(work, "a.txt", 't', &out));
    CHECK(get(work / "a.txt") == "one\nfeature two\nthree\n");
    s = git.status(work);
    CHECK(s.conflictCount() == 0);
    REQUIRE(git.commitStaged(work, "Merge feature", &out));
    s = git.status(work);
    CHECK_FALSE(s.merging);
    history = git.graph(work);
    CHECK(history[0].parents.size() == 2);
    CHECK(history[0].width >= 2);
    REQUIRE(git.push(work, s, &out));

    // Someone else pushes; fetch shows it as incoming, pull brings it.
    REQUIRE(git.run(temp.path, { "clone", server.string(), "other" }).ok());
    const fs::path other = temp.path / "other";
    identity(git, other);
    put(other / "b.txt", "from the other clone\n");
    REQUIRE(git.commitAll(other, "Other work", &out));
    GitStatus os = git.status(other);
    REQUIRE(git.push(other, os, &out));
    REQUIRE(git.fetch(work, &out));
    s = git.status(work);
    CHECK(s.behind == 1);
    REQUIRE(git.pull(work, &out));
    s = git.status(work);
    CHECK(s.behind == 0);
    CHECK(fs::exists(work / "b.txt"));

    // Stash a change away and bring it back.
    put(work / "b.txt", "changed\n");
    put(work / "new.txt", "untracked too\n");
    REQUIRE(git.stashPush(work, "wip", &out));
    s = git.status(work);
    CHECK(s.changes.empty());
    REQUIRE(s.stashes.size() == 1);
    CHECK(s.stashes[0].ref == "stash@{0}");
    CHECK(s.stashes[0].message.find("wip") != std::string::npos);
    REQUIRE(git.stashApply(work, "stash@{0}", true, &out));
    s = git.status(work);
    CHECK(s.changes.size() == 2);
    CHECK(s.stashes.empty());

    // Discard: a modified file comes back, a staged new file goes away.
    REQUIRE(git.stage(work, { "new.txt" }, &out));
    s = git.status(work);
    for (const GitChange& c : s.changes) REQUIRE(git.discard(work, c, &out));
    CHECK(get(work / "b.txt") == "from the other clone\n");
    CHECK_FALSE(fs::exists(work / "new.txt"));
    CHECK(git.status(work).changes.empty());

    // Tags point at their commit.
    REQUIRE(git.createTag(work, "v1", "", &out));
    s = git.status(work);
    const GitRef* tag = findRef(s, GitRef::Tag, "v1");
    REQUIRE(tag);
    CHECK(tag->hash == s.headHash);

    // A project in a sub-folder of the repository: paths stay relative to the root.
    put(work / "Game" / "scripts" / "Main.lua", "print('hi')\n");
    s = git.status(work / "Game");
    CHECK(s.projectPrefix == "Game");
    CHECK(fs::equivalent(s.root, work));
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0].path == "Game/scripts/Main.lua");
    REQUIRE(git.stage(work / "Game", { s.changes[0].path }, &out));
    CHECK(git.status(work / "Game").changes[0].staged());
    REQUIRE(git.discard(work / "Game", git.status(work).changes[0], &out));
    CHECK_FALSE(fs::exists(work / "Game" / "scripts" / "Main.lua"));

    // Branch housekeeping and going back in time.
    REQUIRE(git.renameBranch(work, "feature", "feature-done", &out));
    REQUIRE(git.deleteBranch(work, "feature-done", false, &out));
    CHECK(git.status(work).localBranches().size() == 1);
    const std::string before = git.status(work).headHash;
    REQUIRE(git.checkoutCommit(work, history.back().hash, &out));
    s = git.status(work);
    CHECK(s.branch.empty());
    CHECK(s.headHash == history.back().hash);
    REQUIRE(git.switchBranch(work, "main", false, &out));
    CHECK(git.status(work).headHash == before);
}
