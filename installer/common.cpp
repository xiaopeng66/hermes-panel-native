#include "common.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <objbase.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <tlhelp32.h>

namespace setup {
namespace {

bool DeleteFileWithRetry(const std::wstring& path) {
    for (int attempt = 0; attempt < 20; ++attempt) {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (DeleteFileW(path.c_str())) return true;
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        Sleep(100);
    }
    return false;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &path))) return {};
    std::wstring value = path ? path : L"";
    CoTaskMemFree(path);
    return value;
}

bool ReadRange(HANDLE file, unsigned long long offset, unsigned long long size,
    std::vector<unsigned char>& bytes) {
    if (size > 256ULL * 1024 * 1024) return false;
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
    bytes.resize(static_cast<std::size_t>(size));
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - total, 1u << 20));
        if (!ReadFile(file, bytes.data() + total, want, &chunk, nullptr) || chunk == 0) return false;
        total += chunk;
    }
    return true;
}

bool LoadSiblingPayload(Payload& payload, std::wstring& error) {
    const auto directory = DirectoryName(ModulePath());
    if (directory.empty()) {
        error = L"无法获取安装程序所在目录";
        return false;
    }
    const std::wstring apps[] = {JoinPath(directory, kAppExe), JoinPath(directory, kDevAppExe)};
    const std::wstring uninstallers[] = {JoinPath(directory, kUninstallerExe),
        JoinPath(directory, L"hermes-panel-uninstaller.exe")};
    std::wstring appPath;
    for (const auto& candidate : apps) {
        if (FileExists(candidate)) { appPath = candidate; break; }
    }
    if (appPath.empty()) {
        error = L"安装包里没有主程序，且旁边也找不到 " + std::wstring(kAppExe);
        return false;
    }
    std::wstring uninstallerPath;
    for (const auto& candidate : uninstallers) {
        if (FileExists(candidate)) { uninstallerPath = candidate; break; }
    }
    if (uninstallerPath.empty()) {
        error = L"安装包里没有卸载程序，且旁边也找不到 " + std::wstring(kUninstallerExe);
        return false;
    }
    if (!ReadFileBytes(appPath, payload.app, error)) return false;
    if (!ReadFileBytes(uninstallerPath, payload.uninstaller, error)) return false;
    const auto docPath = JoinPath(directory, kDocFile);
    if (FileExists(docPath) && !ReadFileBytes(docPath, payload.doc, error)) return false;
    payload.bundled = false;
    return true;
}

}   // namespace

std::wstring ModulePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size()) { buffer.resize(length); return buffer; }
        if (buffer.size() >= 32768) return {};
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DirectoryName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring{} : path.substr(0, slash);
}

std::wstring TrimTrailingSeparators(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return path;
}

std::wstring JoinPath(const std::wstring& directory, const std::wstring& name) {
    if (directory.empty()) return name;
    const wchar_t back = directory.back();
    if (back == L'\\' || back == L'/') return directory + name;
    return directory + L"\\" + name;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.size() < 3) return false;
    for (std::size_t index = 3; index <= path.size(); ++index) {
        if (index < path.size() && path[index] != L'\\' && path[index] != L'/') continue;
        CreateDirectoryW(path.substr(0, index).c_str(), nullptr);
    }
    CreateDirectoryW(path.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool ReadFileBytes(const std::wstring& path, std::vector<unsigned char>& bytes,
    std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法读取 " + path + L": " + FormatWin32Error(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        error = L"无法获取文件大小 " + path + L": " + FormatWin32Error(GetLastError());
        CloseHandle(file);
        return false;
    }
    std::size_t total = 0;
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    while (total < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - total, 1u << 20));
        if (!ReadFile(file, bytes.data() + total, want, &chunk, nullptr) || chunk == 0) {
            error = L"读取 " + path + L" 失败: " + FormatWin32Error(GetLastError());
            CloseHandle(file);
            return false;
        }
        total += chunk;
    }
    CloseHandle(file);
    return true;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<unsigned char>& bytes,
    std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法写入 " + path + L": " + FormatWin32Error(GetLastError());
        return false;
    }
    std::size_t total = 0;
    while (total < bytes.size()) {
        DWORD chunk = 0;
        const DWORD want = static_cast<DWORD>(
            std::min<std::size_t>(bytes.size() - total, 1u << 20));
        if (!WriteFile(file, bytes.data() + total, want, &chunk, nullptr) || chunk == 0) {
            error = L"写入 " + path + L" 失败: " + FormatWin32Error(GetLastError());
            CloseHandle(file);
            return false;
        }
        total += chunk;
    }
    FlushFileBuffers(file);
    CloseHandle(file);
    return true;
}

bool DeleteFileIfExists(const std::wstring& path, std::wstring& error) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (!DeleteFileWithRetry(path)) {
        error = L"无法删除 " + path + L": " + FormatWin32Error(GetLastError());
        return false;
    }
    return true;
}

bool RemoveDirectoryTree(const std::wstring& path, std::wstring& error) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return true;
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return DeleteFileIfExists(path, error);
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(JoinPath(path, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..") continue;
            const auto child = JoinPath(path, name);
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                if (!RemoveDirectoryTree(child, error)) { FindClose(find); return false; }
            } else if (!DeleteFileWithRetry(child)) {
                error = L"无法删除 " + child + L": " + FormatWin32Error(GetLastError());
                FindClose(find);
                return false;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (RemoveDirectoryW(path.c_str())) return true;
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) return true;
        Sleep(100);
    }
    error = L"无法删除目录 " + path + L": " + FormatWin32Error(GetLastError());
    return false;
}

std::wstring FormatWin32Error(DWORD code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring text = (length != 0 && message) ? std::wstring(message, length) : std::wstring{};
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) {
        text.pop_back();
    }
    wchar_t suffix[64]{};
    swprintf_s(suffix, L"（错误 %lu）", code);
    return (text.empty() ? std::wstring(L"系统错误") : text) + suffix;
}

std::wstring FormatHResult(HRESULT result) {
    wchar_t prefix[64]{};
    swprintf_s(prefix, L"HRESULT 0x%08lX", static_cast<unsigned long>(result));
    return std::wstring(prefix) + L": " + FormatWin32Error(static_cast<DWORD>(result));
}

std::wstring ExpandEnvironment(const std::wstring& text) {
    if (text.empty()) return {};
    const DWORD needed = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (needed == 0) return text;
    std::wstring result(needed, L'\0');
    const DWORD written = ExpandEnvironmentStringsW(text.c_str(), result.data(), needed);
    if (written == 0 || written > needed) return text;
    result.resize(written > 0 ? written - 1 : 0);
    return result;
}

std::wstring DesktopDirectory() { return KnownFolder(FOLDERID_Desktop); }

std::wstring StartMenuProgramsDirectory() { return KnownFolder(FOLDERID_Programs); }

bool ValidateInstallDirectory(const std::wstring& directory, std::wstring& error) {
    if (directory.empty()) {
        error = L"安装路径不能为空";
        return false;
    }
    if (directory.find(L'"') != std::wstring::npos) {
        error = L"安装路径不能包含引号";
        return false;
    }
    if (PathIsRelativeW(directory.c_str())) {
        error = L"请填写完整的绝对路径，例如 D:\\自制小工具\\HermesModelPanelNative";
        return false;
    }
    if (directory.size() <= 3 && directory.size() >= 2 && directory[1] == L':') {
        error = L"不能直接安装到盘符根目录，请选择下一级文件夹";
        return false;
    }
    const auto windows = ExpandEnvironment(L"%WINDIR%");
    if (!windows.empty() && _wcsnicmp(directory.c_str(), windows.c_str(), windows.size()) == 0) {
        error = L"不能安装到 Windows 系统目录";
        return false;
    }
    return true;
}

bool LoadPayload(Payload& payload, std::wstring& error) {
    const auto self = ModulePath();
    if (self.empty()) {
        error = L"无法获取安装程序路径";
        return false;
    }
    HANDLE file = CreateFileW(self.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"无法打开安装程序: " + FormatWin32Error(GetLastError());
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < static_cast<LONGLONG>(kPayloadFooterSize)) {
        CloseHandle(file);
        return LoadSiblingPayload(payload, error);
    }
    LARGE_INTEGER position{};
    position.QuadPart = size.QuadPart - static_cast<LONGLONG>(kPayloadFooterSize);
    if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return LoadSiblingPayload(payload, error);
    }
    unsigned char footer[kPayloadFooterSize]{};
    DWORD read = 0;
    if (!ReadFile(file, footer, static_cast<DWORD>(sizeof(footer)), &read, nullptr)
        || read != sizeof(footer)) {
        CloseHandle(file);
        return LoadSiblingPayload(payload, error);
    }
    unsigned long long magic = 0, appSize = 0, uninstallerSize = 0, docSize = 0;
    std::memcpy(&magic, footer, 8);
    std::memcpy(&appSize, footer + 8, 8);
    std::memcpy(&uninstallerSize, footer + 16, 8);
    std::memcpy(&docSize, footer + 24, 8);
    if (magic != kPayloadMagic || appSize == 0) {
        CloseHandle(file);
        return LoadSiblingPayload(payload, error);
    }
    const unsigned long long total = appSize + uninstallerSize + docSize;
    const unsigned long long fileSize = static_cast<unsigned long long>(size.QuadPart);
    if (total + kPayloadFooterSize > fileSize) {
        CloseHandle(file);
        error = L"安装包已损坏（payload 长度超出文件大小）";
        return false;
    }
    const unsigned long long start = fileSize - kPayloadFooterSize - total;
    if (!ReadRange(file, start, appSize, payload.app)
        || !ReadRange(file, start + appSize, uninstallerSize, payload.uninstaller)
        || !ReadRange(file, start + appSize + uninstallerSize, docSize, payload.doc)) {
        CloseHandle(file);
        error = L"读取安装包内容失败: " + FormatWin32Error(GetLastError());
        return false;
    }
    CloseHandle(file);
    payload.bundled = true;
    return true;
}

std::vector<std::wstring> RunningAppDirectories() {
    std::vector<std::wstring> directories;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return directories;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, kAppExe) != 0
                && _wcsicmp(entry.szExeFile, kDevAppExe) != 0) continue;
            if (entry.th32ProcessID == GetCurrentProcessId()) continue;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
            if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                const auto directory = DirectoryName(path);
                if (!directory.empty()) directories.push_back(directory);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return directories;
}

bool CloseRunningApp(unsigned timeoutMs, bool force, std::wstring& error) {
    std::vector<DWORD> pids;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (Process32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szExeFile, kAppExe) != 0
                    && _wcsicmp(entry.szExeFile, kDevAppExe) != 0) continue;
                if (entry.th32ProcessID == GetCurrentProcessId()) continue;
                pids.push_back(entry.th32ProcessID);
            } while (Process32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
    }
    if (pids.empty()) return true;
    // 正常路径：给控制窗口发 WM_CLOSE，让面板走自己的退出流程（会持久化设置）。
    HWND window = nullptr;
    while ((window = FindWindowExW(HWND_MESSAGE, window, L"HermesModelPanelNative.Control",
        nullptr)) != nullptr) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        bool alive = false;
        for (const DWORD pid : pids) {
            HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
            if (!process) continue;
            if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) alive = true;
            CloseHandle(process);
        }
        if (!alive) return true;
        if (GetTickCount64() >= deadline) break;
        Sleep(100);
    }
    if (!force) {
        error = L"面板仍在运行，请先退出后再继续";
        return false;
    }
    for (const DWORD pid : pids) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
        if (!process) continue;
        TerminateProcess(process, 1);
        WaitForSingleObject(process, 3000);
        CloseHandle(process);
    }
    return true;
}

bool MigrateSettings(const std::wstring& installDirectory,
    const std::wstring& sourceDataDirectory, std::wstring& note) {
    if (sourceDataDirectory.empty()) return false;
    const auto targetData = JoinPath(installDirectory, kDataDir);
    const auto targetSettings = JoinPath(targetData, L"settings.json");
    if (FileExists(targetSettings)) return false;
    const auto sourceSettings = JoinPath(sourceDataDirectory, L"settings.json");
    if (!FileExists(sourceSettings)) return false;
    if (!EnsureDirectory(targetData)) return false;
    if (!CopyFileW(sourceSettings.c_str(), targetSettings.c_str(), TRUE)) return false;
    const auto sourceProfiles = JoinPath(sourceDataDirectory, L"profiles.json");
    if (FileExists(sourceProfiles)) {
        CopyFileW(sourceProfiles.c_str(), JoinPath(targetData, L"profiles.json").c_str(), TRUE);
    }
    note = L"已导入旧设置：" + sourceDataDirectory;
    return true;
}

bool SetRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name,
    const wchar_t* value, std::wstring& error) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
        nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法打开注册表项: " + FormatWin32Error(static_cast<DWORD>(status));
        return false;
    }
    const DWORD bytes = static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t));
    status = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value), bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = L"无法写入注册表值: " + FormatWin32Error(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

bool SetRegistryDword(HKEY root, const wchar_t* subkey, const wchar_t* name, DWORD value,
    std::wstring& error) {
    HKEY key = nullptr;
    LSTATUS status = RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
        nullptr);
    if (status != ERROR_SUCCESS) {
        error = L"无法打开注册表项: " + FormatWin32Error(static_cast<DWORD>(status));
        return false;
    }
    status = RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
        sizeof(value));
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        error = L"无法写入注册表值: " + FormatWin32Error(static_cast<DWORD>(status));
        return false;
    }
    return true;
}

bool DeleteRegistryValue(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return true;
    RegDeleteValueW(key, name);
    RegCloseKey(key);
    return true;
}

bool DeleteRegistryTree(HKEY root, const wchar_t* subkey) {
    const LSTATUS status = RegDeleteTreeW(root, subkey);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

bool CreateShortcut(const std::wstring& linkPath, const std::wstring& target,
    const std::wstring& arguments, const std::wstring& workingDirectory,
    const std::wstring& description, const std::wstring& iconPath, std::wstring& error) {
    IShellLinkW* link = nullptr;
    HRESULT result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&link));
    if (FAILED(result) || !link) {
        error = L"创建快捷方式失败: " + FormatHResult(result);
        return false;
    }
    link->SetPath(target.c_str());
    link->SetArguments(arguments.c_str());
    link->SetWorkingDirectory(workingDirectory.c_str());
    link->SetDescription(description.c_str());
    link->SetIconLocation(iconPath.c_str(), 0);
    IPersistFile* persist = nullptr;
    result = link->QueryInterface(IID_PPV_ARGS(&persist));
    if (SUCCEEDED(result) && persist) {
        DeleteFileW(linkPath.c_str());
        result = persist->Save(linkPath.c_str(), TRUE);
        persist->Release();
    }
    link->Release();
    if (FAILED(result)) {
        error = L"保存快捷方式 " + linkPath + L" 失败: " + FormatHResult(result);
        return false;
    }
    return true;
}

void LogLine(const std::wstring& text) {
    wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, temporary);
    if (length == 0 || length >= MAX_PATH) return;
    const std::wstring path = std::wstring(temporary) + L"HermesModelPanelNative-setup.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t stamp[80]{};
    swprintf_s(stamp, L"[%04u-%02u-%02u %02u:%02u:%02u] ", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond);
    const std::wstring line = std::wstring(stamp) + text + L"\r\n";
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (bytes > 0) {
        std::vector<char> utf8(static_cast<std::size_t>(bytes));
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), utf8.data(),
            bytes, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    }
    CloseHandle(file);
}

}   // namespace setup
