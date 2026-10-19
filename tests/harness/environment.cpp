#include <windows.h>
#include <shellscalingapi.h>
#include <iostream>

BOOL CALLBACK PrintMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM count) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor,&info);
    UINT x=0,y=0;
    GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&x,&y);
    std::wcout << info.szDevice << L" bounds=[" << info.rcMonitor.left << L',' << info.rcMonitor.top << L','
        << info.rcMonitor.right << L',' << info.rcMonitor.bottom << L"] work=[" << info.rcWork.left << L','
        << info.rcWork.top << L',' << info.rcWork.right << L',' << info.rcWork.bottom << L"] dpi=" << x << L',' << y << L'\n';
    ++*reinterpret_cast<int*>(count);
    return TRUE;
}
int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count=0;
    EnumDisplayMonitors(nullptr,nullptr,PrintMonitor,reinterpret_cast<LPARAM>(&count));
    std::cout << "monitors=" << count << " virtual=[" << GetSystemMetrics(SM_XVIRTUALSCREEN) << ','
        << GetSystemMetrics(SM_YVIRTUALSCREEN) << ',' << GetSystemMetrics(SM_CXVIRTUALSCREEN) << ','
        << GetSystemMetrics(SM_CYVIRTUALSCREEN) << "]\n";
    return count?0:1;
}
