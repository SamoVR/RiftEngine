#pragma once
//
// Process.h -- runs a command-line program and collects what it prints
// (git for source control). No console window appears; stdin is empty, so a
// program that asks a question gets "no answer" instead of hanging. UTF-8.
//
#include <string>
#include <vector>

namespace Platform {

struct ProcessResult {
    bool started = false;   // false: the program could not be started (not installed?)
    bool timedOut = false;  // killed after the time limit
    int exitCode = -1;
    std::string output;     // stdout and stderr, in the order they were written
    bool ok() const { return started && !timedOut && exitCode == 0; }
};

// Runs `exe` (a full path, or a program name looked up on PATH) with
// `arguments` in `workingFolder` and waits for it, at most `timeoutMs`
// (0 = no limit). `environment` adds or replaces variables ("NAME=value").
ProcessResult runProcess(const std::string& exe, const std::vector<std::string>& arguments,
                         const std::string& workingFolder = "", unsigned timeoutMs = 120000,
                         const std::vector<std::string>& environment = {});

// The full path of a program on PATH ("" if there is none). "git" finds git.exe.
std::string findOnPath(const std::string& program);

// %LOCALAPPDATA% ("" if unknown).
std::string localAppDataFolder();

} // namespace Platform
