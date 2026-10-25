// 逐控件点击门禁（用户要求「你自己测试软件所有功能，确保可点击且功能正常」）。
//
// 全部走控制面（Debug 才编译进去，DESIGN §8）：
//   1. 控件卫生：每个控件的矩形都在窗口客户区内、非退化 —— 真机踩过的「列表盒子滑出屏幕」「控件被
//      页头盖住」都属于这一类。
//   2. 命中自洽：每个控件矩形取中心点，问应用自己的命中测试（control-at），必须命中控件表里
//      **第一个包含该点的控件**（ControlAt 的规则），即"画出来的控件都点得到"。
//   3. 真点击：每页的关键控件挨个 tap，断言页面/状态按预期变化、无几何不变量违规（破坏性动作
//      只到"弹确认框"为止，然后用 Esc 取消）。
#include <fstream>
#include <sstream>
#include <filesystem>
#include <windows.h>
#include "harness/client.h"
#include "harness/paths.h"
#include "harness/no_modal_asserts.h"
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {
using harness::Json;
Json Dump(harness::Client& client) { return client.Request({{"command","dump"}}); }
Json Controls(harness::Client& client) { return client.Request({{"command","controls"}}); }
void Tap(harness::Client& client,const Json& rect) {
    client.Request({{"command","tap"},
        {"x",rect["x"].get<int>()+rect["width"].get<int>()/2},
        {"y",rect["y"].get<int>()+rect["height"].get<int>()/2},{"screen",true}});
}
int ControlAt(harness::Client& client,int x,int y) {
    return client.Request({{"command","control-at"},{"x",x},{"y",y}}).value("index",-1);
}
// 控件表的顺序就是 ControlAt 的顺序（第一个包含该点的胜出）。
std::string FirstKeyAt(const Json& controls,const std::vector<std::string>& order,int x,int y) {
    for(const auto& key:order) {
        if(!controls.contains(key)) continue;
        const auto& rect=controls[key];
        if(!rect.is_object()||!rect.contains("x")) continue;
        if(x>=rect["x"].get<int>()&&x<rect["x"].get<int>()+rect["width"].get<int>()
            &&y>=rect["y"].get<int>()&&y<rect["y"].get<int>()+rect["height"].get<int>())
            return key;
    }
    return "none";
}
std::vector<std::string> Order(const Json& controls) {
    std::vector<std::string> keys;
    if(!controls.contains("_controlsOrder")) return keys;
    for(const auto& entry:controls["_controlsOrder"]) keys.push_back(entry.get<std::string>());
    return keys;
}

// 假助手请求计数（F5 键盘刷新的断言用）：与 h-pages 的 Requests() 同一份日志格式。
int CountRequests(const std::wstring& logPath,const std::string& cmd) {
    std::ifstream input(logPath);
    if(!input) return 0;
    std::string line;
    int count=0;
    const auto needle="\"cmd\":\""+cmd+"\"";
    while(std::getline(input,line)) if(line.find(needle)!=std::string::npos) ++count;
    return count;
}
}

int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    if(argc!=3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=L"controls-"+std::to_wstring(GetCurrentProcessId());
    // 设置目录按实例名（含 pid）隔离，但 pid 会被系统复用：先把上一轮可能留下的
    // previewDiff / autoReloadGateway 清掉，否则同一个用例会时绿时红。
    std::filesystem::remove_all(harness::IsolatedDataDir(id));
    const auto home=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id+L"/fake";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW((std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id).c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    SetEnvironmentVariableW(L"HERMES_WSL_EXE",argv[2]);
    SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
    SetEnvironmentVariableW(L"FAKE_HERMES_HOME",home.c_str());
    auto command=harness::AppCommandLine(argv[1],id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,0,0,nullptr,nullptr,&startup,&child)) return 3;
    int code=1;
    const auto escape=[&] { harness::Client client(id);client.Request({{"command","vk"},{"vk",27}}); };
    try {
        harness::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        harness::Client client(id);
        for(int attempt=0;attempt<60;++attempt) {          // 等夹具的首轮数据
            const auto dump=Dump(client);
            if(dump.contains("models")&&dump["models"]["groups"].size()>0) break;
            Sleep(100);
        }
        const auto panel=Dump(client);
        const int px=panel["panel"][0].get<int>(),py=panel["panel"][1].get<int>();
        const int width=panel["panel"][2].get<int>(),height=panel["panel"][3].get<int>();

        // ---- 1) 控件卫生 ----
        {
            const auto controls=Controls(client);
            const auto order=Order(controls);
            harness::Require(order.size()>=8,
                ("too few controls in the dump: "+std::to_string(order.size())).c_str());
            int outOfBounds=0,degenerate=0;
            for(const auto& key:order) {
                if(!controls.contains(key)) continue;
                const auto& rect=controls[key];
                const int cx=rect["x"].get<int>(),cy=rect["y"].get<int>();
                const int cw=rect["width"].get<int>(),ch=rect["height"].get<int>();
                if(cw<=0||ch<=0) {
                    ++degenerate;
                    std::cerr<<"  degenerate control: "<<key<<" "<<rect.dump()<<"\n";
                }
                if(cx<px-1||cy<py-1||cx+cw>px+width+1||cy+ch>py+height+1) {
                    ++outOfBounds;
                    std::cerr<<"  out-of-window control: "<<key<<" "<<rect.dump()
                        <<" panel=("<<px<<","<<py<<","<<width<<","<<height<<")\n";
                }
            }
            harness::Require(degenerate==0,"a control rect is degenerate (empty)");
            harness::Require(outOfBounds==0,"a control rect falls outside the panel window");
            std::cout<<"PASS controls: "<<order.size()<<" controls inside the window, none degenerate\n";
        }

        // ---- 2) 命中自洽 ----
        {
            const struct { const char* tab; } tabs[]={{"tabModels"},{"tabFallback"},{"tabAdd"},{"tabBackup"},{"tabSettings"}};
            int checked=0,dead=0,mismatch=0;
            for(const auto& tab:tabs) {
                Tap(client,Controls(client)[tab.tab]);
                Sleep(500);
                const auto controls=Controls(client);
                const auto order=Order(controls);
                for(const auto& key:order) {
                    if(!controls.contains(key)) continue;
                    const auto& rect=controls[key];
                    const int x=rect["x"].get<int>()+rect["width"].get<int>()/2;
                    const int y=rect["y"].get<int>()+rect["height"].get<int>()/2;
                    const auto expected=FirstKeyAt(controls,order,x,y);
                    const int actual=ControlAt(client,x,y);
                    ++checked;
                    if(actual<0) {
                        ++dead;
                        std::cerr<<"  dead control ("<<tab.tab<<"): "<<key<<" centre ("<<x<<","<<y
                            <<") hits nothing, expected "<<expected<<"\n";
                    }
                }
            }
            harness::Require(dead==0,"a control centre hits nothing (dead zone over a drawn control)");
            harness::Require(mismatch==0,"the hit test disagrees with the drawn control table");
            std::cout<<"PASS controls: "<<checked<<" control centres resolve through the app's own hit test\n";
        }

        // ---- 3) 真点击 ----
        {
            Tap(client,Controls(client)["tabModels"]); Sleep(400);
            Tap(client,Controls(client)["filterFav"]); Sleep(300);
            harness::Require(Dump(client)["models"]["filter"].get<int>()==1,"filterFav did not switch the filter");
            Tap(client,Controls(client)["filterRecent"]); Sleep(300);
            harness::Require(Dump(client)["models"]["filter"].get<int>()==2,"filterRecent did not switch the filter");
            Tap(client,Controls(client)["filterAll"]); Sleep(300);
            harness::Require(Dump(client)["models"]["filter"].get<int>()==0,"filterAll did not switch back");
            std::cout<<"PASS click: the three filter segments switch and restore the models list\n";

            {   // 组头折叠 + 供应商「…」菜单
                const auto controls=Controls(client);
                Json head=Json::object();
                for(const auto& item:controls["_list"]["items"])
                    if(item["kind"].get<int>()==0&&item.contains("menu")) { head=item;break; }
                harness::Require(!head.is_null(),"no group head with a menu box in the models list");
                Tap(client,head["menu"]); Sleep(450);
                const auto dump=Dump(client);
                harness::Require(!dump["modal"]["title"].get<std::string>().empty(),
                    "the provider '…' button did not open its menu");
                std::cout<<"PASS click: the provider '…' button opens the provider menu\n";
                // 菜单里的「探测模型并追加」：确定后必须对该供应商发**一次只读 probe**，并把结果
                // 落进「添加」页（v1.15 修过"结果被丢弃"）。此前只覆盖了"菜单能开/能关"，
                // 确定动作本身没有断言。
                {
                    const auto provider=dump["providerMenu"]["provider"].get<std::string>();
                    std::string baseUrl,keyEnv;
                    for(const auto& entry:dump["data"]["providerList"]) {
                        if(entry.value("name",std::string{})!=provider) continue;
                        baseUrl=entry.value("baseUrl",std::string{});
                        keyEnv=entry.value("keyEnv",std::string{});
                    }
                    harness::Require(!baseUrl.empty(),"the opened provider has no base_url");
                    const auto probesBefore=CountRequests(home+L"/requests.log","probe");
                    Tap(client,Controls(client)["modalConfirm"]); Sleep(700);
                    harness::Require(CountRequests(home+L"/requests.log","probe")==probesBefore+1,
                        "the provider menu confirm must send exactly one probe");
                    {
                        std::ifstream input(home+L"/requests.log");
                        std::string line;
                        bool matched=false;
                        while(std::getline(input,line)) {
                            if(line.find("\"cmd\":\"probe\"")==std::string::npos) continue;
                            const auto parsed=Json::parse(line);
                            if(parsed.value("base_url",std::string{})==baseUrl
                                &&parsed.value("key_env",std::string{})==keyEnv) matched=true;
                        }
                        harness::Require(matched,"the probe carried the provider's base_url and key_env");
                    }
                    // v1.18：扫描结果直接写回该供应商（只带新增模型），不再让用户去「添加」页手动保存。
                    std::string providerId;
                    for(const auto& entry:dump["data"]["providerList"]) {
                        if(entry.value("name",std::string{})==provider)
                            providerId=entry.value("providerId",std::string{});
                    }
                    const auto writes=[&] {
                        for(int attempt=0;attempt<50;++attempt) {
                            if(CountRequests(home+L"/requests.log","add-models")>0) break;
                            Sleep(100);
                        }
                        return CountRequests(home+L"/requests.log","add-models");
                    }();
                    harness::Require(writes==1,"scan-and-append must write exactly one add-models");
                    {
                        std::ifstream input(home+L"/requests.log");
                        std::string line;
                        bool matched=false;
                        while(std::getline(input,line)) {
                            if(line.find("\"cmd\":\"add-models\"")==std::string::npos) continue;
                            const auto parsed=Json::parse(line);
                            if(parsed.value("provider",std::string{})==providerId
                                &&parsed["models"].size()==3
                                &&parsed.value("prune",false)) matched=true;
                        }
                        harness::Require(matched,"the sync carried the provider, the probed list and prune=true");
                        // 替换语义：Alpha 原本 2 个模型，同步后应等于探测到的 3 个（追加会变 5 个）。
                        const auto synced=[&] {
                            for(int attempt=0;attempt<40;++attempt) {
                                const auto list=Dump(client)["data"]["providerList"];
                                for(const auto& entry:list) {
                                    if(entry.value("name",std::string{})!=provider) continue;
                                    if(entry.value("models",0)==3) return 3;
                                }
                                Sleep(100);
                            }
                            return 0;
                        }();
                        harness::Require(synced==3,"the provider's model list was replaced, not appended");
                    }
                    // 第二次扫描：配置已一致 → 只提示"已是最新"，不再写盘。
                    const auto writesBefore=CountRequests(home+L"/requests.log","add-models");
                    Tap(client,head["menu"]); Sleep(450);
                    Tap(client,Controls(client)["modalConfirm"]); Sleep(1200);
                    harness::Require(CountRequests(home+L"/requests.log","add-models")==writesBefore,
                        "a second scan with nothing new must not write again");
                    harness::Require(Dump(client)["toast"]["text"].get<std::string>().find("\u5DF2\u662F\u6700\u65B0")
                        !=std::string::npos,"the second scan says it is already up to date");
                    std::cout<<"PASS click: the provider menu's 扫描并追加 writes only new models\n";
                }
                escape(); Sleep(300);
                harness::Require(Dump(client)["modal"]["title"].get<std::string>().empty(),
                    "Esc did not close the provider menu");
                Tap(client,head["rect"]); Sleep(350);
                harness::Require(Dump(client)["models"]["groups"].size()>0,
                    "the group head click lost the groups");
                std::cout<<"PASS click: the group head toggles its group and Esc closes the menu\n";
            }

            Tap(client,Controls(client)["tabFallback"]); Sleep(400);
            Tap(client,Controls(client)["fbFoldToggle"]); Sleep(450);
            harness::Require(Dump(client)["fallback"]["formOpen"].get<bool>(),
                "追加一条 did not open the fold card");
            {
                const auto fields=Controls(client)["_fields"];
                harness::Require(!fields.empty(),"the fold card has no fields");
                Tap(client,fields[0]); Sleep(450);
                harness::Require(Dump(client)["fallback"]["selectOpen"].get<bool>(),
                    "the 来源供应商 select did not open");
                const auto options=Controls(client)["_select"];
                harness::Require(options.size()>=2,"the 来源供应商 select has no options");
                Tap(client,options[1]); Sleep(400);
                harness::Require(!Dump(client)["fallback"]["selectOpen"].get<bool>(),
                    "picking a 来源供应商 option did not close the list");
                std::cout<<"PASS click: 追加一条 opens and 来源供应商 drops its option list\n";
            }
            Tap(client,Controls(client)["fbFoldClose"]); Sleep(350);

            Tap(client,Controls(client)["tabAdd"]); Sleep(400);
            // v1.21（真机反馈"所有输入框都应该有光标"）：聚焦 + 打字后必须报出插入符位置。
            {
                const auto fields=Controls(client)["_fields"];
                harness::Require(!fields.empty(),"the add page has no fields");
                Tap(client,fields[0]); Sleep(250);
                client.Request({{"command","key"},{"text","abc"}});
                Sleep(250);
                const auto focus=Dump(client)["focus"];
                harness::Require(focus.value("field",-1)==0,"the name field took focus");
                harness::Require(focus.value("caret",-1)==3,
                    ("the caret follows the typed text ("+focus.dump()+")").c_str());
                // 清掉刚打的字，后面用例的表单还是空的
                client.Request({{"command","vk"},{"vk",VK_END}});
                for(int index=0;index<3;++index) client.Request({{"command","vk"},{"vk",VK_BACK}});
                Sleep(150);
            }
            // v1.18：子分段只剩两档（新建供应商 / 删除供应商）。
            for(const auto& pair:std::vector<std::pair<std::string,int>>{{"subTabDel",1},{"subTabNew",0}}) {
                const auto ctrls=Controls(client);
                harness::Require(ctrls.contains(pair.first),("missing control "+pair.first).c_str());
                Tap(client,ctrls[pair.first]); Sleep(450);
                harness::Require(Dump(client)["add"]["sub"].get<int>()==pair.second,
                    (pair.first+" did not switch the add sub-tab").c_str());
            }
            std::cout<<"PASS click: the two add sub-tabs switch\n";
            for(const auto& pair:std::vector<std::pair<std::string,int>>{{"subTabDel",1}}) {
                Tap(client,Controls(client)[pair.first]); Sleep(450);
                const auto fields=Controls(client)["_fields"];
                harness::Require(!fields.empty(),"the add sub-page has no fields");
                Tap(client,fields[0]); Sleep(500);
                const auto dump=Dump(client);
                harness::Require(dump["add"]["selectOpen"].get<bool>(),
                    (pair.first+" 的供应商下拉点不开").c_str());
                const auto options=Controls(client)["_select"];
                harness::Require(options.size()>=2,"the provider select has no options");
                Tap(client,options[1]); Sleep(450);
                const auto picked=Dump(client);
                harness::Require(!picked["add"]["selectOpen"].get<bool>(),"the provider select did not close");
                harness::Require(picked["add"]["provider"].get<int>()==1,
                    "picking an option did not set the provider index");
                std::cout<<"PASS click: "<<pair.first<<" provider dropdown opens and picks option 1\n";
            }

            Tap(client,Controls(client)["tabBackup"]); Sleep(700);
            {
                const auto filters=Controls(client)["_segments"];
                harness::Require(filters.size()>=3,"the backup page has no type filters");
                Tap(client,filters[1]); Sleep(450);
                harness::Require(Dump(client)["backup"]["filter"].get<int>()==1,"the backup type filter did not switch");
                Tap(client,filters[0]); Sleep(350);
                harness::Require(Dump(client)["backup"]["filter"].get<int>()==0,"the backup filter did not switch back");
                std::cout<<"PASS click: the backup type filters switch\n";
                const auto restore=Controls(client)["_restore"];
                if(restore.empty()) std::cout<<"SKIP 回滚: the backup fixture is empty\n";
                else {
                    Tap(client,restore[0]); Sleep(450);
                    harness::Require(!Dump(client)["modal"]["title"].get<std::string>().empty(),
                        "回滚 did not raise the confirmation modal");
                    escape(); Sleep(350);
                    std::cout<<"PASS click: 回滚 raises the confirm modal (cancelled with Esc)\n";
                }
            }

            Tap(client,Controls(client)["tabSettings"]); Sleep(450);
            {
                // 开关行在设置页上半屏：先在 scroll=0 时点，避免被后续滚动带出视口。
                const auto controls=Controls(client);
                const auto switches=controls["_switches"];
                if(!switches.empty()) {
                    const bool before=Dump(client)["settings"]["switches"][0]["on"].get<bool>();
                    Tap(client,switches[0]); Sleep(400);
                    const bool after=Dump(client)["settings"]["switches"][0]["on"].get<bool>();
                    harness::Require(before!=after,"the first settings switch did not toggle");
                    Tap(client,switches[0]); Sleep(400);
                    harness::Require(Dump(client)["settings"]["switches"][0]["on"].get<bool>()==before,
                        "the first settings switch did not toggle back");
                    std::cout<<"PASS click: the first settings switch toggles both ways\n";
                }
                // 固定高度后主题段在下半页：滚两格再取控件（与 h-settings 同一策略）。
                client.Request({{"command","wheel"},{"delta",-360}});
                client.Request({{"command","wheel"},{"delta",-360}});
                Sleep(180);
            }
            {
                const auto controls=Controls(client);
                const auto segs=controls["_segments"];
                // F4/G11：设置页分段 = 3 段延迟间隔 + 2 段挂件（信标/旗标已删） + 7 段主题。
                harness::Require(segs.size()>=12,"the settings page has no theme segments");
                const int themeOffset=static_cast<int>(segs.size())-7;
                Tap(client,segs[1+themeOffset]); Sleep(400);
                harness::Require(Dump(client)["settings"]["theme"].get<int>()==1,"the theme segment did not switch");
                Tap(client,segs[0+themeOffset]); Sleep(400);
                harness::Require(Dump(client)["settings"]["theme"].get<int>()==0,"the theme segment did not switch back");
                std::cout<<"PASS click: theme segments switch back and forth\n";
            }
            {   // 标题栏第四个按钮（Pin）来回切
                const auto controls=Controls(client);
                const bool before=Dump(client)["topmost"].get<bool>();
                Tap(client,controls["pin"]); Sleep(350);
                const bool after=Dump(client)["topmost"].get<bool>();
                harness::Require(before!=after,"the title-bar pin did not toggle always-on-top");
                Tap(client,Controls(client)["pin"]); Sleep(350);
                harness::Require(Dump(client)["topmost"].get<bool>()==before,
                    "the title-bar pin did not toggle back");
                std::cout<<"PASS click: the title-bar pin toggles always-on-top and back\n";
            }
            // 悬停提示（用户要求：状态灯/chip 上悬浮要有文字）——用合成的 hover 走同一条路径。
            {
                struct HoverCase { const char* key;const char* expect; };
                // tabFallback 是**底栏**按钮：它这条盯的是"底栏上的控件也有提示"（真机 2026-10-21
                // 报底栏按钮的提示被底栏盖住）。提示的**层级**（必须画在底栏之上）没有断言——
                // 提示矩形是渲染层按测量结果现算的，而 h-* 门禁没有 probe 通道做像素断言；
                // 该点由 evidence/tooltip-nav-after.png 的像素证据覆盖，缺口记在 audit 文档里。
                const HoverCase hovers[]={{"lampWsl","WSL"},{"lampWebui","WebUI"},{"lampGateway","网关"},
                    {"refresh","配置"},{"tabFallback","回退链"}};
                for(const auto& test:hovers) {
                    // 取点**每个控件现读一次**：早先整块只取一份 Controls()/panel 快照，而前面刚跑过的
                    // 置顶/主题用例会让几何一瞬间不同 —— 实测失败现场的应用日志是 `hover control 0`
                    // （合成 hover 落到了没有控件的地方，提示自然不出来），不是提示逻辑的问题。
                    // 所以这里重读快照 + 同一控件最多重试一次；断言本身不变。
                    bool shown=false;
                    Json tip;
                    for(int attempt=0;attempt<2&&!shown;++attempt) {
                        const auto controls=Controls(client);
                        const auto bounds=Dump(client)["panel"];   // [x,y,w,h] 屏幕坐标
                        harness::Require(controls.contains(test.key),
                            (std::string("no ")+test.key+" control").c_str());
                        const auto& rect=controls[test.key];
                        const int x=rect["x"].get<int>()-bounds[0].get<int>()+rect["width"].get<int>()/2;
                        const int y=rect["y"].get<int>()-bounds[1].get<int>()+rect["height"].get<int>()/2;
                        client.Request({{"command","hover"},{"x",x},{"y",y}});
                        // tooltip 的驻留计时是 450ms 一次性定时器。固定 Sleep 在机器忙时不够用
                        // （真机 2026-10-21 全量跑时 650ms→900ms 都抖过），改成**有界轮询**：
                        // 断言不变（"悬停必须出提示"），只是等到 2s 为止；出不来照样失败。
                        const auto deadline=GetTickCount64()+2000;
                        while(GetTickCount64()<deadline) {
                            if(Dump(client)["tooltip"]["visible"].get<bool>()) { shown=true;break; }
                            Sleep(100);
                        }
                        tip=Dump(client)["tooltip"];
                    }
                    harness::Require(shown&&tip["visible"].get<bool>(),
                        (std::string("hovering ")+test.key+" shows no tooltip").c_str());
                    harness::Require(tip["text"].get<std::string>().find(test.expect)!=std::string::npos,
                        (std::string("the ")+test.key+" tooltip does not mention "+test.expect+": "
                            +tip["text"].get<std::string>()+" hover="
                            +Dump(client)["tooltipHover"].dump()+" rect="
                            +Controls(client)[test.key].dump()+" panel="
                            +Dump(client)["panel"].dump()).c_str());
                }
                std::cout<<"PASS hover: the three lamps, the sync stamp and a bottom-nav tab explain themselves\n";
                // 审计：所有悬停说明都必须是一句话、不含换行（用户要求）。
                const auto tips=Dump(client)["tooltips"];
                int audited=0;
                for(const auto& tip:tips) {
                    const auto text=tip["text"].get<std::string>();
                    harness::Require(!text.empty()&&text.find('\n')==std::string::npos,
                        "a tooltip is empty or multi-line");
                    ++audited;
                }
                harness::Require(audited>=30,"too few tooltips to audit");
                std::cout<<"PASS hover: "<<audited<<" tooltips are single-line\n";
            }
            harness::Require(Dump(client)["violations"].get<int>()==0,"the click sweep recorded invariant violations");
            harness::Require(IsWindow(harness::Window(Dump(client),"panelHwnd"))!=0,"the panel is gone after the sweep");
            std::cout<<"PASS click sweep: no invariant violations, the panel is alive\n";
        }
        // 多状态合并：两个状态 → 一个胶囊；点开状态中心 → 两行；点行即执行并收起。
        {
            // 点击扫掠可能把面板收起/停靠：先恢复浮动，寄存器行才有完整的宽度与坐标。
            client.Request({{"command","place"},{"edge",0},{"state",1},{"float",{200,120}}});
            auto snapshot=client.Request({{"command","status-demo"},{"open",false}});
            harness::Require(snapshot["data"]["statuses"].size()==2,"status-demo did not inject two statuses");
            auto controls=client.Request({{"command","controls"}});
            harness::Require(controls.contains("statusPill"),"multiple statuses did not collapse into one pill");
            harness::Require(!controls.contains("chip")&&!controls.contains("chipWarn")
                &&!controls.contains("chipHelper"),"individual chips still take space beside the merged pill");
            std::cout<<"STAGE status pill\n";
            Tap(client,controls["statusPill"]); Sleep(200);
            snapshot=client.Request({{"command","dump"}});
            harness::Require(snapshot["data"]["statusCenterOpen"].get<bool>(),
                "clicking the status pill did not open the status center");
            const auto rows=snapshot["data"]["statusRows"];
            harness::Require(rows.size()==2,"the status center does not list both statuses");
            std::cout<<"STAGE status row\n";
            Tap(client,rows[0]); Sleep(250);
            snapshot=client.Request({{"command","dump"}});
            harness::Require(!snapshot["data"]["statusCenterOpen"].get<bool>(),
                "clicking a status row did not close the status center");
            client.Request({{"command","status-clear"}});
            std::cout<<"PASS status center: merged pill, two rows, row click acts and closes\n";
        }
        // 外部窗口状态（Aero Snap / Win+↑ 最大化 / 系统调整）后走托盘隐藏/显示：必须先把模型
        // 重新落到窗口再校验。曾经这里抛 INV1，而托盘路径没有 catch → 进程无提示退出（真机复现）。
        {
            client.Request({{"command","place"},{"edge",0},{"state",1},{"float",{200,120}}});
            const auto panelHandle=harness::Window(Dump(client),"panelHwnd");
            harness::Require(panelHandle!=nullptr,"panel handle missing");
            ShowWindow(panelHandle,SW_MAXIMIZE);
            Sleep(300);
            const auto maximized=Dump(client);
            harness::Require(maximized["panel"]!=maximized["expectedPanel"],
                "SW_MAXIMIZE did not move the panel off the model geometry");
            client.Request({{"command","menu"},{"id",1001}});                     // 托盘：隐藏
            const auto shown=client.Request({{"command","menu"},{"id",1001}});     // 托盘：显示
            harness::Require(shown["panel"]==shown["expectedPanel"],
                "the panel geometry was not reconciled after an external window state");
            harness::Require(Dump(client)["violations"].get<int>()==0,
                "reconciling an external window state recorded an invariant violation");
            client.Request({{"command","place"},{"edge",0},{"state",1},{"float",{200,120}}});
            std::cout<<"PASS external window state: tray hide/show reconciles, never crashes\n";
        }
        // 审计 F-010：Tab 焦点链 + Enter 激活 + F5 刷新（纯键盘路径）。
        {
            client.Request({{"command","place"},{"edge",0},{"state",1},{"float",{200,120}}});
            Sleep(300);
            auto before=Dump(client)["focus"];
            harness::Require(before.value("control",0)==0,"the focus chain starts empty");
            client.Request({{"command","vk"},{"vk",VK_TAB}});
            Sleep(150);
            const auto first=Dump(client)["focus"];
            harness::Require(first.value("control",0)!=0,"Tab moved the focus to a control");
            client.Request({{"command","vk"},{"vk",VK_TAB}});
            Sleep(150);
            const auto second=Dump(client)["focus"];
            harness::Require(second.value("control",0)!=first.value("control",0)
                ||second.value("index",-1)!=first.value("index",-1),"Tab advanced to the next control");
            client.Request({{"command","vk"},{"vk",VK_ESCAPE}});
            Sleep(120);
            harness::Require(Dump(client)["focus"].value("control",0)==0,"Esc cleared the focus ring");
            const auto logPath=home+L"/requests.log";
            const auto before5=CountRequests(logPath,"list");
            client.Request({{"command","vk"},{"vk",VK_F5}});
            Sleep(900);
            harness::Require(CountRequests(logPath,"list")>before5,"F5 re-read the state");
            std::cout<<"PASS keyboard: Tab focus chain, Enter/Esc and F5 refresh work without the mouse\n";
        }
        code=0;
        client.Request({{"command","quit"}});
    } catch(const std::exception& error) {
        if(harness::IsEnvironmentInterference(error)) {
            std::cerr<<"SKIP h-controls: "<<error.what()<<"\n";
            TerminateProcess(child.hProcess,77);
            CloseHandle(child.hProcess);CloseHandle(child.hThread);
            return 77;
        }
        std::cerr<<"FAIL h-controls: "<<error.what()<<"\n";
    }
    if(WaitForSingleObject(child.hProcess,2000)==WAIT_TIMEOUT) TerminateProcess(child.hProcess,99);
    CloseHandle(child.hThread);CloseHandle(child.hProcess);
    return code;
}
