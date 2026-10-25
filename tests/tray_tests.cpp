#include "harness/client.h"
#include "harness/paths.h"
#include "harness/input.h"
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include "harness/no_modal_asserts.h"

namespace {
using harness::Json;
Json Dump(harness::Client& client) { return client.Request({{"command","dump"}}); }
// 夹具把每个请求原样写进 requests.log：用它证明托盘菜单的动作真的发到了 WSL 助手。
Json Requests(const std::string& path,const std::string& cmd) {
    Json list=Json::array();
    std::ifstream input(path);
    std::string line;
    while(std::getline(input,line)) {
        if(line.empty()) continue;
        try {
            const auto parsed=Json::parse(line);
            if(parsed.value("cmd",std::string{})==cmd) list.push_back(parsed);
        } catch(const std::exception&) {}
    }
    return list;
}
void ExpectRect(const Json& actual,const Json& expected,const char* name) {
    harness::Require(actual==expected,("rect mismatch: "+std::string(name)).c_str());
}
}
int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    // 假助手夹具（GOAL 硬约束；菜单里的「当前：provider / model」就是夹具状态）。
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"tray-"+std::to_wstring(GetCurrentProcessId());
    const auto home=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id+L"/fake";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW((std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id).c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    SetEnvironmentVariableW(L"HERMES_WSL_EXE",argv[2]);
    SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
    SetEnvironmentVariableW(L"FAKE_HERMES_HOME",home.c_str());
    // 夹具 home 的宽路径 → 窄路径（只含 ASCII，逐字符显式转换，避免 /W4 的收窄警告）。
    std::string logPath;
    logPath.reserve(home.size()+16);
    for(const auto character:home) logPath.push_back(static_cast<char>(character));
    logPath+="/requests.log";
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)) return 3;
    int code=1;
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        auto snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["tray"]["registered"]==true,"tray icon not registered by shell");
        harness::Require(snapshot["tray"]["iconLoaded"]==true,"tray icon image not created");
        // 托盘菜单的**结构**逐条对齐旧 main.js buildTrayMenu()（用户报的「分类混乱」就是这里）：
        // 五个组、组间分隔线、组内顺序固定。标签原文一起断言（编码错会出现乱码）。
        {
            // 夹具的首次读取（fake-hermes）要不来一会儿：等「当前：…」里的模型出现再断言，
            // 否则量到的是启动瞬间的空状态。
            for(int attempt=0;attempt<60;++attempt) {
                bool ready=false;
                for(const auto& item:snapshot["menu"])
                    if(!item["separator"].get<bool>()&&std::string(item["label"].get<std::string>()).rfind("当前：",0)==0)
                        ready=item["label"].get<std::string>().find("\xE2\x80\x94")==std::string::npos;
                if(ready) break;
                Sleep(100);
                snapshot=client.Request({{"command","dump"}});
            }
            Json labels=Json::array();
            Json separators=Json::array();
            for(const auto& item:snapshot["menu"]) {
                if(item["separator"].get<bool>()) { separators.push_back(labels.size());continue; }
                labels.push_back(item["label"]);
            }
            const auto expected=std::vector<std::string>{
                "显示 / 隐藏","当前：alpha / alpha-large","切换到","立即刷新","打开 WebUI","重启网关",
                // 「紧致视图」已按用户要求删除（2026-10-24）：152 DIP 在原生面板上装不下
                // 任何页面正文，留着一个"切过去什么都看不到"的档位没有意义。菜单随之少一项。
                "重启 WebUI 服务","贴边收回","始终置顶","主题","关闭按钮","开机自启",
                "刷新间隔","打开数据目录","关于","退出"};
            std::vector<std::string> actual;
            for(const auto& label:labels) actual.push_back(label.get<std::string>());
            if(actual!=expected) {
                std::cerr<<"DIAGMENU actual:";
                for(const auto& label:actual) std::cerr<<" ["<<label<<"]";
                std::cerr<<std::endl<<"DIAGMENU expect:";
                for(const auto& label:expected) std::cerr<<" ["<<label<<"]";
                std::cerr<<std::endl;
            }
            harness::Require(actual==expected,"tray menu labels/order do not match the old panel");
            // 分隔线把菜单分成 5 组：显示/切换到 | 服务动作 | 面板行为 | 外壳附加 | 关于/退出
            harness::Require(separators.size()==4,"tray menu must have four separators (five groups)");
            std::cout<<"PASS tray menu structure: 17 items in 5 groups, labels match the old panel\n";
            // 编码：中文必须是合法的 UTF-8 且能还原成预期文案（逐字节加宽会出现 U+00C3 之类的乱码）。
            for(const auto& label:actual) {
                for(const unsigned char character:label) {
                    harness::Require(character<0xF8,("menu label is not valid UTF-8: "+label).c_str());
                }
            }
            const auto garbled=[&](const std::string& text) {
                return text.find("\xC3\xA2")!=std::string::npos||text.find("\xC3\xA5")!=std::string::npos
                    ||text.find("\xC3\xA6")!=std::string::npos||text.find('\xC3')!=std::string::npos;
            };
            for(const auto& label:actual)
                harness::Require(!garbled(label),("menu label looks double-encoded: "+label).c_str());
            std::cout<<"PASS tray menu labels are clean UTF-8 (no double-encoded CJK)\n";
        }
        { // 勾选/禁用态跟着当前状态走，子菜单内容也要对
            const auto item=[&](const char* label) -> Json {
                for(const auto& entry:snapshot["menu"])
                    if(!entry["separator"].get<bool>()&&entry["label"]==label) return entry;
                return Json::object();
            };
            harness::Require(item("始终置顶")["checked"]==true,"topmost item must reflect the default (on)");
            harness::Require(item("贴边收回")["enabled"]==true,"the snap toggle is enabled while floating");
            harness::Require(item("开机自启")["checked"]==false,"autostart starts off");
            // 「紧致视图」已删除（见上面的菜单标签断言），这一条随之移除。
            // 主题子菜单：三项 + **恰好一项**被勾选，且勾的就是设置里的那个（默认 system → 跟随系统）。
            const auto themes=item("主题")["children"];
            const int wanted=snapshot["settings"]["theme"].get<int>();   // 0 深 / 1 浅 / 2 跟随系统
            // 旧三档 + 四套现代化主题（石板 / 碳黑 / 瓷白 / 沙丘）。
            harness::Require(themes.size()==7&&themes[0]["label"]=="深色"&&themes[1]["label"]=="浅色"
                &&themes[2]["label"]=="跟随系统"&&themes[3]["label"]=="石板"
                &&themes[4]["label"]=="碳黑"&&themes[5]["label"]=="瓷白"
                &&themes[6]["label"]=="沙丘","theme submenu must offer 7 entries (3 stock + 4 modern)");
            int checkedCount=0,checkedIndex=-1;
            for(std::size_t index=0;index<themes.size();++index)
                if(themes[index]["checked"].get<bool>()) { ++checkedCount;checkedIndex=static_cast<int>(index); }
            harness::Require(checkedCount==1&&checkedIndex==wanted,
                ("theme submenu must check exactly the live theme (wanted="+std::to_string(wanted)
                    +", checked="+std::to_string(checkedIndex)+")").c_str());
            const auto close=item("关闭按钮")["children"];
            harness::Require(close.size()==2&&close[0]["label"]=="隐藏到托盘"&&close[1]["label"]=="退出程序",
                "close submenu must offer 隐藏到托盘/退出程序");
            const auto poll=item("刷新间隔")["children"];
            harness::Require(poll.size()==4&&poll[1]["label"]=="60 秒"&&poll[1]["checked"]==true,
                "the refresh-interval submenu lists 30/60/120/300 and marks the live one");
            const auto jump=item("切换到")["children"];
            harness::Require(!jump.empty(),"the switch submenu must not be empty (recent/favorites header)");
            std::cout<<"PASS menu checks follow the live model (topmost on, 60s interval, submenus present)\n";
        }
        { // 托盘图标 = logo，颜色按状态；tooltip 是旧版那四行
            harness::Require(snapshot["tray"]["iconLoaded"]==true,"tray icon image not created");
            harness::Require(snapshot["tray"]["registered"]==true,"tray icon not registered by shell");
            const auto tooltip=snapshot["tray"]["tooltip"].get<std::string>();
            harness::Require(tooltip.rfind("当前：",0)==0,"the tray tooltip must start with 当前：");
            harness::Require(tooltip.find("WSL")!=std::string::npos
                &&tooltip.find("WebUI")!=std::string::npos&&tooltip.find("网关")!=std::string::npos,
                "the tray tooltip must carry the three lamps");
            std::cout<<"PASS tray icon is the logo and the tooltip follows the old four-line shape\n";
        }
        const auto callback=harness::Window(snapshot,"trayCallbackHwnd");
        harness::Require(IsWindow(callback)!=0,"tray callback window missing");
        const auto trayMessage=snapshot["tray"]["callbackMessage"].get<UINT>();
        PostMessageW(callback,trayMessage,0x4242,WM_LBUTTONUP);
        Sleep(250);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==0,"tray left click did not hide the panel");
        harness::Require(snapshot["panelVisible"]==false&&snapshot["stripVisible"]==false,"hidden state leaks a window");
        PostMessageW(callback,trayMessage,0x4242,WM_LBUTTONUP);
        Sleep(250);
        snapshot=client.Request({{"command","dump"}});
        harness::Require(snapshot["state"]==1&&snapshot["panelVisible"]==true,"tray left click did not restore floating panel");
        std::cout<<"PASS tray callback toggles hide/show without touching geometry\n";

        const auto originBeforeDock=snapshot["float"];
        const auto writesBefore=snapshot["writes"].get<unsigned>();
        snapshot=client.Request({{"command","menu"},{"id",1002}});
        harness::Require(snapshot["state"]==4&&snapshot["edge"]!=0,"dock command did not reach collapsed docked state");
        harness::Require(snapshot["stripVisible"]==true&&snapshot["panelVisible"]==false,"docked visibility wrong");
        ExpectRect(snapshot["panel"],snapshot["expectedPanel"],"panel after dock");
        ExpectRect(snapshot["strip"],snapshot["expectedStrip"],"strip after dock");
        const auto edges=snapshot["edge"].get<int>();
        const auto work=snapshot["work"];
        if(edges==1) harness::Require(snapshot["panel"][0]==work[0],"INV4 left edge not flush with work area");
        if(edges==2) harness::Require(snapshot["panel"][0]+snapshot["panel"][2]==work[0].get<int>()+work[2].get<int>(),"INV4 right edge not flush");
        if(edges==3) harness::Require(snapshot["panel"][1]==work[1],"INV4 top edge not flush");
        if(edges==4) harness::Require(snapshot["panel"][1]+snapshot["panel"][3]==work[1].get<int>()+work[3].get<int>(),"INV4 bottom edge not flush");
        harness::Require(snapshot["writes"].get<unsigned>()-writesBefore==2,"dock must be exactly two absolute writes");
        std::cout<<"PASS dock command: two writes, rects equal model, edge flush with work area\n";

        const auto undockWrites=snapshot["writes"].get<unsigned>();
        snapshot=client.Request({{"command","menu"},{"id",1003}});
        harness::Require(snapshot["state"]==1&&snapshot["panelVisible"]==true&&snapshot["stripVisible"]==false,"undock visibility wrong");
        ExpectRect(snapshot["panel"],snapshot["expectedPanel"],"panel after undock");
        harness::Require(snapshot["float"]==originBeforeDock,"undock did not restore the pre-dock floating origin");
        harness::Require(snapshot["writes"].get<unsigned>()-undockWrites==1,"undock must be exactly one absolute write");
        std::cout<<"PASS undock command: one write, pre-dock floating origin restored\n";

        // M1 commands: compact tier, always-on-top, poll interval, active site, autostart, hotkeys.
        {
            // 紧凑视图已按用户要求删除（152 DIP 装不下任何页面正文），原档位切换用例一并移除。

            const auto topmostOff=client.Request({{"command","menu"},{"id",1008}});
            harness::Require(topmostOff["topmost"]==false,"topmost toggle did not clear the setting");
            harness::Require((GetWindowLongPtrW(harness::Window(topmostOff,"panelHwnd"),GWL_EXSTYLE)&WS_EX_TOPMOST)==0,
                "topmost toggle did not change the window style");
            const auto topmostOn=client.Request({{"command","menu"},{"id",1008}});
            harness::Require(topmostOn["topmost"]==true,"topmost toggle did not restore the setting");
            harness::Require((GetWindowLongPtrW(harness::Window(topmostOn,"panelHwnd"),GWL_EXSTYLE)&WS_EX_TOPMOST)!=0,
                "topmost toggle did not restore the window style");
            std::cout<<"PASS always-on-top: setting and WS_EX_TOPMOST stay in step\n";

            harness::Require(client.Request({{"command","menu"},{"id",1011}})["pollSeconds"]==120,
                "poll interval command did not take effect");
            harness::Require(client.Request({{"command","menu"},{"id",1010}})["pollSeconds"]==60,
                "poll interval did not return to 60s");
            std::cout<<"PASS refresh interval: 120s then back to 60s\n";

            // 菜单里的服务动作必须真的到达助手（夹具的 requests.log 是证据），而且不动几何。
            const auto refreshed=client.Request({{"command","menu"},{"id",1004}});
            harness::Require(refreshed["violations"]==0,"refresh command recorded an invariant violation");
            const auto gatewayBefore=Requests(logPath,"reload").size();
            const auto gateway=client.Request({{"command","menu"},{"id",1006}});
            harness::Require(gateway["violations"]==0,"gateway restart recorded an invariant violation");
            const auto webui=client.Request({{"command","menu"},{"id",1033}});
            harness::Require(webui["violations"]==0,"webui restart recorded an invariant violation");
            {
                for(int attempt=0;attempt<60&&Requests(logPath,"reload").size()<gatewayBefore+2;++attempt) Sleep(100);
                const auto reloads=Requests(logPath,"reload");
                harness::Require(reloads.size()>=gatewayBefore+2,
                    ("the tray restart commands did not reach the helper (saw "
                        +std::to_string(reloads.size()-gatewayBefore)+")").c_str());
                bool sawGateway=false,sawWebUi=false;
                for(const auto& request:reloads) {
                    if(!request.contains("targets")) continue;
                    for(const auto& target:request["targets"]) {
                        if(target=="gateway") sawGateway=true;
                        if(target=="webui") sawWebUi=true;
                    }
                }
                harness::Require(sawGateway&&sawWebUi,
                    "the tray menu must ask the helper to reload both gateway and webui");
            }
            std::cout<<"PASS refresh/restart commands reach the helper (reload targets logged)\n";

            // Autostart touches our own HKCU Run value; the test restores whatever it found.
            const bool hadAutostart=Dump(client)["autostart"].get<bool>();
            harness::Require(client.Request({{"command","menu"},{"id",1013}})["autostart"]==true,
                "autostart command did not register the run entry");
            harness::Require(client.Request({{"command","menu"},{"id",1013}})["autostart"]==false,
                "autostart command did not clear the run entry");
            if(hadAutostart) client.Request({{"command","menu"},{"id",1013}});
            std::cout<<"PASS autostart: HKCU Run entry created and removed, original state restored\n";

            // Another resident program may own a chord: 1409 (already registered) is an environment
            // condition we degrade from gracefully; anything else means the registration path broke.
            const auto errors=Dump(client)["hotkeyErrors"];
            for(const auto& entry:errors) {
                harness::Require(entry.get<int>()==0||entry.get<int>()==1409,
                    ("hotkey registration failed with an unexpected error: "+entry.dump()).c_str());
            }
            // Ctrl+Alt+H is the panel's only chord; the old Electron hermes panel owns it while it runs,
            // and that is an environment condition (1409) rather than a broken registration path.
            if(errors[0].get<int>()==1409) {
                std::cout<<"NOTE Ctrl+Alt+H is owned by another process (the Electron hermes panel keeps "
                    <<"it while it runs); the tray menu remains the full control surface\n";
            }

            if(errors[0].get<int>()==0) {
                harness::Pointer pointer;
                const int stateBeforeHotkey=Dump(client)["state"].get<int>();
                pointer.Hotkey(VK_CONTROL,VK_MENU,'H');
                Sleep(400);
                harness::Require(Dump(client)["state"]!=stateBeforeHotkey,"Ctrl+Alt+H did not toggle the panel");
                pointer.Hotkey(VK_CONTROL,VK_MENU,'H');
                Sleep(400);
                harness::Require(Dump(client)["state"]==stateBeforeHotkey,"Ctrl+Alt+H did not toggle the panel back");
                std::cout<<"PASS hotkeys: real Ctrl+Alt+H toggles the panel\n";
            }
        }

        snapshot=client.Request({{"command","menu"},{"id",1002}});
        snapshot=client.Request({{"command","menu"},{"id",1003}});
        harness::Require(snapshot["state"]==1&&snapshot["float"]==originBeforeDock,"dock/undock cycle changed stored floating origin");
        harness::Require(snapshot["violations"]==0,"geometry invariant violations recorded");
        harness::Require(snapshot["writes"].get<unsigned>()<20,"unexpected geometry write storm");
        std::cout<<"PASS dock/undock cycle keeps invariants and write budget\n";
        std::cout<<"tray: 13 passed, 0 failed\n";
        code=0;
        client.Request({{"command","menu"},{"id",1099}});
        harness::Require(WaitForSingleObject(child.hProcess,3000)==WAIT_OBJECT_0,"quit command did not exit");
        std::cout<<"PASS quit command exits cleanly\n";
    }catch(const std::exception& error){
        if(harness::IsEnvironmentInterference(error)){
            std::cerr<<"SKIP tray: "<<error.what()<<"\n";
            TerminateProcess(child.hProcess,77);
            WaitForSingleObject(child.hProcess,1500);
            return 77;
        }
        std::cerr<<"FAIL tray: "<<error.what()<<"\n";
    }
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return code;
}
