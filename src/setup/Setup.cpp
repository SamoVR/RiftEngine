// RIFT Setup: installs RIFT for the current user, updates it in place, and
// (as Uninstall.exe in the install folder) removes it again.
//
//   RIFT-Setup-1.1.0.exe                     the setup window
//   ... /silent [/dir D] [/wait PID] [/launch] [/update]
//                                            no window (the editor's updater uses this)
//   Uninstall.exe [/silent]                  remove RIFT (Apps & features runs this)
//
#include "setup/Installer.h"
#include "core/Version.h"
#include "platform/Platform.h"

#include <Windows.h>
#include <shellapi.h>
#include <ole2.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>
#include "IconsFontAwesome6.h"
#include "stb_image.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;
using namespace setup;

namespace {

// ---- brand colours (the RIFT logo's violet to cyan)
const ImVec4 kViolet{ 0.545f, 0.361f, 0.965f, 1.0f };
const ImVec4 kCyan{ 0.133f, 0.827f, 0.933f, 1.0f };
const ImVec4 kText{ 0.894f, 0.894f, 0.914f, 1.0f };
const ImVec4 kDim{ 0.553f, 0.561f, 0.600f, 1.0f };
const ImVec4 kGreen{ 0.243f, 0.812f, 0.557f, 1.0f };
const ImVec4 kRed{ 0.941f, 0.322f, 0.322f, 1.0f };
const ImVec4 kBg{ 0.102f, 0.106f, 0.125f, 1.0f };
const ImVec4 kFrame{ 0.165f, 0.173f, 0.200f, 1.0f };

struct Args {
    bool silent = false, update = false, launch = false, uninstall = false;
    fs::path dir;
    unsigned long waitPid = 0;
    fs::path deleteWhenDone; // a temporary copy of the uninstaller
};

Args parseArgs() {
    Args a;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        const std::wstring s = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : std::wstring(); };
        if (s == L"/silent" || s == L"/S" || s == L"--silent") a.silent = true;
        else if (s == L"/update") a.update = true;
        else if (s == L"/launch") a.launch = true;
        else if (s == L"/uninstall") a.uninstall = true;
        else if (s == L"/dir") a.dir = next();
        else if (s == L"/wait") a.waitPid = std::wcstoul(next().c_str(), nullptr, 10);
        else if (s == L"/cleanup") a.deleteWhenDone = next();
    }
    LocalFree(argv);
    return a;
}

fs::path selfPath() {
    wchar_t buffer[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, MAX_PATH * 4);
    return fs::path(std::wstring(buffer, n));
}

std::string megabytes(std::uint64_t bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024 * 1024 * 10) std::snprintf(text, sizeof(text), "%.0f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%.0f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

// After uninstalling, the temporary copy of the uninstaller removes itself.
void deleteSelfLater(const fs::path& file) {
    std::wstring cmd = L"/c ping -n 3 127.0.0.1 >nul & del /f /q \"" + file.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", L"cmd.exe", cmd.c_str(), nullptr, SW_HIDE);
}

// ============================================================================
//  The window
// ============================================================================
class SetupApp {
public:
    SetupApp(const Args& args, Payload& payload, bool uninstalling) : args_(args), payload_(payload), uninstalling_(uninstalling) {}

    int run() {
        if (!createWindow()) return fallback();
        while (!glfwWindowShouldClose(window_) && !quit_) {
            glfwWaitEventsTimeout(working_ ? 0.03 : 0.25);
            frame();
        }
        if (worker_.joinable()) worker_.join();
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        return ok_ ? 0 : 1;
    }

private:
    enum class Page { Options, Working, Done, Failed };
    const Args& args_;
    Payload& payload_;
    bool uninstalling_;
    GLFWwindow* window_ = nullptr;
    GLuint logo_ = 0;
    float scale_ = 1.0f;
    ImFont* body_ = nullptr;
    ImFont* heading_ = nullptr;
    bool icons_ = false;
    Page page_ = Page::Options;
    bool quit_ = false;

    // Options
    std::string dir_;
    Installed installed_;
    bool desktop_ = true, startMenu_ = true, launchAfter_ = true, removeSettings_ = false;

    // Work
    std::thread worker_;
    std::atomic<bool> working_{ false };
    std::atomic<bool> ok_{ false };
    std::atomic<std::uint64_t> done_{ 0 }, total_{ 1 };
    std::mutex mutex_;
    std::string current_, error_;

    // A font file from the payload (installing) or the installed RIFT (uninstalling).
    bool fontData(const char* name, std::vector<unsigned char>& out) {
        if (payload_.present()) {
            const PayloadFile* f = payload_.find(std::string("data/fonts/") + name);
            return f && payload_.read(*f, out, nullptr);
        }
        std::FILE* file = _wfopen((installedDir() / L"data" / L"fonts" / fromUtf8(name)).c_str(), L"rb");
        if (!file) return false;
        std::fseek(file, 0, SEEK_END);
        out.resize(static_cast<size_t>(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
        std::fclose(file);
        return ok;
    }

    fs::path installedDir() const {
        if (!args_.dir.empty()) return args_.dir;
        return installed_.found ? installed_.dir : selfPath().parent_path();
    }

    ImFont* addFont(const char* file, float size, bool withIcons) {
        std::vector<unsigned char> data;
        if (!fontData(file, data)) return nullptr;
        ImGuiIO& io = ImGui::GetIO();
        ImFontConfig config;
        config.FontDataOwnedByAtlas = true;
        void* copy = IM_ALLOC(data.size());
        std::memcpy(copy, data.data(), data.size());
        ImFont* font = io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(data.size()), size, &config);
        if (font && withIcons) {
            std::vector<unsigned char> iconData;
            if (fontData("fa-solid-900.ttf", iconData)) {
                ImFontConfig ic;
                ic.MergeMode = true;
                ic.PixelSnapH = true;
                ic.FontDataOwnedByAtlas = true;
                ic.GlyphMinAdvanceX = size * 1.1f;
                void* iconCopy = IM_ALLOC(iconData.size());
                std::memcpy(iconCopy, iconData.data(), iconData.size());
                static const ImWchar ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
                icons_ = io.Fonts->AddFontFromMemoryTTF(iconCopy, static_cast<int>(iconData.size()), size * 0.9f, &ic, ranges) != nullptr;
            }
        }
        return font;
    }

    bool createWindow() {
        if (!glfwInit()) return false;
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
        window_ = glfwCreateWindow(780, 500, uninstalling_ ? "Uninstall RIFT" : "RIFT Setup", nullptr, nullptr);
        if (!window_) { glfwTerminate(); return false; }
        glfwMakeContextCurrent(window_);
        glfwSwapInterval(1);
        glewExperimental = GL_TRUE;
        if (glewInit() != GLEW_OK) { glfwDestroyWindow(window_); glfwTerminate(); window_ = nullptr; return false; }
        float xs = 1.0f, ys = 1.0f;
        glfwGetWindowContentScale(window_, &xs, &ys);
        scale_ = std::max(1.0f, xs);
        Platform::setMainWindow(window_);

        // Centre on the screen.
        if (GLFWmonitor* m = glfwGetPrimaryMonitor()) {
            const GLFWvidmode* mode = glfwGetVideoMode(m);
            int w = 0, h = 0;
            glfwGetWindowSize(window_, &w, &h);
            if (mode) glfwSetWindowPos(window_, (mode->width - w) / 2, (mode->height - h) / 2);
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init("#version 330");
        body_ = addFont("Inter-Regular.ttf", 15.0f * scale_, true);
        heading_ = addFont("Inter-SemiBold.ttf", 26.0f * scale_, false);
        if (!body_) body_ = io.Fonts->AddFontDefault();
        if (!heading_) heading_ = body_;
        io.FontDefault = body_;
        style();
        loadLogo();

        installed_ = findInstalled();
        dir_ = toUtf8(installed_.found ? installed_.dir : defaultInstallDir());
        glfwShowWindow(window_);
        return true;
    }

    void loadLogo() {
        std::vector<unsigned char> png;
        if (payload_.present()) {
            const PayloadFile* f = payload_.find("data/icons/rift.png");
            if (!f || !payload_.read(*f, png, nullptr)) return;
        } else {
            std::FILE* file = _wfopen((installedDir() / L"data" / L"icons" / L"rift.png").c_str(), L"rb");
            if (!file) return;
            std::fseek(file, 0, SEEK_END);
            png.resize(static_cast<size_t>(std::ftell(file)));
            std::fseek(file, 0, SEEK_SET);
            std::fread(png.data(), 1, png.size(), file);
            std::fclose(file);
        }
        int w = 0, h = 0, n = 0;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* pixels = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &n, 4);
        if (!pixels) return;
        glGenTextures(1, &logo_);
        glBindTexture(GL_TEXTURE_2D, logo_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        GLFWimage icon{ w, h, pixels };
        glfwSetWindowIcon(window_, 1, &icon);
        stbi_image_free(pixels);
    }

    void style() {
        ImGuiStyle& st = ImGui::GetStyle();
        ImGui::StyleColorsDark(&st);
        st.WindowPadding = ImVec2(0, 0);
        st.FramePadding = ImVec2(10, 7);
        st.ItemSpacing = ImVec2(10, 10);
        st.FrameRounding = 6;
        st.GrabRounding = 6;
        st.WindowBorderSize = 0;
        st.ScaleAllSizes(scale_);
        ImVec4* c = st.Colors;
        c[ImGuiCol_WindowBg] = kBg;
        c[ImGuiCol_Text] = kText;
        c[ImGuiCol_TextDisabled] = kDim;
        c[ImGuiCol_FrameBg] = kFrame;
        c[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
        c[ImGuiCol_FrameBgActive] = ImVec4(0.23f, 0.24f, 0.28f, 1.0f);
        c[ImGuiCol_Button] = kFrame;
        c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.23f, 0.27f, 1.0f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.26f, 0.27f, 0.31f, 1.0f);
        c[ImGuiCol_CheckMark] = kCyan;
        c[ImGuiCol_PlotHistogram] = kViolet;
        c[ImGuiCol_TextSelectedBg] = ImVec4(kViolet.x, kViolet.y, kViolet.z, 0.4f);
    }

    bool primaryButton(const char* label, float width) {
        ImGui::PushStyleColor(ImGuiCol_Button, kViolet);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.62f, 0.46f, 0.97f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.49f, 0.30f, 0.86f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
        ImGui::PopStyleColor(4);
        return pressed;
    }

    void frame() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##setup", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        drawBrand(vp->WorkSize);
        drawContent(vp->WorkSize);
        ImGui::End();
        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(kBg.x, kBg.y, kBg.z, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);

        // The work finished on the worker thread.
        if (page_ == Page::Working && !working_) page_ = ok_ ? Page::Done : Page::Failed;
    }

    // Left: the gradient panel with the logo.
    void drawBrand(const ImVec2& size) {
        const float w = size.x * 0.36f;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetWindowPos();
        const ImU32 a = ImGui::GetColorU32(ImVec4(0.27f, 0.16f, 0.62f, 1.0f)), b = ImGui::GetColorU32(ImVec4(0.05f, 0.42f, 0.55f, 1.0f));
        const ImU32 c = ImGui::GetColorU32(ImVec4(0.12f, 0.08f, 0.30f, 1.0f)), d = ImGui::GetColorU32(ImVec4(0.04f, 0.20f, 0.30f, 1.0f));
        draw->AddRectFilledMultiColor(o, ImVec2(o.x + w, o.y + size.y), a, b, d, c);
        // Soft rings for depth.
        for (int k = 0; k < 4; ++k)
            draw->AddCircle(ImVec2(o.x + w * 0.85f, o.y + size.y * 0.92f), size.y * (0.25f + k * 0.12f), IM_COL32(255, 255, 255, 14 - k * 3), 64, 1.5f * scale_);
        const float logo = 96.0f * scale_;
        const ImVec2 lp(o.x + (w - logo) * 0.5f, o.y + size.y * 0.24f);
        if (logo_) draw->AddImageRounded(static_cast<ImTextureID>(logo_), lp, ImVec2(lp.x + logo, lp.y + logo), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 20.0f * scale_);
        auto centered = [&](ImFont* font, float px, float y, const char* text, ImU32 color) {
            const ImVec2 t = font->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
            draw->AddText(font, px, ImVec2(o.x + (w - t.x) * 0.5f, y), color, text);
        };
        const float y = lp.y + logo + 22.0f * scale_;
        centered(heading_, 34.0f * scale_, y, "RIFT", IM_COL32(255, 255, 255, 255));
        centered(body_, 15.0f * scale_, y + 44.0f * scale_, "Engine " RIFT_VERSION_STRING, IM_COL32(255, 255, 255, 190));
        centered(body_, 13.5f * scale_, o.y + size.y - 40.0f * scale_, "Make Roblox-style games with Lua", IM_COL32(255, 255, 255, 150));
    }

    void drawContent(const ImVec2& size) {
        const float left = size.x * 0.36f;
        const float pad = 34.0f * scale_;
        ImGui::SetCursorPos(ImVec2(left + pad, pad));
        ImGui::BeginChild("##content", ImVec2(size.x - left - pad * 2.0f, size.y - pad * 2.0f));
        const float width = ImGui::GetContentRegionAvail().x;
        const float bottom = ImGui::GetContentRegionAvail().y;
        switch (page_) {
            case Page::Options: uninstalling_ ? drawUninstallOptions(width, bottom) : drawInstallOptions(width, bottom); break;
            case Page::Working: drawWorking(width); break;
            case Page::Done: drawDone(width, bottom); break;
            case Page::Failed: drawFailed(width, bottom); break;
        }
        ImGui::EndChild();
    }

    void title(const char* text) {
        ImGui::PushFont(heading_, 0.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
    }

    // Buttons at the bottom right: secondary (may be null), primary.
    int footer(float width, float bottom, const char* secondary, const char* primary, bool primaryEnabled = true) {
        const float bw = 120.0f * scale_;
        ImGui::SetCursorPos(ImVec2(width - (secondary ? bw * 2.0f + ImGui::GetStyle().ItemSpacing.x : bw), bottom - ImGui::GetFrameHeight()));
        int pressed = 0;
        if (secondary) {
            if (ImGui::Button(secondary, ImVec2(bw, 0.0f))) pressed = 1;
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!primaryEnabled);
        if (primaryButton(primary, bw)) pressed = 2;
        ImGui::EndDisabled();
        return pressed;
    }

    void drawInstallOptions(float width, float bottom) {
        const bool updating = installed_.found;
        title(updating ? "Update RIFT" : "Install RIFT");
        ImGui::PushTextWrapPos(width);
        if (updating) ImGui::TextDisabled("RIFT %s is installed. This replaces it with %s; your projects and settings stay as they are.",
                                          installed_.version.c_str(), RIFT_VERSION_STRING);
        else ImGui::TextDisabled("The editor, the player for your games, examples and documentation. RIFT installs for you only, so no administrator rights are needed.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0.0f, 8.0f * scale_));

        ImGui::TextUnformatted("Install location");
        ImGui::SetNextItemWidth(width - 100.0f * scale_ - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##dir", &dir_);
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(100.0f * scale_, 0.0f))) {
            const std::string picked = Platform::pickFolderDialog("Where should RIFT go?", dir_);
            if (!picked.empty()) {
                fs::path p = fromUtf8(picked);
                if (p.filename() != L"RIFT") p /= L"RIFT"; // a folder of its own
                dir_ = toUtf8(p);
            }
        }
        const std::uint64_t need = payload_.totalSize();
        const std::uint64_t available = freeSpace(fromUtf8(dir_));
        const bool enough = available == 0 || available > need;
        if (enough) ImGui::TextDisabled("Needs %s  -  %s free", megabytes(need).c_str(), available ? megabytes(available).c_str() : "?");
        else ImGui::TextColored(kRed, "Not enough space: needs %s, %s free", megabytes(need).c_str(), megabytes(available).c_str());
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale_));
        ImGui::Checkbox("Desktop shortcut", &desktop_);
        ImGui::Checkbox("Start menu shortcut", &startMenu_);
        ImGui::Checkbox("Start RIFT when setup is done", &launchAfter_);

        const int pressed = footer(width, bottom, "Cancel", updating ? "Update" : "Install", !dir_.empty() && enough);
        if (pressed == 1) quit_ = true;
        if (pressed == 2) startInstall();
    }

    void drawUninstallOptions(float width, float bottom) {
        title("Uninstall RIFT");
        ImGui::PushTextWrapPos(width);
        ImGui::TextDisabled("Removes RIFT %s from", installed_.found ? installed_.version.c_str() : "");
        ImGui::TextUnformatted(toUtf8(installedDir()).c_str());
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale_));
        ImGui::TextColored(kGreen, icons_ ? ICON_FA_CIRCLE_CHECK "  Your projects are not touched." : "Your projects are not touched.");
        ImGui::TextDisabled("Games you built and projects in your projects folder stay where they are.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f * scale_));
        ImGui::Checkbox("Also delete my RIFT settings and logs", &removeSettings_);
        ImGui::PopTextWrapPos();
        const int pressed = footer(width, bottom, "Cancel", "Uninstall");
        if (pressed == 1) quit_ = true;
        if (pressed == 2) startUninstall();
    }

    void drawWorking(float width) {
        title(uninstalling_ ? "Removing RIFT..." : installed_.found ? "Updating RIFT..." : "Installing RIFT...");
        ImGui::Dummy(ImVec2(0.0f, 24.0f * scale_));
        const float progress = total_ ? static_cast<float>(static_cast<double>(done_) / static_cast<double>(total_)) : 0.0f;
        ImGui::ProgressBar(progress, ImVec2(width, 10.0f * scale_), "");
        std::string current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = current_;
        }
        ImGui::TextDisabled("%s", current.c_str());
    }

    void drawDone(float width, float bottom) {
        ImGui::Dummy(ImVec2(0.0f, 30.0f * scale_));
        if (icons_) {
            ImGui::PushFont(heading_, 46.0f * scale_);
            ImGui::TextColored(kGreen, ICON_FA_CIRCLE_CHECK);
            ImGui::PopFont();
        }
        title(uninstalling_ ? "RIFT was removed" : "RIFT is ready");
        ImGui::PushTextWrapPos(width);
        if (uninstalling_) ImGui::TextDisabled("Thanks for trying RIFT. Your projects are still where they were.");
        else ImGui::TextDisabled("Find it in the Start menu%s. RIFT keeps itself up to date: it tells you when a new version is out.", desktop_ ? " and on the desktop" : "");
        ImGui::PopTextWrapPos();
        if (uninstalling_) {
            if (footer(width, bottom, nullptr, "Close") == 2) quit_ = true;
            return;
        }
        const int pressed = footer(width, bottom, "Close", "Start RIFT");
        if (pressed == 1) quit_ = true;
        if (pressed == 2) {
            launchEditor(fromUtf8(dir_));
            quit_ = true;
        }
    }

    void drawFailed(float width, float bottom) {
        ImGui::Dummy(ImVec2(0.0f, 30.0f * scale_));
        if (icons_) {
            ImGui::PushFont(heading_, 46.0f * scale_);
            ImGui::TextColored(kRed, ICON_FA_CIRCLE_XMARK);
            ImGui::PopFont();
        }
        title("Something went wrong");
        std::string error;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            error = error_;
        }
        ImGui::PushTextWrapPos(width);
        ImGui::TextUnformatted(error.c_str());
        ImGui::PopTextWrapPos();
        const int pressed = footer(width, bottom, "Close", "Try again");
        if (pressed == 1) quit_ = true;
        if (pressed == 2) page_ = Page::Options;
    }

    void progress(const std::string& file, std::uint64_t done, std::uint64_t total) {
        done_ = done;
        total_ = std::max<std::uint64_t>(total, 1);
        std::lock_guard<std::mutex> lock(mutex_);
        current_ = file;
    }

    void startInstall() {
        page_ = Page::Working;
        working_ = true;
        Options options;
        options.dir = fromUtf8(dir_);
        options.desktopShortcut = desktop_;
        options.startMenuShortcut = startMenu_;
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, options] {
            OleInitialize(nullptr); // shortcuts are COM objects
            std::string error;
            const bool ok = install(payload_, selfPath(), options, [this](const std::string& f, std::uint64_t d, std::uint64_t t) { progress(f, d, t); }, &error);
            OleUninitialize();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = error;
            }
            ok_ = ok;
            if (ok && launchAfter_) {
                launchEditor(options.dir);
                quit_ = true;
            }
            working_ = false;
            glfwPostEmptyEvent();
        });
    }

    void startUninstall() {
        page_ = Page::Working;
        working_ = true;
        const fs::path dir = installedDir();
        const bool removeSettings = removeSettings_;
        if (worker_.joinable()) worker_.join();
        worker_ = std::thread([this, dir, removeSettings] {
            std::string error;
            const bool ok = uninstall(dir, removeSettings, [this](const std::string& f, std::uint64_t d, std::uint64_t t) { progress(f, d, t); }, &error);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                error_ = error;
            }
            ok_ = ok;
            working_ = false;
            glfwPostEmptyEvent();
        });
    }

    // No OpenGL (a very old driver or a remote session): plain Windows dialogs.
    int fallback() {
        if (uninstalling_) {
            if (MessageBoxW(nullptr, L"Remove RIFT from this computer? Your projects are not touched.", L"Uninstall RIFT", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
            std::string error;
            const bool ok = uninstall(installedDir(), false, nullptr, &error);
            MessageBoxW(nullptr, ok ? L"RIFT was removed." : fromUtf8(error).c_str(), L"Uninstall RIFT", ok ? MB_ICONINFORMATION : MB_ICONERROR);
            return ok ? 0 : 1;
        }
        installed_ = findInstalled();
        const fs::path dir = installed_.found ? installed_.dir : defaultInstallDir();
        const std::wstring question = L"Install RIFT " + fromUtf8(RIFT_VERSION_STRING).wstring() + L" to\n" + dir.wstring() + L" ?";
        if (MessageBoxW(nullptr, question.c_str(), L"RIFT Setup", MB_YESNO | MB_ICONQUESTION) != IDYES) return 1;
        Options options;
        options.dir = dir;
        std::string error;
        const bool ok = install(payload_, selfPath(), options, nullptr, &error);
        MessageBoxW(nullptr, ok ? L"RIFT is installed. Find it in the Start menu." : fromUtf8(error).c_str(), L"RIFT Setup", ok ? MB_ICONINFORMATION : MB_ICONERROR);
        return ok ? 0 : 1;
    }

    // (no copy)
    SetupApp(const SetupApp&) = delete;
    SetupApp& operator=(const SetupApp&) = delete;
};

int runSetup() {
    OleInitialize(nullptr);
    Args args = parseArgs();
    const fs::path self = selfPath();
    Payload payload;
    const bool hasPayload = payload.open(self);
    const bool uninstalling = args.uninstall || !hasPayload;
    int result = 0;

    if (uninstalling) {
        if (args.dir.empty()) args.dir = self.parent_path();
        // Uninstall.exe cannot delete itself while it runs: a temporary copy does the work.
        if (args.deleteWhenDone.empty()) {
            const fs::path temp = fs::temp_directory_path() / (L"RIFT-Uninstall-" + std::to_wstring(GetCurrentProcessId()) + L".exe");
            std::error_code ec;
            fs::copy_file(self, temp, fs::copy_options::overwrite_existing, ec);
            if (!ec) {
                std::wstring params = L"/uninstall /dir \"" + args.dir.wstring() + L"\" /cleanup \"" + temp.wstring() + L"\" /wait " + std::to_wstring(GetCurrentProcessId());
                if (args.silent) params += L" /silent";
                ShellExecuteW(nullptr, L"open", temp.c_str(), params.c_str(), nullptr, SW_SHOWNORMAL);
                OleUninitialize();
                return 0;
            }
        }
        waitForProcess(args.waitPid, 15000); // Uninstall.exe itself must be gone before it is deleted
        if (args.silent) {
            std::string error;
            if (!uninstall(args.dir, false, nullptr, &error)) result = 1;
        } else {
            SetupApp app(args, payload, true);
            result = app.run();
        }
        if (!args.deleteWhenDone.empty()) deleteSelfLater(args.deleteWhenDone);
        OleUninitialize();
        return result;
    }

    if (args.silent) {
        // The editor's updater: wait for it to close, update, start it again.
        waitForProcess(args.waitPid, 60000);
        Options options;
        const Installed installed = findInstalled();
        options.dir = !args.dir.empty() ? args.dir : installed.found ? installed.dir : defaultInstallDir();
        // An update keeps the shortcuts the user had.
        if (installed.found) {
            options.desktopShortcut = hasDesktopShortcut();
            options.startMenuShortcut = hasStartMenuShortcut();
        }
        std::string error;
        if (!install(payload, self, options, nullptr, &error)) {
            const std::wstring text = L"RIFT could not be updated:\n\n" + fromUtf8(error).wstring();
            MessageBoxW(nullptr, text.c_str(), L"RIFT Setup", MB_ICONERROR);
            result = 1;
        }
        if (args.launch) launchEditor(options.dir);
        OleUninitialize();
        return result;
    }

    SetupApp app(args, payload, false);
    result = app.run();
    OleUninitialize();
    return result;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return runSetup();
}

int main() {
    return runSetup();
}
