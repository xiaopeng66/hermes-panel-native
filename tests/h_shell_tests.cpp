// h-offline / h-migrate (PLAN §6 H6, gates `h-offline`, `h-migrate`):
//   h-offline: WSL 不可达时整块替换内容区、底栏不可点、离线卡有「启动 WSL」；恢复后回到正常页。
//   h-migrate: 旧 Electron 版设置一次性迁移（只读旧文件、只搬一次、旧文件不被改写）。
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
    state["providers"]=Json::array({{{"name","Alpha"},{"provider_id","alpha"},{"base_url","https://a/v1"},
        {"key_env","A"},{"key_set",true},{"api_mode","chat"},{"default_model","alpha-1"},
        {"models",{"alpha-1"}},{"models_discovered",false},{"is_active",true},{"line",1}}});
    state["fallbacks"]=Json::array();
    return state;
}
std::string ReadFile(const std::string& path) {
    std::ifstream input(path,std::ios::binary);
    std::ostringstream text;
    text<<input.rdbuf();
    return text.str();
}
void Tap(h::Client& client,const Json& rect) {
    const auto x=rect["x"].get<int>()+rect["width"].get<int>()/2;
    const auto y=rect["y"].get<int>()+rect["height"].get<int>()/2;
    client.Request({{"command","tap"},{"x",x},{"y",y},{"screen",true}});
    Sleep(150);
}
Json WaitFor(h::Client& client,const std::function<bool(const Json&)>& ready,int timeoutMs=12000) {
    const auto deadline=GetTickCount64()+static_cast<unsigned long long>(timeoutMs);
    auto snapshot=client.Request({{"command","dump"}});
    while(!ready(snapshot)&&GetTickCount64()<deadline) {
        Sleep(100);
        snapshot=client.Request({{"command","dump"}});
    }
    return snapshot;
}
}

int wmain(int argc,wchar_t** argv) {
    h::SilenceDebugAssertDialogs();
    if(argc!=4) return 2;
    const std::wstring scenario=argv[3];
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring id=scenario+L"-"+std::to_wstring(GetCurrentProcessId());
    const auto base=std::wstring(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/")+id;
    const auto home=base+L"/fake",legacy=base+L"/legacy";
    CreateDirectoryW(L"E:/AI/Zcode/tmp/hermes-panel-native/testdata",nullptr);
    CreateDirectoryW(base.c_str(),nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    CreateDirectoryW(legacy.c_str(),nullptr);
    {
        std::ofstream state(Narrow(home)+"/state.json",std::ios::trunc);
        state<<Fixture().dump(2);
    }
    SetEnvironmentVariableW(L"FAKE_HERMES_HOME",home.c_str());
    std::string legacyBefore;
    if(scenario==L"migrate") {
        // 旧 Electron 版设置（夹具；真实文件在 %APPDATA%，门禁从不碰它）
        std::ofstream file(Narrow(legacy)+"/settings.json",std::ios::trunc);
        file<<Json{{"distro","Ubuntu-22.04"},{"webuiPort",9119},{"gatewayPort",8642},{"pollSec",120},
            {"hotkey","Control+Alt+J"},{"theme","dark"},{"closeAction","quit"},{"alwaysOnTop",false},
            {"previewDiff",true},{"autoReloadGateway",true},
            {"favorites",{"custom:old|old-model","custom:other|second"}},
            {"recents",{"custom:old|old-model"}}}.dump(2);
        file.close();
        legacyBefore=ReadFile(Narrow(legacy)+"/settings.json");
        SetEnvironmentVariableW(L"HERMES_DISABLE_UNC",L"1");
    SetEnvironmentVariableW(L"HERMES_WSL_EXE",h::NativePath(argv[2]).c_str());
    } else {
        // 离线：桥指向"没有 WSL"的夹具（list 失败、ping 退出码非 0）
        SetEnvironmentVariableW(L"HERMES_WSL_EXE",h::NativePath(argv[2]).c_str());
        SetEnvironmentVariableW(L"FAKE_WSL_MODE",L"fail");
        SetEnvironmentVariableW(L"FAKE_WSL_PING",L"down");
    }

    auto command=h::AppCommandLine(argv[1],id,legacy);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,0,0,nullptr,nullptr,&startup,&child)) return 3;
    auto status=1;
    try {
        h::Require(WaitForInputIdle(child.hProcess,4000)==0,"app not ready");
        h::Client client(id);
        if(scenario==L"offline") {
            // 离线：整块替换 + 底栏不可点 + 离线卡按钮在
            auto snapshot=WaitFor(client,[](const Json& value) {
                return !value["models"]["offlineReason"].get<std::string>().empty();
            });
            h::Require(!snapshot["models"]["offlineReason"].get<std::string>().empty(),
                "a failing WSL shows the offline block");
            h::Require(!snapshot["models"]["lights"].empty()
                &&snapshot["models"]["lights"][0]["state"].get<int>()==3,"the WSL lamp shows err");
            const auto controls=client.Request({{"command","controls"}});
            h::Require(controls.contains("_offline"),"the offline card exposes its start button");
            std::cout<<"PASS h-offline: WSL 不可达时内容区整块换成离线卡\n";
            // 底栏在离线下不可点（旧 body.offline .nav .tab { pointer-events: none }）
            Tap(client,controls["tabAdd"]);
            snapshot=client.Request({{"command","dump"}});
            h::Require(snapshot["activePage"].get<int>()==0,"the bottom navigation is inert while offline");
            std::cout<<"PASS h-offline: 离线时底栏不接受点击\n";
            // 伪 WSL 仍然答不上来：点「启动 WSL」给出失败提示，仍然是离线
            {
                const auto restarted=client.Request({{"command","controls"}});
                const auto button=restarted["_offline"];
                Tap(client,button);
                snapshot=WaitFor(client,[](const Json& value) {
                    return value["toast"]["text"].get<std::string>().find("\xE5\x90\xAF\xE5\x8A\xA8 WSL")
                        !=std::string::npos;
                });
                h::Require(snapshot["toast"]["text"].get<std::string>().find("\xE5\xA4\xB1\xE8\xB4\xA5")
                    !=std::string::npos,"a failed start reports it");
                h::Require(!client.Request({{"command","dump"}})["models"]["offlineReason"]
                    .get<std::string>().empty(),"and the panel stays offline");
            }
            std::cout<<"PASS h-offline: 「启动 WSL」失败时给出提示且保持离线\n";
            status=0;
        } else {
            // 迁移：新设置文件里应出现旧值，旧文件原样不动
            const auto path=Narrow(base)+"/settings.json";
            bool migrated=false;
            for(int attempt=0;attempt<30&&!migrated;++attempt) {
                const auto text=ReadFile(path);
                if(!text.empty()) {
                    try {
                        const auto stored=Json::parse(text);
                        migrated=stored.value("legacyMigrated",false)
                            &&stored.value("distro",std::string{})==std::string("Ubuntu-22.04")
                            &&stored.value("webuiPort",0)==9119
                            &&stored.value("pollSeconds",0)==120
                            &&stored.value("hotkey",std::string{})==std::string("Control+Alt+J")
                            &&stored.value("closeAction",std::string{})==std::string("quit")
                            &&stored.value("previewDiff",false)
                            &&stored.value("autoReloadGateway",false)
                            &&stored.value("topmost",true)==false
                            &&stored["favorites"].size()==2
                            &&stored["favorites"][0].get<std::string>()=="custom:old|old-model"
                            &&stored["recents"].size()==1;
                    } catch(const std::exception&) {}
                }
                if(!migrated) Sleep(100);
            }
            h::Require(migrated,std::string("the legacy settings were migrated; file holds: ")
                +ReadFile(path).substr(0,240));
            h::Require(ReadFile(Narrow(legacy)+"/settings.json")==legacyBefore,
                "the legacy file was not modified");
            std::cout<<"PASS h-migrate: 旧设置搬进新设置，旧文件原样不动\n";
            // 迁移只发生一次：改掉旧文件后重启，面板不该再看它
            {
                std::ofstream file(Narrow(legacy)+"/settings.json",std::ios::trunc);
                file<<Json{{"distro","Changed-After-Migration"},{"webuiPort",1234}}.dump(2);
            }
            TerminateProcess(child.hProcess,0);
            WaitForSingleObject(child.hProcess,3000);
            CloseHandle(child.hProcess);CloseHandle(child.hThread);
            STARTUPINFOW again{sizeof(again)};
            PROCESS_INFORMATION restarted{};
            auto second=h::AppCommandLine(argv[1],id,legacy);
            h::Require(CreateProcessW(nullptr,second.data(),nullptr,nullptr,0,0,nullptr,nullptr,&again,
                &restarted)!=0,"restart failed");
            h::Require(WaitForInputIdle(restarted.hProcess,4000)==0,"restarted app not ready");
            h::Client second_client(id);
            auto snapshot=WaitFor(second_client,[](const Json& value) {
                return value["data"]["providers"].get<int>()==1;
            });
            const auto stored=second_client.Request({{"command","dump"}})["stored"];
            h::Require(stored["distro"].get<std::string>()=="Ubuntu-22.04",
                std::string("the migration does not run twice; stored=")+stored.dump());
            TerminateProcess(restarted.hProcess,0);
            CloseHandle(restarted.hProcess);CloseHandle(restarted.hThread);
            child={};
            std::cout<<"PASS h-migrate: 第二次启动不再读旧文件\n";
            status=0;
        }
    } catch(const std::exception& error) {
        std::cerr<<Narrow(scenario)<<": "<<error.what()<<"\n";
        status=1;
    }
    if(child.hProcess) {
        TerminateProcess(child.hProcess,status);
        CloseHandle(child.hProcess);CloseHandle(child.hThread);
    }
    return status;
}
