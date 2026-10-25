#pragma once
//
// Installer.h -- what RIFT Setup does, without its window: reads the files
// appended to the setup (the payload), puts them in place, creates the
// shortcuts and the "Apps & features" entry, and removes everything again.
// Everything is per user (%LOCALAPPDATA%\Programs\RIFT, HKCU): no
// administrator rights are ever needed.
//
// Payload layout (written by tools/make_installer.py after the setup .exe):
//   per file:  "FILE" u32 pathLength, path (UTF-8, '/'), u64 size, u64 packedSize,
//              u32 crc32, packedSize bytes of zlib data
//   trailer:   "RIFTPAK1" u64 payloadOffset, u32 fileCount, u32 reserved, u64 totalSize
//
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace setup {

struct PayloadFile {
    std::string path;
    std::uint64_t size = 0, packedSize = 0, offset = 0;
    std::uint32_t crc = 0;
};

class Payload {
public:
    // Reads the index of the files appended to `exe`. False: there are none.
    bool open(const std::filesystem::path& exe);
    bool present() const { return !files_.empty(); }
    const std::vector<PayloadFile>& files() const { return files_; }
    std::uint64_t totalSize() const { return total_; }
    std::uint64_t stubSize() const { return offset_; } // the setup program itself ends here
    const PayloadFile* find(const std::string& path) const;
    // The file's contents (unpacked and checked).
    bool read(const PayloadFile& f, std::vector<unsigned char>& out, std::string* error) const;

private:
    std::filesystem::path exe_;
    std::vector<PayloadFile> files_;
    std::uint64_t offset_ = 0, total_ = 0;
};

struct Installed {
    bool found = false;
    std::filesystem::path dir;
    std::string version;
};

struct Options {
    std::filesystem::path dir;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
};

// file being written, bytes done, bytes in total
using ProgressFn = std::function<void(const std::string&, std::uint64_t, std::uint64_t)>;

std::filesystem::path defaultInstallDir();     // %LOCALAPPDATA%\Programs\RIFT
Installed findInstalled();                     // from the Apps & features entry
std::uint64_t freeSpace(const std::filesystem::path& dir);

// Copies the payload into options.dir (replacing an older RIFT there),
// writes Uninstall.exe from `setupExe`, the shortcuts and the uninstall entry.
bool install(const Payload& payload, const std::filesystem::path& setupExe, const Options& options,
             const ProgressFn& progress, std::string* error);
// Removes RIFT from `dir` (and the user's settings if asked; never projects).
bool uninstall(const std::filesystem::path& dir, bool removeSettings, const ProgressFn& progress, std::string* error);

// Waits for another process (the editor that started an update) to exit.
void waitForProcess(unsigned long pid, unsigned timeoutMs);
bool launchEditor(const std::filesystem::path& dir);
// Whether the shortcuts exist (an update keeps the user's choice).
bool hasDesktopShortcut();
bool hasStartMenuShortcut();

std::string toUtf8(const std::filesystem::path& p);
std::filesystem::path fromUtf8(const std::string& s);

} // namespace setup
