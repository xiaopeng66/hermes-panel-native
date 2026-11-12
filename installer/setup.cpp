// Hermes 模型面板 安装程序：把内置的主程序/卸载器释放到目标目录，创建快捷方式、开机自启项
// 与「应用和功能」里的卸载登记。支持 GUI 与静默两种模式（--silent，供脚本/我自己验证）。
#include "common.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace setup;

namespace {

constexpr int kWindowWidth = 620;
constexpr int kWindowHeight = 350;

enum ControlId {
    kIdPath = 1001,
    kIdBrowse,
    kIdAutoStart,
    kIdDesktop,
    kIdStartMenu,
    kIdStatus,
    kIdNote
};

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HWND g_path = nullptr;
HWND g_browse = nullptr;
HWND g_autoStart = nullptr;
HWND g_desktop = nullptr;
HWND g_startMenu = nullptr;
HWND g_status = nullptr;
HWND g_note = nullptr;
HWND g_install = nullptr;
HWND g_cancel = nullptr;
HFONT g_font = nullptr;
HFONT g_titleFont = nullptr;
int g_dpi = 96;

int Scale(int value) { return MulDiv(value, g_dpi, 96); }

void ApplyFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

std::wstring ControlText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return {};
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetStatus(const std::wstring& text) {
    if (g_status) SetWindowTextW(g_status, text.c_str());
}

void SetControlsEnabled(bool enabled) {
    for (HWND control : {g_path, g_browse, g_autoStart, g_desktop, g_startMenu, g_install, g_cancel}) {
        if (control) EnableWindow(control, enabled ? TRUE : FALSE);
    }
}

std::wstring NormalizeDirectory(std::wstring directory) {
    for (auto& character : directory) {
        if (character == L'/') character = L'\\';
    }
    return TrimTrailingSeparators(directory);
}

std::wstring VersionText() {
#ifdef HERMES_VERSION
    return HERMES_VERSION;
#else
    return L"1.0.0";
#endif
}

std::wstring UninstallStringFor(const std::wstring& directory) {
    return L"\"" + JoinPath(directory, kUninstallerExe) + L"\"";
}

bool InstallNow(const InstallOptions& rawOptions, std::wstring& details) {
    InstallOptions options = rawOptions;
    options.directory = NormalizeDirectory(options.directory);
    std::wstring error;
    if (!ValidateInstallDirectory(options.directory, error)) {
        details = error;
        return false;
    }
    const auto oldDirectories = RunningAppDirectories();
    if (!CloseRunningApp(4000, true, error)) {
        details = error;
        return false;
    }
    if (!EnsureDirectory(options.directory)) {
        details = L"无法创建安装目录 " + options.directory + L": "
            + FormatWin32Error(GetLastError());
        return false;
    }
    Payload payload;
    if (!LoadPayload(payload, error)) {
        details = error;
        return false;
    }
    const auto appPath = JoinPath(options.directory, kAppExe);
    const auto temporary = appPath + L".new";
    if (!WriteFileBytes(temporary, payload.app, error)) {
        details = error;
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), appPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        details = L"无法写入主程序 " + appPath + L": " + FormatWin32Error(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    const auto uninstallerPath = JoinPath(options.directory, kUninstallerExe);
    if (!WriteFileBytes(uninstallerPath, payload.uninstaller, error)) {
        details = error;
        return false;
    }
    if (!payload.doc.empty() && !WriteFileBytes(JoinPath(options.directory, kDocFile), payload.doc,
        error)) {
        details = error;
        return false;
    }
    // 旧便携版的设置/档案：只在目标还没有 settings.json 时才导入，绝不覆盖已有数据。
    std::wstring migrationNote;
    if (!options.importDataDirectory.empty()) {
        MigrateSettings(options.directory, options.importDataDirectory, migrationNote);
    }
    const auto desktop = DesktopDirectory();
    const auto programs = StartMenuProgramsDirectory();
    const auto desktopLink = JoinPath(desktop, std::wstring(kShortcutName) + L".lnk");
    const auto menuFolder = JoinPath(programs, kShortcutFolder);
    const auto menuLink = JoinPath(menuFolder, std::wstring(kShortcutName) + L".lnk");
    const auto uninstallLink = JoinPath(menuFolder,
        std::wstring(kUninstallShortcutName) + L".lnk");
    if (options.desktopShortcut) {
        if (!CreateShortcut(desktopLink, appPath, L"", options.directory, kAppName, appPath,
            error)) {
            details = error;
            return false;
        }
    } else {
        DeleteFileW(desktopLink.c_str());
    }
    if (options.startMenuShortcut) {
        if (!EnsureDirectory(menuFolder)) {
            details = L"无法创建开始菜单文件夹 " + menuFolder;
            return false;
        }
        if (!CreateShortcut(menuLink, appPath, L"", options.directory, kAppName, appPath, error)
            || !CreateShortcut(uninstallLink, uninstallerPath, L"", options.directory,
                kUninstallShortcutName, appPath, error)) {
            details = error;
            return false;
        }
    } else {
        std::wstring ignored;
        RemoveDirectoryTree(menuFolder, ignored);
    }
    const std::wstring runCommand = L"\"" + appPath + L"\" --startup";
    if (options.autoStart) {
        if (!SetRegistryString(HKEY_CURRENT_USER, kRunKey, kRunValue, runCommand.c_str(), error)) {
            details = error;
            return false;
        }
    } else {
        DeleteRegistryValue(HKEY_CURRENT_USER, kRunKey, kRunValue);
    }
    const auto version = VersionText();
    const std::wstring estimated = std::to_wstring(
        (payload.app.size() + payload.uninstaller.size() + payload.doc.size()) / 1024);
    const std::wstring uninstallKey = kUninstallKey;
    const auto setValue = [&](const wchar_t* name, const std::wstring& value) -> bool {
        return SetRegistryString(HKEY_CURRENT_USER, uninstallKey.c_str(), name, value.c_str(),
            error);
    };
    if (!setValue(L"DisplayName", kAppName)
        || !setValue(L"DisplayVersion", version)
        || !setValue(L"DisplayIcon", appPath + L",0")
        || !setValue(L"InstallLocation", options.directory)
        || !setValue(L"UninstallString", UninstallStringFor(options.directory))
        || !setValue(L"QuietUninstallString", UninstallStringFor(options.directory) + L" --silent")
        || !setValue(L"EstimatedSize", estimated)
        || !SetRegistryDword(HKEY_CURRENT_USER, uninstallKey.c_str(), L"NoModify", 1, error)
        || !SetRegistryDword(HKEY_CURRENT_USER, uninstallKey.c_str(), L"NoRepair", 1, error)) {
        details = error;
        return false;
    }
    details = L"安装位置：" + options.directory + L"\r\n";
    details += options.autoStart
        ? L"开机自启：已开启（登录后启动到托盘）\r\n"
        : L"开机自启：未开启（可在托盘菜单或设置页打开）\r\n";
    details += std::wstring(L"桌面快捷方式：")
        + (options.desktopShortcut ? L"已创建" : L"未创建") + L"\r\n";
    details += std::wstring(L"开始菜单：")
        + (options.startMenuShortcut ? L"已创建" : L"未创建") + L"\r\n";
    if (!migrationNote.empty()) details += migrationNote + L"\r\n";
    LogLine(L"installed to " + options.directory + L" (autostart="
        + (options.autoStart ? L"on" : L"off") + L", payload="
        + (payload.bundled ? L"bundled" : L"sibling") + L")");
    return true;
}

void BrowseForFolder(HWND owner) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"选择 Hermes 模型面板的安装文件夹";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&info);
    if (!item) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(item, path)) SetWindowTextW(g_path, path);
    CoTaskMemFree(item);
}

void StartInstall(HWND owner) {
    InstallOptions options;
    options.directory = NormalizeDirectory(ControlText(g_path));
    options.autoStart = IsDlgButtonChecked(owner, kIdAutoStart) == BST_CHECKED;
    options.desktopShortcut = IsDlgButtonChecked(owner, kIdDesktop) == BST_CHECKED;
    options.startMenuShortcut = IsDlgButtonChecked(owner, kIdStartMenu) == BST_CHECKED;
    SetControlsEnabled(false);
    SetStatus(L"正在安装…");
    UpdateWindow(owner);
    std::wstring details;
    if (!InstallNow(options, details)) {
        SetStatus(L"安装失败");
        MessageBoxW(owner, details.c_str(), L"安装失败", MB_OK | MB_ICONERROR);
        SetControlsEnabled(true);
        return;
    }
    SetStatus(L"安装完成");
    const std::wstring question = details + L"\r\n是否立即启动？";
    if (MessageBoxW(owner, question.c_str(), L"安装完成", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
        const auto appPath = JoinPath(options.directory, kAppExe);
        ShellExecuteW(nullptr, L"open", appPath.c_str(), nullptr, options.directory.c_str(),
            SW_SHOWNORMAL);
    }
    DestroyWindow(owner);
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
        const int buttonWidth = Scale(104);
        const int buttonHeight = Scale(30);
        const int browseWidth = Scale(78);
        const int editHeight = Scale(26);
        HWND title = CreateWindowExW(0, L"STATIC", L"Hermes 模型面板",
            WS_CHILD | WS_VISIBLE, margin, Scale(16), width - margin * 2, Scale(30), window,
            nullptr, g_instance, nullptr);
        HWND intro = CreateWindowExW(0, L"STATIC", L"选择安装位置，然后点「安装」。",
            WS_CHILD | WS_VISIBLE, margin, Scale(56), width - margin * 2, Scale(20), window,
            nullptr, g_instance, nullptr);
        g_path = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", kDefaultDirectory,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            margin, Scale(84), width - margin * 2 - browseWidth - Scale(8), editHeight, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPath)), g_instance, nullptr);
        g_browse = CreateWindowExW(0, L"BUTTON", L"浏览…",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            width - margin - browseWidth, Scale(84), browseWidth, editHeight, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdBrowse)), g_instance, nullptr);
        g_autoStart = CreateWindowExW(0, L"BUTTON", L"开机自动启动（登录后常驻托盘）",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin, Scale(126), width - margin * 2, Scale(24), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdAutoStart)), g_instance, nullptr);
        g_desktop = CreateWindowExW(0, L"BUTTON", L"创建桌面快捷方式",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin, Scale(152), width - margin * 2, Scale(24), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdDesktop)), g_instance, nullptr);
        g_startMenu = CreateWindowExW(0, L"BUTTON", L"创建开始菜单快捷方式",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            margin, Scale(178), width - margin * 2, Scale(24), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStartMenu)), g_instance, nullptr);
        g_note = CreateWindowExW(0, L"STATIC",
            L"安装不会删除目标文件夹里已有的 data 设置；卸载时可选择保留。",
            WS_CHILD | WS_VISIBLE, margin, Scale(210), width - margin * 2, Scale(40), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNote)), g_instance, nullptr);
        g_status = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE, margin, Scale(252), width - margin * 2, Scale(20), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStatus)), g_instance, nullptr);
        g_cancel = CreateWindowExW(0, L"BUTTON", L"取消",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            width - margin - Scale(84), height - Scale(50), Scale(84), buttonHeight, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), g_instance, nullptr);
        g_install = CreateWindowExW(0, L"BUTTON", L"安装",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            width - margin - Scale(84) - Scale(10) - buttonWidth, height - Scale(50),
            buttonWidth, buttonHeight, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_instance, nullptr);
        for (HWND control : {title, intro, g_path, g_browse, g_autoStart, g_desktop, g_startMenu,
                 g_note, g_status, g_install, g_cancel}) {
            ApplyFont(control, g_font);
        }
        ApplyFont(title, g_titleFont);
        CheckDlgButton(window, kIdAutoStart, BST_CHECKED);
        CheckDlgButton(window, kIdDesktop, BST_CHECKED);
        CheckDlgButton(window, kIdStartMenu, BST_CHECKED);
        SetFocus(g_path);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == kIdBrowse) {
            BrowseForFolder(window);
            return 0;
        }
        if (id == IDOK) {
            StartInstall(window);
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
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

struct Arguments {
    bool silent = false;
    InstallOptions options;
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
    arguments.options.directory = kDefaultDirectory;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        if (argument == L"--silent" || argument == L"/S" || argument == L"/s") {
            arguments.silent = true;
        } else if (StartsWithNoCase(argument, L"--dir=")) {
            arguments.options.directory = StripQuotes(argument.substr(6));
        } else if (StartsWithNoCase(argument, L"/DIR=")) {
            arguments.options.directory = StripQuotes(argument.substr(5));
        } else if (argument == L"--no-autostart") {
            arguments.options.autoStart = false;
        } else if (argument == L"--no-desktop") {
            arguments.options.desktopShortcut = false;
        } else if (argument == L"--no-startmenu") {
            arguments.options.startMenuShortcut = false;
        } else if (argument == L"--launch") {
            arguments.options.launch = true;
        } else if (StartsWithNoCase(argument, L"--import-data=")) {
            arguments.options.importDataDirectory = StripQuotes(argument.substr(14));
        }
    }
    arguments.options.silent = arguments.silent;
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
    if (arguments.silent) {
        LogLine(L"setup: silent install dir=" + arguments.options.directory);
        std::wstring details;
        if (!InstallNow(arguments.options, details)) {
            LogLine(L"setup: failed: " + details);
            if (SUCCEEDED(com)) CoUninitialize();
            return 1;
        }
        LogLine(L"setup: ok: " + details);
        if (arguments.options.launch) {
            const auto appPath = JoinPath(NormalizeDirectory(arguments.options.directory), kAppExe);
            ShellExecuteW(nullptr, L"open", appPath.c_str(), nullptr,
                arguments.options.directory.c_str(), SW_SHOWNORMAL);
        }
        if (SUCCEEDED(com)) CoUninitialize();
        return 0;
    }
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_DBLCLKS;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!windowClass.hIcon) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = L"HermesModelPanelSetup";
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
    g_window = CreateWindowExW(WS_EX_CONTROLPARENT, windowClass.lpszClassName, kAppName,
        style, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 3;
    }
    ShowWindow(g_window, SW_SHOWDEFAULT);
    UpdateWindow(g_window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (g_font) DeleteObject(g_font);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
