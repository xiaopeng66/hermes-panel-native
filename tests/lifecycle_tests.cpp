#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include "harness/no_modal_asserts.h"
#include "harness/paths.h"

namespace {
struct Child {
    PROCESS_INFORMATION info{};
    explicit Child(const std::wstring& command) {
        auto writable = command;
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(nullptr, harness::NativePath(writable).data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) {
            throw std::runtime_error("CreateProcess failed");
        }
    }
    ~Child() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT) {
                TerminateProcess(info.hProcess, 99);
                WaitForSingleObject(info.hProcess, 1000);
            }
            CloseHandle(info.hThread);
            CloseHandle(info.hProcess);
        }
    }
    bool alive() const { return WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT; }
};
void Require(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
}
HWND AwaitControl(const std::wstring& name, const Child& child) {
    const auto deadline = GetTickCount64() + 4000;
    do {
        const auto window = FindWindowExW(HWND_MESSAGE, nullptr, L"HermesModelPanelNative.Control", name.c_str());
        if (window) return window;
        if (!child.alive()) break;
        Sleep(20);
    } while (GetTickCount64() < deadline);
    return nullptr;
}
}

int wmain(int argc, wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if (argc != 2) return 2;
    try {
        const std::wstring id = L"lifecycle-" + std::to_wstring(GetCurrentProcessId());
        // Isolated data directory: the shell persists its state, so tests must not share one.
        const std::wstring command = L"\"" + harness::NativePath(argv[1]) + L"\" --instance " + id
            + L" --data-dir \"E:/AI/Zcode/tmp/hermes-panel-native/testdata/" + id + L"\"";
        Child first(command);
        const auto control = AwaitControl(id, first);
        Require(control != nullptr && first.alive(), "first instance did not enter its message loop");
        std::cout << "PASS first instance enters message loop\n";
        Child second(command);
        Require(WaitForSingleObject(second.info.hProcess, 4000) == WAIT_OBJECT_0, "second instance failed to exit");
        DWORD code = 99;
        GetExitCodeProcess(second.info.hProcess, &code);
        Require(code == 0 && first.alive(), "duplicate launch disrupted first instance");
        DWORD_PTR wakeCount = 0;
        Require(SendMessageTimeoutW(control, WM_APP + 3, 0, 0, SMTO_ABORTIFHUNG, 1000,
            &wakeCount) != 0, "first instance not responsive");
        Require(wakeCount == 1, "second instance did not notify first instance exactly once");
        std::cout << "PASS duplicate launch exits 0 and wakes first instance once\n";
        // FIX-021 回归：安装器 RestartManager 的关闭握手。面板/贴条是普通顶层窗口（控制窗是
        // message-only，收不到广播），必须自己响应 WM_QUERYENDSESSION/WM_ENDSESSION：
        // 只同意不退出的话，RM 等待超时后会 TerminateProcess（真机 2026-10-19 的“闪退”）。
#ifndef ENDSESSION_CLOSEAPP
#define ENDSESSION_CLOSEAPP 0x40000000
#endif
        // 按进程 ID 过滤：机器上可能同时跑着用户自己的真机实例，绝不能碰到它。
        HWND panelWindow = nullptr;
        for (HWND candidate = FindWindowExW(nullptr, nullptr, L"HermesModelPanelNative.Panel", nullptr);
             candidate; candidate = FindWindowExW(nullptr, candidate, L"HermesModelPanelNative.Panel", nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(candidate, &pid);
            if (pid == first.info.dwProcessId) { panelWindow = candidate; break; }
        }
        Require(panelWindow != nullptr, "panel window not found");
        DWORD_PTR consent = 0;
        Require(SendMessageTimeoutW(panelWindow, WM_QUERYENDSESSION, 1, ENDSESSION_CLOSEAPP,
            SMTO_ABORTIFHUNG, 2000, &consent) != 0 && consent != 0,
            "panel refused WM_QUERYENDSESSION");
        SendMessageTimeoutW(panelWindow, WM_ENDSESSION, 1, ENDSESSION_CLOSEAPP,
            SMTO_ABORTIFHUNG, 2000, &consent);
        Require(WaitForSingleObject(first.info.hProcess, 5000) == WAIT_OBJECT_0,
            "panel ignored WM_ENDSESSION - RestartManager would force-kill it");
        GetExitCodeProcess(first.info.hProcess, &code);
        Require(code == 0, "session-end exit was not clean");
        std::cout << "PASS RestartManager session end exits cleanly\n";
        Child replacement(command);
        const auto next = AwaitControl(id, replacement);
        Require(next != nullptr && replacement.alive(), "mutex not released on shutdown");
        PostMessageW(next, WM_CLOSE, 0, 0);
        Require(WaitForSingleObject(replacement.info.hProcess, 4000) == WAIT_OBJECT_0, "replacement failed to shut down");
        std::cout << "PASS orderly shutdown releases mutex; restart succeeds\n";
        std::cout << "lifecycle: 4 passed, 0 failed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL lifecycle: " << error.what() << '\n';
        return 1;
    }
}
