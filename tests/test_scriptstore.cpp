// ScriptStore: path validation, dirty tracking, Save / Save As, hot reload.
#include "ScriptStore.h"
#include <cstdio>
#include <fstream>
#include <thread>
#include <chrono>

#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); return 1; } else std::printf("[PASS] %s\n", msg); } while (0)
namespace fs = std::filesystem;

static void writeText(const fs::path& p, const std::string& t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}
static std::string readText(const fs::path& p) { std::ifstream in(p, std::ios::binary); return { std::istreambuf_iterator<char>(in), {} }; }

int main() {
    const fs::path base = fs::temp_directory_path() / "rift_scriptstore_test";
    fs::remove_all(base);
    fs::create_directories(base / "A");

    CHECK(ScriptStore::isValidPath("scripts/Coin.lua"), "valid relative path");
    CHECK(ScriptStore::isValidPath("scripts\\Coin.lua"), "backslashes are normalized");
    CHECK(!ScriptStore::isValidPath("../evil.lua"), "rejects ..");
    CHECK(!ScriptStore::isValidPath("scripts/../../evil.lua"), "rejects nested ..");
    CHECK(!ScriptStore::isValidPath("/etc/passwd.lua"), "rejects absolute path");
    CHECK(!ScriptStore::isValidPath("C:/x.lua"), "rejects drive path");
    CHECK(!ScriptStore::isValidPath("scripts/readme.txt"), "rejects non-.lua");
    CHECK(!ScriptStore::isValidPath(""), "rejects empty");

    writeText(base / "A" / "scripts" / "Coin.lua", "print('hi')\n");
    ScriptStore store;
    store.reset((base / "A").string());
    CHECK(store.load("scripts/Coin.lua"), "loads an existing file");
    CHECK(!store.load("scripts/Missing.lua"), "missing file fails");
    CHECK(!store.load("../x.lua"), "unsafe path fails");
    CHECK(!store.anyDirty(), "freshly loaded is clean");

    *store.editableText("scripts/Coin.lua") += "-- edit\n";
    store.touch("scripts/Coin.lua");
    CHECK(store.anyDirty(), "edit marks dirty");
    *store.editableText("scripts/Coin.lua") = "print('hi')\n";
    store.touch("scripts/Coin.lua");
    CHECK(!store.anyDirty(), "reverting the edit clears dirty");

    const std::string fresh = store.uniquePath("Spinner");
    CHECK(fresh == "scripts/Spinner.lua", "uniquePath picks a free name");
    CHECK(store.create(fresh, "-- new\n"), "create in memory");
    CHECK(store.uniquePath("Spinner") == "scripts/Spinner2.lua", "uniquePath skips names in the store");
    CHECK(store.anyDirty() && !fs::exists(base / "A" / fresh), "new script is dirty and not on disk yet");
    CHECK(store.saveAll(), "saveAll");
    CHECK(!store.anyDirty() && readText(base / "A" / fresh) == "-- new\n", "saved to disk and clean");

    // Save As: everything is copied to the new folder and the store re-roots.
    CHECK(store.saveAll((base / "B").string()), "Save As to another folder");
    CHECK(readText(base / "B" / "scripts" / "Coin.lua") == "print('hi')\n" && fs::exists(base / "B" / fresh), "all scripts written to the new folder");
    CHECK(store.root() == (base / "B").string(), "store re-rooted");

    // Hot reload.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    writeText(base / "B" / "scripts" / "Coin.lua", "print('changed outside')\n");
    fs::last_write_time(base / "B" / "scripts" / "Coin.lua", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    std::vector<std::string> conflicts;
    auto reloaded = store.pollExternalChanges(&conflicts);
    CHECK(reloaded.size() == 1 && conflicts.empty(), "external change reloads a clean script");
    CHECK(store.find("scripts/Coin.lua")->text == "print('changed outside')\n", "reloaded text is the new content");

    // Conflict: unsaved edit + external change must not be overwritten.
    *store.editableText("scripts/Coin.lua") = "-- my unsaved work\n";
    store.touch("scripts/Coin.lua");
    writeText(base / "B" / "scripts" / "Coin.lua", "print('changed again')\n");
    fs::last_write_time(base / "B" / "scripts" / "Coin.lua", fs::file_time_type::clock::now() + std::chrono::seconds(10));
    conflicts.clear();
    reloaded = store.pollExternalChanges(&conflicts);
    CHECK(reloaded.empty() && conflicts.size() == 1, "dirty + external change is reported as a conflict");
    CHECK(store.find("scripts/Coin.lua")->text == "-- my unsaved work\n", "unsaved edit is kept");

    fs::remove_all(base);
    std::printf("\nALL SCRIPTSTORE TESTS PASSED\n");
    return 0;
}
