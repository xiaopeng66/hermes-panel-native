// 安装器/卸载器共用的基础设施：路径、文件、注册表、快捷方式、进程收尾、payload 读取。
// 两者都是原生 Win32（无运行时依赖），与主程序同一套构建产物；源码统一 UTF-8（/utf-8）。
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace setup {

inline constexpr wchar_t kAppName[] = L"Hermes 模型面板";
inline constexpr wchar_t kAppExe[] = L"HermesModelPanelNative.exe";
inline constexpr wchar_t kDevAppExe[] = L"hermes-panel-native.exe";
inline constexpr wchar_t kUninstallerExe[] = L"uninstall.exe";
inline constexpr wchar_t kDocFile[] = L"THIRD-PARTY.md";
inline constexpr wchar_t kDataDir[] = L"data";
inline constexpr wchar_t kRunValue[] = L"HermesModelPanelNative";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\HermesModelPanelNative";
inline constexpr wchar_t kShortcutFolder[] = L"Hermes 模型面板";
inline constexpr wchar_t kShortcutName[] = L"Hermes 模型面板";
inline constexpr wchar_t kUninstallShortcutName[] = L"卸载 Hermes 模型面板";
inline constexpr wchar_t kDefaultDirectory[] = L"D:\\自制小工具\\HermesModelPanelNative";
// 安装包尾部标记：H N P A Y L 2 \0（小端 uint64）。
inline constexpr unsigned long long kPayloadMagic = 0x00324C5941504E48ULL;
inline constexpr unsigned long long kPayloadFooterSize = 32;

struct Payload {
    std::vector<unsigned char> app;
    std::vector<unsigned char> uninstaller;
    std::vector<unsigned char> doc;
    bool bundled = true;   // false = 开发态（payload 就在 setup.exe 旁边）
};

struct InstallOptions {
    std::wstring directory;
    bool autoStart = true;
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
    bool launch = false;
    bool silent = false;
    std::wstring importDataDirectory;   // 可选：从旧便携版的 data 目录导入 settings/profiles
};

struct UninstallOptions {
    std::wstring directory;
    bool keepData = true;
    bool silent = false;
};

std::wstring ModulePath();
std::wstring DirectoryName(const std::wstring& path);
std::wstring JoinPath(const std::wstring& directory,const std::wstring& name);
std::wstring TrimTrailingSeparators(std::wstring path);
bool EnsureDirectory(const std::wstring& path);
bool FileExists(const std::wstring& path);
bool DirectoryExists(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path,std::vector<unsigned char>& bytes,std::wstring& error);
bool WriteFileBytes(const std::wstring& path,const std::vector<unsigned char>& bytes,std::wstring& error);
bool DeleteFileIfExists(const std::wstring& path,std::wstring& error);
bool RemoveDirectoryTree(const std::wstring& path,std::wstring& error);
std::wstring FormatWin32Error(DWORD code);
std::wstring FormatHResult(HRESULT result);
std::wstring ExpandEnvironment(const std::wstring& text);
std::wstring DesktopDirectory();
std::wstring StartMenuProgramsDirectory();
bool ValidateInstallDirectory(const std::wstring& directory,std::wstring& error);

bool LoadPayload(Payload& payload,std::wstring& error);
std::vector<std::wstring> RunningAppDirectories();
bool CloseRunningApp(unsigned timeoutMs,bool force,std::wstring& error);
bool MigrateSettings(const std::wstring& installDirectory,const std::wstring& sourceDataDirectory,
    std::wstring& note);

bool SetRegistryString(HKEY root,const wchar_t* subkey,const wchar_t* name,const wchar_t* value,
    std::wstring& error);
bool SetRegistryDword(HKEY root,const wchar_t* subkey,const wchar_t* name,DWORD value,
    std::wstring& error);
bool DeleteRegistryValue(HKEY root,const wchar_t* subkey,const wchar_t* name);
bool DeleteRegistryTree(HKEY root,const wchar_t* subkey);

bool CreateShortcut(const std::wstring& linkPath,const std::wstring& target,
    const std::wstring& arguments,const std::wstring& workingDirectory,
    const std::wstring& description,const std::wstring& iconPath,std::wstring& error);

void LogLine(const std::wstring& text);

}   // namespace setup
