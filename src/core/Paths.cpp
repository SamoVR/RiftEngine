#include "core/Paths.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#include <ShlObj.h>
#endif

namespace Paths {

namespace {
    std::string g_userFolderName = "RIFT";
    fs::path g_projectRoot;

    fs::path computeExeDir() {
#ifdef _WIN32
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (n == 0) return fs::current_path();
            if (n < buffer.size()) { buffer.resize(n); break; }
            buffer.resize(buffer.size() * 2);
        }
        return fs::path(buffer).parent_path();
#else
        std::error_code ec;
        const fs::path self = fs::read_symlink("/proc/self/exe", ec);
        return ec ? fs::current_path() : self.parent_path();
#endif
    }

    bool isDataFolder(const fs::path& p) {
        std::error_code ec;
        return fs::is_directory(p / "shaders", ec) && fs::is_directory(p / "fonts", ec);
    }

    fs::path computeEngineData() {
        // Shipped layout: <exe>/data. Development: walk up to the repository.
        fs::path dir = exeDir();
        for (int depth = 0; depth < 6 && !dir.empty(); ++depth) {
            if (isDataFolder(dir / "data")) return dir / "data";
            const fs::path parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        // Last resort for tools/tests run from the repository root.
        std::error_code ec;
        if (isDataFolder(fs::current_path(ec) / "data")) return fs::current_path(ec) / "data";
        throw std::runtime_error("RIFT could not find its data folder (shaders, fonts). It must be next to the executable: " + toUtf8(exeDir() / "data"));
    }

    fs::path computeUserData() {
        fs::path base;
#ifdef _WIN32
        PWSTR appData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &appData)) && appData) {
            base = fs::path(appData);
            CoTaskMemFree(appData);
        }
#else
        if (const char* home = std::getenv("HOME")) base = fs::path(home) / ".config";
#endif
        if (base.empty()) base = exeDir();
        fs::path dir = base / fromUtf8(g_userFolderName);
        std::error_code ec;
        fs::create_directories(dir, ec);
        return dir;
    }
}

const fs::path& exeDir() {
    static const fs::path dir = computeExeDir();
    return dir;
}

const fs::path& engineData() {
    static const fs::path dir = computeEngineData();
    return dir;
}

fs::path shader(const std::string& fileName) { return engineData() / "shaders" / fromUtf8(fileName); }
fs::path font(const std::string& fileName) { return engineData() / "fonts" / fromUtf8(fileName); }
fs::path icon(const std::string& fileName) { return engineData() / "icons" / fromUtf8(fileName); }
fs::path engineTexture(const std::string& fileName) { return engineData() / "textures" / fromUtf8(fileName); }

void setUserDataFolderName(const std::string& name) { g_userFolderName = name.empty() ? "RIFT" : name; }

const fs::path& userData() {
    static const fs::path dir = computeUserData();
    return dir;
}

void setProjectRoot(const fs::path& root) { g_projectRoot = root; }
const fs::path& projectRoot() { return g_projectRoot; }

fs::path resolveAsset(const std::string& stored) {
    if (stored.empty()) return {};
    if (stored.rfind(kEnginePrefix, 0) == 0) return engineData() / fromUtf8(stored.substr(std::strlen(kEnginePrefix)));
    const fs::path p = fromUtf8(stored);
    if (p.is_absolute()) return p;
    std::error_code ec;
    if (!g_projectRoot.empty()) {
        const fs::path inProject = g_projectRoot / p;
        if (fs::exists(inProject, ec)) return inProject;
    }
    const fs::path inData = engineData() / p;
    if (fs::exists(inData, ec)) return inData;
    return g_projectRoot.empty() ? p : g_projectRoot / p;
}

std::string toStored(const fs::path& path) {
    std::error_code ec;
    auto relativeInside = [&](const fs::path& root, std::string& out) {
        if (root.empty() || !path.is_absolute()) return false;
        const fs::path rel = fs::relative(path, root, ec);
        if (ec || rel.empty() || *rel.begin() == "..") return false;
        out = toUtf8(rel);
        std::replace(out.begin(), out.end(), '\\', '/');
        return true;
    };
    std::string rel;
    if (relativeInside(g_projectRoot, rel)) return rel;
    if (relativeInside(engineData(), rel)) return kEnginePrefix + rel;
    std::string s = toUtf8(path);
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

fs::path fromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string toUtf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

} // namespace Paths
