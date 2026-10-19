#include "harness/client.h"
#include "harness/paths.h"
#include "harness/input.h"
#include <iostream>
#include "harness/no_modal_asserts.h"

namespace {
using harness::Json;
int State(harness::Client& client) { return client.Request({{"command","dump"}})["state"].get<int>(); }
}
int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    // 假助手夹具（GOAL 硬约束）：真 WSL 冷启动会让面板先停在离线页（640 DIP）再切模型页（634 DIP），
    // 那是状态切换，不是跨 DPI 缩放。
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"dpi-"+std::to_wstring(GetCurrentProcessId());
    const auto home=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id+L"/fake";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW((std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id).c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    SetEnvironmentVariableW(L"HERMES_WSL_EXE",argv[2]);
    SetEnvironmentVariableW(L"FAKE_HERMES_HOME",home.c_str());
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
        const auto panelHwnd=harness::Window(snapshot,"panelHwnd");
        harness::Require(snapshot["state"]==1,"expected floating start");
        const int startDpi=snapshot["dpi"].get<int>();
        {
            // 首次刷新回来时面板会按内容再贴一次高（离线页 640 DIP vs 模型页 634 DIP）：先等它稳定，
            // 否则下面量到的"跨 DPI 缩放前尺寸"是启动瞬间的旧值。
            auto current=snapshot["panel"];
            const auto deadline=GetTickCount64()+6000;
            auto lastChange=GetTickCount64();
            while(GetTickCount64()<deadline) {
                Sleep(120);
                const auto next=client.Request({{"command","dump"}})["panel"];
                if(next!=current) { current=next;lastChange=GetTickCount64();continue; }
                if(GetTickCount64()-lastChange>=400) break;
            }
            snapshot=client.Request({{"command","dump"}});
        }

        // Contract of WM_GETDPISCALEDSIZE: the tier size must be recomputed for the candidate DPI,
        // never left to the system's bitmap-style scaling of the current size.
        struct SizeCase { unsigned dpi;int width; };
        // 宽度是固定的内容宽度 384 DIP（旧 CANON_W）；高度按**当前**内容的 DIP 高度等比（旧 app:fit 量出来的
        // 那个窗口）：模型页 634、离线页 640，写死其中一个会把状态差异误判成失败。
        const SizeCase sizes[]{{96,384},{120,480},{144,576},{192,768},{202,808}};
        const int dipW=static_cast<int>(std::lround(snapshot["panel"][2].get<double>()*96.0/startDpi));
        const int dipH=static_cast<int>(std::lround(snapshot["panel"][3].get<double>()*96.0/startDpi));
        harness::Require(dipW==384,"the floating panel is CANON_W wide (384 DIP)");
        harness::Require(dipH>=500&&dipH<=700,
            ("the fitted panel height is a legal page height, got "+std::to_string(dipH)+" DIP").c_str());
        for(const auto& test:sizes) {
            const auto probe=client.Request({{"command","dpi-probe"},{"dpi",test.dpi}});
            harness::Require(probe["handled"]==true,"WM_GETDPISCALEDSIZE was not handled by the app");
            const int expectedW=MulDiv(384,static_cast<int>(test.dpi),96);
            const int expectedH=MulDiv(dipH,static_cast<int>(test.dpi),96);
            harness::Require(probe["width"].get<int>()==expectedW&&probe["height"].get<int>()==expectedH,
                ("WM_GETDPISCALEDSIZE returned the wrong tier size for dpi "+std::to_string(test.dpi)
                    +" (got "+std::to_string(probe["width"].get<int>())+"x"
                    +std::to_string(probe["height"].get<int>())+", expected "
                    +std::to_string(expectedW)+"x"+std::to_string(expectedH)+")").c_str());
        }
        std::cout<<"PASS WM_GETDPISCALEDSIZE recomputes tier size for 96/120/144/192/202 dpi\n";

        // Real cross-DPI drag: move the panel from the current monitor to the other one and back.
        auto monitors=client.Request({{"command","monitors"}});
        harness::Require(monitors["monitors"].size()>=2,"two monitors required for the cross-DPI drag");
        const auto work=snapshot["work"],bounds=snapshot["bounds"];
        int otherIndex=-1;
        for(std::size_t i=0;i<monitors["monitors"].size();++i) {
            const auto& candidate=monitors["monitors"][i];
            if(candidate["dpi"].get<int>()!=startDpi||candidate["work"]!=work) { otherIndex=static_cast<int>(i); }
        }
        harness::Require(otherIndex>=0,"no monitor with a different DPI or work area found");
        const auto other=monitors["monitors"][otherIndex];
        const int otherLeft=other["work"][0].get<int>(),otherTop=other["work"][1].get<int>();
        const int otherRight=otherLeft+other["work"][2].get<int>(),otherBottom=otherTop+other["work"][3].get<int>();
        const int otherDpi=other["dpi"].get<int>();
        // 跨 DPI 的真不变式：外壳按**DIP 档位**重算物理尺寸（不是把旧物理尺寸乘比例——那会把
        // 793@120 → 1269，而 app 从 634 DIP 算出 1268，四舍五入差 1）。所以先还原 DIP 档位。
        const int startDipW=static_cast<int>(std::lround(snapshot["panel"][2].get<double>()*96.0/startDpi));
        const int startDipH=static_cast<int>(std::lround(snapshot["panel"][3].get<double>()*96.0/startDpi));
        const int scaledW=static_cast<int>(MulDiv(startDipW,otherDpi,96));
        const int dropX=otherLeft+(otherRight-otherLeft)/2,dropY=otherTop+(otherBottom-otherTop)/2;
        const int grabX=60,grabY=14;
        const int scaledH=static_cast<int>(MulDiv(startDipH,otherDpi,96));
        auto panel=snapshot["panel"];
        const auto writesBeforeDrag=snapshot["writes"].get<unsigned>();
        {
            // 合成点击只有在面板确实是该点最顶层窗口时才有意义；被别的窗口盖住是环境问题。
            const POINT press{panel[0].get<int>()+grabX,panel[1].get<int>()+grabY};
            if(WindowFromPoint(press)!=panelHwnd)
                throw std::runtime_error("the panel is not the topmost window at the press point; the desktop is in use");
        }
        pointer.MoveTo(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY);
        Sleep(150);
        pointer.Button(true);
        {
            // 按下后指针必须仍停在按下点：桌面有人用时，物理鼠标会在合成点击之间把光标带走，
            // 此时应用记录的抓取点与测试以为的不同，属于环境干扰而不是产品缺陷。
            const auto now=harness::Pointer::Position();
            if(std::abs(now.x-(panel[0].get<int>()+grabX))>2||std::abs(now.y-(panel[1].get<int>()+grabY))>2)
                throw std::runtime_error("the pointer drifted during the press; the desktop is in use");
        }
        Sleep(200);
        pointer.MoveTowards(dropX,dropY,22);
        pointer.MoveTo(dropX,dropY);
        if(client.Request({{"command","dump"}})["state"].get<int>()!=2)
            throw std::runtime_error("the caption press did not begin a system drag; the desktop is in use");
        Sleep(300);
        {
            const auto now=harness::Pointer::Position();
            if(std::abs(now.x-dropX)>2||std::abs(now.y-dropY)>2)
                throw std::runtime_error("the pointer drifted before the drop; the desktop is in use");
        }
        pointer.Button(false);
        Sleep(450);
        snapshot=client.Request({{"command","dump"}});
        const auto drop=snapshot["lastDrop"];
        if(!drop["valid"].get<bool>())
            throw std::runtime_error("the drop was not committed; the desktop is in use");
        // 落点不变式按**应用实际看到的**光标结算；再确认它就是测试瞄准的点，
        // 否则说明拖动期间指针被外部移走（SKIP），不拿它给产品判罪。
        const int dropCursorX=drop["cursor"][0].get<int>(),dropCursorY=drop["cursor"][1].get<int>();
        if(std::abs(dropCursorX-dropX)>2||std::abs(dropCursorY-dropY)>2)
            throw std::runtime_error("the pointer moved during the drag; the desktop is in use");
        const int expectedX=dropCursorX-(startDpi?grabX*otherDpi/startDpi:grabX);
        const int expectedY=dropCursorY-(startDpi?grabY*otherDpi/startDpi:grabY);
        if(snapshot["dpi"].get<int>()!=otherDpi||snapshot["state"]!=1||
            std::abs(snapshot["panel"][0].get<int>()-expectedX)>1||std::abs(snapshot["panel"][1].get<int>()-expectedY)>1) {
            std::cerr<<"diagnostic: state="<<snapshot["state"]<<" dpi="<<snapshot["dpi"]<<" panel="<<snapshot["panel"]
                <<" dipExpected=["<<expectedX<<","<<expectedY<<","<<scaledW<<","<<scaledH<<"]"
                <<" monitor="<<snapshot["monitor"]<<" lastDrop="<<snapshot["lastDrop"]
                <<" writes="<<snapshot["writes"]<<"/"<<writesBeforeDrag
                <<" cursor=("<<harness::Pointer::Position().x<<","<<harness::Pointer::Position().y<<")\n";
            for(const auto& event:snapshot["events"]) std::cerr<<"  "<<event["tick"]<<" "<<event["name"]
                <<" state="<<event["state"]<<" at ("<<event["x"]<<","<<event["y"]<<")\n";
        }
        harness::Require(snapshot["dpi"].get<int>()==otherDpi,"cross-DPI drop did not adopt the target monitor DPI");
        harness::Require(snapshot["state"]==1,"cross-DPI drop did not end floating");
        harness::Require(snapshot["panel"][2].get<int>()==scaledW&&snapshot["panel"][3].get<int>()==scaledH,
            "physical size after the DPI change is not MulDiv(tier,dpi,96)");
        harness::Require(std::abs(snapshot["panel"][0].get<int>()-expectedX)<=1&&std::abs(snapshot["panel"][1].get<int>()-expectedY)<=1,
            "cross-DPI drop does not keep the documented DIP-relative grab offset");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBeforeDrag+1,
            "cross-DPI drop must stay a single absolute write");
        std::cout<<"PASS real cross-DPI drag: size scales with the new DPI, one write, exact drop\n";

        snapshot=client.Request({{"command","menu"},{"id",1002}});
        harness::Require(snapshot["state"]==4&&snapshot["edge"]!=0,"dock on the second monitor failed");
        const auto dockedPanel=snapshot["panel"],dockedStrip=snapshot["strip"];
        const auto writesDocked=snapshot["writes"].get<unsigned>();
        const auto workAfterDock=snapshot["work"];
        const int edge=snapshot["edge"].get<int>();
        const int flush=dockedPanel[0].get<int>();
        harness::Require((edge==1&&flush==workAfterDock[0].get<int>())||edge==2||edge==3||edge==4,
            "docked panel is not flush with the work area on the second monitor");
        std::cout<<"PASS docked state established on the second monitor\n";

        // External interference must NOT be self-healed: the model is authoritative, there is no watchdog.
        RECT interference{dockedPanel[0].get<int>()+40,dockedPanel[1].get<int>()+400,
            dockedPanel[0].get<int>()+40+dockedPanel[2].get<int>(),dockedPanel[1].get<int>()+400+dockedPanel[3].get<int>()};
        SetWindowPos(panelHwnd,nullptr,interference.left,interference.top,
            interference.right-interference.left,interference.bottom-interference.top,SWP_NOACTIVATE|SWP_NOZORDER);
        Sleep(700);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["panel"][0].get<int>()==interference.left,
            "a watchdog moved the window back: the model must never self-heal");
        harness::Require(snapshot["writes"].get<unsigned>()==writesDocked,"interference caused a geometry write");
        std::cout<<"PASS external move is not self-healed (no watchdog, no periodic writes)\n";

        SendMessageW(panelHwnd,WM_SETTINGCHANGE,SPI_SETWORKAREA,0);
        Sleep(250);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["panel"]==snapshot["expectedPanel"]&&snapshot["strip"]==snapshot["expectedStrip"],
            "SPI_SETWORKAREA did not restore the docked rects from the model");
        harness::Require(snapshot["panel"]==dockedPanel&&snapshot["strip"]==dockedStrip,
            "docked rects changed although the work area did not");
        harness::Require(snapshot["writes"].get<unsigned>()-writesDocked==2,
            "docked topology refresh must be exactly two writes");
        std::cout<<"PASS SPI_SETWORKAREA re-places the docked pair from the model in two writes\n";

        const auto writesAfterSetting=snapshot["writes"].get<unsigned>();
        SendMessageW(panelHwnd,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080));
        Sleep(250);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["panel"]==snapshot["expectedPanel"]&&snapshot["strip"]==snapshot["expectedStrip"],
            "WM_DISPLAYCHANGE left the docked pair off the model rects");
        harness::Require(snapshot["writes"].get<unsigned>()-writesAfterSetting==2,
            "WM_DISPLAYCHANGE refresh must be exactly two writes");
        std::cout<<"PASS WM_DISPLAYCHANGE refreshes the docked pair from the model\n";

        snapshot=client.Request({{"command","menu"},{"id",1003}});
        harness::Require(snapshot["state"]==1,"undock before the floating topology check failed");
        const auto floatPanel=snapshot["panel"],floatStrip=snapshot["strip"];
        const auto writesFloating=snapshot["writes"].get<unsigned>();
        SendMessageW(panelHwnd,WM_SETTINGCHANGE,SPI_SETWORKAREA,0);
        SendMessageW(panelHwnd,WM_DISPLAYCHANGE,32,MAKELPARAM(1920,1080));
        Sleep(300);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["panel"]==floatPanel&&snapshot["strip"]==floatStrip,
            "a floating panel moved on a topology event");
        harness::Require(snapshot["writes"].get<unsigned>()==writesFloating,"floating topology events wrote geometry");
        harness::Require(snapshot["violations"]==0,"dpi/topology slice recorded invariant violations");
        std::cout<<"PASS floating panel ignores topology events with zero writes\n";
        std::cout<<"dpi: 7 passed, 0 failed\n";
        status=0;
        client.Request({{"command","quit"}});
        WaitForSingleObject(child.hProcess,3000);
    }catch(const std::exception& error){
        // 桌面有人在用时合成鼠标会被抢走：环境属性，按 SKIP 处理（见 harness::IsEnvironmentInterference）。
        if(harness::IsEnvironmentInterference(error)){
            std::cerr<<"SKIP dpi: "<<error.what()<<'\n';
            // SKIP 也必须收掉子进程：留下的顶层面板会盖住下一次测试的点击点。
            TerminateProcess(child.hProcess,77);
            WaitForSingleObject(child.hProcess,1500);
            return 77;
        }
        std::cerr<<"FAIL dpi: "<<error.what()<<'\n';
    }
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return status;
}
