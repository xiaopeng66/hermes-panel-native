#include "harness/client.h"
#include "harness/paths.h"
#include <iostream>
#include "harness/no_modal_asserts.h"

int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"shell-"+std::to_wstring(GetCurrentProcessId());
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) {
        std::wcerr<<L"diagnostic(shell): CreateProcess failed err="<<GetLastError()<<L" app="<<argv[1]<<L"\n";
        return 3;
    }
    int code=1;
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        auto snapshot=client.Request({{"command","dump"}});
        const auto panel=harness::Window(snapshot,"panelHwnd");
        const auto strip=harness::Window(snapshot,"stripHwnd");
        harness::Require(IsWindow(panel)&&IsWindow(strip)&&panel!=strip,"two independent windows missing");
        harness::Require((GetWindowLongPtrW(panel,GWL_STYLE)&WS_POPUP)!=0,"panel not popup");
        harness::Require((GetWindowLongPtrW(panel,GWL_EXSTYLE)&WS_EX_TOOLWINDOW)!=0,"panel taskbar style wrong");
        harness::Require((GetWindowLongPtrW(strip,GWL_EXSTYLE)&WS_EX_NOACTIVATE)!=0,"strip must not activate");
        harness::Require(AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(panel),DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)!=0,"panel not PMv2");
        harness::Require(harness::RectOf(panel)==snapshot["expectedPanel"],"startup panel differs from pure model");
        harness::Require(IsWindowVisible(panel)&&!IsWindowVisible(strip),"floating visibility wrong");
        std::cout<<"PASS dual popup/tool windows, NOACTIVATE strip, PMv2, exact startup rect\n";
        client.Request({{"command","hide"}});
        harness::Require(!IsWindowVisible(panel)&&!IsWindowVisible(strip),"hidden state leaks window");
        client.Request({{"command","show"}});
        harness::Require(IsWindowVisible(panel)&&!IsWindowVisible(strip),"restore visibility wrong");
        std::cout<<"PASS hide/show exclusive visibility\n";
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["violations"]==0,"geometry invariant violations");
        std::cout<<"shell: 2 passed, 0 failed\n";
        client.Request({{"command","quit"}});
        code=0;
    }catch(const std::exception& error){std::cerr<<"FAIL shell: "<<error.what()<<'\n';}
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return code;
}
