// ScriptStore: path validation, dirty tracking, Save / Save As, hot reload.
#include "doctest.h"
#include "script/ScriptStore.h"
#include "core/Paths.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

namespace {
    void writeText(const fs::path& p, const std::string& t) {
        fs::create_directories(p.parent_path());
        std::ofstream(p, std::ios::binary) << t;
    }
    std::string readText(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        return { std::istreambuf_iterator<char>(in), {} };
    }
}

TEST_CASE("script paths must stay inside the scene folder") {
    CHECK(ScriptStore::isValidPath("scripts/Coin.lua"));
    CHECK(ScriptStore::isValidPath("scripts\\Coin.lua"));
    CHECK_FALSE(ScriptStore::isValidPath("../evil.lua"));
    CHECK_FALSE(ScriptStore::isValidPath("scripts/../../evil.lua"));
    CHECK_FALSE(ScriptStore::isValidPath("/etc/passwd.lua"));
    CHECK_FALSE(ScriptStore::isValidPath("C:/x.lua"));
    CHECK_FALSE(ScriptStore::isValidPath("scripts/readme.txt"));
    CHECK_FALSE(ScriptStore::isValidPath(""));
}

TEST_CASE("ScriptStore load, edit, save, save as, hot reload") {
    const fs::path base = fs::temp_directory_path() / "rift_scriptstore_test";
    fs::remove_all(base);
    writeText(base / "A" / "scripts" / "Coin.lua", "print('hi')\n");

    ScriptStore store;
    store.reset(Paths::toUtf8(base / "A"));
    CHECK(store.load("scripts/Coin.lua"));
    CHECK_FALSE(store.load("scripts/Missing.lua"));
    CHECK_FALSE(store.load("../x.lua"));
    CHECK_FALSE(store.anyDirty());

    *store.editableText("scripts/Coin.lua") += "-- edit\n";
    store.touch("scripts/Coin.lua");
    CHECK(store.anyDirty());
    *store.editableText("scripts/Coin.lua") = "print('hi')\n";
    store.touch("scripts/Coin.lua");
    CHECK_FALSE(store.anyDirty());

    const std::string fresh = store.uniquePath("Spinner");
    CHECK(fresh == "scripts/Spinner.lua");
    CHECK(store.create(fresh, "-- new\n"));
    CHECK(store.uniquePath("Spinner") == "scripts/Spinner2.lua");
    CHECK(store.anyDirty());
    CHECK_FALSE(fs::exists(base / "A" / fresh));
    CHECK(store.saveAll());
    CHECK_FALSE(store.anyDirty());
    CHECK(readText(base / "A" / fresh) == "-- new\n");

    CHECK(store.saveAll(Paths::toUtf8(base / "B")));
    CHECK(readText(base / "B" / "scripts" / "Coin.lua") == "print('hi')\n");
    CHECK(fs::exists(base / "B" / fresh));

    // Hot reload: an external edit is picked up; a conflicting one is reported.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writeText(base / "B" / "scripts" / "Coin.lua", "print('external')\n");
    auto bump = fs::last_write_time(base / "B" / "scripts" / "Coin.lua") + std::chrono::seconds(2);
    fs::last_write_time(base / "B" / "scripts" / "Coin.lua", bump);
    const auto reloaded = store.pollExternalChanges();
    CHECK(reloaded.size() == 1);
    CHECK(store.find("scripts/Coin.lua")->text == "print('external')\n");

    *store.editableText(fresh) = "-- local edit\n";
    store.touch(fresh);
    writeText(base / "B" / fresh, "-- disk edit\n");
    fs::last_write_time(base / "B" / fresh, fs::last_write_time(base / "B" / fresh) + std::chrono::seconds(2));
    std::vector<std::string> conflicts;
    store.pollExternalChanges(&conflicts);
    CHECK(conflicts.size() == 1);
    CHECK(store.find(fresh)->text == "-- local edit\n"); // never silently overwritten

    fs::remove_all(base);
}
