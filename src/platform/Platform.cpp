#include "platform/Platform.h"
#include "core/Paths.h"
#include "core/Log.h"

#ifdef _WIN32
#include <Windows.h>
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <shellapi.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#endif

#include <filesystem>

namespace Platform {

#ifdef _WIN32
namespace {
    HWND g_owner = nullptr;

    std::wstring widen(const std::string& s) {
        if (s.empty()) return {};
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }
    std::string narrow(const wchar_t* w) {
        if (!w || !*w) return {};
        const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    // COM must be initialised on the thread that shows the dialog. Done lazily,
    // once (GLFW does not initialise COM).
    void ensureCom() {
        static bool done = false;
        if (!done) {
            done = true;
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        }
    }

    template <class T> struct ComPtr {
        T* p = nullptr;
        ~ComPtr() { if (p) p->Release(); }
        T** operator&() { return &p; }
        T* operator->() { return p; }
        explicit operator bool() const { return p != nullptr; }
    };

    void setStartFolder(IFileDialog* dialog, const std::string& folder) {
        if (folder.empty()) return;
        std::error_code ec;
        const std::filesystem::path abs = std::filesystem::absolute(Paths::fromUtf8(folder), ec);
        if (ec || !std::filesystem::is_directory(abs, ec)) return;
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(abs.wstring().c_str(), nullptr, IID_PPV_ARGS(&item)))) {
            dialog->SetFolder(item.p);
        }
    }

    void setFilters(IFileDialog* dialog, const std::vector<FileFilter>& filters, std::vector<std::wstring>& storage) {
        if (filters.empty()) return;
        storage.clear();
        storage.reserve(filters.size() * 2);
        for (const auto& f : filters) { storage.push_back(widen(f.name)); storage.push_back(widen(f.patterns)); }
        std::vector<COMDLG_FILTERSPEC> specs;
        for (size_t i = 0; i < filters.size(); ++i) specs.push_back({ storage[i * 2].c_str(), storage[i * 2 + 1].c_str() });
        dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
    }

    std::string resultPath(IFileDialog* dialog) {
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return {};
        PWSTR path = nullptr;
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
        std::string out = narrow(path);
        CoTaskMemFree(path);
        return out;
    }
}

void setMainWindow(GLFWwindow* window) {
    g_owner = window ? glfwGetWin32Window(window) : nullptr;
}

std::string openFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startFolder) {
    ensureCom();
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    std::vector<std::wstring> storage;
    setFilters(dialog.p, filters, storage);
    dialog->SetTitle(widen(title).c_str());
    setStartFolder(dialog.p, startFolder);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    if (FAILED(dialog->Show(g_owner))) return {};
    return resultPath(dialog.p);
}

std::string saveFileDialog(const std::string& title, const std::vector<FileFilter>& filters, const std::string& startFolder,
                           const std::string& defaultName, const std::string& defaultExtension) {
    ensureCom();
    ComPtr<IFileSaveDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    std::vector<std::wstring> storage;
    setFilters(dialog.p, filters, storage);
    dialog->SetTitle(widen(title).c_str());
    setStartFolder(dialog.p, startFolder);
    if (!defaultName.empty()) dialog->SetFileName(widen(defaultName).c_str());
    if (!defaultExtension.empty()) dialog->SetDefaultExtension(widen(defaultExtension).c_str());
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
    if (FAILED(dialog->Show(g_owner))) return {};
    return resultPath(dialog.p);
}

std::string pickFolderDialog(const std::string& title, const std::string& startFolder) {
    ensureCom();
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    dialog->SetTitle(widen(title).c_str());
    setStartFolder(dialog.p, startFolder);
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(g_owner))) return {};
    return resultPath(dialog.p);
}

void messageBox(const std::string& title, const std::string& text, MessageType type) {
    UINT flags = MB_OK | MB_SETFOREGROUND;
    flags |= type == MessageType::Error ? MB_ICONERROR : type == MessageType::Warning ? MB_ICONWARNING : MB_ICONINFORMATION;
    MessageBoxW(g_owner, widen(text).c_str(), widen(title).c_str(), flags);
}

bool askYesNo(const std::string& title, const std::string& text) {
    return MessageBoxW(g_owner, widen(text).c_str(), widen(title).c_str(), MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
}

void revealInExplorer(const std::string& path) {
    std::error_code ec;
    const std::filesystem::path p = std::filesystem::absolute(Paths::fromUtf8(path), ec);
    if (std::filesystem::is_directory(p, ec)) {
        ShellExecuteW(nullptr, L"open", p.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    const std::wstring args = L"/select,\"" + p.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void openWithShell(const std::string& pathOrUrl) {
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", widen(pathOrUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARN("Platform", "Could not open " + pathOrUrl);
}

void setClipboard(const std::string& text) {
    if (!OpenClipboard(g_owner)) return;
    EmptyClipboard();
    const std::wstring w = widen(text);
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (w.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(mem), w.c_str(), (w.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
}

bool launch(const std::string& exe, const std::vector<std::string>& arguments, const std::string& workingFolder) {
    std::wstring params;
    for (const std::string& a : arguments) {
        if (!params.empty()) params += L' ';
        params += L'"' + widen(a) + L'"';
    }
    const std::wstring file = widen(exe), dir = widen(workingFolder);
    const HINSTANCE r = ShellExecuteW(g_owner, L"open", file.c_str(), params.c_str(), dir.empty() ? nullptr : dir.c_str(), SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) {
        LOG_WARN("Platform", "Could not start " + exe);
        return false;
    }
    return true;
}

std::string documentsFolder() {
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) out = narrow(path);
    CoTaskMemFree(path);
    return out;
}

bool moveToRecycleBin(const std::string& path) {
    // SHFileOperation wants a double-NUL-terminated list.
    std::wstring from = widen(path);
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.hwnd = g_owner;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    const int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted) {
        LOG_WARN("Platform", "Could not move " + path + " to the Recycle Bin");
        return false;
    }
    return true;
}

#else // non-Windows: not supported (RIFT targets Windows); keep the build working for tests.
void setMainWindow(GLFWwindow*) {}
std::string openFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&) { return {}; }
std::string saveFileDialog(const std::string&, const std::vector<FileFilter>&, const std::string&, const std::string&, const std::string&) { return {}; }
std::string pickFolderDialog(const std::string&, const std::string&) { return {}; }
void messageBox(const std::string& title, const std::string& text, MessageType) { LOG_WARN(title, text); }
bool askYesNo(const std::string&, const std::string&) { return false; }
void revealInExplorer(const std::string&) {}
void openWithShell(const std::string&) {}
void setClipboard(const std::string&) {}
bool launch(const std::string&, const std::vector<std::string>&, const std::string&) { return false; }
std::string documentsFolder() { return {}; }
bool moveToRecycleBin(const std::string&) { return false; }
#endif

} // namespace Platform
