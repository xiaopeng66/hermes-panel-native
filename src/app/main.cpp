#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <string>
#include <fstream>
#include <exception>
#include "application.h"
#include "control_plane.h"
#include "control_pipe.h"
#include "data/log.h"
#include "data/settings.h"

namespace {
constexpr wchar_t kControlClass[] = L"HermesModelPanelNative.Control";
constexpr UINT kWake = WM_APP + 2;
constexpr UINT kWakeCount = WM_APP + 3;
constexpr UINT kTrayMessage = WM_APP + 10;
constexpr UINT kPipeRequest = WM_APP + 11;
struct PipeMessage { const std::string* request; std::string response; };

class Handle {
public:
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() { if (value_) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};

using hermes::Application;

LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    switch (message) {
    case kWake:
        try { if (app) app->Wake(); }
        catch(const std::exception& error) { if (app) app->ReportControlFailure(error); }
        return 0;
    case kWakeCount:
        return app ? app->WakeCount() : 0;
    case kTrayMessage:
        // 托盘点击/菜单里的动作同步执行；这里必须接住异常，否则它穿出 DispatchMessage 就是无提示退出。
        try { if (app) app->TrayMessage(lparam); }
        catch(const std::exception& error) { if (app) app->ReportControlFailure(error); }
        return 0;
#if HERMES_CONTROL_PLANE
    case kPipeRequest: {
        auto* payload = reinterpret_cast<PipeMessage*>(lparam);
        if (!app || !payload || !payload->request) return 0;
        try { payload->response = app->Command(hermes::Json::parse(*payload->request)).dump(); }
        catch(const std::exception& error) { payload->response = hermes::Json{{"error", error.what()}}.dump(); }
        return 0;
    }
#endif
#if HERMES_CONTROL_PLANE
    case WM_COPYDATA: {
        if (!app) return 0;
        const auto* copy=reinterpret_cast<COPYDATASTRUCT*>(lparam);
        if (!copy || !copy->lpData || copy->cbData<2 || copy->cbData>65536) return 0;
        hermes::Json reply;
        try {
            const auto request=hermes::Json::parse(static_cast<const char*>(copy->lpData),static_cast<const char*>(copy->lpData)+copy->cbData-1);
            reply=app->Command(request);
        } catch(const std::exception& error) { reply={{"error",error.what()}}; }
        auto data=reply.dump();
        COPYDATASTRUCT response{1,static_cast<DWORD>(data.size()+1),data.data()};
        SendMessageW(reinterpret_cast<HWND>(wparam),WM_COPYDATA,reinterpret_cast<WPARAM>(window),reinterpret_cast<LPARAM>(&response));
        return 1;
    }
#endif
    case WM_QUERYENDSESSION:
        if (app) app->PersistSettings(true);
        return TRUE;
    case WM_ENDSESSION:
        return 0;

    case WM_HOTKEY:
        try { if (app) app->InvokeHotkey(static_cast<int>(wparam)); }
        catch(const std::exception& error) { if (app) app->ReportControlFailure(error); }
        return 0;
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

std::wstring DataDirectory(int argc, wchar_t** argv, bool* explicitDirectory = nullptr) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::wstring(argv[i]) == L"--data-dir") {
            if (explicitDirectory) *explicitDirectory = true;
            return argv[i + 1];
        }
    }
    wchar_t buffer[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH) == 0) return L".";
    return std::wstring(buffer) + L"\\HermesModelPanelNative";
}
std::wstring InstanceName() {
    std::wstring id = L"default";
    int count = 0;
    auto** args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return id;
    for (int i = 1; i + 1 < count; ++i) {
        if (std::wstring(args[i]) == L"--instance") id = args[++i];
    }
    LocalFree(args);
    if (id.empty() || id.size() > 120 || id.find_first_not_of(L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::wstring::npos) {
        return {};
    }
    return id;
}
}

namespace {
hermes::Log* g_log = nullptr;
std::wstring g_crashProbePath;
extern "C" LONG WINAPI CrashProbe(EXCEPTION_POINTERS* info) {
    std::ofstream probe(g_crashProbePath, std::ios::app);
    probe << "fault code=0x" << std::hex << info->ExceptionRecord->ExceptionCode
        << " addr=" << info->ExceptionRecord->ExceptionAddress
        << " base=" << GetModuleHandleW(nullptr) << "\n";
    void* frames[12]{};
    const auto count=RtlCaptureStackBackTrace(0,12,frames,nullptr);
    for(DWORD index=0;index<count;++index) probe << "  frame" << index << "=" << frames[index] << "\n";
    return EXCEPTION_EXECUTE_HANDLER;
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // WebView2 (login window) needs an apartment-threaded COM apartment on the UI thread.
    const auto comStatus = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // Startup hard-crash probe: any unhandled fault leaves its code and faulting address on disk.
    g_crashProbePath = DataDirectory(__argc, __wargv) + L"\\logs\\crash-probe.txt";
    SetUnhandledExceptionFilter(&CrashProbe);
    const auto id = InstanceName();
    if (id.empty()) return 2;
    const auto mutexName = L"Local\\HermesModelPanelNative.Mutex." + id;
    Handle mutex(CreateMutexW(nullptr, FALSE, mutexName.c_str()));
    const auto mutexError = GetLastError();
    if (!mutex.get()) return 3;
    const auto readyName = L"Local\\HermesModelPanelNative.Ready." + id;
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, readyName.c_str()));
    if (!ready.get()) return 4;
    if (mutexError == ERROR_ALREADY_EXISTS) {
        if (WaitForSingleObject(ready.get(), 4000) != WAIT_OBJECT_0) return 5;
        const auto existing = FindWindowExW(HWND_MESSAGE, nullptr, kControlClass, id.c_str());
        DWORD_PTR result = 0;
        return existing && SendMessageTimeoutW(existing, kWake, 0, 0, SMTO_ABORTIFHUNG, 2000, &result) ? 0 : 6;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = ControlProc;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!windowClass.hIconSm) windowClass.hIconSm = windowClass.hIcon;
    windowClass.lpszClassName = kControlClass;
    if (!RegisterClassExW(&windowClass)) return 7;
    Application app;
    const auto control = CreateWindowExW(0, kControlClass, id.c_str(), 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, &app);
    if (!control) return 8;
    int argumentCount = 0;
    auto** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    bool explicitDataDirectory = false;
    std::wstring dataDirectory = DataDirectory(argumentCount, arguments, &explicitDataDirectory);
    static_cast<void>(explicitDataDirectory);
    // 门禁夹具的旧设置目录（默认 %APPDATA%\HermesModelPanel，只读）：--legacy-dir 只在带控制面的
    // 构建里生效，发布版永远按默认目录找。
    std::wstring legacyDirectory;
#if HERMES_CONTROL_PLANE
    for (int index = 1; index + 1 < argumentCount; ++index) {
        if (std::wstring(arguments[index]) == L"--legacy-dir") legacyDirectory = arguments[index + 1];
    }
#endif
    if (arguments) LocalFree(arguments);
    hermes::SettingsStore settings{dataDirectory + L"\\settings.json"};
    hermes::Log log{dataDirectory + L"\\logs\\app.log"};
    if (!legacyDirectory.empty()) app.SetLegacyDirectory(legacyDirectory);
    try { if (!app.Initialize(instance,control,&settings,&log)) return 10; }
    catch(const std::exception& error) { OutputDebugStringA(error.what());return 11; }
#if HERMES_CONTROL_PLANE
    hermes::ControlPipe pipe;
    const std::wstring pipeName = LR"(\\.\pipe\HermesModelPanelNative.)" + id;
    pipe.Start(control,kPipeRequest,pipeName,[&](const std::string& request) {
        PipeMessage message{&request,{}};
        SendMessageW(control,kPipeRequest,0,reinterpret_cast<LPARAM>(&message));
        return message.response;
    });
#endif
    SetEvent(ready.get());

    // A silent exit is impossible to diagnose: every loop exit and any std::terminate is logged, so a
    // test that loses the process learns why instead of seeing a bare timeout.
    g_log = &log;
    std::set_terminate([] {
        // 先留证据再退出：std::abort 走 __fastfail，SetUnhandledExceptionFilter 收不到，
        // 真机上就是"无日志、无探针、无 WER"的闪退。这里写 crash-probe 并给出专用退出码 3，
        // 下次能区分"未捕获异常"与"被外部 TerminateProcess（什么都留不下）"。
        if (g_log) g_log->Error("app", "std::terminate called");
        {
            std::ofstream probe(g_crashProbePath, std::ios::app);
            probe << "terminate";
            try {
                const auto current = std::current_exception();
                if (current) std::rethrow_exception(current);
            } catch (const std::exception& error) {
                probe << " what=" << error.what();
            } catch (...) {
                probe << " what=<non-std>";
            }
            probe << std::endl;
            probe.flush();
        }
        std::_Exit(3);
    });
    MSG message{};
    BOOL status = 0;
    while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    log.Info("app", "message loop finished status=" + std::to_string(status));
    app.PersistSettings(true);
    settings.Flush();
    if (SUCCEEDED(comStatus)) CoUninitialize();
    log.Info("app", "exiting with " + std::to_string(status == -1 ? 9 : static_cast<int>(message.wParam)));
    return status == -1 ? 9 : static_cast<int>(message.wParam);
}
