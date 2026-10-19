// One-off drag repro for the failing "drag into the left snap band" case: replays the exact
// drag-tests sequence while dumping state between the steps, so the failing stage is visible.
#include <windows.h>
#include <iostream>
#include <string>
#include "harness/client.h"
#include "harness/input.h"
#include "harness/paths.h"

int wmain(int argc,wchar_t** argv) {
    if(argc!=2) { std::wcerr<<L"usage: drag-repro <app.exe>\n"; return 2; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"repro-"+std::to_wstring(GetCurrentProcessId());
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) return 3;
    auto status=1;
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        harness::Pointer pointer;
        auto snapshot=client.Request({{"command","dump"}});
        const auto work=snapshot["work"];
        const int workLeft=work[0].get<int>(),workTop=work[1].get<int>();
        const int workRight=workLeft+work[2].get<int>(),workBottom=workTop+work[3].get<int>();
        const int centerX=(workLeft+workRight)/2,centerY=(workTop+workBottom)/2;
        const int grabX=60,grabY=14;
        auto dumpField=[&](harness::Client& c,const char* field) {
            auto dump=c.Request({{"command","dump"}});
            std::cout<<"  "<<field<<"="<<dump[field].dump()<<"\n";
            return dump;
        };
        // Drag 1: open-desktop move.
        auto panel=snapshot["panel"];
        pointer.MoveTo(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY);
        Sleep(120);
        pointer.Button(true);
        Sleep(180);
        std::cout<<"drag1 pressed: state="<<client.Request({{"command","dump"}})["state"].dump()<<"\n";
        pointer.MoveTo(centerX+120,centerY+90);
        Sleep(120);
        pointer.Button(false);
        Sleep(300);
        snapshot=client.Request({{"command","dump"}});
        std::cout<<"drag1 released: state="<<snapshot["state"].dump()<<" panel="<<snapshot["panel"].dump()
            <<" writes="<<snapshot["writes"].dump()<<"\n";
        // Drag 2: into the left snap band, with a mid-drag dump.
        panel=snapshot["panel"];
        const auto press1=pointer.Position();
        pointer.MoveTo(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY);
        Sleep(150);
        std::cout<<"drag2 cursor settled at ("<<pointer.Position().x<<","<<pointer.Position().y
            <<") press at ("<<press1.x<<","<<press1.y<<")\n";
        pointer.Button(true);
        Sleep(200);
        const auto midDrag=client.Request({{"command","dump"}});
        std::cout<<"drag2 pressed: state="<<midDrag["state"].dump()<<" writes="<<midDrag["writes"].dump()<<"\n";
        const int dockTargetY=workTop+220+grabY;
        pointer.MoveTowards(workLeft+4+grabX,dockTargetY,14);
        pointer.MoveTo(workLeft+4+grabX,dockTargetY);
        Sleep(260);
        std::cout<<"drag2 mid-move: state="<<client.Request({{"command","dump"}})["state"].dump()<<"\n";
        pointer.Button(false);
        Sleep(360);
        snapshot=client.Request({{"command","dump"}});
        std::cout<<"drag2 released: state="<<snapshot["state"].dump()<<" edge="<<snapshot["edge"].dump()
            <<" panel="<<snapshot["panel"].dump()<<" writes="<<snapshot["writes"].dump()
            <<" lastDrop="<<snapshot["lastDrop"].dump()<<"\n";
        status=snapshot["edge"].get<int>()==1?0:1;
    } catch(const std::exception& error) {
        std::cerr<<"repro: "<<error.what()<<"\n";
        status=1;
    }
    TerminateProcess(child.hProcess,status);
    CloseHandle(child.hProcess);CloseHandle(child.hThread);
    return status;
}
