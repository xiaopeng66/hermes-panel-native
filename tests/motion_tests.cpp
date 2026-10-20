// 动效门禁：验证移植自 balance-panel-native 的动画框架真的接在状态迁移上，
// 而且动画结束后计时器会停、关掉动效后进度恒为 1（门禁确定性模式）。
// 只断言控制面暴露的时间戳/计时器状态，不做像素比较：中间帧取决于调度，像素断言会 flaky。
#include "harness/client.h"
#include "harness/paths.h"
#include "harness/no_modal_asserts.h"
#include <iostream>
#include <string>

namespace {
using harness::Json;
Json Dump(harness::Client& client) { return client.Request({{"command","dump"}}); }
unsigned long long Motion(harness::Client& client,const char* key) {
    return Dump(client)["motion"][key].get<unsigned long long>();
}
void WaitMotionDone(harness::Client& client,const char* key,const char* name) {
    // 最长动画 180ms（主题）：等 400ms 足以让 16ms 计时器把 start 清零。
    for(int attempt=0;attempt<40;++attempt) {
        if(Motion(client,key)==0) return;
        Sleep(20);
    }
    throw std::runtime_error(std::string(name)+" motion did not finish");
}
void WaitTimerIdle(harness::Client& client,const char* name) {
    // 同一个 16ms 计时器服务所有动画：一个动画刚结束的那一帧，另一个可能还在跑
    // （真实光标停在面板上会带出 hover 过渡）。断言"最终会停"，而不是"此刻已停"。
    for(int attempt=0;attempt<60;++attempt) {
        if(!Dump(client)["motion"]["timer"].get<bool>()) return;
        Sleep(20);
    }
    throw std::runtime_error(std::string(name)+": the motion timer never stopped");
}
}
int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if(argc!=2) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"motion-"+std::to_wstring(GetCurrentProcessId());
    SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) return 3;
    auto status=1;
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        auto snapshot=Dump(client);
        harness::Require(snapshot["state"]==1,"expected floating start");
        harness::Require(snapshot["motion"]["enabled"].get<bool>()==false,
            "the harness must start in deterministic (no-animation) mode");

        // 1) 隐藏 → 显示：面板从 Hidden 出现时走入场动效（alpha 0→1 + 8 DIP 上滑）。
        client.Request({{"command","hide"}});
        harness::Require(Dump(client)["state"]==0,"hide did not reach Hidden");
        snapshot=client.Request({{"command","show"}});
        harness::Require(snapshot["state"]==1,"show did not reach Floating");
        harness::Require(snapshot["motion"]["enabled"].get<bool>()==false,
            "configure animations=false was not honoured");
        // 确定性模式下不应留下任何 start：上面的 show 触发了 StartPanelMotion，但它必须被关掉。
        harness::Require(snapshot["motion"]["panelStart"].get<unsigned long long>()==0,
            "animations=false still started a panel motion");
        harness::Require(snapshot["motion"]["timer"].get<bool>()==false,
            "animations=false still armed the motion timer");

        // 2) 打开动效后，show 必须启动入场，并在 140ms 后由 MotionTick 收尾。
        client.Request({{"command","configure"},{"animations",true}});
        client.Request({{"command","hide"}});
        snapshot=client.Request({{"command","show"}});
        harness::Require(snapshot["motion"]["enabled"].get<bool>()==true,"animations were not enabled");
        harness::Require(snapshot["motion"]["panelStart"].get<unsigned long long>()!=0,
            "panel entrance did not start on show");
        harness::Require(snapshot["motion"]["timer"].get<bool>()==true,
            "panel entrance did not arm the 16ms motion timer");
        WaitMotionDone(client,"panelStart","panel entrance");
        WaitTimerIdle(client,"panel entrance");

        // 3) 贴边收起：贴条淡入（110ms）。
        client.Request({{"command","menu"},{"id",1002}});   // MenuCommand::Dock
        snapshot=Dump(client);
        harness::Require(snapshot["state"]==4&&snapshot["stripVisible"].get<bool>(),
            "dock did not collapse to the strip");
        harness::Require(snapshot["motion"]["stripStart"].get<unsigned long long>()!=0,
            "strip fade-in did not start on collapse");
        WaitMotionDone(client,"stripStart","strip fade-in");

        // 4) 悬浮卡：入场 120ms，退场 80ms（逻辑上立即隐藏，窗口留到淡出结束）。
        // 先确定性地回到收起态：真实桌面上的点击可能把贴条展开，门禁只测动效本身。
        client.Request({{"command","place"},{"state",4},{"edge",1},{"anchor",400}});
        Sleep(250);
        snapshot=client.Request({{"command","strip-card"},{"show",true}});
        harness::Require(snapshot["data"]["hoverCard"]["visible"].get<bool>(),
            "strip-card did not show the hover card");
        harness::Require(snapshot["motion"]["cardStart"].get<unsigned long long>()!=0,
            "hover card entrance did not start");
        WaitMotionDone(client,"cardStart","hover card entrance");
        // 重复的 300ms 计时器（鼠标在贴条上每动一下都会重新排）不能在已显示时重放入场：
        // 那会把 alpha 打回 0，卡片看起来一直在闪。
        snapshot=client.Request({{"command","strip-card"},{"show",true}});
        harness::Require(snapshot["motion"]["cardStart"].get<unsigned long long>()==0,
            "showing an already-visible hover card restarted its entrance animation");
        snapshot=client.Request({{"command","strip-card"},{"show",false}});
        harness::Require(!snapshot["data"]["hoverCard"]["visible"].get<bool>(),
            "hover card stayed logically visible after hide");
        harness::Require(snapshot["motion"]["cardExit"].get<unsigned long long>()!=0,
            "hover card exit fade did not start");
        WaitMotionDone(client,"cardExit","hover card exit");
        harness::Require(!Dump(client)["data"]["hoverCard"]["windowVisible"].get<bool>(),
            "hover card window was not hidden after its exit fade");

        // 5) 主题交叉淡入（180ms）。断言必须看**调色板颜色**，不能只看 light 布尔：
        //    宿系统主题恰好是浅色时 light 恒真，主题切换失效也能全绿（真机 2026-10-20 回归）。
        client.Request({{"command","configure"},{"theme","dark"}});
        WaitMotionDone(client,"themeStart","theme settle to dark");
        const auto darkPalette=Dump(client)["palette"];
        client.Request({{"command","configure"},{"theme","light"}});
        snapshot=Dump(client);
        harness::Require(snapshot["motion"]["themeStart"].get<unsigned long long>()!=0,
            "theme crossfade did not start");
        // 淡入过程中 palette 还是旧色（这是交叉淡入的定义）；收尾后必须落到浅色。
        WaitMotionDone(client,"themeStart","theme crossfade");
        const auto lit=Dump(client)["palette"];
        snapshot=Dump(client);
        harness::Require(snapshot["themeLight"].get<bool>(),"theme did not switch to light");
        harness::Require(lit["ink"].get<int>()!=darkPalette["ink"].get<int>()
            &&lit["surf"].get<int>()!=darkPalette["surf"].get<int>(),
            "the palette did not actually change on the light switch");
        client.Request({{"command","configure"},{"theme","dark"}});
        WaitMotionDone(client,"themeStart","theme crossfade back");
        const auto back=Dump(client)["palette"];
        harness::Require(back["ink"].get<int>()==darkPalette["ink"].get<int>()
            &&back["surf"].get<int>()==darkPalette["surf"].get<int>(),
            "switching back to dark did not restore the dark palette");
        harness::Require(lit["ink"].get<int>()!=back["ink"].get<int>(),
            "light and dark palettes are indistinguishable");

        // 6) 贴条点击展开：面板入场从收起方向也必须启动。
        client.Request({{"command","place"},{"state",4},{"edge",1},{"anchor",400}});
        Sleep(250);
        snapshot=client.Request({{"command","strip-tap"}});
        harness::Require(snapshot["state"]==3,"strip-tap did not expand the panel");
        harness::Require(snapshot["motion"]["panelStart"].get<unsigned long long>()!=0,
            "panel entrance did not start when expanding from the strip");
        WaitMotionDone(client,"panelStart","panel entrance from strip");

        std::cout<<"PASS motion: show/expand entrance, strip fade, card enter/exit, theme crossfade, "
            "timer stops, animations=false is deterministic\n";
        status=0;
    } catch(const std::exception& error) {
        if(harness::IsEnvironmentInterference(error)) {
            std::cout<<"SKIP motion: "<<error.what()<<"\n";
            status=77;
        } else {
            std::cerr<<"FAIL motion: "<<error.what()<<"\n";
        }
    }
    TerminateProcess(child.hProcess,0);
    WaitForSingleObject(child.hProcess,2000);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    return status;
}
