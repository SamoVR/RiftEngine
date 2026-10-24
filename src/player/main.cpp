// RIFT Player entry point: runs a game made with RIFT.
//
//     MyGame.exe                     runs game/project.json next to the exe
//     RiftPlayer.exe <project dir>   runs a project folder (testing)
#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#include <ole2.h>
#endif

#include "player/PlayerApp.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"
#include "project/Exporter.h"

#include <nlohmann/json.hpp>
#include <exception>
#include <fstream>
#include <string>

namespace {

    // The game's name, read early: saves and logs go to %APPDATA%\RIFT Games\<name>.
    std::string gameName(const std::filesystem::path& folder) {
        std::ifstream in(folder / "project.json");
        nlohmann::json j;
        try {
            in >> j;
            if (j.contains("name") && j["name"].is_string()) return j["name"].get<std::string>();
        } catch (...) {}
        return "Game";
    }

    int runGame(const std::string& argument) {
#ifdef _WIN32
        OleInitialize(nullptr); // single-threaded COM before the audio library (see the editor's main)
#endif
        const std::filesystem::path folder = rift::findGameFolder(Paths::fromUtf8(argument));
        const std::string name = gameName(folder);
        Paths::setUserDataFolderName("RIFT Games/" + rift::executableName(name));
        Log::init("game");
        LOG_INFO("Main", "RIFT Player " RIFT_VERSION_STRING " running " + name);
        int result = 1;
        try {
            rift::PlayerApp app;
            result = app.run(folder);
        } catch (const std::exception& e) {
            LOG_ERROR("Main", e.what());
            Platform::messageBox(name, std::string("The game could not start.\n\n") + e.what() + "\n\nLog: " + Log::filePath(), Platform::MessageType::Error);
        }
        Log::shutdown();
#ifdef _WIN32
        OleUninitialize();
#endif
        return result;
    }
}

int main(int argc, char** argv) {
    return runGame(argc > 1 ? argv[1] : "");
}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    std::string argument;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
            if (size > 1) {
                argument.resize(static_cast<size_t>(size) - 1);
                WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, argument.data(), size, nullptr, nullptr);
            }
        }
        LocalFree(argv);
    }
    return runGame(argument);
}
#endif
