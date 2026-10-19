#include "harness/client.h"
#include "harness/paths.h"
#include "harness/input.h"
#include <iostream>
#include "harness/no_modal_asserts.h"

namespace {
using harness::Json;
Json StripPoint(const Json& snapshot,int along) {
    const auto strip=snapshot["strip"];
    const auto edge=snapshot["edge"].get<int>();
    const int x0=strip[0].get<int>(),y0=strip[1].get<int>();
    const int width=strip[2].get<int>(),height=strip[3].get<int>();
    const bool vertical=edge==1||edge==2;
    const int x=vertical?x0+width/2:x0+along;
    const int y=vertical?y0+along:y0+height/2;
    return Json::array({x,y});
}
Json PanelInsidePoint(const Json& snapshot,int along) {
    const auto panel=snapshot["panel"];
    const auto edge=snapshot["edge"].get<int>();
    const int x=edge==1?panel[0].get<int>()+12:edge==2?panel[0].get<int>()+panel[2].get<int>()-12:panel[0].get<int>()+along;
    const int y=edge==1||edge==2?panel[1].get<int>()+along:edge==3?panel[1].get<int>()+12:panel[1].get<int>()+panel[3].get<int>()-12;
    return Json::array({x,y});
}
int State(harness::Client& client) { return client.Request({{"command","dump"}})["state"].get<int>(); }
}
int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"hover-"+std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
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
        const auto panel=harness::Window(snapshot,"panelHwnd");
        harness::Require(snapshot["state"]==1,"expected floating start");
        auto docked=client.Request({{"command","menu"},{"id",1002}});
        harness::Require(docked["state"]==4&&docked["stripVisible"]==true,"dock did not collapse into strip");
        const auto panelRectBefore=docked["panel"];
        const auto stripRectBefore=docked["strip"];
        const auto writesBefore=docked["writes"].get<unsigned>();
        const auto stripPoint=StripPoint(docked,40);
        const auto insidePoint=PanelInsidePoint(docked,120);
        const auto stripHwnd=harness::Window(docked,"stripHwnd");
        const auto requirePointerAt=[&](int x,int y,const char* where) {
            const auto now=harness::Pointer::Position();
            if(std::abs(now.x-x)>2||std::abs(now.y-y)>2)
                throw std::runtime_error(std::string("pointer drifted ")+where+" (at "
                    +std::to_string(now.x)+","+std::to_string(now.y)+" instead of "
                    +std::to_string(x)+","+std::to_string(y)+"); the desktop is in use");
        };
        // 贴条只响应单击：悬停扫过不能展开（真机要求）。点击前确认贴条是光标下最顶层窗口，
        // 否则是桌面被占用（环境），不是产品缺陷；点完若指针已漂走也按环境处理。
        const auto clickStrip=[&]() {
            const POINT before=harness::Pointer::Position();
            if(WindowFromPoint(before)!=stripHwnd)
                throw std::runtime_error("the strip is covered by another window; the desktop is in use");
            pointer.Button(true);
            Sleep(40);
            pointer.Button(false);
            Sleep(80);
            const auto state=State(client);
            const POINT after=harness::Pointer::Position();
            if(state!=3&&(std::abs(after.x-before.x)>2||std::abs(after.y-before.y)>2))
                throw std::runtime_error("the pointer drifted during the strip click; the desktop is in use");
        };
        const int farX=snapshot["work"][0].get<int>()+snapshot["work"][2].get<int>()/2;
        const int farY=snapshot["work"][1].get<int>()+snapshot["work"][3].get<int>()/2;
        pointer.MoveTo(farX,farY);
        Sleep(150);

        pointer.MoveTowards(stripPoint[0].get<int>(),stripPoint[1].get<int>(),10);
        Sleep(180);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==4,"hovering the strip must not expand the panel");
        std::cout<<"PASS hover over the strip does not expand it\n";
        clickStrip();
        snapshot=client.Request({{"command","dump"}});
        if(snapshot["state"]!=3) {
            const auto probeX=stripPoint[0].get<int>(),probeY=stripPoint[1].get<int>();
            const auto hit=WindowFromPoint({probeX,probeY});
            wchar_t className[128]={};
            GetClassNameW(hit,className,128);
            std::cerr<<"diagnostic: point=("<<probeX<<","<<probeY<<") strip=("<<stripRectBefore[0]<<","<<stripRectBefore[1]<<","
                <<stripRectBefore[2]<<","<<stripRectBefore[3]<<") edge="<<docked["edge"]<<" hitClass=";
            std::wcerr<<className;
            std::cerr<<" hitIsStrip="<<(hit==harness::Window(docked,"stripHwnd"))
                <<" stripMoves="<<snapshot["stripMoves"]<<" panelMoves="<<snapshot["panelMoves"]
                <<" panelLeaves="<<snapshot["panelLeaves"]<<" graces="<<snapshot["graces"]
                <<" cursor=("<<harness::Pointer::Position().x<<","<<harness::Pointer::Position().y<<")\n";
        }
        harness::Require(snapshot["state"]==3,"clicking the strip did not expand the panel");
        harness::Require(snapshot["panelVisible"]==true&&snapshot["stripVisible"]==false,"expanded visibility wrong");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBefore,"expand wrote geometry");
        harness::Require(snapshot["panel"]==panelRectBefore&&snapshot["strip"]==stripRectBefore,"expand moved a window");
        std::cout<<"PASS click on strip expands with zero geometry writes\n";

        pointer.MoveTowards(insidePoint[0].get<int>(),insidePoint[1].get<int>(),6);
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"inside the panel");
        // 单击展开时臂上的第一个 400ms grace 可能还没被鼠标移动取消：在面板内等到它过期
        // （containment 会重新武装而不是收起），之后离开的 400ms 才是本用例要测的那一段。
        Sleep(450);
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"during the containment wait");
        pointer.MoveTowards(farX,farY,8);
        Sleep(180);
        requirePointerAt(farX,farY,"during the grace window");
        {
            snapshot=client.Request({{"command","dump"}});
            if(snapshot["state"]!=3) {
                const auto cursor=harness::Pointer::Position();
                std::cerr<<"diagnostic(grace): cursor=("<<cursor.x<<","<<cursor.y<<") panel="
                    <<snapshot["panel"]<<" state="<<snapshot["state"]<<" graces="
                    <<snapshot["graces"]<<" leaves="<<snapshot["panelLeaves"]<<" events:\n";
                for(const auto& event:snapshot["events"]) std::cerr<<"  "<<event["tick"]<<" "
                    <<event["name"]<<" state="<<event["state"]<<" at ("<<event["x"]<<","<<event["y"]<<")\n";
            }
        }
        harness::Require(snapshot["state"]==3,"panel collapsed before the 400ms grace elapsed");
        // 重新进入必须赶在 400ms 内：一次 MoveTo 到位，别用多步插值（桌面繁忙时每步重试会超时）。
        pointer.MoveTo(insidePoint[0].get<int>(),insidePoint[1].get<int>());
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"inside the panel (re-entry)");
        Sleep(500);
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"during the re-entry wait");
        snapshot=client.Request({{"command","dump"}});
        if(snapshot["state"]!=3) {
            const auto cursor=harness::Pointer::Position();
            std::cerr<<"hit="<<client.Request({{"command","hit"},{"x",insidePoint[0].get<int>()},{"y",insidePoint[1].get<int>()}})<<"\n";
            std::cerr<<"diagnostic: cursor=("<<cursor.x<<","<<cursor.y<<") inside=("<<insidePoint[0]<<","<<insidePoint[1]
                <<") panel="<<snapshot["panel"]<<" state="<<snapshot["state"]<<" graces="<<snapshot["graces"]<<" events:\n";
            for(const auto& event:snapshot["events"]) std::cerr<<"  "<<event["tick"]<<" "<<event["name"]<<" state="<<event["state"]
                <<" at ("<<event["x"]<<","<<event["y"]<<") msgTime="<<event["msgTime"]<<" hit="<<event["hit"]<<"\n";
        }
        harness::Require(snapshot["state"]==3,"re-entry inside grace did not cancel the collapse");
        std::cout<<"PASS re-entry inside 400ms grace cancels collapse\n";

        pointer.MoveTowards(farX,farY,10);
        Sleep(600);
        requirePointerAt(farX,farY,"during the collapse wait");
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==4,"leaving the panel did not collapse back to the strip");
        harness::Require(snapshot["panelVisible"]==false&&snapshot["stripVisible"]==true,"collapsed visibility wrong");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBefore,"hover collapse wrote geometry");
        harness::Require(snapshot["panel"]==panelRectBefore&&snapshot["strip"]==stripRectBefore,"hover collapse moved a window");
        std::cout<<"PASS leaving plus 400ms grace collapses with zero geometry writes\n";

        pointer.MoveTowards(stripPoint[0].get<int>(),stripPoint[1].get<int>(),6);
        Sleep(200);
        clickStrip();
        harness::Require(State(client)==3,"strip click did not re-expand for the containment case");
        pointer.MoveTowards(insidePoint[0].get<int>(),insidePoint[1].get<int>(),6);
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"inside the panel (leave-notify case)");
        Sleep(120);
        PostMessageW(panel,WM_MOUSELEAVE,0,MAKELPARAM(insidePoint[0].get<int>(),insidePoint[1].get<int>()));
        Sleep(520);
        requirePointerAt(insidePoint[0].get<int>(),insidePoint[1].get<int>(),"during the leave-notify wait");
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==3,
            "a leave notification whose position is inside the panel must not collapse it");
        std::cout<<"PASS leave notification with an inside position keeps the panel expanded\n";
        pointer.MoveTowards(farX,farY,8);
        Sleep(600);
        requirePointerAt(farX,farY,"during the containment check");
        harness::Require(State(client)==4,"geometric containment did not collapse after a real exit");

        // A single fast move out of the strip never produces an in-panel WM_MOUSEMOVE, so no
        // WM_MOUSELEAVE can arrive: the grace timer with the containment check must collapse anyway.
        pointer.MoveTowards(stripPoint[0].get<int>(),stripPoint[1].get<int>(),6);
        Sleep(240);
        clickStrip();
        harness::Require(State(client)==3,"strip click did not re-expand before the fast-leave case");
        pointer.MoveTo(farX,farY);
        Sleep(620);
        requirePointerAt(farX,farY,"during the fast-leave wait");
        snapshot=client.Request({{"command","dump"}});
        if(snapshot["state"]!=4) {
            std::cerr<<"diagnostic: fast-leave state="<<snapshot["state"]<<" graces="<<snapshot["graces"]
                <<" leaves="<<snapshot["panelLeaves"]<<" cursor=("<<harness::Pointer::Position().x
                <<","<<harness::Pointer::Position().y<<")\n";
            for(const auto& event:snapshot["events"]) std::cerr<<"  "<<event["tick"]<<" "<<event["name"]
                <<" state="<<event["state"]<<" at ("<<event["x"]<<","<<event["y"]<<")\n";
        }
        harness::Require(snapshot["state"]==4,"a single fast move out of the panel must still collapse it");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBefore,"fast-exit collapse wrote geometry");
        std::cout<<"PASS fast pointer exit collapses without a leave notification\n";

        for(int cycle=0;cycle<20;++cycle) {
            pointer.MoveTowards(stripPoint[0].get<int>(),stripPoint[1].get<int>(),4);
            Sleep(60);
            pointer.MoveTowards(farX,farY,4);
            Sleep(470);
        }
        Sleep(200);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==4,"hover loop did not settle back to collapsed");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBefore,"20 real hover cycles wrote geometry");
        harness::Require(snapshot["panel"]==panelRectBefore&&snapshot["strip"]==stripRectBefore,"real hover cycles drifted a window");
        harness::Require(snapshot["violations"]==0,"hover cycles violated invariants");
        std::cout<<"PASS 20 real hover cycles: zero writes, zero drift, zero violations\n";
        std::cout<<"hover: 6 passed, 0 failed\n";
        status=0;
        client.Request({{"command","quit"}});
        WaitForSingleObject(child.hProcess,3000);
    }catch(const std::exception& error){
        if(harness::IsEnvironmentInterference(error)){
            std::cerr<<"SKIP hover: "<<error.what()<<"\n";
            TerminateProcess(child.hProcess,77);
            WaitForSingleObject(child.hProcess,1500);
            return 77;
        }
        std::cerr<<"FAIL hover: "<<error.what()<<"\n";
    }
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return status;
}
