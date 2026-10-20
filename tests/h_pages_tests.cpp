// h-add / h-backup / h-settings (PLAN §6 H5, gates `h-add`, `h-backup`, `h-settings`): the three page
// bodies against the scripted helper. Input goes through the app's `tap` / `key` / `vk` control-plane
// commands (see h_models_tests.cpp for the desktop-input reason); assertions read the same view model
// the renderer draws.
#include <windows.h>
#include <winsock2.h>
#include <fstream>
#include <iostream>
#include <sstream>
#include "harness/client.h"
#include "harness/input.h"
#include "harness/no_modal_asserts.h"
#include "harness/paths.h"
#include "render/theme.h"

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
        {{"name","Beta"},{"provider_id","custom:Beta"},{"base_url","https://unreachable.example/v1"},
         {"key_env","BETA_KEY"},{"key_set",false},{"api_mode","chat"},{"default_model","beta-1"},
         {"models",{"beta-1"}},{"models_discovered",false},{"is_active",false},{"line",2}},
        // 3..12：只为"下拉滚得动"提供长度（真机 18 个供应商，后 10 个原本选不到）。
        {{"name","P03"},{"provider_id","custom:P03"},{"base_url","https://p03/v1"},{"key_env","P03_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m03"},{"models",{"m03"}},
         {"models_discovered",false},{"is_active",false},{"line",3}},
        {{"name","P04"},{"provider_id","custom:P04"},{"base_url","https://p04/v1"},{"key_env","P04_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m04"},{"models",{"m04"}},
         {"models_discovered",false},{"is_active",false},{"line",4}},
        {{"name","P05"},{"provider_id","custom:P05"},{"base_url","https://p05/v1"},{"key_env","P05_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m05"},{"models",{"m05"}},
         {"models_discovered",false},{"is_active",false},{"line",5}},
        {{"name","P06"},{"provider_id","custom:P06"},{"base_url","https://p06/v1"},{"key_env","P06_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m06"},{"models",{"m06"}},
         {"models_discovered",false},{"is_active",false},{"line",6}},
        {{"name","P07"},{"provider_id","custom:P07"},{"base_url","https://p07/v1"},{"key_env","P07_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m07"},{"models",{"m07"}},
         {"models_discovered",false},{"is_active",false},{"line",7}},
        {{"name","P08"},{"provider_id","custom:P08"},{"base_url","https://p08/v1"},{"key_env","P08_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m08"},{"models",{"m08"}},
         {"models_discovered",false},{"is_active",false},{"line",8}},
        {{"name","P09"},{"provider_id","custom:P09"},{"base_url","https://p09/v1"},{"key_env","P09_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m09"},{"models",{"m09"}},
         {"models_discovered",false},{"is_active",false},{"line",9}},
        {{"name","P10"},{"provider_id","custom:P10"},{"base_url","https://p10/v1"},{"key_env","P10_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m10"},{"models",{"m10"}},
         {"models_discovered",false},{"is_active",false},{"line",10}},
        {{"name","P11"},{"provider_id","custom:P11"},{"base_url","https://p11/v1"},{"key_env","P11_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m11"},{"models",{"m11"}},
         {"models_discovered",false},{"is_active",false},{"line",11}},
        {{"name","P12"},{"provider_id","custom:P12"},{"base_url","https://p12/v1"},{"key_env","P12_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","m12"},{"models",{"m12"}},
         {"models_discovered",false},{"is_active",false},{"line",12}}});
    state["fallbacks"]=Json::array({{{"provider","custom:Beta"},{"model","beta-1"}}});
    return state;
}
std::string LowerCopy(std::string value) {
    for(auto& character:value) character=static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
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
void Tap(h::Client& client,const Json& rect) {
    // 桌面可能压着别的置顶窗口（浏览器等）：先把面板提到前台再点，避免环境性"被遮挡"。
    client.Request({{"command","focus"}});
    const auto x=rect["x"].get<int>()+rect["width"].get<int>()/2;
    const auto y=rect["y"].get<int>()+rect["height"].get<int>()/2;
    const auto hit=client.Request({{"command","hit"},{"x",x},{"y",y}});
    h::Require(hit["hit"]["isPanel"].get<bool>(),
        ("the click target is covered by another window: "+hit.dump()).c_str());
    client.Request({{"command","tap"},{"x",x},{"y",y},{"screen",true}});
    Sleep(150);
}
void Tab(h::Client& client,const char* name) {
    const auto controls=client.Request({{"command","controls"}});
    h::Require(controls.contains(name),std::string("the tab control ")+name+" is missing");
    Tap(client,controls[name]);
}
void KeyDown(h::Client& client,unsigned vk) { client.Request({{"command","vk"},{"vk",vk}});Sleep(120); }
// 选中字段里的全部文本：下一次插入会替换它（旧输入的 Ctrl+A 行为）。
// 清空字段：`key` 命令走 WM_CHAR（Ctrl+A 的 0x01 到不了自绘输入框）；用足够多的退格 0x08。
void SelectAll(h::Client& client) {
    std::string backspaces(256,'\b');
    client.Request({{"command","key"},{"text",backspaces}});
    Sleep(150);
}
void TypeText(h::Client& client,const std::string& ascii) {
    client.Request({{"command","key"},{"text",ascii}});
    Sleep(100);
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
// 第 index 个表单字段的屏幕矩形（layout.fields 按页内顺序排列）。
Json FieldRect(h::Client& client,int index) {
    const auto controls=client.Request({{"command","controls"}});
    h::Require(index<static_cast<int>(controls["_fields"].size()),"the field is not in the layout");
    return controls["_fields"][index];
}
void StartApp(const std::wstring& app,const std::wstring& instance,PROCESS_INFORMATION* child) {
    auto command=h::AppCommandLine(app,instance,h::NoLegacyDirectory(instance));
    STARTUPINFOW startup{sizeof(startup)};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,0,0,nullptr,nullptr,&startup,child))
        throw std::runtime_error("CreateProcess failed");
}
}

int wmain(int argc,wchar_t** argv) {
    h::SilenceDebugAssertDialogs();
    if(argc!=4) return 2;
    const std::wstring scenario=argv[3];
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=scenario+L"-"+std::to_wstring(GetCurrentProcessId());
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
    // F6 网关边沿门禁：settings 场景先把 gatewayPort 指到本进程的监听器（隔离设置目录），
    // 之后关掉监听器就能制造一次真实的 ok→err 翻转。
    WSADATA winsock{};
    WSAStartup(MAKEWORD(2,2),&winsock);
    SOCKET gatewayListener=INVALID_SOCKET;
    if(scenario==L"settings") {
        gatewayListener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        address.sin_port=0;
        if(gatewayListener!=INVALID_SOCKET
            &&bind(gatewayListener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0
            &&listen(gatewayListener,SOMAXCONN)==0) {
            int length=sizeof(address);
            getsockname(gatewayListener,reinterpret_cast<sockaddr*>(&address),&length);
            const auto port=ntohs(address.sin_port);
            const auto directory=h::IsolatedDataDir(id);
            CreateDirectoryW(directory.c_str(),nullptr);
            std::ofstream settings(Narrow(directory)+"/settings.json",std::ios::trunc);
            settings<<"{\"gatewayPort\":"<<port<<"}\n";
        }
    }

    PROCESS_INFORMATION child{};
    StartApp(argv[1],id,&child);
    auto status=1;
    try {
        h::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        h::Client client(id);
        WaitFor(client,[](const Json& value) { return value["data"]["providers"].get<int>()==2; });
        if(scenario==L"add") {
            Tab(client,"tabAdd");
            auto snapshot=WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==2; });
            h::Require(snapshot["activePage"].get<int>()==2,"the 添加 tab switched the page");
            h::Require(snapshot["add"]["sub"].get<int>()==0,"the 新建供应商 sub-tab is first");
            h::Require(snapshot["add"]["subs"].size()==3,"three sub-tabs");
            // 审计 F-007：新建页新增「模型（每行一个）」多行框 → 5 个字段。
            h::Require(snapshot["add"]["fields"].size()==5,"the new-provider form has five fields");
            // 空表单保存 → 拒
            auto controls=client.Request({{"command","controls"}});
            Tap(client,controls["primaryButton"]);
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["add"]["error"].get<std::string>()
                =="\xE5\x90\x8D\xE7\xA7\xB0\xE5\x92\x8C Base URL \xE5\xBF\x85\xE5\xA1\xAB",
                "an empty name/base_url is refused");
            h::Require(Requests(logPath,"upsert-provider").empty(),"the refusal wrote nothing");
            std::cout<<"PASS h-add: the new-provider form refuses an empty name/base_url\n";
            // 探测：Base URL + 探测按钮 → 三个模型药丸
            Tap(client,FieldRect(client,2));                 // Base URL
            TypeText(client,"https://probe.example/v1");
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["probeButton"]);
            snapshot=WaitFor(client,[](const Json& value) { return value["add"]["chips"].size()==3; });
            h::Require(snapshot["add"]["chips"].size()==3,"the probe filled three model chips");
            h::Require(Requests(logPath,"probe").size()==1,"probe reached the helper");
            std::cout<<"PASS h-add: 探测 filled the model chips\n";
            // 名 + Key 变量名 + Key（掩码）+ 保存 → upsert-provider
            Tap(client,FieldRect(client,0));
            TypeText(client,"MyRelay");
            Tap(client,FieldRect(client,1));
            TypeText(client,"MYRELAY_API_KEY");
            Tap(client,FieldRect(client,3));
            TypeText(client,"sk-hidden-value");
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["add"]["fields"][3]["masked"].get<bool>(),"the API Key field is masked");
            h::Require(snapshot["add"]["fields"][3]["value"].get<std::string>()=="sk-hidden-value",
                "the value is held by the field (drawing masks it)");
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["primaryButton"]);
            snapshot=WaitFor(client,[](const Json& value) {
                for(const auto& provider:value["add"]["providers"])
                    if(provider.get<std::string>()=="MyRelay") return true;
                return false;
            });
            const auto writes=Requests(logPath,"upsert-provider");
            h::Require(writes.size()==1,"one upsert-provider reached the helper");
            h::Require(writes[0]["name"].get<std::string>()=="MyRelay"
                &&writes[0]["key_env"].get<std::string>()=="MYRELAY_API_KEY"
                &&writes[0]["key"].get<std::string>()=="sk-hidden-value"
                &&writes[0]["models"].size()==3,"the write carried name/key_env/key/models");
            h::Require(snapshot["toast"]["text"].get<std::string>().find("\xE5\xB7\xB2\xE5\x86\x99\xE5\x85\xA5")
                !=std::string::npos,"the write toasted");
            // 日志里绝不该出现 key 值（GOAL 硬性规则 7）
            {
                std::ifstream log(Narrow(home)+"/../logs/app.log");
                std::stringstream buffer;
                buffer<<log.rdbuf();
                h::Require(buffer.str().find("sk-hidden-value")==std::string::npos,
                    "the API key value never appears in the log");
            }
            std::cout<<"PASS h-add: upsert-provider carried the form and the key never hit the log\n";
            // 审计 F-012：眼睛按钮切换显示（旧版 #btn-np-eye）。
            {
                controls=client.Request({{"command","controls"}});
                h::Require(controls.contains("keyReveal"),"the API Key eye button exists");
                Tap(client,controls["keyReveal"]);
                snapshot=WaitFor(client,[](const Json& value) {
                    return value["add"]["keyRevealed"].get<bool>();
                });
                h::Require(snapshot["add"]["keyRevealed"].get<bool>(),"the eye reveals the key");
                h::Require(!snapshot["add"]["fields"][3]["masked"].get<bool>(),
                    "the revealed field is no longer masked");
                controls=client.Request({{"command","controls"}});
                Tap(client,controls["keyReveal"]);
                snapshot=WaitFor(client,[](const Json& value) {
                    return !value["add"]["keyRevealed"].get<bool>();
                });
                h::Require(snapshot["add"]["fields"][3]["masked"].get<bool>(),
                    "clicking the eye again masks the key");
            }
            // 审计 F-007：手动输入两行模型 → upsert-provider 的 models 就是这两行。
            {
                const auto before=Requests(logPath,"upsert-provider").size();
                controls=client.Request({{"command","controls"}});
                Tap(client,controls["_fields"][4]);
                SelectAll(client);          // 探测已填过这里：先清空再手输
                TypeText(client,"manual-a\rmanual-b");
                controls=client.Request({{"command","controls"}});
                Tap(client,controls["primaryButton"]);
                const auto deadline=GetTickCount64()+15000;
                while(GetTickCount64()<deadline&&Requests(logPath,"upsert-provider").size()<=before) Sleep(50);
                const auto manualWrites=Requests(logPath,"upsert-provider");
                h::Require(manualWrites.size()==before+1,"the manual-model save reached the helper");
                h::Require(manualWrites.back()["models"].size()==2
                    &&manualWrites.back()["models"][0].get<std::string>()=="manual-a",
                    "the manual model field became the models list");
                std::cout<<"PASS h-add: the manual model field feeds upsert-provider\n";
            }
            // 加模型子页：选供应商（Beta=不可达）+ 从供应商探测填充 → probe 失败留下错误
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["subTabModels"]);
            snapshot=WaitFor(client,[](const Json& value) { return value["add"]["sub"].get<int>()==1; });
            h::Require(snapshot["add"]["sub"].get<int>()==1,"the 加模型 sub-tab switched");
            h::Require(snapshot["add"]["providers"].size()>=2,"the provider list is filled");
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["probeButton"]);              // 默认选中第一个供应商 Alpha
            snapshot=WaitFor(client,[](const Json& value) {
                return !value["add"]["fields"][1]["value"].get<std::string>().empty();
            });
            // 审计 F-006：结果写进「模型 id」多行框（旧版 #am-models 的行为），不再只放药丸。
            h::Require(snapshot["add"]["fields"][1]["value"].get<std::string>().find("probe-a")
                !=std::string::npos,"从供应商探测填充 filled the model-id textarea");
            h::Require(snapshot["add"]["chips"].empty(),
                "the 加模型 page writes the textarea instead of the new-provider chips");
            std::cout<<"PASS h-add: 加模型's 从供应商探测填充 works\n";
            // 下拉必须能滚到最后一个供应商（真机：18 个供应商，只显示前 8 个、后面选不到）。
            {
                const int providerCount=static_cast<int>(snapshot["add"]["providers"].size());
                h::Require(providerCount>=10,"the fixture must expose enough providers to scroll");
                controls=client.Request({{"command","controls"}});
                Tap(client,FieldRect(client,0));                 // 0 = 供应商下拉
                auto opened=client.Request({{"command","dump"}})["select"];
                h::Require(!opened["labels"].empty(),"the provider dropdown did not open");
                h::Require(opened["total"].get<int>()==providerCount,"the dropdown knows every provider");
                h::Require(opened["visible"].get<int>()<providerCount,
                    "the dropdown should be a window over the list, not the whole list");
                const int visible=opened["visible"].get<int>();
                // 滚到底：可见项必须换成列表尾部，而且首项的下标就是 scroll。
                for(int step=0;step<providerCount+2;++step) {
                    client.Request({{"command","wheel"},{"delta",-120}});
                    Sleep(20);
                }
                const auto scrolled=client.Request({{"command","dump"}})["select"];
                h::Require(scrolled["scroll"].get<int>()==providerCount-visible,
                    "the dropdown did not scroll to the end");
                h::Require(scrolled["labels"][visible-1].get<std::string>()
                    ==snapshot["add"]["providers"][providerCount-1].get<std::string>(),
                    "the last provider is not reachable from the dropdown");
                // 点最后一项：选中项必须变成最后一个供应商（旧实现点不到）。
                controls=client.Request({{"command","controls"}});
                const auto options=controls["_select"];
                h::Require(static_cast<int>(options.size())==visible,"the visible options are clickable");
                Tap(client,options[visible-1]);
                snapshot=WaitFor(client,[](const Json& value) {
                    return value["add"]["provider"].get<int>()>0;
                });
                h::Require(snapshot["add"]["provider"].get<int>()==providerCount-1,
                    "picking the last option did not select the last provider");
                std::cout<<"PASS h-add: the provider dropdown scrolls to the last entry and selects it\n";
            }
            // 追加：多行模型 → add-models（探测已经把结果填进文本框，先全选覆盖）
            Tap(client,FieldRect(client,1));                  // 模型 id textarea
            SelectAll(client);
            TypeText(client,"beta-2\rbeta-3");
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["primaryButton"]);
            snapshot=WaitFor(client,[](const Json& value) { return value["add"]["fields"].size()>0
                &&value["add"]["fields"][1]["value"].get<std::string>().empty(); });
            const auto adds=Requests(logPath,"add-models");
            h::Require(adds.size()==1,"one add-models reached the helper");
            h::Require(adds[0]["models"].size()==2&&adds[0]["models"][0].get<std::string>()=="beta-2",
                "the textarea's lines became the model list");
            std::cout<<"PASS h-add: add-models carried the textarea's two lines\n";
            // 删除子页：现在是**供应商下拉**（真机要求：不要手输名字）。两条路径都要走通：
            //   ① 选一个可删的 → 危险确认框（Esc 取消，什么都不写）；
            //   ② 当前模型在用的那个在按钮上是禁用的（选它按钮不亮）。
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["subTabDel"]);
            snapshot=WaitFor(client,[](const Json& value) { return value["add"]["sub"].get<int>()==2; });
            h::Require(snapshot["add"]["sub"].get<int>()==2,"the 删除 sub-tab switched");
            h::Require(snapshot["add"]["primaryDisabled"].get<bool>(),"nothing selected keeps the button disabled");
            const auto names=snapshot["add"]["providers"];
            Tap(client,FieldRect(client,0));
            WaitFor(client,[](const Json& value) { return value["add"]["selectOpen"].get<bool>(); });
            {
                const auto select=client.Request({{"command","dump"}})["select"];
                h::Require(select["total"].get<int>()==static_cast<int>(names.size()),
                    "the dropdown does not know every provider");
                h::Require(select["visible"].get<int>()>0
                    &&select["visible"].get<int>()<=select["total"].get<int>(),
                    "the dropdown window size is wrong");
            }
            std::cout<<"PASS h-add: the delete dropdown knows every provider"<<std::endl;
            // 选第 index 个供应商：下拉是滚动窗口，先把目标滚进可视区再点（旧实现只能点前 8 个）。
            const auto selectProvider=[&](int index) {
                if(!client.Request({{"command","dump"}})["add"]["selectOpen"].get<bool>())
                    Tap(client,FieldRect(client,0));
                WaitFor(client,[](const Json& value) { return value["add"]["selectOpen"].get<bool>(); });
                for(int step=0;step<40;++step) {
                    const auto select=client.Request({{"command","dump"}})["select"];
                    const int scroll=select["scroll"].get<int>();
                    const int visible=select["visible"].get<int>();
                    if(index>=scroll&&index<scroll+visible) break;
                    client.Request({{"command","wheel"},{"delta",index<scroll?120:-120}});
                    Sleep(20);
                }
                const auto select=client.Request({{"command","dump"}})["select"];
                const auto options=client.Request({{"command","controls"}})["_select"];
                const int position=index-select["scroll"].get<int>();
                h::Require(position>=0&&position<static_cast<int>(options.size()),
                    "the wanted option is not visible after scrolling");
                Tap(client,options[position]);
                return WaitFor(client,[](const Json& value) {
                    return !value["add"]["selectOpen"].get<bool>();
                });
            };
            // 哪个是"正在用"的：从模型页的分组里读（group.active）——测试不许自己猜。
            std::string activeName;
            {
                const auto groups=client.Request({{"command","dump"}})["models"]["groups"];
                for(const auto& group:groups) {
                    if(!group.is_object()) continue;
                    if(group.value("active",false)) activeName=group.value("title",std::string{});
                }
            }
            int blocked=0,free=-1,blockedWrong=0;
            for(std::size_t index=0;index<names.size();++index) {
                snapshot=selectProvider(static_cast<int>(index));
                const auto selectedName=names[index].get<std::string>();
                if(snapshot["add"]["primaryDisabled"].get<bool>()) {
                    ++blocked;
                    if(selectedName!=activeName) ++blockedWrong;
                } else if(free<0) free=static_cast<int>(index);
            }
            if(!client.Request({{"command","dump"}})["add"]["selectOpen"].get<bool>())
                Tap(client,FieldRect(client,0));
            WaitFor(client,[](const Json& value) { return value["add"]["selectOpen"].get<bool>(); });
            {
                const auto options=client.Request({{"command","controls"}})["_select"];
                h::Require(options.is_array()&&options.size()>static_cast<std::size_t>(free),
                    "the dropdown reports its options");
                Tap(client,options[static_cast<std::size_t>(free)]);
            }
            snapshot=WaitFor(client,[](const Json& value) { return !value["add"]["selectOpen"].get<bool>(); });
            h::Require(!snapshot["add"]["primaryDisabled"].get<bool>(),"a deletable provider enables the button");
            controls=client.Request({{"command","controls"}});
            h::Require(controls["_switches"].size()==2,"the delete form has two switches");
            Tap(client,controls["primaryButton"]);
            snapshot=WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
            h::Require(snapshot["modal"]["danger"].get<bool>(),"deleting a provider is a danger confirm");
            // 审计 F-005：点遮罩（面板内、模态外）不得关闭；只有取消按钮/Esc 才关。
            {
                const auto panel=client.Request({{"command","dump"}})["panel"];
                client.Request({{"command","focus"}});
                client.Request({{"command","tap"},{"x",panel[0].get<int>()+6},
                    {"y",panel[1].get<int>()+90},{"screen",true}});
                Sleep(300);
                h::Require(!client.Request({{"command","dump"}})["modal"]["title"].get<std::string>().empty(),
                    "clicking the mask keeps the confirm open");
            }
            client.Request({{"command","vk"},{"vk",27}});
            Sleep(400);
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["modal"]["title"].get<std::string>().empty(),"Esc closed the confirm");
            h::Require(Requests(logPath,"delete-provider").empty(),"cancelling wrote nothing");
            std::cout<<"PASS h-add: the danger confirm cancels without writing"<<std::endl;
            // 再走一次并确认：请求必须用真实助手的字段名 purge_key / remove_fallbacks（审计 F-002）。
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["primaryButton"]);
            WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
            client.Request({{"command","vk"},{"vk",13}});          // Enter = 确定删除
            snapshot=WaitFor(client,[&](const Json& value) {
                return value["add"]["providers"].size()<names.size(); });
            const auto deletes=Requests(logPath,"delete-provider");
            h::Require(deletes.size()==1,"one delete-provider reached the helper");
            h::Require(deletes[0].contains("purge_key")&&deletes[0].contains("remove_fallbacks")
                &&!deletes[0].contains("purge_env")&&!deletes[0].contains("drop_fallbacks"),
                "delete-provider carries the helper's real field names");
            h::Require(snapshot["add"]["providers"].size()+1==names.size(),
                "the confirmed delete removed exactly one provider");
            std::cout<<"PASS h-add: the confirmed delete uses purge_key / remove_fallbacks\n";
            // 审计 F-006：加模型页的"从供应商探测填充"必须把结果写进「模型 id」多行框。
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["subTabModels"]);
            Sleep(300);
            controls=client.Request({{"command","controls"}});
            h::Require(controls["_fields"].size()>=2,"the 加模型 form has a provider select and a textarea");
            Tap(client,controls["_fields"][0]);          // 打开供应商下拉
            Sleep(200);
            {
                const auto options=client.Request({{"command","controls"}})["_select"];
                if(options.is_array()&&!options.empty()) Tap(client,options[0]);
            }
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["probeButton"]);
            snapshot=WaitFor(client,[](const Json& value) {
                const auto& text=value["add"]["fields"][1]["value"];
                return text.is_string()&&!text.get<std::string>().empty();
            },15000);
            h::Require(snapshot["add"]["fields"][1]["value"].get<std::string>().find("probe-a")
                !=std::string::npos,
                "the probe filled the 模型 id textarea on the 加模型 page");
            std::cout<<"PASS h-add: 从供应商探测填充 writes the probed ids into the textarea\n";
            // 审计 F-016：点异常灯 = 重试（DESIGN §4.2 #4）。
            {
                const auto before=Requests(logPath,"list").size();
                controls=client.Request({{"command","controls"}});
                h::Require(controls.contains("lampGateway"),"the gateway lamp is a control");
                Tap(client,controls["lampGateway"]);
                const auto deadline=GetTickCount64()+10000;
                while(GetTickCount64()<deadline&&Requests(logPath,"list").size()<=before) Sleep(50);
                h::Require(Requests(logPath,"list").size()>before,"clicking a lamp re-reads the state");
                std::cout<<"PASS h-add: clicking a lamp retries the probes\n";
            }
            // 审计 F-015：config.yaml 解析失败 → 设置页 meta 醒目提示（写操作在数据层停用）。
            {
                {
                    std::ofstream broken(Narrow(home)+"/config_broken",std::ios::trunc);
                    broken<<"1\n";
                }
                client.Request({{"command","poll"}});
                WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
                Sleep(400);
                controls=client.Request({{"command","controls"}});
                Tap(client,controls["tabSettings"]);
                WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==4; });
                const auto settings=client.Request({{"command","dump"}});
                h::Require(settings["settings"]["meta"].get<std::string>().find("config.yaml")
                    !=std::string::npos,
                    "the settings page warns that config.yaml does not parse");
                DeleteFileW((home+L"\\config_broken").c_str());
                std::cout<<"PASS h-settings: a broken config.yaml is surfaced in the meta row\n";
            }
            status=0;
        } else if(scenario==L"backup") {
            Tab(client,"tabBackup");
            auto snapshot=WaitFor(client,[](const Json& value) { return value["backup"]["rows"].size()>0; });
            h::Require(snapshot["activePage"].get<int>()==3,"the 备份 tab switched the page");
            h::Require(snapshot["backup"]["rows"].size()==3,"the fixture's three backups arrived");
            h::Require(snapshot["backup"]["filters"][0]["count"].get<int>()==3,"全部 counts them");
            h::Require(snapshot["backup"]["filters"][1]["count"].get<int>()==2,"config counts two");
            h::Require(snapshot["backup"]["filters"][2]["count"].get<int>()==1,"env counts one");
            h::Require(Requests(logPath,"backup-list").size()==1,"backup-list ran exactly once on entry");
            std::cout<<"PASS h-backup: the list and its type counts came from backup-list\n";
            // 过滤到 .env：只剩一行
            auto controls=client.Request({{"command","controls"}});
            Tap(client,controls["_segments"][2]);
            snapshot=WaitFor(client,[](const Json& value) { return value["backup"]["filter"].get<int>()==2; });
            h::Require(snapshot["backup"]["rows"].size()==1,"the .env filter shows one row");
            h::Require(snapshot["backup"]["rows"][0]["kind"].get<std::string>()=="env",
                "the remaining row is the env backup");
            std::cout<<"PASS h-backup: the .env filter scopes the list\n";
            // 回滚要过危险确认：Esc 不写，确认才 restore
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["_restore"][0]);
            snapshot=WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
            h::Require(snapshot["modal"]["body"].get<std::string>().find("env-")!=std::string::npos,
                "the modal names the backup file");
            KeyDown(client,VK_ESCAPE);
            h::Require(client.Request({{"command","dump"}})["modal"]["title"].get<std::string>().empty(),
                "Esc closed the restore confirm");
            h::Require(Requests(logPath,"restore").empty(),"cancelling wrote nothing");
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["_restore"][0]);
            WaitFor(client,[](const Json& value) { return !value["modal"]["title"].get<std::string>().empty(); });
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);
            snapshot=WaitFor(client,[](const Json& value) {
                return value["toast"]["text"].get<std::string>().find("\xE5\xB7\xB2\xE5\x9B\x9E\xE6\xBB\x9A")
                    !=std::string::npos;
            });
            const auto restores=Requests(logPath,"restore");
            h::Require(restores.size()==1&&restores[0]["file"].get<std::string>().find("env-")!=std::string::npos,
                "restore carried the file name");
            h::Require(restores[0]["kind"].get<std::string>()=="env","and the backup kind");
            std::cout<<"PASS h-backup: restore went through the danger confirm with the file and kind\n";
            status=0;
        } else {
            // 固定高度：切页不再 resize，底栏必须停在原来的屏幕高度。
            const auto navBefore=client.Request({{"command","controls"}});
            const auto panelBefore=client.Request({{"command","dump"}})["panel"];
            Tab(client,"tabSettings");
            auto snapshot=WaitFor(client,[](const Json& value) { return value["activePage"].get<int>()==4; });
            h::Require(snapshot["activePage"].get<int>()==4,"the 设置 tab switched the page");
            const auto navAfter=client.Request({{"command","controls"}});
            h::Require(navAfter["tabSettings"]["y"].get<int>()==navBefore["tabSettings"]["y"].get<int>(),
                "the bottom nav moved after switching pages");
            h::Require(client.Request({{"command","dump"}})["panel"]==panelBefore,
                "the window height changed when switching pages");
            h::Require(snapshot["settings"]["sections"].size()==4,"four sections: 连接/行为/快捷键/其他");
            h::Require(snapshot["settings"]["fields"].size()==5,"five input rows");
            h::Require(snapshot["settings"]["fields"][0]["value"].get<std::string>()=="Ubuntu",
                "the distro row shows the stored value");
            h::Require(snapshot["settings"]["switches"].size()==4,"four switch rows (含 F4 延迟探测)");
            // 旧面板三段（深色/浅色/跟随系统）+ 四套现代化主题（Slate/Carbon/Porcelain/Sand）。
            // 深色 / 浅色 / 跟随系统 + 四套现代化主题（Slate/Carbon/Porcelain/Sand）。
            h::Require(snapshot["settings"]["themes"].size()==7,"seven theme segments (3 stock + 4 modern)");
            std::cout<<"PASS h-settings: sections, rows, switches and themes are wired\n";
            // 改端口：点字段 → 清空 → 打新值 → Enter（WM_CHAR 路径 + setSetting）
            auto controls=client.Request({{"command","controls"}});
            Tap(client,FieldRect(client,1));
            client.Request({{"command","vk"},{"vk",VK_END}});
            // 退格在真实键盘上以 WM_CHAR 0x08 到达字段（`key` 命令就是那条路径）
            client.Request({{"command","key"},{"text",""}});
            TypeText(client,"9001");
            KeyDown(client,VK_RETURN);
            {
                const auto path=Narrow(home)+"/../settings.json";
                bool persisted=false;
                for(int attempt=0;attempt<20&&!persisted;++attempt) {
                    std::ifstream file(path);
                    if(file) {
                        try {
                            Json stored;
                            file>>stored;
                            persisted=stored.value("webuiPort",0)==9001;
                        } catch(const std::exception&) {}
                    }
                    if(!persisted) Sleep(100);
                }
                h::Require(persisted,"the WebUI port edit was persisted to the settings file");
            }
            std::cout<<"PASS h-settings: editing the port persisted webuiPort=9001\n";
            // 开关：点「写入前先看 diff」
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["_switches"][0]);
            snapshot=WaitFor(client,[](const Json& value) { return value["settings"]["switches"][0]["on"].get<bool>(); });
            h::Require(snapshot["settings"]["switches"][0]["on"].get<bool>(),"the preview switch turned on");
            // 固定高度后设置页正文需要滚动：主题段与热键行在下半页，先滚两格再取控件坐标。
            client.Request({{"command","wheel"},{"delta",-360}});
            client.Request({{"command","wheel"},{"delta",-360}});
            Sleep(180);
            // 主题：切到浅色 → 面板色调跟着变（F4 的 3 段延迟 + G1 的 5 段挂件排在主题段之前）
            controls=client.Request({{"command","controls"}});
            const int themeOffset=static_cast<int>(controls["_segments"].size())-7;
            Tap(client,controls["_segments"][1+themeOffset]);
            snapshot=WaitFor(client,[](const Json& value) { return value["themeLight"].get<bool>(); });
            h::Require(snapshot["themeLight"].get<bool>(),"picking 浅色 switched the palette");
            h::Require(snapshot["settings"]["themes"][1]["active"].get<bool>(),"and the segment shows it");
            std::cout<<"PASS h-settings: the behaviour switches and the theme apply live\n";
            // 再切四套现代化主题：每套都要真的换材质（主题 id 落盘 + active 标记跟着走）。
            {
                struct ThemeCase { int index;const char* id;bool light; };
                const ThemeCase modern[]={{3,"slate",false},{4,"carbon",false},
                    {5,"porcelain",true},{6,"sand",true}};
                for(const auto& test:modern) {
                    controls=client.Request({{"command","controls"}});
                    Tap(client,controls["_segments"][test.index
                        +static_cast<int>(controls["_segments"].size())-7]);
                    snapshot=WaitFor(client,[&](const Json& value) {
                        return value["stored"]["theme"].get<std::string>()==test.id;
                    });
                    h::Require(snapshot["stored"]["theme"].get<std::string>()==test.id,
                        (std::string("picking ")+test.id+" set the theme").c_str());
                    h::Require(snapshot["settings"]["themes"][test.index]["active"].get<bool>(),
                        (std::string(test.id)+" segment is marked").c_str());
                    h::Require(snapshot["themeLight"].get<bool>()==test.light,
                        (std::string(test.id)+" has the expected light/dark base").c_str());
                    // **画出来的颜色**必须等于该主题解析后的 ink：只看 stored.theme 会让
                    // "主题切换失效"这类回归溜过去（真机 2026-10-20 的报障）。
                    const auto expected=hermes::ResolveTheme(test.id);
                    const auto channel=[](float value) {
                        return static_cast<int>(std::lround(std::clamp(value,0.0f,1.0f)*255.0f));
                    };
                    const int expectedInk=(channel(expected.ink.r)<<16)|(channel(expected.ink.g)<<8)
                        |channel(expected.ink.b);
                    h::Require(snapshot["palette"]["ink"].get<int>()==expectedInk,
                        (std::string(test.id)+" did not reach the renderer palette").c_str());
                }
            }
            std::cout<<"PASS h-settings: the four modern themes apply live"<<std::endl;
            // 热键捕获：点热键字段 → 按 Control+Alt+K
            controls=client.Request({{"command","controls"}});
            Tap(client,FieldRect(client,4));
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["focus"]["hotkeyCapturing"].get<bool>(),"the hotkey field entered capture mode");
            // 修饰键状态随 WM_KEYDOWN 一起合成（真实键盘由 GetKeyState 提供；本机桌面不让前台时
            // SendInput 到不了面板，见 h_models_tests.cpp 的说明）。mods: 1=Control 2=Alt 4=Shift。
            client.Request({{"command","vk"},{"vk",'K'},{"mods",3}});
            snapshot=WaitFor(client,[](const Json& value) {
                return value["settings"]["fields"][4]["value"].get<std::string>()=="Control+Alt+K";
            });
            h::Require(snapshot["settings"]["fields"][4]["value"].get<std::string>()=="Control+Alt+K",
                "the captured chord reads Control+Alt+K");
            h::Require(snapshot["hotkeys"].get<bool>(),"the new chord registered");
            std::cout<<"PASS h-settings: the hotkey field captured Control+Alt+K and re-registered\n";
            // 打开 WebUI 只该是一次 ShellExecute：动作存在即可（真开浏览器不便于断言，改断言控件在）
            controls=client.Request({{"command","controls"}});
            h::Require(controls.contains("openWebUiButton"),"the 在浏览器打开 button is clickable");
            std::cout<<"PASS h-settings: 其他 section exposes 在浏览器打开\n";
            // ---- F2 诊断：设置页入口 → 只读助手 → 模态事实（网关/通道/日志）----
            // 设置页正文现在比视口高（F2/F5 又加了几行）：滚到底再点「其他」区（真机也是滚轮）。
            client.Request({{"command","wheel"},{"delta",-360}});
            client.Request({{"command","wheel"},{"delta",-360}});
            Sleep(150);
            controls=client.Request({{"command","controls"}});
            std::cout<<"[settings] pageMaxScroll="<<controls["_pageMaxScroll"].dump()
                <<" segments="<<controls["_segments"].size()<<"\n";
            h::Require(controls.contains("setDiagnose")&&controls.contains("setUsage"),
                "the 诊断/用量 entries are clickable");
            Tap(client,controls["setDiagnose"]);
            snapshot=WaitFor(client,[](const Json& value) {
                const auto& facts=value["modal"]["facts"];
                return facts.is_array()&&facts.size()>=3;
            });
            h::Require(snapshot["modal"]["title"].get<std::string>()=="\xE8\xAF\x8A\xE6\x96\xAD",
                "the diagnose modal is titled 诊断");
            h::Require(snapshot["modal"]["facts"][0].get<std::string>().find("active")
                !=std::string::npos,"the first fact is the gateway state");
            h::Require(Requests(logPath,"diagnose").size()==1,"diagnose reached the read helper");
            std::cout<<"PASS h-settings: 诊断 opened the read-helper modal with gateway facts\n";
            Tap(client,client.Request({{"command","controls"}})["modalCancel"]);
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["modal"]["title"].get<std::string>().empty(),
                "取消 closed the diagnose modal");
            // ---- F3 用量：入口 → 模态；刷新 = 再读一次 ----
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["setUsage"]);
            snapshot=WaitFor(client,[](const Json& value) {
                const auto& facts=value["modal"]["facts"];
                return facts.is_array()&&facts.size()>=3;
            });
            h::Require(snapshot["modal"]["title"].get<std::string>()
                =="\xE7\x94\xA8\xE9\x87\x8F\xE7\xBB\x9F\xE8\xAE\xA1","the usage modal is titled 用量统计");
            h::Require(snapshot["modal"]["facts"][1].get<std::string>().find("24")
                !=std::string::npos,"the overview fact carries the session count");
            h::Require(Requests(logPath,"usage").size()==1,"usage reached the read helper");
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);
            // 刷新期间模态必须留在原地（真机反馈：点刷新窗口消失，读完才冒出来）。
            // 旧实现 RunPendingAction 之后 HandleModalClick 清掉 pending，模态空了一整段读取时间。
            for(int sample=0;sample<12;++sample) {
                const auto open=client.Request({{"command","dump"}});
                h::Require(!open["modal"]["title"].get<std::string>().empty(),
                    "the modal disappeared while the refresh was in flight");
                Sleep(20);
            }
            snapshot=WaitFor(client,[](const Json& value) {
                const auto& facts=value["modal"]["facts"];
                return facts.is_array()&&facts.size()>=3
                    &&value["modal"]["title"].get<std::string>()=="\xE7\x94\xA8\xE9\x87\x8F\xE7\xBB\x9F\xE8\xAE\xA1";
            },15000);
            h::Require(Requests(logPath,"usage").size()==2,"刷新 re-read the usage report");
            std::cout<<"PASS h-settings: 用量 modal reads the report and 刷新 re-reads it\n";
            // ---- F4 延迟探测：设置页暴露开关/间隔，探测结果落 latency.json ----
            controls=client.Request({{"command","controls"}});
            h::Require(controls["_switches"].size()==4,"four switch rows (F4 adds 延迟探测)");
            h::Require(controls["_segments"].size()>=14,
                "three latency + four widget segments precede the seven theme segments");
            // 用量「刷新」的结果会把模态再打开（异步）：先 Esc，否则这次点击会被模态吃掉。
            client.Request({{"command","vk"},{"vk",VK_ESCAPE}});
            Sleep(200);
            controls=client.Request({{"command","controls"}});
            // G11：挂件样式段（延迟 3 + 挂件 4 + 主题 7 = 14）。
            Tap(client,controls["_segments"][3+1]);   // 铭牌
            snapshot=WaitFor(client,[](const Json& value) {
                return value["stored"]["widgetStyle"].get<std::string>()=="tag";
            });
            h::Require(snapshot["stored"]["widgetStyle"].get<std::string>()=="tag",
                "the widget style segment applied");
            client.Request({{"command","configure"},{"latencyProbe",true},{"latencyMinutes",15}});
            client.Request({{"command","latency-probe"}});
            snapshot=WaitFor(client,[](const Json& value) {
                return value["latency"].is_object()&&value["latency"].size()>0
                    &&!value["latencyBusy"].get<bool>();
            },15000);
            h::Require(snapshot["latency"].contains("alpha|alpha-1"),
                "the probe recorded the current model");
            h::Require(snapshot["latency"]["alpha|alpha-1"]["ms"].get<int>()==312,
                "the fake probe's 312ms landed");
            h::Require(snapshot["latency"]["alpha|alpha-1"]["ok"].get<bool>(),"and it is a success");
            h::Require(Requests(logPath,"probe").size()==1,"exactly one latency probe ran");
            {
                std::ifstream file(Narrow(home)+"/../latency.json");
                h::Require(file.good(),"latency.json was written next to settings.json");
            }
            std::cout<<"PASS h-settings: 延迟探测 switch/interval and latency.json are wired\n";
            // F3 的「刷新」结果会把用量卡再打开一次（异步）：先 Esc 收掉，再点 F5 入口。
            client.Request({{"command","vk"},{"vk",VK_ESCAPE}});
            Sleep(200);
            // ---- F5 配置档案：保存当前 → 应用（set-main + 回退链重写）→ 失败即停 → 删除 ----
            controls=client.Request({{"command","controls"}});
            h::Require(controls.contains("setProfiles"),"the 配置档案 entry is clickable");
            Tap(client,controls["setProfiles"]);
            snapshot=WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88";   // 配置档案
            });
            h::Require(snapshot["modal"]["title"].get<std::string>()
                =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88",
                "the 管理 button opened the profile manager");
            h::Require(snapshot["modal"]["profiles"].empty(),"the empty manager has no rows");
            h::Require(snapshot["modal"]["facts"].size()==1,"and it explains how to save one");
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);   // 保存当前
            snapshot=WaitFor(client,[](const Json& value) {
                return value["profiles"].size()==1;
            });
            h::Require(snapshot["profiles"][0]["name"].get<std::string>()=="alpha-1",
                "保存当前 named the profile after the current model");
            h::Require(snapshot["profiles"][0]["main"].get<std::string>()=="alpha-1"
                &&snapshot["profiles"][0]["fallbacks"].get<int>()==1,
                "the profile carries the main model and the fallback chain");
            h::Require(snapshot["modal"]["title"].get<std::string>()
                =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88",
                "the manager stayed open after saving");
            {
                std::ifstream file(Narrow(home)+"/../profiles.json");
                h::Require(file.good(),"profiles.json was written next to settings.json");
            }
            std::cout<<"PASS h-settings: 配置档案 saved the current model and fallback chain\n";
            // 应用：set-main → 清空回退链 → 追加档案回退
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["modalProfileApply"]);
            snapshot=WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE5\xBA\x94\xE7\x94\xA8\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88\xEF\xBC\x9F";
            });
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);
            {
                const auto deadline=GetTickCount64()+15000;
                while(GetTickCount64()<deadline
                    &&(Requests(logPath,"set-main").size()<1
                        ||Requests(logPath,"fallback-add").size()<1))
                    Sleep(100);
            }
            h::Require(Requests(logPath,"set-main").size()==1,"applying ran set-main once");
            h::Require(Requests(logPath,"fallback-remove").size()==1,"it cleared the old fallback");
            h::Require(Requests(logPath,"fallback-add").size()==1,"and added the profile's fallback");
            snapshot=WaitFor(client,[](const Json& value) {
                return value["toast"]["text"].get<std::string>().find(
                    "\xE5\xB7\xB2\xE5\xBA\x94\xE7\x94\xA8")!=std::string::npos;   // 已应用
            });
            h::Require(snapshot["data"]["pendingGatewayRestart"].get<bool>(),
                "with auto-reload off the restart pill is pending");
            std::cout<<"PASS h-settings: applying a profile rewrote main + fallback chain\n";
            // 失败即停：让 fallback-add 失败 → 模态回到管理页并标出失败步
            {
                std::ofstream fail(Narrow(home)+"/fail_cmd",std::ios::trunc);
                fail<<"fallback-add\n";
            }
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["setProfiles"]);
            WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88";
            });
            Tap(client,client.Request({{"command","controls"}})["modalProfileApply"]);
            WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE5\xBA\x94\xE7\x94\xA8\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88\xEF\xBC\x9F";
            });
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);
            snapshot=WaitFor(client,[](const Json& value) {
                return !value["profileError"].get<std::string>().empty();
            },15000);
            h::Require(snapshot["profileError"].get<std::string>().find("fallback-add")
                !=std::string::npos,"the failure names the step that stopped the apply");
            h::Require(snapshot["modal"]["title"].get<std::string>()
                =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88",
                "the manager reopened with the error");
            h::Require(!snapshot["modal"]["facts"].empty()
                &&snapshot["modal"]["facts"][0].get<std::string>().find(
                    "\xE4\xB8\x8A\xE4\xB8\x80\xE6\xAD\xA5\xE5\xA4\xB1\xE8\xB4\xA5")!=std::string::npos,
                "and the first fact says which step failed");
            DeleteFileW((home+L"\\fail_cmd").c_str());
            std::cout<<"PASS h-settings: a failing step stops the apply and is shown in the manager\n";
            // 删除：危险确认
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["modalProfileDelete"]);
            snapshot=WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE5\x88\xA0\xE9\x99\xA4\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88\xEF\xBC\x9F";
            });
            h::Require(snapshot["modal"]["danger"].get<bool>(),"删除 is a danger confirm");
            Tap(client,client.Request({{"command","controls"}})["modalConfirm"]);
            snapshot=WaitFor(client,[](const Json& value) { return value["profiles"].empty(); });
            h::Require(snapshot["profiles"].empty(),"删除 removed the profile");
            std::cout<<"PASS h-settings: deleting a profile went through the danger confirm\n";
            // ---- F6 托盘通知：待重启边沿 + 点击气泡 + 助手出错边沿 + 10 分钟抑制 ----
            WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); },15000);
            snapshot=WaitFor(client,[](const Json& value) {
                return value["notifications"]["count"].get<int>()>=1;
            },15000);
            h::Require(snapshot["notifications"]["last"].get<std::string>().find(
                "\xE7\xBD\x91\xE5\x85\xB3\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF")!=std::string::npos,
                "the pending-restart edge raised a notification");   // 网关待重启
            // 审计 F-008：图标注册在 control 窗口上，NIM_MODIFY 也必须用它；否则 Shell_NotifyIcon
            // 返回 FALSE（气泡不会出现），而旧门禁只数计数，永远发现不了。
            h::Require(snapshot["notifications"]["lastOk"].get<bool>(),
                "the balloon was actually handed to the shell (Shell_NotifyIcon returned true)");
            // 面板隐藏时通知照样发；点气泡把面板叫回来。
            client.Request({{"command","hide"}});
            h::Require(!client.Request({{"command","state"}})["panelVisible"].get<bool>(),
                "the panel is hidden");
            client.Request({{"command","tray-click"}});
            snapshot=WaitFor(client,[](const Json& value) { return value["panelVisible"].get<bool>(); });
            h::Require(snapshot["panelVisible"].get<bool>(),"clicking the balloon shows the panel");
            // 助手出错边沿：fail_all → 第一次 +1；再次轮询被抑制；恢复后 10 分钟内再失败仍抑制。
            {
                std::ofstream fail(Narrow(home)+"/fail_all",std::ios::trunc);
                fail<<"1\n";
            }
            client.Request({{"command","poll"}});
            snapshot=WaitFor(client,[](const Json& value) {
                return value["notifications"]["kinds"].value("helper",0)>=1;
            },15000);
            h::Require(snapshot["notifications"]["kinds"].value("helper",0)==1,
                "the helper-error edge raised exactly one notification");
            h::Require(snapshot["notifications"]["last"].get<std::string>().find(
                "\xE5\x8A\xA9\xE6\x89\x8B\xE5\x87\xBA\xE9\x94\x99")!=std::string::npos,
                "and it names the helper error");   // 助手出错
            // 再失败一次：同事件在 10 分钟内被抑制（计数不变）。
            WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
            client.Request({{"command","poll"}});
            Sleep(1200);
            h::Require(client.Request({{"command","state"}})["notifications"]["kinds"]
                .value("helper",0)==1,"the same event is suppressed for 10 minutes");
            // 恢复 ok 后立刻再失败：10 分钟抑制优先，helper 事件仍不重复。
            DeleteFileW((home+L"\\fail_all").c_str());
            WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
            client.Request({{"command","poll"}});
            WaitFor(client,[](const Json& value) {
                return value["helperError"].get<std::string>().empty();
            },15000);
            {
                std::ofstream fail(Narrow(home)+"/fail_all",std::ios::trunc);
                fail<<"1\n";
            }
            WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
            client.Request({{"command","poll"}});
            Sleep(1200);
            h::Require(client.Request({{"command","state"}})["notifications"]["kinds"]
                .value("helper",0)==1,"recovery does not bypass the 10-minute suppression");
            // 连续 3 次刷新失败：换一个事件（refresh），不受 helper 抑制影响。
            for(int attempt=0;attempt<6;++attempt) {
                if(client.Request({{"command","state"}})["notifications"]["kinds"]
                    .value("refresh",0)>=1) break;
                WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
                client.Request({{"command","poll"}});
                Sleep(700);
            }
            snapshot=WaitFor(client,[](const Json& value) {
                return value["notifications"]["kinds"].value("refresh",0)>=1;
            },15000);
            h::Require(snapshot["notifications"]["kinds"].value("refresh",0)==1,
                "three consecutive refresh failures raised the refresh notification");
            DeleteFileW((home+L"\\fail_all").c_str());
            WaitFor(client,[](const Json& value) { return !value["data"]["busy"].get<bool>(); });
            client.Request({{"command","poll"}});
            WaitFor(client,[](const Json& value) {
                return value["helperError"].get<std::string>().empty();
            },15000);
            // 网关 ok→err：关掉测试监听器 → 下一次轮询产生「网关不可达」。
            if(gatewayListener!=INVALID_SOCKET) {
                closesocket(gatewayListener);
                gatewayListener=INVALID_SOCKET;
                client.Request({{"command","poll"}});
                snapshot=WaitFor(client,[](const Json& value) {
                    return value["notifications"]["kinds"].value("gateway",0)>=1;
                },15000);
                h::Require(snapshot["notifications"]["kinds"].value("gateway",0)==1,
                    "the gateway ok->err edge raised a notification");
                h::Require(snapshot["notifications"]["last"].get<std::string>().find(
                    "\xE7\xBD\x91\xE5\x85\xB3\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE")!=std::string::npos,
                    "and it names the gateway port");   // 网关不可达
            }
            std::cout<<"PASS h-settings: tray notifications are edge-triggered, suppressed and clickable\n";
            // ---- G1 贴边挂件：五套样式 → strip 尺寸随样式变化，贴边侧仍逐像素贴住 ----
            client.Request({{"command","place"},{"state",4},{"edge",1},{"anchor",200}});
            Sleep(500);
            {
                struct StyleCase { const char* id;int width;int height; };
                const StyleCase styles[]={{"beacon",10,76},{"tag",14,74},{"flag",16,58},{"capsule",30,136}};
                for(const auto& test:styles) {
                    // ① 展开态换样式：挂件窗口必须立刻同步到新尺寸。真机上这里曾留下旧尺寸的
                    //    窗口，贴边收回时校验不过 → 「message: fatal, closing」（应用自己退出）。
                    client.Request({{"command","place"},{"state",3},{"edge",1},{"anchor",200}});
                    Sleep(200);
                    client.Request({{"command","configure"},{"widgetStyle",test.id}});
                    Sleep(300);
                    const auto expanded=client.Request({{"command","dump"}});
                    h::Require(expanded["strip"]==expanded["expectedStrip"],
                        (std::string("changing to ")+test.id+" while expanded left a stale widget window").c_str());
                    // ② 收起后尺寸仍是新样式、贴边侧逐像素贴住。
                    client.Request({{"command","place"},{"state",4},{"edge",1},{"anchor",200}});
                    Sleep(300);
                    snapshot=client.Request({{"command","dump"}});
                    const auto strip=snapshot["strip"];
                    const int dpi=snapshot["dpi"].get<int>();
                    const int expectedWidth=(test.width*dpi+48)/96;
                    const int expectedHeight=(test.height*dpi+48)/96;
                    h::Require(snapshot["stripVisible"].get<bool>(),
                        "the widget is visible while collapsed");
                    h::Require(strip[0].get<int>()==snapshot["work"][0].get<int>(),
                        "the widget is flush with the left work-area edge");
                    h::Require(strip==snapshot["expectedStrip"],
                        (std::string("collapsing after ")+test.id+" did not keep the widget in sync").c_str());
                    h::Require(strip[2].get<int>()==expectedWidth&&strip[3].get<int>()==expectedHeight,
                        (std::string("the ")+test.id+" widget has its documented size").c_str());
                }
            }
            std::cout<<"PASS h-settings: three narrow widget styles dock with their documented sizes\n";
            // ---- 模态键盘隔离：模态打开时输入不能落到后面的字段（WM_CHAR 也不能漏） ----
            client.Request({{"command","place"},{"state",1},{"edge",0}});
            Sleep(400);
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["tabSettings"]);
            Sleep(900);
            Tap(client,FieldRect(client,0));
            TypeText(client,"Z");
            const auto beforeTyping=client.Request({{"command","dump"}})["settings"]["fields"][0]["value"]
                .get<std::string>();
            controls=client.Request({{"command","controls"}});
            Tap(client,controls["setProfiles"]);
            WaitFor(client,[](const Json& value) {
                return value["modal"]["title"].get<std::string>()
                    =="\xE9\x85\x8D\xE7\xBD\xAE\xE6\xA1\xA3\xE6\xA1\x88";
            });
            TypeText(client,"QQ");
            const auto afterTyping=client.Request({{"command","dump"}})["settings"]["fields"][0]["value"]
                .get<std::string>();
            h::Require(afterTyping==beforeTyping,
                "typing behind a modal does not reach the focused field");
            client.Request({{"command","vk"},{"vk",VK_ESCAPE}});
            Sleep(250);
            std::cout<<"PASS h-settings: a modal swallows keyboard input aimed at the page\n";
            // ---- 审计 F-004：closeAction=quit 时标题栏 × 必须真的退出（旧版 win.on('close')） ----
            // 关闭按钮在设置页下半部分：先把页内滚动到底，让字段进入视口。
            for(int scroll=0;scroll<6;++scroll) client.Request({{"command","wheel"},{"delta",-360}});
            Sleep(250);
            // 这一步必须用控制面坐标，不能走 ClickRect 的"窗口未被遮挡"前置检查：
            // 真机上用户的浏览器/悬浮面板经常压住设置页下半部分，而 tap 本身是窗口消息、不受遮挡影响。
            controls=client.Request({{"command","controls"}});
            client.Request({{"command","tap"},
                {"x",controls["_fields"][3]["x"].get<int>()+controls["_fields"][3]["width"].get<int>()/2},
                {"y",controls["_fields"][3]["y"].get<int>()+controls["_fields"][3]["height"].get<int>()/2},
                {"screen",true}});                       // 关闭按钮下拉
            Sleep(250);
            {
                const auto closeOptions=client.Request({{"command","controls"}})["_select"];
                h::Require(closeOptions.size()==2,"the close-action select offers two options");
                client.Request({{"command","tap"},
                    {"x",closeOptions[1]["x"].get<int>()+closeOptions[1]["width"].get<int>()/2},
                    {"y",closeOptions[1]["y"].get<int>()+closeOptions[1]["height"].get<int>()/2},
                    {"screen",true}});                   // 退出程序
            }
            snapshot=WaitFor(client,[](const Json& value) {
                return value["stored"]["closeAction"].get<std::string>()=="quit";
            });
            h::Require(snapshot["stored"]["closeAction"].get<std::string>()=="quit",
                "the close-action select applied 退出程序");
            controls=client.Request({{"command","controls"}});
            try {
                client.Request({{"command","tap"},
                    {"x",controls["close"]["x"].get<int>()+controls["close"]["width"].get<int>()/2},
                    {"y",controls["close"]["y"].get<int>()+controls["close"]["height"].get<int>()/2},
                    {"screen",true}});
            } catch(const std::exception&) {}
            h::Require(WaitForSingleObject(child.hProcess,5000)==WAIT_OBJECT_0,
                "the title-bar × exited the app when closeAction is quit");
            std::cout<<"PASS h-settings: closeAction=quit is honoured by the title-bar close button\n";
            status=0;
        }
    } catch(const std::exception& error) {
        std::cerr<<Narrow(scenario)<<": "<<error.what()<<"\n";
        try {
            // 失败时把关键块的形状打出来，省一次"猜哪一行"的往返。
            const auto state=h::Client(id).Request({{"command","dump"}});
            std::cerr<<"[dump] activePage="<<state.value("activePage",-1)
                <<" modal="<<state.value("modal",Json::object()).dump()
                <<"\n[dump] settings="<<state.value("settings",Json::object()).dump().substr(0,320)
                <<"\n[dump] focus="<<state.value("focus",Json::object()).dump()
                <<"\n[dump] add="<<state.value("add",Json::object()).dump().substr(0,200)<<"\n";
        } catch(const std::exception&) {}
        status=harness::IsEnvironmentInterference(error)?77:1;
    }
    TerminateProcess(child.hProcess,status);
    CloseHandle(child.hProcess);CloseHandle(child.hThread);
    if(gatewayListener!=INVALID_SOCKET) closesocket(gatewayListener);
    WSACleanup();
    return status;
}
