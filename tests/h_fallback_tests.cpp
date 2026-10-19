// h-fallback (PLAN §6 H5, gate `h-fallback`): the 回退链 page against the scripted helper - the ladder
// rendering, the up/down/remove buttons, the fold form (select + manual fields), the per-row probe and
// the danger modal, all through the app's own message path (see h_models_tests.cpp for why the desktop's
// real input is not used) and asserted through the debug control plane.
#include <windows.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include "harness/client.h"
#include "harness/no_modal_asserts.h"
#include "harness/paths.h"

namespace {
using Json=nlohmann::json;
namespace h=harness;

std::string Narrow(const std::wstring& value) {
    if(value.empty()) return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string text(static_cast<std::size_t>(length),char(0));
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),text.data(),length,nullptr,nullptr);
    return text;
}
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
         {"key_set",false},{"api_mode","chat"},{"default_model","beta-1"},
         {"models",{"beta-1"}},{"models_discovered",false},{"is_active",false},{"line",2}}});
    state["fallbacks"]=Json::array({
        {{"provider","custom:Beta"},{"model","beta-1"},{"base_url","https://b/v1"},{"key_env","BETA_KEY"},{"key_set",false}},
        // 第二条故意不可达：逐条探测现在按行的 base_url 发 probe，可达性由地址决定（夹具里
        // "unreachable" 前缀就是"连不上"）。旧夹具靠 fallback-probe 的 index==1 硬编码。
        {{"provider","custom:Gamma"},{"model","gamma-pro"},{"base_url","https://unreachable.example/v1"},
         {"key_env","GAMMA_KEY"},{"key_set",true}}});
    return state;
}
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
void ClickPoint(h::Client& client,const Json& rect) {
    const auto x=rect["x"].get<int>()+rect["width"].get<int>()/2;
    const auto y=rect["y"].get<int>()+rect["height"].get<int>()/2;
    client.Request({{"command","focus"}});
    const auto hit=client.Request({{"command","hit"},{"x",x},{"y",y}});
    h::Require(hit["hit"]["isPanel"].get<bool>(),"the click target is covered by another window");
    client.Request({{"command","tap"},{"x",x},{"y",y},{"screen",true}});
    Sleep(150);
}
void KeyDown(h::Client& client,unsigned vk) { client.Request({{"command","vk"},{"vk",vk}});Sleep(80); }
void TypeText(h::Client& client,const std::string& ascii) {
    client.Request({{"command","key"},{"text",ascii}});
    Sleep(80);
}
Json WaitFor(h::Client& client,const std::function<bool(const Json&)>& ready,int timeoutMs=10000) {
    const auto deadline=GetTickCount64()+static_cast<unsigned long long>(timeoutMs);
    auto snapshot=client.Request({{"command","dump"}});
    while(!ready(snapshot)&&GetTickCount64()<deadline) {
        Sleep(100);
        snapshot=client.Request({{"command","dump"}});
    }
    return snapshot;
}
// 切页会触发内容高度自适应（窗口 resize）：等面板矩形连续两次一致再取控件坐标，
// 否则拿到的是 resize 前的旧位置（门禁在全量跑时偶发点到空处）。
void Settle(h::Client& client) {
    auto previous=client.Request({{"command","dump"}})["panel"];
    for(int attempt=0;attempt<40;++attempt) {
        Sleep(80);
        const auto current=client.Request({{"command","dump"}})["panel"];
        if(current==previous) return;
        previous=current;
    }
}
}

int wmain(int argc,wchar_t** argv) {
    h::SilenceDebugAssertDialogs();
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"fallback-"+std::to_wstring(GetCurrentProcessId());
    const auto home=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id+L"/fake";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW((std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id).c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    {
        std::ofstream state(Narrow(home)+"/state.json",std::ios::trunc);
        state<<Fixture().dump(2);
        std::ofstream log(Narrow(home)+"/requests.log",std::ios::trunc);
    }
    SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
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
        WaitFor(client,[](const Json& value) { return value["data"]["providers"].get<int>()==2; });
        // 切到回退链页：底部导航的 tab（控制面 tap 走真实命中框）
        auto controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["tabFallback"]);
        auto snapshot=WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==1; });
        h::Require(snapshot["activePage"].get<int>()==1,"the 回退链 tab switched the page");
        Settle(client);
        // ---- 1. 阶梯：两条、序号、当前首位用 accent ----
        auto page=snapshot["fallback"];
        h::Require(page["rows"].size()==2,"the ladder lists both fallbacks");
        h::Require(page["rows"][0]["provider"].get<std::string>()=="custom:Beta"
            &&page["rows"][0]["model"].get<std::string>()=="beta-1","the first row is the first entry");
        h::Require(page["rows"][0]["keyWarn"].get<bool>(),"a missing key shows the warning");
        h::Require(!page["rows"][1]["keyWarn"].get<bool>(),"a configured key shows no warning");
        h::Require(page["title"].get<std::string>()=="\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE","the page head names the chain");
        std::cout<<"PASS the ladder renders both entries with their key warnings\n";
        // ---- 2. 逐条探测：第一条 ●7、第二条 ○ ----
        // 先放慢助手，验证"检测中 n/N"和当前行的 … 真的会显示（真机反馈：点了没有状态）。
        {
            std::ofstream slow(Narrow(home)+"/probe_slow",std::ios::trunc);
            slow<<"1\n";
        }
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["fbProbe"]);
        bool sawProgress=false;
        for(int attempt=0;attempt<80&&!sawProgress;++attempt) {
            const auto value=client.Request({{"command","dump"}});
            const auto label=value["fallback"]["probeLabel"].get<std::string>();
            bool pending=false;
            for(const auto& row:value["fallback"]["rows"])
                if(row["probePending"].get<bool>()) pending=true;
            sawProgress=label.find("\xE6\xA3\x80\xE6\xB5\x8B\xE4\xB8\xAD")!=std::string::npos&&pending;
            if(!sawProgress) Sleep(20);
        }
        DeleteFileW((home+L"\\probe_slow").c_str());
        h::Require(sawProgress,"the probe button shows 检测中 n/N and the active row shows …");
        std::cout<<"PASS 逐条探测 shows live progress (检测中 n/N + …)\n";
        // 测完一条就显示（真机反馈：不能等整条链跑完）：第一条已出结果、第二条还没测完的中间态
        // 必须能被观察到（夹具 probe_slow 每条 400ms）。
        {
            std::ofstream slow(Narrow(home)+"/probe_slow",std::ios::trunc);
            slow<<"1\n";
        }
        client.Request({{"command","controls"}});
        ClickPoint(client,controls["fbProbe"]);
        bool sawPartial=false;
        for(int attempt=0;attempt<80&&!sawPartial;++attempt) {
            const auto value=client.Request({{"command","dump"}})["fallback"]["rows"];
            if(value.size()==2&&value[0]["hasProbe"].get<bool>()&&!value[1]["hasProbe"].get<bool>())
                sawPartial=true;
            if(!sawPartial) Sleep(20);
        }
        Sleep(700);   // 让第二条也跑完，下面的断言才有完整结果
        DeleteFileW((home+L"/probe_slow").c_str());
        harness::Require(sawPartial,"the first row's result was not shown before the chain finished");
        std::cout<<"PASS 逐条探测 shows each row's result as soon as it is probed\n";
        snapshot=WaitFor(client,[](const Json& value) {
            const auto rows=value["fallback"]["rows"];
            return rows.size()==2&&rows[0]["hasProbe"].get<bool>()&&rows[1]["hasProbe"].get<bool>();
        });
        page=snapshot["fallback"];
        h::Require(page["rows"][0]["probeOk"].get<bool>()&&page["rows"][0]["probeCount"].get<int>()==7,
            "the first row probed ●7");
        h::Require(!page["rows"][1]["probeOk"].get<bool>(),"the unreachable row probed ○");
        // 每行一次只读 probe（带该行 base_url/key_env）：真助手的 fallback-probe 忽略 index 且
        // 一次性返回全部行，既没有逐行进度，has_model 判定也与面板看到的模型名写法不一致。
        h::Require(Requests(logPath,"probe").size()>=2,"one probe call per row");
        h::Require(Requests(logPath,"fallback-probe").empty(),
            "the panel must not use the all-rows fallback-probe call any more");
        std::cout<<"PASS 逐条探测 produced one result per row (●7 / ○)\n";
        // 探测结果的悬停说明：绿=含该模型、红=连不上；夹具切到 probe_missing 后变琥珀=不含该模型。
        {
            const auto bounds=client.Request({{"command","dump"}})["panel"];
            const auto hoverDot=[&](const Json& rect) {
                const int x=rect["x"].get<int>()-bounds[0].get<int>()+rect["width"].get<int>()/2;
                const int y=rect["y"].get<int>()-bounds[1].get<int>()+rect["height"].get<int>()/2;
                std::string text;
                for(int attempt=0;attempt<3&&text.empty();++attempt) {
                    client.Request({{"command","hover"},{"x",x},{"y",y}});
                    Sleep(1200);
                    text=client.Request({{"command","dump"}})["tooltip"]["text"].get<std::string>();
                }
                return text;
            };
            auto dotControls=client.Request({{"command","controls"}});
            const auto dots=dotControls["_probeDots"];
            h::Require(dots.size()==2,"the layout exposes both probe dots");
            const auto green=hoverDot(dots[0]);
            h::Require(green.find("\xE5\x90\xAB\xE8\xAF\xA5\xE6\xA8\xA1\xE5\x9E\x8B")!=std::string::npos,
                "the green dot says the model is listed");
            const auto red=hoverDot(dots[1]);
            h::Require(red.find("\xE8\xBF\x9E\xE4\xB8\x8D\xE4\xB8\x8A")!=std::string::npos,
                "the red dot says the provider is unreachable");
            // 连接层抖动（供应商偶发 reset）：重试一次后必须还是●（真机 s3ai 4 次里 1 次）。
            {
                std::ofstream flaky(Narrow(home)+"/probe_flaky",std::ios::trunc);
                flaky<<"1\n";
                DeleteFileW((home+L"/probe_flaky_fired").c_str());
                const auto before=Requests(logPath,"probe").size();
                dotControls=client.Request({{"command","controls"}});
                ClickPoint(client,dotControls["fbProbe"]);
                snapshot=WaitFor(client,[](const Json& value) {
                    const auto rows=value["fallback"]["rows"];
                    return rows.size()==2&&rows[0]["probeOk"].get<bool>()
                        &&rows[0]["probeHasModel"].get<bool>();
                });
                h::Require(Requests(logPath,"probe").size()-before>=2,
                    "a transport-level probe failure was not retried");
                DeleteFileW((home+L"/probe_flaky").c_str());
                DeleteFileW((home+L"/probe_flaky_fired").c_str());
                std::cout<<"PASS probe retries once when the provider drops the connection\n";
            }
            {
                std::ofstream missing(Narrow(home)+"/probe_missing",std::ios::trunc);
                missing<<"1\n";
            }
            dotControls=client.Request({{"command","controls"}});
            ClickPoint(client,dotControls["fbProbe"]);
            WaitFor(client,[](const Json& value) {
                const auto rows=value["fallback"]["rows"];
                return rows.size()==2&&rows[0]["probeOk"].get<bool>()
                    &&!rows[0]["probeHasModel"].get<bool>();
            });
            dotControls=client.Request({{"command","controls"}});
            const auto amber=hoverDot(dotControls["_probeDots"][0]);
            h::Require(amber.find("\xE4\xB8\x8D\xE5\x90\xAB\xE8\xAF\xA5\xE6\xA8\xA1\xE5\x9E\x8B")!=std::string::npos,
                "the amber dot says the model is missing");
            DeleteFileW((home+L"\\probe_missing").c_str());
            std::cout<<"PASS probe tooltip: green / amber / red each explain themselves\n";
        }
        // ---- 3. 下移第一条：顺序交换并把 fallback-move 交给助手 ----
        controls=client.Request({{"command","controls"}});
        const auto ladder=controls["_list"]["ladder"];
        h::Require(static_cast<int>(ladder.size())==2,"the layout exposes both ladder rows");
        ClickPoint(client,ladder[0]["down"]);
        snapshot=WaitFor(client,[](const Json& value) {
            const auto rows=value["fallback"]["rows"];
            return rows.size()==2&&rows[0]["model"].get<std::string>()=="gamma-pro";
        });
        h::Require(snapshot["fallback"]["rows"][0]["model"].get<std::string>()=="gamma-pro",
            "↓ moved the entry down the chain");
        const auto moves=Requests(logPath,"fallback-move");
        h::Require(moves.size()==1&&moves[0]["index"].get<int>()==0&&moves[0]["to"].get<int>()==1,
            "fallback-move carried index 0 / to 1 (the helper's real contract)");
        std::cout<<"PASS the ladder's ↓ button moved the entry and called fallback-move\n";
        // ---- 4. 上移把它放回去（边界：第一条的上移按钮不生效） ----
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["_list"]["ladder"][1]["up"]);
        snapshot=WaitFor(client,[](const Json& value) {
            return value["fallback"]["rows"][0]["model"].get<std::string>()=="beta-1";
        });
        h::Require(snapshot["fallback"]["rows"][0]["model"].get<std::string>()=="beta-1","↑ put it back on top");
        ClickPoint(client,controls["_list"]["ladder"][0]["up"]);      // 第一条再上移：不该发生写
        Sleep(300);
        h::Require(Requests(logPath,"fallback-move").size()==2,"the first row's ↑ is a no-op");
        std::cout<<"PASS ↑ moved it back and the first row's ↑ is a no-op\n";
        // ---- 5. 移除要过危险确认（模态），Esc 取消不写 ----
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["_list"]["ladder"][1]["remove"]);
        snapshot=WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); },3000);
        h::Require(snapshot["modal"]["title"].get<std::string>()
            =="\xE4\xBB\x8E\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE\xE7\xA7\xBB\xE9\x99\xA4\xEF\xBC\x9F",
            "the remove button opened the confirm modal");
        h::Require(snapshot["modal"]["body"].get<std::string>().find("gamma-pro")!=std::string::npos,
            "the modal names the entry");
        h::Require(snapshot["modal"]["danger"].get<bool>(),"the remove confirm is a danger action");
        KeyDown(client,VK_ESCAPE);
        snapshot=client.Request({{"command","dump"}});
        h::Require(snapshot["modal"]["title"].get<std::string>().empty(),"Esc closed the modal");
        h::Require(Requests(logPath,"fallback-remove").empty(),"cancelling wrote nothing");
        std::cout<<"PASS the remove confirm is a danger modal and Esc cancels without a write\n";
        // ---- 6. 确认移除：点「移除」按钮，助手收到 fallback-remove ----
        ClickPoint(client,controls["_list"]["ladder"][1]["remove"]);
        WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
        ClickPoint(client,client.Request({{"command","controls"}})["modalConfirm"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["fallback"]["rows"].size()==1; });
        h::Require(snapshot["fallback"]["rows"].size()==1,"the confirmed remove trimmed the chain");
        h::Require(Requests(logPath,"fallback-remove").size()==1,"fallback-remove reached the helper");
        h::Require(snapshot["toast"]["text"].get<std::string>().find("\xE5\xB7\xB2\xE7\xA7\xBB\xE9\x99\xA4")
            !=std::string::npos,"a toast confirmed the removal");
        std::cout<<"PASS confirming the modal removed the entry and toasted\n";
        // ---- 7. 追加一条：折叠表单（下拉来源供货商 + 模型 id）+ fallback-add ----
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["fbFoldToggle"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["fallback"]["formOpen"].get<bool>(); });
        h::Require(snapshot["fallback"]["formOpen"].get<bool>(),"追加一条 opened the fold form");
        h::Require(snapshot["fallback"]["sources"][0].get<std::string>()
            =="\xE2\x80\x94 \xE6\x89\x8B\xE5\xA1\xAB provider + base_url \xE2\x80\x94","the select leads with the manual entry");
        h::Require(snapshot["fallback"]["sources"].size()==3,"the select lists both providers");
        // 先点「追加到链尾」不填模型：错误行亮起，不写
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["fbSubmit"]);
        snapshot=client.Request({{"command","dump"}});
        h::Require(!snapshot["fallback"]["error"].get<std::string>().empty(),"an empty model id is refused");
        h::Require(Requests(logPath,"fallback-add").empty(),"the refusal wrote nothing");
        std::cout<<"PASS the fold form refuses an empty model id\n";
        // 选来源供货商（Alpha）→ 手填字段收起 → 填模型 → 追加
        ClickPoint(client,controls["fbSelect"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["fallback"]["selectOpen"].get<bool>(); });
        h::Require(snapshot["fallback"]["selectOpen"].get<bool>(),"the select opened");
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["_select"][1]);                    // 第 0 项是手填，第 1 项是 Alpha
        snapshot=WaitFor(client,[](const Json& value) {
            return value["fallback"]["source"].get<int>()==1&&!value["fallback"]["selectOpen"].get<bool>();
        });
        h::Require(snapshot["fallback"]["source"].get<int>()==1,"picking a provider set the source");
        h::Require(snapshot["fallback"]["manual"].get<bool>()==false,"the manual fields collapse");
        TypeText(client,"alpha-2");
        controls=client.Request({{"command","controls"}});
        ClickPoint(client,controls["_fields"][1]);                    // 模型 id 字段
        TypeText(client,"alpha-2");
        ClickPoint(client,controls["fbSubmit"]);
        snapshot=WaitFor(client,[](const Json& value) { return value["fallback"]["rows"].size()==2; });
        h::Require(snapshot["fallback"]["rows"].size()==2,"the entry was appended");
        h::Require(snapshot["fallback"]["rows"][1]["model"].get<std::string>()=="alpha-2",
            "the appended entry is at the tail");
        const auto adds=Requests(logPath,"fallback-add");
        h::Require(adds.size()==1&&adds[0]["from_custom"].get<std::string>()=="alpha"
            &&adds[0]["model"].get<std::string>()=="alpha-2","fallback-add carried from_custom + model");
        h::Require(snapshot["fallback"]["modelValue"].get<std::string>().empty(),
            "the model field is cleared after a successful add");
        std::cout<<"PASS the fold form appended via the provider select and cleared itself\n";
        // ---- N. 写入前先看 diff（previewDiff）：开启后写操作先 dry_run 出预演卡，确认才真写 ----
        {
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["tabSettings"]);
            WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==4; });
            Settle(client);
            controls=client.Request({{"command","controls"}});
            const auto switches=controls["_switches"];
            h::Require(switches.size()>=1,"the settings page exposes the switch rows");
            ClickPoint(client,switches[0]);                    // 开关行 0 = 写入前先看 diff
            auto settings=WaitFor(client,[](const Json& value) { return value["stored"]["previewDiff"].get<bool>(); });
            h::Require(settings["stored"]["previewDiff"].get<bool>(),"the previewDiff switch toggled on");
            // 用「追加一条」（fallback-add）做一次写操作：它是助手真正支持 dry_run 的命令。
            // 应弹预演卡，助手收到的请求带 dry_run，链不变。
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["tabFallback"]);
            WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==1; });
            Settle(client);
            // 折叠表单在上面的用例里已经打开并选好了来源供应商；没开就重新打开。
            if(!client.Request({{"command","dump"}})["fallback"]["formOpen"].get<bool>()) {
                controls=client.Request({{"command","controls"}});
                ClickPoint(client,controls["fbFoldToggle"]);
                WaitFor(client,[](const Json& value) { return value["fallback"]["formOpen"].get<bool>(); });
            }
            const auto addsBefore=Requests(logPath,"fallback-add");
            const auto rowsBefore=client.Request({{"command","dump"}})["fallback"]["rows"].size();
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["_fields"][1]);
            TypeText(client,"alpha-3");
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["fbSubmit"]);
            const auto preview=WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()!=""; });
            h::Require(preview["modal"]["title"].get<std::string>().find("\xE5\x86\x99\xE5\x85\xA5\xE9\xA2\x84\xE8\xA7\x88")==0,
                "the write opened the 写入预览 modal");
            h::Require(!preview["modal"]["facts"].empty(),"the preview modal carries diff facts");
            const auto dryRuns=Requests(logPath,"fallback-add");
            h::Require(dryRuns.size()==addsBefore.size()+1&&dryRuns.back().value("dry_run",false)==true,
                "the pre-confirm request carried dry_run");
            h::Require(client.Request({{"command","dump"}})["fallback"]["rows"].size()==rowsBefore,
                "nothing was written before confirmation");
            std::cout<<"PASS previewDiff: the write first returns a dry-run diff in a confirm card\n";
            // 确认写入：请求重发（dry_run=false）且链真的变长
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["modalConfirm"]);
            WaitFor(client,[&](const Json& value) {
                return value["fallback"]["rows"].size()==rowsBefore+1; });
            const auto addsAfter=Requests(logPath,"fallback-add");
            h::Require(addsAfter.size()==addsBefore.size()+2&&addsAfter.back().value("dry_run",false)==false,
                "confirmation re-sent the write without dry_run");
            std::cout<<"PASS previewDiff: confirmation writes for real (dry_run=false)\n";
            // 审计 F-003：助手不支持 dry_run 的命令（移除）绝不能被注入 dry_run，否则会直接落盘。
            const auto removesBefore=Requests(logPath,"fallback-remove");
            controls=client.Request({{"command","controls"}});
            ClickPoint(client,controls["_list"]["ladder"][0]["remove"]);
            WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
            client.Request({{"command","vk"},{"vk",13}});      // Enter = 确认移除
            WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>().empty(); });
            // 移除请求可能排在刚结束的写操作后面（队列）：等它真的到助手再断言。
            {
                const auto deadline=GetTickCount64()+15000;
                while(GetTickCount64()<deadline
                    &&Requests(logPath,"fallback-remove").size()==removesBefore.size()) Sleep(50);
            }
            const auto removesAfter=Requests(logPath,"fallback-remove");
            h::Require(removesAfter.size()==removesBefore.size()+1
                &&!removesAfter.back().contains("dry_run"),
                "previewDiff never injects dry_run into commands the helper cannot preview");
            std::cout<<"PASS previewDiff: unsupported writes are not falsely previewed\n";
        }
        status=0;
    } catch(const std::exception& error) {
        std::cerr<<"h-fallback: "<<error.what()<<"\n";
        status=1;
    }
    TerminateProcess(child.hProcess,status);
    CloseHandle(child.hProcess);CloseHandle(child.hThread);
    return status;
}
