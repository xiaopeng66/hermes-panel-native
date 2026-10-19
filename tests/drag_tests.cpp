#include "harness/client.h"
#include "harness/paths.h"
#include "harness/input.h"
#include <iostream>
#include "harness/no_modal_asserts.h"
#include <string>

namespace {
using harness::Json;
int State(harness::Client& client) { return client.Request({{"command","dump"}})["state"].get<int>(); }
Json PanelOf(harness::Client& client) { return client.Request({{"command","dump"}})["panel"]; }
}
int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"drag-"+std::to_wstring(GetCurrentProcessId());
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) return 3;
    auto status=1;
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        harness::Pointer pointer;
        // 合成拖拽的前提是指针一直待在测试放的位置；桌面有人用时物理鼠标会中途把它带走，
        // 那是环境属性（SKIP），不能拿它给应用判失败。
        const auto requirePointerAt=[&](int x,int y,const char* where) {
            const auto now=harness::Pointer::Position();
            if(std::abs(now.x-x)>2||std::abs(now.y-y)>2)
                throw std::runtime_error(std::string("pointer drifted ")+where+" (at "
                    +std::to_string(now.x)+","+std::to_string(now.y)+" instead of "
                    +std::to_string(x)+","+std::to_string(y)+"); the desktop is in use");
        };
        // 应用在 WM_EXITSIZEMOVE 记录的光标才是落点判定的输入：它在松手后才被采样，
        // 能抓住"按下/松手之间那 120ms 里被物理鼠标带走"的情况。
        const auto requireDropAt=[&](const Json& value,int x,int y,const char* where) {
            const auto drop=value["lastDrop"];
            const auto cursor=drop["cursor"];
            if(!drop["valid"].get<bool>()||std::abs(cursor[0].get<int>()-x)>2
                ||std::abs(cursor[1].get<int>()-y)>2)
                throw std::runtime_error(std::string("the recorded drop point drifted ")+where
                    +" (got "+std::to_string(cursor[0].get<int>())+","+std::to_string(cursor[1].get<int>())
                    +" expected "+std::to_string(x)+","+std::to_string(y)+"); the desktop is in use");
        };
        auto snapshot=client.Request({{"command","dump"}});
        const int workLeft=snapshot["work"][0].get<int>(),workTop=snapshot["work"][1].get<int>();
        const int workRight=workLeft+snapshot["work"][2].get<int>(),workBottom=workTop+snapshot["work"][3].get<int>();
        const int centerX=(workLeft+workRight)/2,centerY=(workTop+workBottom)/2;
        harness::Require(snapshot["state"]==1,"expected floating start");

        // Drag 1: title bar to open desktop, expect the exact drop point to be kept.
        // 落点要保证**整块面板都在工作区内**：贴边判定只看窗口（真机要求），窗口越过某条边就会贴到
        // 那条边 —— 那正是"窗口碰到边缘"的意思。所以这里把落差夹到能放下的范围里。
        auto panel=snapshot["panel"];
        const int grabX=60,grabY=14;
        const int spanW=panel[2].get<int>(),spanH=panel[3].get<int>();
        const int openDropX=std::clamp(centerX+120,workLeft+grabX,workRight-grabX-spanW);
        const int openDropY=std::clamp(centerY+90,workTop+grabY,workBottom-grabY-spanH);
        pointer.MoveTo(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY);
        Sleep(120);
        pointer.Button(true);
        requirePointerAt(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY,"at press");
        Sleep(180);
        const auto writesBefore=client.Request({{"command","dump"}})["writes"].get<unsigned>();
        pointer.MoveTowards(openDropX,openDropY,10);
        requirePointerAt(openDropX,openDropY,"during the drag");
        const auto midDrag=client.Request({{"command","dump"}});
        harness::Require(midDrag["state"]==2,"system move loop did not put the app into S2");
        harness::Require(midDrag["writes"].get<unsigned>()==writesBefore,"geometry was written during the drag (INV2)");
        pointer.MoveTo(openDropX,openDropY);
        requirePointerAt(openDropX,openDropY,"before the drop");
        Sleep(120);
        pointer.Button(false);
        Sleep(300);
        snapshot=client.Request({{"command","dump"}});
        requireDropAt(snapshot,openDropX,openDropY,"during the open-desktop drop");
        harness::Require(snapshot["state"]==1,"open-desktop drop did not return to floating");
        if(snapshot["panel"][0].get<int>()!=openDropX-grabX||snapshot["panel"][1].get<int>()!=openDropY-grabY) {
            const auto cursor=harness::Pointer::Position();
            std::cerr<<"diagnostic: state="<<snapshot["state"]<<" panel="<<snapshot["panel"]
                <<" expected=["<<(openDropX-grabX)<<","<<(openDropY-grabY)<<"]"
                <<" float="<<snapshot["float"]<<" dpi="<<snapshot["dpi"]
                <<" lastDrop="<<snapshot["lastDrop"]
                <<" cursor=("<<cursor.x<<","<<cursor.y<<") target=("<<openDropX<<","<<openDropY<<")\n";
        }
        harness::Require(snapshot["panel"][0].get<int>()==openDropX-grabX&&snapshot["panel"][1].get<int>()==openDropY-grabY,
            "floating drop position does not match the release point");
        harness::Require(snapshot["float"][0].get<int>()==snapshot["panel"][0].get<int>()&&snapshot["float"][1].get<int>()==snapshot["panel"][1].get<int>(),
            "floating model origin not persisted from the drop");
        harness::Require(snapshot["writes"].get<unsigned>()==writesBefore+1,"floating drop must be exactly one absolute write");
        std::cout<<"PASS drag on open desktop: S2 with zero writes, drop kept exactly, one write on release\n";

        // Drag 2: title bar into the snap band of the left work-area edge.
        panel=snapshot["panel"];
        pointer.MoveTo(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY);
        Sleep(150);
        pointer.Button(true);
        requirePointerAt(panel[0].get<int>()+grabX,panel[1].get<int>()+grabY,"at press");
        Sleep(200);
        const auto writesBeforeDock=client.Request({{"command","dump"}})["writes"].get<unsigned>();
        const int dockTargetY=workTop+220+grabY;
        pointer.MoveTowards(workLeft+4+grabX,dockTargetY,14);
        pointer.MoveTo(workLeft+4+grabX,dockTargetY);
        requirePointerAt(workLeft+4+grabX,dockTargetY,"before the drop");
        Sleep(260);
        pointer.Button(false);
        Sleep(360);
        snapshot=client.Request({{"command","dump"}});
        requireDropAt(snapshot,workLeft+4+grabX,dockTargetY,"during the snap drop");
        if(snapshot["edge"]!=1||snapshot["anchor"].get<int>()!=workTop+220) {
            const auto cursor=harness::Pointer::Position();
            std::cerr<<"diagnostic: state="<<snapshot["state"]<<" edge="<<snapshot["edge"]
                <<" panel="<<snapshot["panel"]<<" expectedPanel="<<snapshot["expectedPanel"]
                <<" anchor="<<snapshot["anchor"]<<" work="<<snapshot["work"]<<" dpi="<<snapshot["dpi"]
                <<" writes="<<snapshot["writes"]<<"/"<<writesBeforeDock
                <<" cursor=("<<cursor.x<<","<<cursor.y<<") target=("<<workLeft+4+grabX<<","<<dockTargetY<<")"
                <<" lastDrop="<<snapshot["lastDrop"]<<"\n";
        }
        harness::Require(snapshot["edge"]==1,"drag into the left snap band did not dock left");
        harness::Require(snapshot["state"]==4&&snapshot["stripVisible"]==true,"docked-left state wrong after snap");
        harness::Require(snapshot["panel"][0].get<int>()==workLeft,"INV4: docked panel not flush with the left work-area edge");
        harness::Require(snapshot["strip"][0].get<int>()==workLeft,"INV4: strip not flush with the left work-area edge");
        harness::Require(snapshot["panel"]==snapshot["expectedPanel"]&&snapshot["strip"]==snapshot["expectedStrip"],
            "snap result differs from the pure SnapDecision model");
        harness::Require(snapshot["anchor"].get<int>()==workTop+220,"anchor was not taken from the drop point (unclamped case)");
        harness::Require(snapshot["writes"].get<unsigned>()-writesBeforeDock==2,"snap to edge must be exactly two writes");
        std::cout<<"PASS drag into snap band: docks flush to the edge with two writes and model-equal rects\n";

        // Drag 3: hover the strip to expand, then pull the panel out of the collapsed dock.
        // G1 起挂件不再占满整条边：悬停点取挂件自己的中心。
        const auto stripPoint=Json::array({snapshot["strip"][0].get<int>()+snapshot["strip"][2].get<int>()/2,
            snapshot["strip"][1].get<int>()+snapshot["strip"][3].get<int>()/2});
        pointer.MoveTo(stripPoint[0].get<int>(),stripPoint[1].get<int>());
        Sleep(250);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==4,"hovering the strip must not expand before the pull-out drag");
        {
            const auto stripHwnd=harness::Window(snapshot,"stripHwnd");
            const POINT point{stripPoint[0].get<int>(),stripPoint[1].get<int>()};
            if(WindowFromPoint(point)!=stripHwnd)
                throw std::runtime_error("the strip is covered by another window; the desktop is in use");
        }
        pointer.Button(true);
        Sleep(60);
        pointer.Button(false);
        requirePointerAt(stripPoint[0].get<int>(),stripPoint[1].get<int>(),"after the strip click");
        Sleep(180);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==3,"clicking the strip did not expand before the pull-out drag");
        const auto innerBandX=workLeft+5;
        pointer.MoveTo(innerBandX,stripPoint[1].get<int>());
        Sleep(120);
        pointer.Button(true);
        requirePointerAt(innerBandX,stripPoint[1].get<int>(),"at press");
        Sleep(160);
        const auto writesBeforePull=client.Request({{"command","dump"}})["writes"].get<unsigned>();
        pointer.MoveTowards(centerX-150,centerY-120,12);
        requirePointerAt(centerX-150,centerY-120,"during the drag");
        const auto pullMid=client.Request({{"command","dump"}});
        harness::Require(pullMid["state"]==2,"pull-out drag did not enter S2");
        harness::Require(pullMid["writes"].get<unsigned>()==writesBeforePull,"pull-out wrote geometry during the drag (INV2)");
        pointer.MoveTo(centerX-150,centerY-120);
        requirePointerAt(centerX-150,centerY-120,"before the drop");
        Sleep(140);
        pointer.Button(false);
        Sleep(320);
        snapshot=client.Request({{"command","dump"}});
        requireDropAt(snapshot,centerX-150,centerY-120,"during the pull-out drop");
        harness::Require(snapshot["state"]==1&&snapshot["edge"]==0,"pull-out drag did not end floating");
        harness::Require(snapshot["stripVisible"]==false&&snapshot["panelVisible"]==true,"pull-out visibility wrong");
        harness::Require(snapshot["panel"][0].get<int>()==centerX-150-5,"pull-out drop position does not match the release point");
        harness::Require(snapshot["writes"].get<unsigned>()-writesBeforePull==1,"pull-out release must be exactly one write");
        std::cout<<"PASS pull-out from collapsed dock: no re-snap, exact drop, one write\n";

        // Drag 4: bottom-edge height resize (fixed height is user-adjustable; one commit write).
        {
            const auto before=client.Request({{"command","dump"}});
            const auto rect=before["panel"];
            const int dpi=before["dpi"].get<int>();
            const int left=rect[0].get<int>(),top=rect[1].get<int>(),width=rect[2].get<int>();
            const int oldH=rect[3].get<int>();
            const int edgeX=left+width/2,edgeY=top+oldH-2;
            const auto writes=before["writes"].get<unsigned>();
            pointer.MoveTo(edgeX,edgeY);
            requirePointerAt(edgeX,edgeY,"at the resize handle");
            Sleep(120);
            pointer.Button(true);
            Sleep(160);
            const int targetY=top+oldH-120-2;
            pointer.MoveTowards(edgeX,targetY,10);
            pointer.MoveTo(edgeX,targetY);
            requirePointerAt(edgeX,targetY,"before the resize release");
            Sleep(160);
            pointer.Button(false);
            Sleep(350);
            const auto resized=client.Request({{"command","dump"}});
            const int newH=resized["panel"][3].get<int>();
            const int expectedDip=(newH*96+dpi/2)/dpi;
            if(std::abs(newH-(oldH-120))>2) {
                std::cerr<<"diagnostic: resize oldH="<<oldH<<" newH="<<newH
                    <<" panel="<<resized["panel"]<<" stored="<<resized["stored"]<<"\n";
            }
            harness::Require(std::abs(newH-(oldH-120))<=2,"bottom-edge resize did not follow the pointer");
            harness::Require(resized["panel"][0].get<int>()==left&&resized["panel"][1].get<int>()==top
                &&resized["panel"][2].get<int>()==width,"resize moved the top-left or changed the width");
            harness::Require(resized["stored"]["heightDip"].get<int>()==expectedDip,
                "resized height was not persisted in DIP");
            harness::Require(resized["writes"].get<unsigned>()==writes+1,
                "resize must commit with exactly one absolute write");
            // Restore: drag the edge back down.
            const int restoreY=top+newH-2,restoreTarget=top+oldH-2;
            pointer.MoveTo(edgeX,restoreY);
            requirePointerAt(edgeX,restoreY,"at the resize handle (restore)");
            Sleep(120);
            pointer.Button(true);
            Sleep(160);
            pointer.MoveTowards(edgeX,restoreTarget,10);
            pointer.MoveTo(edgeX,restoreTarget);
            requirePointerAt(edgeX,restoreTarget,"before the restore release");
            Sleep(160);
            pointer.Button(false);
            Sleep(350);
            const auto restored=client.Request({{"command","dump"}});
            harness::Require(std::abs(restored["panel"][3].get<int>()-oldH)<=2,"restoring the height failed");
            std::cout<<"PASS bottom-edge resize: follows the pointer, persists in DIP, one write\n";
        }

        // Drag 5: strip drag keeps the small widget, never expands, then snaps to the nearest edge.
        {
            // Deterministic part first (no real pointer): control-plane strip-drag to the work centre.
            const auto docked=client.Request({{"command","menu"},{"id",1002}});
            harness::Require(docked["state"]==4,"strip drag: dock did not collapse into the strip");
            const int centreX=workLeft+(workRight-workLeft)/2;
            const int centreY=workTop+(workBottom-workTop)/2;
            const auto stripWrites=docked["writes"].get<unsigned>();
            const auto synth=client.Request({{"command","strip-drag"},{"x",centreX},{"y",centreY}});
            harness::Require(synth["state"]==4&&synth["panelVisible"]==false&&synth["stripVisible"]==true,
                "synthetic strip drag expanded the panel");
            const int leftDistance=centreX-workLeft,rightDistance=workRight-centreX;
            const int topDistance=centreY-workTop,bottomDistance=workBottom-centreY;
            int expectedEdge=1,nearest=leftDistance;
            if(rightDistance<nearest) { nearest=rightDistance;expectedEdge=2; }
            if(topDistance<nearest) { nearest=topDistance;expectedEdge=3; }
            if(bottomDistance<nearest) { nearest=bottomDistance;expectedEdge=4; }
            harness::Require(synth["edge"].get<int>()==expectedEdge,
                "synthetic strip drag did not snap to the nearest edge");
            const auto synthStrip=synth["strip"];
            const bool synthFlush=expectedEdge==1?synthStrip[0].get<int>()==workLeft
                :expectedEdge==2?synthStrip[0].get<int>()+synthStrip[2].get<int>()==workRight
                :expectedEdge==3?synthStrip[1].get<int>()==workTop
                :synthStrip[1].get<int>()+synthStrip[3].get<int>()==workBottom;
            harness::Require(synthFlush,"synthetic strip drag is not flush with the edge");
            harness::Require(synth["writes"].get<unsigned>()==stripWrites+2,
                "synthetic strip drag must place panel+strip once");
            std::cout<<"PASS strip drag (control plane): centre drop snaps to the nearest edge\n";
            // 悬浮信息卡：显示在贴条内侧、尺寸/位置正确；300ms 计时器路径也能打开。
            {
                const int dpi=synth["dpi"].get<int>();
                const auto strip=synth["strip"];
                auto shown=client.Request({{"command","strip-card"},{"show",true}});
                auto card=shown["data"]["hoverCard"];
                harness::Require(card["visible"].get<bool>(),"the strip hover card did not show");
                const auto window=card["window"];
                const int expectedHeight=card["note"].get<std::string>().empty()?146:164;
                harness::Require(window[2].get<int>()==MulDiv(252,dpi,96)
                    &&window[3].get<int>()==MulDiv(expectedHeight,dpi,96),
                    "the hover card has the wrong size");
                harness::Require(window[0].get<int>()>=workLeft&&window[1].get<int>()>=workTop
                    &&window[0].get<int>()+window[2].get<int>()<=workRight
                    &&window[1].get<int>()+window[3].get<int>()<=workBottom,
                    "the hover card is outside the work area");
                if(expectedEdge==3)
                    harness::Require(window[1].get<int>()==strip[1].get<int>()+strip[3].get<int>()
                        +MulDiv(8,dpi,96),"the top-edge card is not below the strip");
                const auto rows=card["rows"];
                harness::Require(rows.size()==3&&rows[0]["label"].get<std::string>()=="WSL"
                    &&rows[2]["label"].get<std::string>()=="网关",
                    "the hover card does not list the three services");
                client.Request({{"command","strip-card"},{"show",false}});
                harness::Require(!client.Request({{"command","dump"}})["data"]["hoverCard"]["visible"].get<bool>(),
                    "the hover card did not hide");
                client.Request({{"command","strip-hover"}});
                Sleep(420);
                harness::Require(client.Request({{"command","dump"}})["data"]["hoverCard"]["visible"].get<bool>(),
                    "the 300ms hover timer did not open the card");
                client.Request({{"command","strip-card"},{"show",false}});
                std::cout<<"PASS strip hover card: positioned beside the widget, timer opens it\n";
            }
            const auto floating=client.Request({{"command","menu"},{"id",1003}});
            harness::Require(floating["state"]==1,"strip drag test could not undock for the real-pointer pass");
        }
        {
            const auto docked=client.Request({{"command","menu"},{"id",1002}});
            harness::Require(docked["state"]==4,"strip drag: dock did not collapse into the strip");
            const auto strip=docked["strip"];
            const int stripX=strip[0].get<int>()+strip[2].get<int>()/2;
            const int stripY=strip[1].get<int>()+strip[3].get<int>()/2;
            const auto stripWritesBefore=docked["writes"].get<unsigned>();
            pointer.MoveTo(stripX,stripY);
            requirePointerAt(stripX,stripY,"at the strip");
            Sleep(120);
            pointer.Button(true);
            requirePointerAt(stripX,stripY,"at the strip press");
            Sleep(120);
            const int targetX=workLeft+(workRight-workLeft)/2;
            const int targetY=workTop+(workBottom-workTop)/2;
            pointer.MoveTowards(targetX,targetY,12);
            pointer.MoveTo(targetX,targetY);
            requirePointerAt(targetX,targetY,"during the strip drag");
            Sleep(120);
            const auto mid=client.Request({{"command","dump"}});
            harness::Require(mid["state"]==4&&mid["panelVisible"]==false&&mid["stripVisible"]==true,
                "dragging the strip expanded the panel or hid the widget");
            pointer.Button(false);
            Sleep(360);
            const auto dropped=client.Request({{"command","dump"}});
            requirePointerAt(targetX,targetY,"after the strip drop");
            harness::Require(dropped["state"]==4&&dropped["panelVisible"]==false
                &&dropped["stripVisible"]==true,"strip drop did not stay collapsed");
            const int leftDistance=targetX-workLeft,rightDistance=workRight-targetX;
            const int topDistance=targetY-workTop,bottomDistance=workBottom-targetY;
            int expectedEdge=1,nearest=leftDistance;
            if(rightDistance<nearest) { nearest=rightDistance;expectedEdge=2; }
            if(topDistance<nearest) { nearest=topDistance;expectedEdge=3; }
            if(bottomDistance<nearest) { nearest=bottomDistance;expectedEdge=4; }
            harness::Require(dropped["edge"].get<int>()==expectedEdge,
                "strip drop did not snap to the nearest edge");
            const auto placed=dropped["strip"];
            const bool flush=expectedEdge==1?placed[0].get<int>()==workLeft
                :expectedEdge==2?placed[0].get<int>()+placed[2].get<int>()==workRight
                :expectedEdge==3?placed[1].get<int>()==workTop
                :placed[1].get<int>()+placed[3].get<int>()==workBottom;
            harness::Require(flush,"the snapped strip is not flush with the edge");
            harness::Require(dropped["writes"].get<unsigned>()==stripWritesBefore+2,
                "strip drop must place panel+strip exactly once");
            // The click still expands after a drag (drag and click share the same press).
            const int newX=placed[0].get<int>()+placed[2].get<int>()/2;
            const int newY=placed[1].get<int>()+placed[3].get<int>()/2;
            pointer.MoveTo(newX,newY);
            requirePointerAt(newX,newY,"at the snapped strip");
            Sleep(120);
            pointer.Button(true);
            Sleep(60);
            pointer.Button(false);
            Sleep(220);
            harness::Require(client.Request({{"command","dump"}})["state"].get<int>()==3,
                "a click on the strip no longer expands after a drag");
            std::cout<<"PASS strip drag: widget-only move, nearest-edge snap, click still expands\n";
        }
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["violations"]==0,"drag slice recorded invariant violations");
        std::cout<<"drag: 5 passed, 0 failed\n";
        status=0;
        client.Request({{"command","quit"}});
        WaitForSingleObject(child.hProcess,3000);
    }catch(const std::exception& error){
        if(harness::IsEnvironmentInterference(error)){
            std::cerr<<"SKIP drag: "<<error.what()<<"\n";
            TerminateProcess(child.hProcess,77);
            WaitForSingleObject(child.hProcess,1500);
            return 77;
        }
        std::cerr<<"FAIL drag: "<<error.what()<<"\n";
    }
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return status;
}
