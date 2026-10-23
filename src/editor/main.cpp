// RIFT Editor entry point.
//
//     RiftEditor.exe [project folder | project.json | scene.json]
//
// Both `main` and `wWinMain` exist so the editor links as a console or a
// Windows-subsystem application.
#ifdef _WIN32
#include <Windows.h> // before GLEW (via the editor headers), which would define APIENTRY itself
#include <shellapi.h>
#include <DbgHelp.h>
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

#ifdef _WIN32
    // On a crash, write a minidump next to the log so it can be debugged later.
    LONG WINAPI onCrash(EXCEPTION_POINTERS* info) {
        const std::wstring path = (Paths::userData() / "logs" / "editor-crash.dmp").wstring();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{ GetCurrentThreadId(), info, FALSE };
            MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &mei, nullptr, nullptr);
            CloseHandle(file);
        }
        LOG_ERROR("Main", "RIFT crashed. A crash dump was written to " + Paths::toUtf8(Paths::userData() / "logs" / "editor-crash.dmp"));
        Log::shutdown();
        return EXCEPTION_CONTINUE_SEARCH;
    }
#endif

    int runApplication(const std::string& startupPath) {
        Paths::setUserDataFolderName("RIFT");
        Log::init("editor");
        LOG_INFO("Main", std::string("RIFT Editor ") + RIFT_VERSION_STRING + " starting");
#ifdef _WIN32
        SetUnhandledExceptionFilter(onCrash);
#endif
        int result = -1;
        try {
            rift::EditorApp app;
            result = app.run(startupPath);
        } catch (const std::exception& e) {
            reportFatalError(e.what());
        } catch (...) {
            reportFatalError("An unknown error occurred.");
        }
        LOG_INFO("Main", "RIFT Editor closed");
        Log::shutdown();
        return result;
    }
}

int main(int argc, char** argv) {
    return runApplication(argc > 1 ? argv[1] : "");
}

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    // First argument as UTF-8, e.g. a project folder dropped onto the exe.
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
