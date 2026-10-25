#include "setup/Installer.h"
#include "core/Version.h"

#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shellapi.h>

#include "stb_image.h" // zlib decoder (implementation in RiftCore)

#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>
#include <thread>

namespace setup {

namespace fs = std::filesystem;

namespace {
    constexpr const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RIFT";
    constexpr const char* kManifest = "install.manifest"; // the files the last install wrote

    std::uint32_t crc32(const unsigned char* data, size_t n) {
        static std::uint32_t table[256];
        static bool ready = false;
        if (!ready) {
            for (std::uint32_t i = 0; i < 256; ++i) {
                std::uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            ready = true;
        }
        std::uint32_t c = 0xFFFFFFFFu;
        for (size_t i = 0; i < n; ++i) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    std::wstring knownFolder(REFKNOWNFOLDERID id) {
        PWSTR path = nullptr;
        std::wstring out;
        if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) out = path;
        CoTaskMemFree(path);
        return out;
    }

    std::string lastError() {
        const DWORD code = GetLastError();
        wchar_t* text = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       reinterpret_cast<LPWSTR>(&text), 0, nullptr);
        std::string out = text ? toUtf8(fs::path(text)) : "error " + std::to_string(code);
        LocalFree(text);
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == '.')) out.pop_back();
        return out;
    }

    // Writes `data` to `target` safely: a temporary file first, then swapped in.
    // A program that is running (RIFT itself during an update) cannot be
    // overwritten, but it can be renamed: it becomes .old and is deleted later.
    bool writeFile(const fs::path& target, const std::vector<unsigned char>& data, std::string* error) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        fs::path temp = target;
        temp += L".new";
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out.is_open()) { *error = "cannot write " + toUtf8(temp) + " (is the folder writable?)"; return false; }
            out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
            if (!out) { *error = "cannot write " + toUtf8(temp) + " (disk full?)"; return false; }
        }
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
            fs::path old = target;
            old += L".old";
            DeleteFileW(old.c_str());
            if (MoveFileExW(target.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING) && MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT); // if it is still in use now
                DeleteFileW(old.c_str());
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        *error = "cannot replace " + toUtf8(target) + ": " + lastError() + ". Close RIFT and games made with it, then try again.";
        DeleteFileW(temp.c_str());
        return false;
    }

    bool shortcut(const fs::path& link, const fs::path& target, const std::wstring& description) {
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
        sl->SetPath(target.c_str());
        sl->SetWorkingDirectory(target.parent_path().c_str());
        sl->SetDescription(description.c_str());
        sl->SetIconLocation(target.c_str(), 0);
        IPersistFile* pf = nullptr;
        bool ok = false;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
            std::error_code ec;
            fs::create_directories(link.parent_path(), ec);
            ok = SUCCEEDED(pf->Save(link.c_str(), TRUE));
            pf->Release();
        }
        sl->Release();
        return ok;
    }

    fs::path startMenuLink() { return fs::path(knownFolder(FOLDERID_Programs)) / L"RIFT Engine.lnk"; }
    fs::path desktopLink() { return fs::path(knownFolder(FOLDERID_Desktop)) / L"RIFT Engine.lnk"; }

    void setString(HKEY key, const wchar_t* name, const std::wstring& value) {
        RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    void setDword(HKEY key, const wchar_t* name, DWORD value) {
        RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    }
    std::wstring getString(HKEY key, const wchar_t* name) {
        wchar_t buffer[2048];
        DWORD size = sizeof(buffer), type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &size) != ERROR_SUCCESS || type != REG_SZ) return {};
        buffer[std::min<size_t>(size / sizeof(wchar_t), 2047)] = L'\0';
        return buffer;
    }

    std::vector<std::string> readManifest(const fs::path& dir) {
        std::vector<std::string> files;
        std::ifstream in(dir / kManifest);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) files.push_back(line);
        }
        return files;
    }

    // Deletes a folder if it is empty, then its parents up to `stop`.
    void pruneEmpty(fs::path folder, const fs::path& stop) {
        std::error_code ec;
        while (!folder.empty() && folder != stop && fs::is_directory(folder, ec) && fs::is_empty(folder, ec)) {
            fs::remove(folder, ec);
            folder = folder.parent_path();
        }
    }
}

std::string toUtf8(const fs::path& p) {
    const std::wstring w = p.wstring();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return fs::path(w);
}

// ============================================================================
//  Payload
// ============================================================================
bool Payload::open(const fs::path& exe) {
    exe_ = exe;
    files_.clear();
    std::ifstream in(exe, std::ios::binary);
    if (!in.is_open()) return false;
    in.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
    if (size < 32) return false;
    in.seekg(static_cast<std::streamoff>(size - 32));
    char magic[8];
    std::uint64_t offset = 0, total = 0;
    std::uint32_t count = 0, reserved = 0;
    in.read(magic, 8);
    in.read(reinterpret_cast<char*>(&offset), 8);
    in.read(reinterpret_cast<char*>(&count), 4);
    in.read(reinterpret_cast<char*>(&reserved), 4);
    in.read(reinterpret_cast<char*>(&total), 8);
    if (!in || std::memcmp(magic, "RIFTPAK1", 8) != 0 || offset >= size) return false;
    offset_ = offset;
    total_ = total;
    in.seekg(static_cast<std::streamoff>(offset));
    for (std::uint32_t k = 0; k < count; ++k) {
        char tag[4];
        std::uint32_t length = 0;
        in.read(tag, 4);
        in.read(reinterpret_cast<char*>(&length), 4);
        if (!in || std::memcmp(tag, "FILE", 4) != 0 || length > 4096) { files_.clear(); return false; }
        PayloadFile f;
        f.path.resize(length);
        in.read(f.path.data(), length);
        in.read(reinterpret_cast<char*>(&f.size), 8);
        in.read(reinterpret_cast<char*>(&f.packedSize), 8);
        in.read(reinterpret_cast<char*>(&f.crc), 4);
        f.offset = static_cast<std::uint64_t>(in.tellg());
        if (!in || f.offset + f.packedSize > size) { files_.clear(); return false; }
        in.seekg(static_cast<std::streamoff>(f.packedSize), std::ios::cur);
        files_.push_back(std::move(f));
    }
    return !files_.empty();
}

const PayloadFile* Payload::find(const std::string& path) const {
    for (const PayloadFile& f : files_) if (f.path == path) return &f;
    return nullptr;
}

bool Payload::read(const PayloadFile& f, std::vector<unsigned char>& out, std::string* error) const {
    std::ifstream in(exe_, std::ios::binary);
    std::vector<char> packed(static_cast<size_t>(f.packedSize));
    in.seekg(static_cast<std::streamoff>(f.offset));
    in.read(packed.data(), static_cast<std::streamsize>(packed.size()));
    if (!in) { if (error) *error = "the setup file is damaged (" + f.path + " cannot be read); download it again"; return false; }
    int length = 0;
    char* raw = stbi_zlib_decode_malloc_guesssize_headerflag(packed.data(), static_cast<int>(packed.size()), static_cast<int>(std::max<std::uint64_t>(f.size, 1)), &length, 1);
    if (!raw || static_cast<std::uint64_t>(length) != f.size) {
        free(raw);
        if (error) *error = "the setup file is damaged (" + f.path + " does not unpack); download it again";
        return false;
    }
    out.assign(reinterpret_cast<unsigned char*>(raw), reinterpret_cast<unsigned char*>(raw) + length);
    free(raw);
    if (crc32(out.data(), out.size()) != f.crc) {
        if (error) *error = "the setup file is damaged (" + f.path + " has the wrong checksum); download it again";
        return false;
    }
    return true;
}

// ============================================================================
//  Install / uninstall
// ============================================================================
fs::path defaultInstallDir() {
    return fs::path(knownFolder(FOLDERID_LocalAppData)) / L"Programs" / L"RIFT";
}

Installed findInstalled() {
    Installed r;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return r;
    const std::wstring dir = getString(key, L"InstallLocation");
    const std::wstring version = getString(key, L"DisplayVersion");
    RegCloseKey(key);
    std::error_code ec;
    if (dir.empty() || !fs::exists(fs::path(dir) / L"RiftEditor.exe", ec)) return r;
    r.found = true;
    r.dir = dir;
    r.version = toUtf8(fs::path(version));
    return r;
}

std::uint64_t freeSpace(const fs::path& dir) {
    // The nearest folder that exists.
    fs::path p = dir;
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec) && p.has_parent_path() && p.parent_path() != p) p = p.parent_path();
    ULARGE_INTEGER available{};
    if (!GetDiskFreeSpaceExW(p.c_str(), &available, nullptr, nullptr)) return 0;
    return available.QuadPart;
}

bool install(const Payload& payload, const fs::path& setupExe, const Options& options, const ProgressFn& progress, std::string* error) {
    std::string err;
    auto fail = [&](const std::string& message) { if (error) *error = message; return false; };
    std::error_code ec;
    const fs::path dir = options.dir;
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir, ec)) return fail("cannot create " + toUtf8(dir) + ". Choose a folder you can write to (installing for all users is not supported).");
    if (freeSpace(dir) < payload.totalSize() + 16ull * 1024 * 1024) return fail("there is not enough free disk space in " + toUtf8(dir));

    // Leftovers of the previous update (programs that were running then).
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == L".old") fs::remove(it->path(), ec);
    }

    const std::vector<std::string> previous = readManifest(dir);
    std::uint64_t done = 0;
    std::vector<unsigned char> data;
    std::set<std::string> written;
    for (const PayloadFile& f : payload.files()) {
        if (progress) progress(f.path, done, payload.totalSize());
        if (!payload.read(f, data, &err)) return fail(err);
        if (!writeFile(dir / fromUtf8(f.path), data, &err)) return fail(err);
        written.insert(f.path);
        done += f.size;
    }
    // The uninstaller: this program without the payload.
    {
        std::ifstream in(setupExe, std::ios::binary);
        std::vector<unsigned char> stub(static_cast<size_t>(payload.stubSize()));
        in.read(reinterpret_cast<char*>(stub.data()), static_cast<std::streamsize>(stub.size()));
        if (!in) return fail("cannot read the setup program");
        if (!writeFile(dir / L"Uninstall.exe", stub, &err)) return fail(err);
        written.insert("Uninstall.exe");
    }
    // Files the old version had and this one does not.
    for (const std::string& old : previous) {
        if (written.count(old)) continue;
        const fs::path p = dir / fromUtf8(old);
        fs::remove(p, ec);
        pruneEmpty(p.parent_path(), dir);
    }
    {
        std::ofstream manifest(dir / kManifest, std::ios::trunc);
        for (const std::string& path : written) manifest << path << "\n";
    }
    if (progress) progress("Shortcuts", done, payload.totalSize());

    // Shortcuts
    const fs::path editor = dir / L"RiftEditor.exe";
    if (options.startMenuShortcut) shortcut(startMenuLink(), editor, L"RIFT Engine: make Roblox-style games with Lua");
    else fs::remove(startMenuLink(), ec);
    if (options.desktopShortcut) shortcut(desktopLink(), editor, L"RIFT Engine");
    else fs::remove(desktopLink(), ec);

    // Apps & features
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const std::wstring uninstaller = L"\"" + (dir / L"Uninstall.exe").wstring() + L"\"";
        setString(key, L"DisplayName", L"RIFT Engine");
        setString(key, L"DisplayVersion", fromUtf8(RIFT_VERSION_STRING).wstring());
        setString(key, L"Publisher", L"RIFT");
        setString(key, L"DisplayIcon", editor.wstring() + L",0");
        setString(key, L"InstallLocation", dir.wstring());
        setString(key, L"UninstallString", uninstaller);
        setString(key, L"QuietUninstallString", uninstaller + L" /silent");
        setString(key, L"URLInfoAbout", fromUtf8(RIFT_WEBSITE).wstring());
        setString(key, L"HelpLink", fromUtf8(std::string("https://github.com/") + RIFT_GITHUB_REPO + "/issues").wstring());
        const std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        wchar_t date[16];
        wcsftime(date, 16, L"%Y%m%d", &tm);
        setString(key, L"InstallDate", date);
        setDword(key, L"NoModify", 1);
        setDword(key, L"NoRepair", 1);
        setDword(key, L"EstimatedSize", static_cast<DWORD>(payload.totalSize() / 1024));
        setDword(key, L"VersionMajor", RIFT_VERSION_MAJOR);
        setDword(key, L"VersionMinor", RIFT_VERSION_MINOR);
        RegCloseKey(key);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (progress) progress("", payload.totalSize(), payload.totalSize());
    return true;
}

bool uninstall(const fs::path& dir, bool removeSettings, const ProgressFn& progress, std::string* error) {
    std::error_code ec;
    std::vector<std::string> files = readManifest(dir);
    if (files.empty() && !fs::exists(dir / L"RiftEditor.exe", ec)) {
        if (error) *error = toUtf8(dir) + " does not contain RIFT";
        return false;
    }
    std::uint64_t done = 0;
    const std::uint64_t total = files.size() + 2;
    // Only what RIFT installed: anything the user put in the folder stays.
    for (const std::string& f : files) {
        if (progress) progress(f, done++, total);
        const fs::path p = dir / fromUtf8(f);
        if (!fs::remove(p, ec) && fs::exists(p)) MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        pruneEmpty(p.parent_path(), dir);
    }
    fs::remove(dir / kManifest, ec);
    for (auto it = fs::recursive_directory_iterator(dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == L".old" || it->path().extension() == L".new") fs::remove(it->path(), ec);
    }
    pruneEmpty(dir, dir.parent_path());

    if (progress) progress("Shortcuts", done++, total);
    fs::remove(startMenuLink(), ec);
    fs::remove(desktopLink(), ec);
    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKey);
    if (removeSettings) {
        const fs::path roaming = knownFolder(FOLDERID_RoamingAppData);
        if (!roaming.empty()) fs::remove_all(roaming / L"RIFT", ec);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (progress) progress("", total, total);
    return true;
}

void waitForProcess(unsigned long pid, unsigned timeoutMs) {
    if (!pid) return;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return; // already gone
    WaitForSingleObject(h, timeoutMs);
    CloseHandle(h);
}

bool hasDesktopShortcut() {
    std::error_code ec;
    return fs::exists(desktopLink(), ec);
}

bool hasStartMenuShortcut() {
    std::error_code ec;
    return fs::exists(startMenuLink(), ec);
}

bool launchEditor(const fs::path& dir) {
    const fs::path exe = dir / L"RiftEditor.exe";
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

} // namespace setup
