// Hermes 模型面板 卸载程序：关闭面板 → 清快捷方式/自启/Run 键/卸载登记 → 删文件。
// 因为不能删除正在运行的自己，先把自身复制到 %TEMP% 再启动副本，由副本完成真正的卸载。
#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace setup;

namespace {

constexpr int kWindowWidth = 560;
constexpr int kWindowHeight = 268;

enum ControlId {
    kIdPath = 2001,
    kIdKeepData,
    kIdStatus
};

HINSTANCE g_instance = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
HWND g_path = nullptr;
HWND g_keepData = nullptr;
HWND g_status = nullptr;
HWND g_confirm = nullptr;
HWND g_cancel = nullptr;
int g_dpi = 96;
std::wstring g_directory;
bool g_selfDeleteScheduled = false;

int Scale(int value) { return MulDiv(value, g_dpi, 96); }

void ApplyFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void SetControlsEnabled(bool enabled) {
    for (HWND control : {g_keepData, g_confirm, g_cancel}) {
        if (control) EnableWindow(control, enabled ? TRUE : FALSE);
    }
}

std::wstring NormalizeDirectory(std::wstring directory) {
    for (auto& character : directory) {
        if (character == L'/') character = L'\\';
    }
    return TrimTrailingSeparators(directory);
}

void ScheduleSelfDelete() {
    if (g_selfDeleteScheduled) return;
    g_selfDeleteScheduled = true;
    std::wstring self = ModulePath();
    if (self.empty()) return;
    wchar_t comspec[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH) == 0) {
        wcscpy_s(comspec, L"C:\\Windows\\System32\\cmd.exe");
    }
    const std::wstring command = L"/c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self + L"\"";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(comspec, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        DirectoryName(self).c_str(), &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

bool UninstallNow(const std::wstring& rawDirectory, bool keepData, std::wstring& details) {
    const auto directory = NormalizeDirectory(rawDirectory);
    std::wstring error;
    if (!CloseRunningApp(4000, true, error)) {
        details = error;
        return false;
    }
    DeleteRegistryValue(HKEY_CURRENT_USER, kRunKey, kRunValue);
    const auto desktop = DesktopDirectory();
    const auto programs = StartMenuProgramsDirectory();
    DeleteFileW(JoinPath(desktop, std::wstring(kShortcutName) + L".lnk").c_str());
    RemoveDirectoryTree(JoinPath(programs, kShortcutFolder), error);
    DeleteRegistryTree(HKEY_CURRENT_USER, kUninstallKey);
    if (keepData) {
        // 逐项删除，跳过 data：设置/档案/日志留给用户，重装后直接接着用。
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..") continue;
                if (_wcsicmp(name.c_str(), kDataDir) == 0) continue;
                const auto child = JoinPath(directory, name);
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    RemoveDirectoryTree(child, error);
                } else {
                    DeleteFileIfExists(child, error);
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        details = L"已卸载，设置与日志保留在：" + JoinPath(directory, kDataDir);
    } else {
        if (!RemoveDirectoryTree(directory, error)) {
            details = L"卸载未完全完成：" + error;
            return false;
        }
        details = L"已卸载，并删除了安装目录与全部数据。";
    }
    LogLine(L"uninstalled from " + directory + (keepData ? L" (data kept)" : L" (data purged)"));
    return true;
}

bool RelaunchFromTemp(const std::wstring& directory, bool silent, bool purge,
    std::wstring& error) {
    const auto self = ModulePath();
    if (self.empty()) {
        error = L"无法获取卸载程序路径";
        return false;
    }
    wchar_t temporary[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, temporary) == 0) {
        error = L"无法获取临时目录";
        return false;
    }
    const auto copy = std::wstring(temporary) + L"HermesModelPanelNative-uninstall-"
        + std::to_wstring(GetCurrentProcessId()) + L".exe";
    DeleteFileW(copy.c_str());
    if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
        error = L"无法复制卸载程序到临时目录: " + FormatWin32Error(GetLastError());
        return false;
    }
    std::wstring command = L"\"" + copy + L"\" --from-temp --origin=\"" + directory
        + L"\" --wait-pid=" + std::to_wstring(GetCurrentProcessId());
    if (silent) command += L" --silent";
    if (purge) command += L" --purge";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(copy.c_str(), buffer.data(), nullptr, nullptr, FALSE, 0, nullptr,
        DirectoryName(copy).c_str(), &startup, &process)) {
        error = L"无法启动卸载程序: " + FormatWin32Error(GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void StartUninstall(HWND window) {
    const bool keepData = IsDlgButtonChecked(window, kIdKeepData) == BST_CHECKED;
    const std::wstring prompt = keepData
        ? L"确定要卸载 Hermes 模型面板吗？\r\n\r\n设置与日志（data 文件夹）会保留。"
        : L"确定要卸载 Hermes 模型面板吗？\r\n\r\n安装目录和全部数据都会被删除。";
    if (MessageBoxW(window, prompt.c_str(), L"卸载 Hermes 模型面板",
        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    SetControlsEnabled(false);
    SetStatus(L"正在卸载…");
    UpdateWindow(window);
    std::wstring details;
    if (!UninstallNow(g_directory, keepData, details)) {
        SetStatus(L"卸载未完成");
        MessageBoxW(window, details.c_str(), L"卸载失败", MB_OK | MB_ICONERROR);
        SetControlsEnabled(true);
        return;
    }
    SetStatus(L"卸载完成");
    MessageBoxW(window, details.c_str(), L"卸载完成", MB_OK | MB_ICONINFORMATION);
    ScheduleSelfDelete();
    DestroyWindow(window);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE: {
        g_dpi = GetDpiForWindow(window);
        if (g_dpi <= 0) g_dpi = 96;
        g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        g_titleFont = CreateFontW(-MulDiv(16, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Segoe UI");
        const int margin = Scale(22);
        const int width = Scale(kWindowWidth);
        const int height = Scale(kWindowHeight);
        HWND title = CreateWindowExW(0, L"STATIC", L"卸载 Hermes 模型面板",
            WS_CHILD | WS_VISIBLE, margin, Scale(16), width - margin * 2, Scale(30), window,
            nullptr, g_instance, nullptr);
        HWND intro = CreateWindowExW(0, L"STATIC", L"将从下面的位置卸载：",
            WS_CHILD | WS_VISIBLE, margin, Scale(56), width - margin * 2, Scale(20), window,
            nullptr, g_instance, nullptr);
        g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", g_directory.c_str(),
            WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
            margin, Scale(80), width - margin * 2, Scale(26), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPath)), g_instance, nullptr);
        g_keepData = CreateWindowExW(0, L"BUTTON", L"保留设置与日志（data 文件夹）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin, Scale(122), width - margin * 2, Scale(24), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdKeepData)), g_instance, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, margin, Scale(160), width - margin * 2, Scale(20), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), g_instance, nullptr);
        g_cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            width - margin - Scale(84), height - Scale(50), Scale(84), Scale(30), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), g_instance, nullptr);
        g_confirm = CreateWindowExW(0, L"BUTTON", L"卸载",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            width - margin - Scale(84) - Scale(10) - Scale(104), height - Scale(50), Scale(104),
            Scale(30), window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_instance,
            nullptr);
        for (HWND control : {title, intro, g_path, g_keepData, g_status, g_confirm, g_cancel}) {
            ApplyFont(control, g_font);
        }
        ApplyFont(title, g_titleFont);
        CheckDlgButton(window, kIdKeepData, BST_CHECKED);
        SetFocus(g_confirm);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDOK) {
            StartUninstall(window);
            return 0;
        }
        if (id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    }
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        // 取消卸载时临时副本也要自己收尾：稍后由 cmd 删除（进程还活着时删不掉自己）。
        ScheduleSelfDelete();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct Arguments {
    bool silent = false;
    bool purge = false;
    bool fromTemp = false;
    DWORD waitPid = 0;
    std::wstring origin;
};

std::wstring StripQuotes(std::wstring value) {
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

bool StartsWithNoCase(const std::wstring& text, const wchar_t* prefix) {
    return _wcsnicmp(text.c_str(), prefix, wcslen(prefix)) == 0;
}

Arguments ParseArguments(int argc, wchar_t** argv) {
    Arguments arguments;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--silent" || argument == L"/S" || argument == L"/s") {
            arguments.silent = true;
        } else if (argument == L"--purge") {
            arguments.purge = true;
        } else if (argument == L"--from-temp") {
            arguments.fromTemp = true;
        } else if (StartsWithNoCase(argument, L"--origin=")) {
            arguments.origin = StripQuotes(argument.substr(9));
        } else if (StartsWithNoCase(argument, L"--wait-pid=")) {
            arguments.waitPid = static_cast<DWORD>(_wtoi(argument.c_str() + 11));
        }
    }
    return arguments;
}

}   // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_instance = instance;
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const Arguments arguments = ParseArguments(argc, argv);
    if (argv) LocalFree(argv);
    g_directory = arguments.origin.empty() ? DirectoryName(ModulePath())
        : NormalizeDirectory(arguments.origin);
    if (!arguments.fromTemp) {
        // 先把自身搬到临时目录：副本运行时原文件已解锁，安装目录才能被整个删掉。
        std::wstring error;
        if (!RelaunchFromTemp(g_directory, arguments.silent, arguments.purge, error)) {
            LogLine(L"uninstall: relaunch failed: " + error);
            if (!arguments.silent) {
                MessageBoxW(nullptr, error.c_str(), L"卸载失败", MB_OK | MB_ICONERROR);
            }
            if (SUCCEEDED(com)) CoUninitialize();
            return 1;
        }
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    if (arguments.waitPid != 0) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, arguments.waitPid);
        if (process) {
            WaitForSingleObject(process, 8000);
            CloseHandle(process);
        }
    }
    if (arguments.silent) {
        std::wstring details;
        const bool ok = UninstallNow(g_directory, !arguments.purge, details);
        LogLine(L"uninstall: silent " + details);
        ScheduleSelfDelete();
        if (SUCCEEDED(com)) CoUninitialize();
        return ok ? 0 : 1;
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"HermesModelPanelUninstall";
    if (!RegisterClassExW(&windowClass)) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 2;
    }
    g_dpi = GetDpiForSystem();
    if (g_dpi <= 0) g_dpi = 96;
    RECT rect{0, 0, Scale(kWindowWidth), Scale(kWindowHeight)};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&rect, style, FALSE, WS_EX_CONTROLPARENT);
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int x = (GetSystemMetrics(SM_CXSCREEN) - width) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - height) / 2;
    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass.lpszClassName,
        L"卸载 Hermes 模型面板", style, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 3;
    }
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
