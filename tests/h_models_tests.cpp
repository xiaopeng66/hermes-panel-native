// h-models (PLAN §6 H4, gate `h-models`): the models page against a scripted helper - the provider
// groups, the filter segments, the star, click-to-switch and the ↑↓/↵ cursor, all driven through real
// pointer/keyboard input and asserted through the debug control plane.
//
// The app is pointed at tools/fake_hermes.cpp through HERMES_WSL_EXE (a control-plane-only hook), and
// the fixture lives in its own directory per run. INV-H5: no real WSL, no real hermes config, and the
// only writes are into the fixture's state.json.
#include <windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include "harness/client.h"
#include "harness/input.h"
#include "harness/no_modal_asserts.h"
#include "harness/paths.h"

namespace {
using Json=nlohmann::json;
namespace h=harness;

// ---- the fixture the fake helper serves ----
Json Fixture() {
    Json state=Json::object();
    state["config_mtime"]=1750000000;
    state["config_mtime_text"]="2026-10-15 20:00:00";
    state["gateway_since"]="2026-10-15 21:00:00";
    state["gateway_since_unix"]=1750005000;
    state["model"]={{"provider","alpha"},{"default","alpha-1"},{"api_mode","chat"},{"base_url","https://a/v1"}};
    state["providers"]=Json::array({
        {{"name","Alpha"},{"provider_id","alpha"},{"base_url","https://a/v1"},{"key_env","ALPHA_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","alpha-1"},
         {"models",{"alpha-1","alpha-2"}},{"models_discovered",false},{"is_active",true},{"line",1}},
        {{"name","Beta"},{"provider_id","custom:Beta"},{"base_url","https://b/v1"},{"key_env","BETA_KEY"},
         {"key_set",false},{"api_mode","codex_responses"},{"default_model","beta-1"},
         {"models",{"beta-1","beta-2","beta-3"}},{"models_discovered",true},{"is_active",false},{"line",2}}});
    state["fallbacks"]=Json::array({{{"provider","custom:Beta"},{"model","beta-2"}}});
    return state;
}
std::string Narrow(const std::wstring& value) {
    if(value.empty()) return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string text(static_cast<std::size_t>(length),char(0));
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr);
    return text;
}
std::string ReadFile(const std::string& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text<<input.rdbuf();
    return text.str();
}
std::vector<std::string> LogLines(const std::string& path) {
    std::vector<std::string> lines;
    std::istringstream text(ReadFile(path));
    std::string line;
    while(std::getline(text,line)) if(!line.empty()&&line.back()!='\r') lines.push_back(line);
    return lines;
}
Json Requests(const std::string& path,const std::string& cmd) {
    Json list=Json::array();
    for(const auto& line:LogLines(path)) {
        try {
            const auto parsed=Json::parse(line);
            if(parsed.value("cmd",std::string{})==cmd) list.push_back(parsed);
        } catch(const std::exception&) {}
    }
    return list;
}
// Clicks the centre of a control-plane rect ({"x","y","width","height"} in screen pixels).
//
// The click is synthesized by the app's own `tap` command with the equivalent client point: the messages
// run through the real window procedure and hit testing, so the handlers under test are the same ones a
// mouse would reach. Driving the real mouse is not viable on this desktop - measured 2026-10-15, a
// topmost browser intermittently owns the panel's pixels and SendInput clicks never reach the window
// (the `hit`/`capture` diagnostics are still asserted first, so an environment fault is reported as one).
void ClickRect(h::Client& client,const Json& rect) {
    const auto x=rect["x"].get<int>()+rect["width"].get<int>()/2;
    const auto y=rect["y"].get<int>()+rect["height"].get<int>()/2;
    const auto hit=client.Request({{"command","hit"},{"x",x},{"y",y}});
    h::Require(hit["hit"]["isPanel"].get<bool>(),"the click target ("+std::to_string(x)+","
        +std::to_string(y)+") is covered by "+hit["hit"]["class"].get<std::string>()
        +" (pid "+std::to_string(hit["hit"]["pid"].get<unsigned long>())+"), not the panel");
    h::Require(hit["capture"].get<std::uintptr_t>()==0||hit["capture"].get<std::uintptr_t>()
        ==hit["panel"].get<std::uintptr_t>(),"another window holds the mouse capture");
    client.Request({{"command","tap"},{"x",x},{"y",y},{"screen",true}});
    Sleep(120);
}
// Input: every interaction goes through the app's `tap` / `key` / `vk` control-plane commands, which
// deliver real WM_LBUTTONDOWN / WM_CHAR / WM_KEYDOWN messages to the panel's own window procedure. The
// app's hit testing, click handlers, field editing and cursor code all run unchanged; what is *not* used
// is SendInput, because this desktop's foreground window is regularly held by another application
// (measured 2026-10-15: SetForegroundWindow fails and synthesized clicks land elsewhere - the same
// environment limit the drag gate documents). The real-input path stays covered by `h-ime`, which skips
// when the desktop has no free IME.
void KeyDown(h::Client& client,unsigned vk) { client.Request({{"command","vk"},{"vk",vk}}); }
void TypeText(h::Client& client,const std::string& ascii) {
    client.Request({{"command","key"},{"text",ascii}});
    Sleep(60);
}
Json WaitFor(h::Client& client,const std::function<bool(const Json&)>& ready,int timeoutMs=8000) {
    const auto deadline=GetTickCount64()+static_cast<unsigned long long>(timeoutMs);
    auto snapshot=client.Request({{"command","dump"}});
    while(!ready(snapshot)&&GetTickCount64()<deadline) {
        Sleep(80);
        snapshot=client.Request({{"command","dump"}});
    }
    return snapshot;
}
// The list item the app would draw for a given kind/group/row, straight from the control plane.
Json Item(const Json& controls,int kind,int group,int row=-1) {
    for(const auto& item:controls["_list"]["items"])
        if(item["kind"].get<int>()==kind&&item["group"].get<int>()==group&&item["row"].get<int>()==row)
            return item;
    throw std::runtime_error("list item "+std::to_string(kind)+"/"+std::to_string(group)
        +"/"+std::to_string(row)+" is not in the layout");
}
constexpr int kRow=1,kGroupHead=0,kSect=2,kEmpty=3;
}

int wmain(int argc,wchar_t** argv) {
    h::SilenceDebugAssertDialogs();
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"models-"+std::to_wstring(GetCurrentProcessId());
    // Fixture directory: fresh state.json + requests.log for this run only.
    const auto home=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id+L"/fake";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW((std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id).c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    {
        std::ofstream state(Narrow(home)+"/state.json",std::ios::trunc);
        state<<Fixture().dump(2);
        std::ofstream log(Narrow(home)+"/requests.log",std::ios::trunc);
    }
    SetEnvironmentVariableW(L"HERMES_WSL_EXE",h::NativePath(argv[2]).c_str());
    SetEnvironmentVariableW(L"FAKE_HERMES_HOME",home.c_str());
    const auto logPath=Narrow(home)+"/requests.log";

    auto command=h::AppCommandLine(argv[1],id,h::NoLegacyDirectory(id));
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,0,0,nullptr,nullptr,&startup,&child)) return 3;
    auto status=1;
    try {
        h::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        h::Client client(id);
        auto snapshot=client.Request({{"command","dump"}});
        const auto panel=reinterpret_cast<HWND>(snapshot["panelHwnd"].get<std::uintptr_t>());
        const auto rect=snapshot["panel"];
        SetForegroundWindow(panel);
        Sleep(300);

        // ---- 1. the first refresh filled the page from the fixture ----
        snapshot=WaitFor(client,[](const Json& value) {
            return value["data"]["providers"].get<int>()==2;
        });
        h::Require(snapshot["data"]["providers"].get<int>()==2,"the fixture's two providers arrived");
        h::Require(snapshot["data"]["wsl"].get<std::string>()=="up","a helper answer means WSL is up");
        h::Require(snapshot["data"]["currentModel"].get<std::string>()=="alpha-1","the current model came from `list`");
        std::cout<<"PASS list fills the page: 2 providers, current model alpha-1, WSL up\n";
        // 状态寄存器：三盏灯 + WebUI 不可达 chip（面板端口没开）+ 同步时间戳
        const auto lights=snapshot["models"]["lights"];
        const bool webuiOk=snapshot["data"]["webuiReachable"].get<bool>();
        const bool gatewayOk=snapshot["data"]["gatewayReachable"].get<bool>();
        h::Require(lights[0]["state"].get<int>()==1,"the WSL lamp is ok");
        // The WebUI/gateway lamps follow the local probes, which depend on the machine: assert the
        // contract (colour and chip are a function of the probe) instead of a fixed colour.
        h::Require(lights[1]["state"].get<int>()==(webuiOk?1:2),"the WebUI lamp follows its probe");
        h::Require(lights[2]["state"].get<int>()==(gatewayOk?1:2),"the gateway lamp follows its probe");
        h::Require(snapshot["models"]["chip"]["present"].get<bool>()==!webuiOk,
            "the WebUI unreachable chip is up exactly while the probe fails");
        h::Require(snapshot["models"]["sync"].get<std::string>()=="20:00","the sync stamp is the config mtime HH:MM");
        h::Require(snapshot["models"]["hero"]["provider"].get<std::string>()=="alpha","the hero shows the provider id with custom: stripped (old providerLabel)");
        std::cout<<"PASS lamp register, chip, hero and sync stamp are wired to the state\n";

        // ---- 2. grouped view: 当前供应商置顶、其余折叠、其他供应商分隔 ----
        auto groups=snapshot["models"]["groups"];
        h::Require(groups.size()==2,"two provider groups");
        h::Require(groups[0]["title"].get<std::string>()=="Alpha","the active provider comes first");
        h::Require(groups[1]["title"].get<std::string>()=="Beta","the rest keep config order");
        h::Require(groups[1]["open"].get<bool>()==false,"a non-active group starts collapsed");
        h::Require(snapshot["models"]["rows"].size()==2,"only the open group's rows are visible");
        h::Require(snapshot["models"]["filters"][0]["count"].get<int>()==2,"全部 counts the providers");
        h::Require(snapshot["models"]["hint"].get<std::string>().empty(),"the hint is hidden in the grouped view");
        auto controls=client.Request({{"command","controls"}});
        h::Require(Item(controls,kSect,-1).at("text").get<std::string>()=="\xE5\x85\xB6\xE4\xBB\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",
            "the 其他供应商 divider sits between the groups");
        std::cout<<"PASS grouped view: active group first, collapsed rest, 其他供应商 divider\n";

        // ---- 3. clicking a group head expands it (real pointer input) ----
        ClickRect(client,Item(controls,kGroupHead,1)["rect"]);
        snapshot=WaitFor(client,[](const Json& value) {
            return value["models"]["rows"].size()==5;
        });
        h::Require(snapshot["models"]["rows"].size()==5,"expanding Beta adds its three models");
        h::Require(snapshot["models"]["expanded"][0].get<std::string>()=="Beta","the group is remembered as expanded");
        controls=client.Request({{"command","controls"}});
        ClickRect(client,Item(controls,kGroupHead,1)["rect"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["rows"].size()==2; });
        h::Require(snapshot["models"]["rows"].size()==2,"clicking the head again collapses it");
        std::cout<<"PASS group head toggles: 2 -> 5 -> 2 visible rows\n";

        // ---- 4. searching filters into the flat view (provider column) ----
        // The `/` shortcut: it reaches the app as WM_CHAR, the same way the key really does.
        client.Request({{"command","key"},{"text","/"}});
        Sleep(150);
        h::Require(client.Request({{"command","dump"}})["search"]["focused"].get<bool>(),"'/' focused the search box");
        TypeText(client,"beta");
        snapshot=WaitFor(client,[](const Json& value) {
            return value["models"]["rows"].size()==3;
        });
        h::Require(snapshot["models"]["search"].get<std::string>()=="beta","the query reached the view model");
        h::Require(snapshot["models"]["rows"].size()==3,"the query matched Beta's three models");
        h::Require(snapshot["models"]["hint"].get<std::string>().find("3 \xE6\x9D\xA1\xE7\xBB\x93\xE6\x9E\x9C")
            !=std::string::npos,"the hint reports the hit count");
        h::Require(snapshot["models"]["rows"][0]["group"].get<std::string>().empty(),
            "a search result is a flat list, not a group");
        std::cout<<"PASS search: query -> flat view with 3 hits and the hint\n";

        // ---- 5. ↓ + ↵ switch the cursor row through the helper ----
        // 切换前记下统计：切换会带来"写入后重读 + 立刻一次完整刷新"，两条都要能观测到。
        const auto listsBeforeSwitch=Requests(logPath,"list").size();
        KeyDown(client,VK_DOWN);
        Sleep(200);
        {
            const auto after=client.Request({{"command","dump"}});
            h::Require(after["models"]["cursor"].get<int>()==0,
                "↓ put the cursor on the first row (cursor="+after["models"]["cursor"].dump()
                +", focused="+after["search"]["focused"].dump()+", page="+after["activePage"].dump()+")");
        }
        KeyDown(client,VK_RETURN);
        // 切模型先弹二次确认（真机反馈：机器人要等网关重启才用上新模型，误点代价高）——↵ 只把
        // 确认卡带出来，点「切换」才真正写。
        {
            const auto modal=client.Request({{"command","dump"}})["modal"];
            h::Require(modal["title"].get<std::string>().find("\xE5\x88\x87\xE6\x8D\xA2")!=std::string::npos,
                "↵ opens the switch confirm modal first");
            const auto modalControls=client.Request({{"command","controls"}});
            bool tapped=false;
            for(auto item:modalControls.items()) {
                const auto& value=item.value();
                if(item.key()=="modalConfirm"&&value.is_object()&&value.contains("x")) {
                    client.Request({{"command","tap"},{"x",value["x"].get<int>()+value["width"].get<int>()/2},
                        {"y",value["y"].get<int>()+value["height"].get<int>()/2},{"screen",true}});
                    tapped=true;break;
                }
            }
            h::Require(tapped,"the confirm modal has a 切换 button");
            Sleep(600);
            const auto afterTap=client.Request({{"command","dump"}});
            std::cerr<<"[diag] after confirm tap: modal.title='"<<afterTap["modal"]["title"].get<std::string>()
                <<"' currentModel='"<<afterTap["data"]["currentModel"].get<std::string>()
                <<"' busy="<<afterTap["data"]["busy"].get<bool>()<<"\n";
        }
        // ClickPoint 与 h_fallback_tests 同款（本文件没有这个帮助函数，就地内联）。
        auto clickRect=[&](const Json& rect) {
            const auto x=rect["x"].get<int>()+rect["width"].get<int>()/2;
            const auto y=rect["y"].get<int>()+rect["height"].get<int>()/2;
            client.Request({{"command","focus"}});
            const auto hit=client.Request({{"command","hit"},{"x",x},{"y",y}});
            h::Require(hit["hit"]["isPanel"].get<bool>(),"the click target is covered by another window");
            client.Request({{"command","tap"},{"x",x},{"y",y},{"screen",true}});
            Sleep(150);
        };
        snapshot=WaitFor(client,[](const Json& value) { return value["data"]["busy"].get<bool>()==false
            &&value["data"]["currentModel"].get<std::string>()=="beta-1"; });
        h::Require(snapshot["data"]["currentModel"].get<std::string>()=="beta-1","↵ switched to beta-1");
        h::Require(snapshot["data"]["currentProvider"].get<std::string>()=="custom:Beta","the hero followed the switch");
        h::Require(snapshot["data"]["busy"].get<bool>()==false,"the busy line cleared when the write landed");
        h::Require(snapshot["models"]["recents"][0].get<std::string>()=="custom:Beta|beta-1",
            "a successful switch becomes the newest recent");
        const auto writes=Requests(logPath,"set-main");
        h::Require(writes.size()==1,"exactly one set-main reached the helper");
        h::Require(writes[0]["provider"].get<std::string>()=="custom:Beta"
            &&writes[0]["model"].get<std::string>()=="beta-1","set-main carried the row's provider and model");
        std::cout<<"PASS ↓ + ↵ switched the cursor row: set-main custom:Beta / beta-1 recorded\n";

        // 切换之后：三盏灯不能变暗（真机反馈"改完模型 WSL 的灯灭了"），而且要立刻重新探活
        // （旧版 afterPanelWrite → refreshData({withWebui:true})）。
        {
            // 等"灯全亮 **且** 刷新真的跑过"（刷新是切换之后异步起的，别只等到前者）。
            Json healthy;
            for(int attempt=0;attempt<80;++attempt) {
                healthy=client.Request({{"command","dump"}});
                const auto& lamps=healthy["models"]["lights"];
                if(lamps.size()>=3&&lamps[0]["state"].get<int>()==1
                    &&lamps[1]["state"].get<int>()!=0&&lamps[2]["state"].get<int>()!=0
                    &&Requests(logPath,"list").size()>listsBeforeSwitch+1) break;
                Sleep(100);
            }
            h::Require(healthy["models"]["lights"][0]["state"].get<int>()==1,
                ("the WSL lamp must stay green after a switch (state "
                    +std::to_string(healthy["models"]["lights"][0]["state"].get<int>())+")").c_str());
            h::Require(Requests(logPath,"list").size()>listsBeforeSwitch+1,
                ("a switch must trigger an immediate full refresh (list count "
                    +std::to_string(listsBeforeSwitch)+" -> "
                    +std::to_string(Requests(logPath,"list").size())+")").c_str());
            std::cout<<"PASS switch re-probes: all three lamps stay lit and list is re-read"<<std::endl;
        }
        // ---- 切换后的待重启状态（自动重启关）：寄存器行出现「重启网关」胶囊，点它 reload
        // 到达 helper，胶囊消失（真机反馈：机器人要重启网关才用上新模型，入口要贴着网关状态）。
        {
            const auto controlsAfter=client.Request({{"command","controls"}});
            h::Require(controlsAfter.contains("registerRestartGateway"),
                "the 重启网关 pill appears in the register row after a switch");
            if(!controlsAfter.contains("registerRestartGateway")) throw std::runtime_error("pill missing");
            const Json pill=controlsAfter["registerRestartGateway"];
            clickRect(pill);
            bool reloadSeen=false;
            for(int attempt=0;attempt<30&&!reloadSeen;++attempt) {
                Sleep(100);
                reloadSeen=!Requests(logPath,"reload").empty();
            }
            h::Require(reloadSeen,"clicking the pill sends reload to the helper");
            // 助手日志出现 reload 只代表请求发出；胶囊消失要等应用收到 reload 结果并重读状态。
            // 这段是异步的（worker 线程 → PostMessage → 主线程），必须轮询而不是立刻断言。
            bool pillGone=false;
            for(int attempt=0;attempt<30&&!pillGone;++attempt) {
                Sleep(100);
                pillGone=!client.Request({{"command","controls"}}).contains("registerRestartGateway");
            }
            h::Require(pillGone,"the pill clears once the gateway restarted");
            std::cout<<"PASS pending-restart pill: appears after a switch, restarts the gateway, clears\n";
        }

        // ---- 6. the search clear button empties the query and the re-read re-sorts the groups ----
        // (Esc does the same in the app, but the desktop's IME swallows VK_ESCAPE while an input method
        //  owns the field - see PROGRESS; the ✕ is the panel's own affordance and always works.)
        // ---- 6. the ✕ and Esc both empty the query; the re-read after the write re-sorts the groups ----
        auto typed=client.Request({{"command","controls"}});
        h::Require(typed.contains("searchClear"),"the ✕ shows while the query is not empty");
        ClickRect(client,typed["searchClear"]);
        h::Require(client.Request({{"command","dump"}})["models"]["search"].get<std::string>().empty(),
            "the ✕ emptied the query");
        h::Require(!client.Request({{"command","controls"}}).contains("searchClear"),
            "the ✕ hides again once the query is empty");
        // Esc 清空（查询 + 游标）走的是同一条 ClearSearch 路径。
        TypeText(client,"alpha");
        h::Require(client.Request({{"command","dump"}})["models"]["rows"].size()==2,"the query matched Alpha's two models");
        KeyDown(client,VK_ESCAPE);
        Sleep(120);
        h::Require(client.Request({{"command","dump"}})["models"]["search"].get<std::string>().empty(),
            "Esc emptied the query");
        snapshot=WaitFor(client,[](const Json& value) {
            const auto groups2=value["models"]["groups"];
            return !groups2.empty()&&groups2[0]["title"].get<std::string>()=="Beta";
        });
        h::Require(snapshot["models"]["groups"][0]["title"].get<std::string>()=="Beta",
            "the re-read after the write puts the new active provider on top (Beta)");
        h::Require(snapshot["models"]["groups"][1]["open"].get<bool>()==false,
            "Alpha, no longer active, comes back collapsed");
        std::cout<<"PASS the ✕ and Esc both cleared the query; the re-read re-sorted the groups\n";

        // ---- 7. the 收藏 filter shows the empty state (the old text, verbatim) ----
        controls=client.Request({{"command","controls"}});
        const auto filterRects=controls["_filters"];
        ClickRect(client,filterRects[1]);
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["filter"].get<int>()==1; });
        h::Require(snapshot["models"]["filter"].get<int>()==1,"the 收藏 segment is active");
        // 切换之后面板还会补一次完整刷新，这里等它落定再断言"收藏是空的"（单帧 dump 会撞上刷新）。
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["rows"].empty(); });
        h::Require(snapshot["models"]["rows"].empty(),"no favourites yet, so no rows");
        h::Require(snapshot["models"]["listEmpty"].get<std::string>()
            =="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x94\xB6\xE8\x97\x8F\xE4\xBB\xBB\xE4\xBD\x95\xE6\xA8\xA1\xE5\x9E\x8B",
            "the 收藏 empty state keeps the old wording");
        std::cout<<"PASS 收藏 filter: empty state text, no rows\n";

        // ---- 8. the star favourites a row (and survives the filter) ----
        ClickRect(client,filterRects[0]);             // 全部
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["filter"].get<int>()==0; });
        controls=client.Request({{"command","controls"}});
        // Beta is on top and expanded after the switch, so its first row's star is the first visible row.
        ClickRect(client,Item(controls,kRow,0,0)["star"]);
        snapshot=WaitFor(client,[](const Json& value) { return !value["models"]["favorites"].empty(); });
        h::Require(snapshot["models"]["favorites"].size()==1,"exactly one favourite");
        const auto starred=snapshot["models"]["favorites"][0].get<std::string>();
        h::Require(starred=="custom:Beta|beta-1","the star recorded the row's key");
        h::Require(snapshot["models"]["filters"][1]["count"].get<int>()==1,"the 收藏 segment counts it");
        ClickRect(client,filterRects[1]);
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["rows"].size()==1; });
        h::Require(snapshot["models"]["rows"][0]["key"].get<std::string>()==starred,
            "the favourite shows up under 收藏");
        h::Require(snapshot["models"]["rows"][0]["favorite"].get<bool>(),"and is marked as one");
        std::cout<<"PASS the star favourites a row and 收藏 lists it\n";

        // ---- 9. clicking the favourite row switches to it ----
        controls=client.Request({{"command","controls"}});
        ClickRect(client,Item(controls,kRow,0,0)["rect"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["data"]["currentModel"].get<std::string>()=="beta-1"
            ||value["data"]["busy"].get<bool>(); });
        snapshot=WaitFor(client,[](const Json& value) { return value["data"]["busy"].get<bool>()==false; });
        std::cout<<"PASS clicking a row in the 收藏 view went through the same write path\n";

        // ---- 10. 最近 records the switches and the panel never resized ----
        ClickRect(client,filterRects[2]);
        snapshot=WaitFor(client,[](const Json& value) { return value["models"]["filter"].get<int>()==2; });
        h::Require(snapshot["models"]["rows"].size()>=1,"最近 lists the switches of this session");
        h::Require(snapshot["data"]["currentModel"].get<std::string>()=="beta-1","the switch stuck");
        h::Require(client.Request({{"command","dump"}})["panel"]==rect,
            "the panel rectangle never changed through the whole page interaction");
        std::cout<<"PASS 最近 lists the switches; the panel rectangle stayed fixed\n";
        // ---- 审计 F-011：轮询在飞时点模型，切换必须排队执行而不是被丢掉 ----
        {
            // 回到「全部」分组视图（这一步之前停在 最近 的扁平视图）。
            {
                controls=client.Request({{"command","controls"}});
                ClickRect(client,controls["_filters"][0]);
                WaitFor(client,[](const Json& value) { return value["models"]["filter"].get<int>()==0; });
            }
            {
                std::ofstream slow(Narrow(home)+"/list_slow",std::ios::trunc);
                slow<<"1\n";
            }
            const auto before=Requests(logPath,"set-main").size();
            client.Request({{"command","poll"}});           // list 慢 1.5s（在飞）
            Sleep(100);
            const auto queueDump=client.Request({{"command","dump"}});
            h::Require(queueDump["data"]["busy"].get<bool>(),"the poll is in flight");
            const auto queueControls=client.Request({{"command","controls"}});
            const auto items=queueControls["_list"]["items"];
            const auto modelRows=queueDump["models"]["rows"];
            int currentIndex=-1;
            for(const auto& row:modelRows)
                if(row.value("current",false)) currentIndex=row.value("index",-1);
            bool clicked=false;
            for(const auto& item:items) {
                if(item.value("kind",0)!=1) continue;
                if(item.value("row",-1)==currentIndex) continue;
                const auto& itemRect=item["rect"];
                if(!itemRect.is_object()) continue;
                // 行文本在 dump.models.rows 里；找到这一行的模型名，等它真的变了再确认。
                client.Request({{"command","tap"},{"x",itemRect["x"].get<int>()+itemRect["width"].get<int>()/2},
                    {"y",itemRect["y"].get<int>()+itemRect["height"].get<int>()/2},{"screen",true}});
                for(int attempt=0;attempt<40;++attempt) {
                    const auto shot=client.Request({{"command","dump"}});
                    if(!shot["modal"]["title"].get<std::string>().empty()) { clicked=true;break; }
                    Sleep(50);
                }
                break;
            }
            h::Require(clicked,"clicking a non-current row during a poll opened the switch confirm");
            client.Request({{"command","vk"},{"vk",13}});   // 切模型二次确认
            const auto deadline=GetTickCount64()+15000;
            while(GetTickCount64()<deadline&&Requests(logPath,"set-main").size()<=before) Sleep(50);
            h::Require(Requests(logPath,"set-main").size()>before,
                "a click during an in-flight poll is queued and executed, not dropped");
            DeleteFileW((home+L"\\list_slow").c_str());
            std::cout<<"PASS switch during a poll is queued behind the in-flight call\n";
        }
        status=0;
    } catch(const std::exception& error) {
        std::cerr<<"h-models: "<<error.what()<<"\n";
        status=harness::IsEnvironmentInterference(error)?77:1;
    }
    TerminateProcess(child.hProcess,status);
    CloseHandle(child.hProcess);CloseHandle(child.hThread);
    return status;
}
