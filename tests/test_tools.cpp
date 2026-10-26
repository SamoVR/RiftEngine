// Version control (git), running programs, the updater's version logic and
// project folders.
#include "doctest.h"
#include "editor/Updater.h"
#include "platform/Process.h"
#include "project/Git.h"
#include "project/Project.h"
#include "core/Paths.h"

#include <filesystem>
#include <fstream>

using namespace rift;
namespace fs = std::filesystem;

namespace {
    struct TempFolder {
        fs::path path;
        explicit TempFolder(const char* name) {
            path = fs::temp_directory_path() / (std::string(name) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
            fs::remove_all(path);
            fs::create_directories(path);
        }
        ~TempFolder() {
            std::error_code ec;
            // git makes its object files read-only.
            for (auto it = fs::recursive_directory_iterator(path, ec); it != fs::recursive_directory_iterator(); it.increment(ec))
                fs::permissions(it->path(), fs::perms::owner_write, fs::perm_options::add, ec);
            fs::remove_all(path, ec);
        }
    };

    void write(const fs::path& file, const std::string& text) {
        fs::create_directories(file.parent_path());
        std::ofstream(file, std::ios::binary) << text;
    }
}

TEST_CASE("running a program collects its output and exit code") {
    const auto ok = Platform::runProcess("cmd.exe", { "/c", "echo hello world" });
    CHECK(ok.ok());
    CHECK(ok.output.find("hello world") != std::string::npos);
    const auto failed = Platform::runProcess("cmd.exe", { "/c", "exit 3" });
    CHECK(failed.started);
    CHECK(failed.exitCode == 3);
    const auto missing = Platform::runProcess("no-such-program-rift", {});
    CHECK_FALSE(missing.started);
    // Arguments with spaces and quotes arrive unchanged.
    const auto quoted = Platform::runProcess("cmd.exe", { "/c", "echo", "a b" });
    CHECK(quoted.output.find("a b") != std::string::npos);
}

TEST_CASE("versions compare numerically") {
    CHECK(Updater::compareVersions("1.1.0", "1.0.0") > 0);
    CHECK(Updater::compareVersions("v1.10.0", "1.9.9") > 0);
    CHECK(Updater::compareVersions("1.0", "1.0.0") == 0);
    CHECK(Updater::compareVersions("1.0.0", "1.0.1") < 0);
    CHECK(Updater::compareVersions("2.0.0-beta", "1.9.0") > 0);
}

TEST_CASE("git status output is parsed") {
    GitStatus s;
    const std::string text = std::string("## main...origin/main [ahead 2, behind 1]") + '\0'
        + " M scenes/Main/scene.json" + '\0'
        + "?? assets/new.png" + '\0'
        + "R  scripts/New.lua" + '\0' + "scripts/Old.lua" + '\0'
        + "UU project.json" + '\0';
    Git::parseStatus(text, s);
    CHECK(s.branch == "main");
    CHECK(s.upstream == "origin/main");
    CHECK(s.ahead == 2);
    CHECK(s.behind == 1);
    REQUIRE(s.changes.size() == 4);
    CHECK(s.changes[0].kind() == 'M');
    CHECK(s.changes[0].path == "scenes/Main/scene.json");
    CHECK(s.changes[1].kind() == 'A');
    CHECK(s.changes[1].untracked());
    CHECK(s.changes[2].kind() == 'R');
    CHECK(s.changes[2].oldPath == "scripts/Old.lua");
    CHECK(s.changes[3].kind() == 'C');

    GitStatus fresh;
    Git::parseStatus(std::string("## No commits yet on main") + '\0', fresh);
    CHECK(fresh.noCommits);
    CHECK(fresh.branch == "main");

    CHECK(Git::webUrl("https://github.com/SamoVR/RiftEngine.git") == "https://github.com/SamoVR/RiftEngine");
    CHECK(Git::webUrl("git@github.com:me/game.git") == "https://github.com/me/game");
    CHECK(Git::webUrl("https://example.com/x.git").empty());
}

TEST_CASE("a project under git: init, commit, change, discard, branch") {
    const Git git;
    if (!git.available()) {
        MESSAGE("git is not installed; skipped");
        return;
    }
    TempFolder temp("rift-git-test");
    fs::path folder;
    std::string error;
    REQUIRE(Project::create(temp.path, "Versioned", Project::Template::Baseplate, &folder, &error));
    // A name and email only for this repository (the test must not need the user's).
    REQUIRE(git.run(folder, { "init", "-b", "main" }).ok());
    REQUIRE(git.setIdentity(folder, "RIFT Test", "test@example.com", false, &error));
    REQUIRE(git.init(folder, &error)); // already a repository: adds .gitignore and the first commit
    CHECK(fs::exists(folder / ".gitignore"));

    GitStatus s = git.status(folder);
    REQUIRE(s.repository);
    CHECK(s.branch == "main");
    CHECK(s.changes.empty());
    CHECK(fs::equivalent(s.root, folder));
    CHECK(s.projectPrefix.empty());
    auto history = git.graph(folder);
    REQUIRE(history.size() == 1);
    CHECK(history[0].subject == "Create project");

    // Builds and autosaves are ignored; a real change shows up.
    write(folder / "Builds" / "Game.exe", "binary");
    write(folder / "scenes" / "Main" / "scene.autosave.json", "{}");
    write(folder / "scripts" / "Util.lua", "return {}\n");
    s = git.status(folder);
    REQUIRE(s.changes.size() == 1);
    CHECK(s.changes[0].path == "scripts/Util.lua");
    CHECK(s.changes[0].untracked());

    std::string out;
    REQUIRE(git.commitAll(folder, "Add a module", &out));
    s = git.status(folder);
    CHECK(s.changes.empty());
    CHECK(git.graph(folder).size() == 2);

    // Discarding a change restores the committed file.
    write(folder / "scripts" / "Util.lua", "return { broken = true }\n");
    s = git.status(folder);
    REQUIRE(s.changes.size() == 1);
    REQUIRE(git.discard(folder, s.changes[0], &out));
    std::ifstream in(folder / "scripts" / "Util.lua");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text == "return {}\n");

    // Branches
    REQUIRE(git.switchBranch(folder, "experiment", true, &out));
    s = git.status(folder);
    CHECK(s.branch == "experiment");
    CHECK(s.localBranches().size() == 2);
    REQUIRE(git.switchBranch(folder, "main", false, &out));
    CHECK(git.status(folder).branch == "main");
}

TEST_CASE("a project's scenes can move to another folder") {
    TempFolder temp("rift-scenes-test");
    fs::path folder;
    std::string error;
    REQUIRE(Project::create(temp.path, "Mover", Project::Template::Obby, &folder, &error));
    Project p;
    REQUIRE(p.load(folder, &error));
    CHECK(p.scenesFolder() == folder / "scenes");

    CHECK(Project::isValidScenesFolder("Levels"));
    CHECK(Project::isValidScenesFolder("Content/Scenes"));
    CHECK_FALSE(Project::isValidScenesFolder("../outside"));
    CHECK_FALSE(Project::isValidScenesFolder("C:/Windows"));
    CHECK_FALSE(Project::isValidScenesFolder("assets"));
    CHECK_FALSE(Project::isValidScenesFolder(""));

    REQUIRE(p.changeScenesFolder("Content/Levels", true, &error));
    CHECK(p.settings().scenesFolder == "Content/Levels");
    CHECK(fs::exists(p.startScenePath()));
    CHECK(p.settings().startScene.rfind("Content/Levels/", 0) == 0);
    CHECK_FALSE(fs::exists(folder / "scenes"));
    CHECK(p.scenes().size() == 1);

    // Saved: loading again finds them.
    Project again;
    REQUIRE(again.load(folder, &error));
    CHECK(again.scenesFolder() == folder / "Content" / "Levels");
    CHECK(fs::exists(again.startScenePath()));
}
