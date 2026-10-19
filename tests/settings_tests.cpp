// 设置持久化测试（第 5 轮迭代）：单字段类型错误只回落该字段、极端值夹取、原子写与坏文件。
#include <windows.h>
#include "data/settings.h"
#include <json.hpp>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {
int passed=0,failed=0;
void Check(bool condition,const std::string& name) {
    if(condition) ++passed;
    else { ++failed;std::cerr<<"FAIL "<<name<<"\n"; }
}
std::wstring TempDirectory() {
    wchar_t buffer[MAX_PATH]{};
    GetTempPathW(MAX_PATH,buffer);
    return std::wstring(buffer)+L"hermes-settings-test-"+std::to_wstring(GetCurrentProcessId());
}
}
int main() {
    const auto directory=TempDirectory();
    CreateDirectoryW(directory.c_str(),nullptr);
    const auto path=directory+L"\\settings.json";

    // 1) 单字段类型错误：其余字段必须保留（以前一处错会让整份设置回到默认）。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<R"({"theme":"dark","pollSeconds":"sixty","webuiPort":999999,
            "favorites":["a|b",7,null,"c|d"],"recents":[true,"e|f"],
            "latencyMinutes":33,"widgetStyle":"nope","float":["x","y"]})";
        out.close();
        hermes::SettingsStore store(path);
        const auto settings=store.Load();
        Check(settings.theme=="dark","a wrong-typed field does not reset the whole file");
        Check(settings.pollSeconds==60,"a wrong-typed number falls back to its default");
        Check(settings.webuiPort==65535,"an out-of-range port is clamped");
        Check(settings.favorites.size()==2&&settings.favorites[0]=="a|b"
            &&settings.favorites[1]=="c|d","only string favorites survive");
        Check(settings.recents.size()==1&&settings.recents[0]=="e|f","only string recents survive");
        Check(settings.latencyMinutes==5,"an invalid latency interval falls back to 5");
        Check(settings.widgetStyle=="beacon","an invalid widget style falls back to beacon");
        Check(settings.floating.x==200&&settings.floating.y==120,
            "a wrong-typed float pair keeps the default position");
    }

    // 2) 负端口夹到 1；轮询间隔夹到 [5,3600]。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<R"({"gatewayPort":-5,"pollSeconds":1,"latencyMinutes":60})";
        out.close();
        hermes::SettingsStore store(path);
        const auto settings=store.Load();
        Check(settings.gatewayPort==1,"a negative port is clamped to 1");
        Check(settings.pollSeconds==5,"a tiny poll interval is clamped to 5");
        Check(settings.latencyMinutes==60,"a valid latency interval is kept");
    }

    // 3) 坏 JSON：整体回落默认，且不崩。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<"{ this is not json";
        out.close();
        hermes::SettingsStore store(path);
        const auto settings=store.Load();
        Check(settings.theme=="system"&&settings.pollSeconds==60,"a corrupt file loads defaults");
    }

    // 4) 原子写 + 往返：保存后立即能读回，目录里不留 .tmp。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<"{}";
        out.close();
        hermes::SettingsStore store(path);
        auto settings=store.Load();
        settings.theme="porcelain";
        settings.widgetStyle="tag";
        settings.favorites={"a|b"};
        settings.webuiPort=9001;
        store.Save(settings,true);
        store.Flush();
        hermes::SettingsStore reloaded(path);
        const auto roundtrip=reloaded.Load();
        Check(roundtrip.theme=="porcelain","the theme round-trips through the file");
        Check(roundtrip.widgetStyle=="tag","the widget style round-trips through the file");
        Check(roundtrip.favorites.size()==1&&roundtrip.favorites[0]=="a|b",
            "favorites round-trip through the file");
        Check(roundtrip.webuiPort==9001,"the port round-trips through the file");
        Check(GetFileAttributesW((path+L".tmp").c_str())==INVALID_FILE_ATTRIBUTES,
            "an atomic write leaves no .tmp file behind");
    }

    // 5) 顶层不是对象（数组）：回落默认而不是抛异常。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<"[1,2,3]";
        out.close();
        hermes::SettingsStore store(path);
        const auto settings=store.Load();
        // 审计 F-014：默认发行版与旧版 main.js 的 SETTING_DEFAULTS.distro 对齐。
        Check(settings.theme=="system"&&settings.distro=="Ubuntu-24.04","a non-object root loads defaults");
    }

    // 5b) 审计 F-019：未知键在 Load→Save 往返后必须保留（向前兼容）。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<"{\"theme\":\"sand\",\"futureKey\":{\"nested\":42},\"another\":[1,2]}";
        out.close();
        hermes::SettingsStore store(path);
        const auto settings=store.Load();
        Check(settings.theme=="sand","the known key still loads next to unknown ones");
        store.Save(settings,true);
        std::ifstream input(path);
        nlohmann::json round;
        input>>round;
        Check(round.contains("futureKey")&&round["futureKey"].value("nested",0)==42,
            "unknown object keys survive a save");
        Check(round.contains("another")&&round["another"].size()==2,"unknown array keys survive a save");
    }

    // 6) 固定窗口高度：缺省 634，越界夹到 420..960，且与其它字段互不干扰。
    {
        std::ofstream out(path,std::ios::trunc);
        out<<"{\"height\":5000,\"theme\":\"sand\"}";
        out.close();
        hermes::SettingsStore store(path);
        auto settings=store.Load();
        Check(settings.heightDip==960,"an oversized height clamps to the maximum");
        Check(settings.theme=="sand","a tall height does not reset other fields");
        settings.heightDip=700;
        store.Save(settings,true);
        store.Flush();
        hermes::SettingsStore reloaded(path);
        Check(reloaded.Load().heightDip==700,"the panel height round-trips through the file");
        std::ofstream tiny(path,std::ios::trunc);
        tiny<<"{\"height\":100}";
        tiny.close();
        Check(hermes::SettingsStore(path).Load().heightDip==420,"a tiny height clamps to the minimum");
    }

    DeleteFileW(path.c_str());
    RemoveDirectoryW(directory.c_str());
    std::cout<<"settings: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}
