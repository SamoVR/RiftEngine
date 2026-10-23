// RIFT Editor entry point.
//
// Both `main` and `wWinMain` exist so the editor links as either a console or a
// Windows-subsystem application. Usage:
//     RiftEditor.exe [path\to\project.json | path\to\scene.json]
//
#ifdef _WIN32
#include <Windows.h> // before GLEW (via EditorApp.h), which otherwise defines APIENTRY itself
#include <shellapi.h>
#endif

#include "editor/EditorApp.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Version.h"
#include "platform/Platform.h"

#include <exception>
#include <string>

namespace {

    // A start-up failure in a Windows-subsystem build has no console to print
    // to: show the reason in a dialog and keep it in the log file.
    void reportFatalError(const std::string& message) {
        LOG_ERROR("Main", "Fatal error: " + message);
        Platform::messageBox("RIFT - fatal error", message + "\n\nLog: " + Log::filePath(), Platform::MessageType::Error);
    }

    int runApplication(const std::string& startupPath) {
        Paths::setUserDataFolderName("RIFT");
        Log::init("editor");
        LOG_INFO("Main", std::string("RIFT Editor ") + RIFT_VERSION_STRING + " starting");
        int result = -1;
        try {
            EditorApp app;
            result = app.run(startupPath);
        } catch (const std::exception& e) {
            reportFatalError(e.what());
        } catch (...) {
            reportFatalError("An unknown error occurred.");
        }
        Log::shutdown();
        return result;
    }
}

int main(int argc, char** argv) {
    return runApplication(argc > 1 ? argv[1] : "");
}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // First argument as UTF-8, e.g. a project.json dropped onto the exe.
    std::string startupPath;
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, nullptr, 0, nullptr, nullptr);
            if (size > 1) {
                startupPath.resize(static_cast<size_t>(size) - 1);
                WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, startupPath.data(), size, nullptr, nullptr);
            }
        }
        LocalFree(argv);
    }
    return runApplication(startupPath);
}
#endif
