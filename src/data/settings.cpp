#include "settings.h"
#include "core/text.h"   // Narrow（全产品一份）
#include <windows.h>
#include "paths.h"
#include <json.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace hermes {
// 助手脚本随项目放在仓库 tools/。设置里这两个键**空串 = 自动**：按 exe 所在目录推算仓库根——
// 便携布局是 <仓库>\dist-live\HermesModelPanelNative.exe（脚本就在 <仓库>\tools），再往上的
// 目录层级逐级找 tools\<文件名>（最多 4 级，覆盖非标准安装深度）。Windows 侧拼出 WSL 的
// /mnt/<盘>/... 路径；exe 不在盘根形态（UNC 等）或找不到 tools 时，回退到开发机仓库根
// （kFallbackRepoRoot，保持旧默认行为）。用户显式设置的路径原样返回，任何自动值不落盘。
std::string ResolveHelperPath(const std::string& stored,const char* fileName) {
    if(!stored.empty()&&stored!=kLegacyHelperPathHome&&stored!=kLegacyHelperPathOld
        &&stored!=kLegacyHelperPathNative&&stored!=kLegacyReadHelperPathNative)
        return stored;
    wchar_t exePath[MAX_PATH]{};
    if(GetModuleFileNameW(nullptr,exePath,MAX_PATH)&&exePath[0]>=L'A'&&exePath[0]<=L'Z'
        &&exePath[1]==L':') {
        std::wstring directory=DirectoryOfPath(exePath);
        for(int depth=0;depth<4&&!directory.empty();++depth) {
            const auto candidate=directory+L"\\tools\\"+Widen(fileName);
            if(GetFileAttributesW(candidate.c_str())!=INVALID_FILE_ATTRIBUTES) {
                // W:/workspace → /mnt/w/workspace（小写盘符，与 /mnt 约定一致；盘符字母与
                // 冒号都已在 posix 前缀里，遍历时跳过，否则拼出 /mnt/eE:/...）
                std::string posix="/mnt/";
                posix.push_back(static_cast<char>(tolower(exePath[0])));
                const auto windows=directory+L"\\tools";
                for(std::size_t index=2;index<windows.size();++index) {
                    const wchar_t ch=windows[index];
                    posix.push_back(ch==L'\\'?'/':static_cast<char>(ch));
                }
                return posix+"/"+fileName;
            }
            const auto slash=directory.find_last_of(L"\\/");
            if(slash==std::wstring::npos) break;
            directory.resize(slash);
        }
    }
    return std::string{kFallbackRepoRoot}+"/tools/"+fileName;
}
// 旧值清成空串走自动：旧内置默认 ~、旧 Electron 项目绝对路径，以及 v1.0.0 发行版硬编码的
// 开发机绝对路径（只可能由程序写入，设置页没有这两个键的输入框）；用户自定的其它路径原样保留。
std::string NormalizeHelperPath(std::string path) {
    if(path==kLegacyHelperPathHome||path==kLegacyHelperPathOld
        ||path==kLegacyHelperPathNative||path==kLegacyReadHelperPathNative) return {};
    return path;
}
namespace {
constexpr unsigned long long kThrottleMilliseconds=1000;
std::wstring DirectoryOf(const std::wstring& path) {
    const auto slash=path.find_last_of(L"\\/");
    return slash==std::wstring::npos?std::wstring{}:path.substr(0,slash);
}
nlohmann::json ToJson(const Settings& settings) {
    return {{"version",1},
        {"edge",static_cast<int>(settings.edge)},
        // 不再写 tier：紧凑视图已删除，落盘只保留 Full（读回时也会被归一）。
        {"tier",static_cast<int>(Tier::Full)},
        {"float",{settings.floating.x,settings.floating.y}},
        {"stripFloat",settings.stripFloat},
        {"stripFloatPos",{settings.stripFloatPos.x,settings.stripFloatPos.y}},
        {"anchor",settings.anchor},
        {"height",settings.heightDip},
        {"monitorId",settings.monitorId},
        {"topmost",settings.topmost},
        {"pollSeconds",settings.pollSeconds},
        {"theme",settings.theme},
        {"distro",settings.distro},
        {"pythonExe",settings.pythonExe},
        {"helperPath",settings.helperPath},
        {"readHelperPath",settings.readHelperPath},
        {"webuiPort",settings.webuiPort},
        {"gatewayPort",settings.gatewayPort},
        {"favorites",settings.favorites},
        {"recents",settings.recents},
        {"closeAction",settings.closeAction},
        {"hotLoadedMtime",settings.hotLoadedMtime},
        {"hotkey",settings.hotkey},
        {"previewDiff",settings.previewDiff},
        {"autoReloadGateway",settings.autoReloadGateway},
        {"latencyProbe",settings.latencyProbe},
        {"latencyMinutes",settings.latencyMinutes},
        {"widgetStyle",settings.widgetStyle},
        {"legacyMigrated",settings.legacyMigrated}};
}
namespace {
// 逐字段类型检查：单个字段类型写错只回落到该字段的默认值，不能让整个设置文件被当成损坏。
int IntOr(const nlohmann::json& value,const char* key,int fallback) {
    return value.contains(key)&&value[key].is_number()?value[key].get<int>():fallback;
}
long long LongOr(const nlohmann::json& value,const char* key,long long fallback) {
    return value.contains(key)&&value[key].is_number()?value[key].get<long long>():fallback;
}
bool BoolOr(const nlohmann::json& value,const char* key,bool fallback) {
    return value.contains(key)&&value[key].is_boolean()?value[key].get<bool>():fallback;
}
std::string StrOr(const nlohmann::json& value,const char* key,const std::string& fallback) {
    return value.contains(key)&&value[key].is_string()?value[key].get<std::string>():fallback;
}
}
Settings FromJson(const nlohmann::json& value) {
    Settings settings;
    if(!value.is_object()) return settings;
    const auto readEdge=IntOr(value,"edge",0);
    settings.edge=readEdge>=0&&readEdge<=4?static_cast<Edge>(readEdge):Edge::None;
    // 紧凑视图已按用户要求删除（152 DIP 在原生面板上装不下任何页面正文）。
    // 旧配置里可能还留着 tier=1 —— 一律归到 Full，否则读回一个 152 DIP 的窗口。
    // （Tier 枚举与 TierSize 的 152 契约保留：几何门禁仍在断言它，且 TierSize(Tier,...)
    //   这个重载被 widget/app 的尺寸测试直接调用。）
    settings.tier=Tier::Full;
    if(value.contains("float")&&value["float"].is_array()&&value["float"].size()==2
        &&value["float"][0].is_number()&&value["float"][1].is_number()) {
        settings.floating={value["float"][0].get<int>(),value["float"][1].get<int>()};
    }
    settings.stripFloat=BoolOr(value,"stripFloat",false);
    if(value.contains("stripFloatPos")&&value["stripFloatPos"].is_array()
        &&value["stripFloatPos"].size()==2&&value["stripFloatPos"][0].is_number()
        &&value["stripFloatPos"][1].is_number()) {
        settings.stripFloatPos={value["stripFloatPos"][0].get<int>(),value["stripFloatPos"][1].get<int>()};
    }
    settings.anchor=IntOr(value,"anchor",0);
    // 高度落盘只做静态范围检查；具体显示器的上限在初始化/拉伸时按工作区再夹一次。
    settings.heightDip=std::clamp(IntOr(value,"height",kPanelDefaultHeightDip),
        kPanelMinHeightDip,kPanelMaxHeightDip);
    settings.monitorId=StrOr(value,"monitorId",std::string{});
    settings.topmost=BoolOr(value,"topmost",true);
    settings.pollSeconds=IntOr(value,"pollSeconds",60);
    settings.theme=StrOr(value,"theme",std::string{"system"});
    settings.distro=StrOr(value,"distro",std::string{"Ubuntu"});
    settings.pythonExe=StrOr(value,"pythonExe",std::string{"python3"});
    settings.helperPath=NormalizeHelperPath(StrOr(value,"helperPath",std::string{}));
    settings.readHelperPath=NormalizeHelperPath(StrOr(value,"readHelperPath",std::string{}));
    settings.webuiPort=IntOr(value,"webuiPort",8787);
    settings.gatewayPort=IntOr(value,"gatewayPort",8642);
    // 加载即截断（与写入路径同一个上限）：损坏/手改的文件里可能有几十万条，
    // 全读进内存会让启动变慢、每次保存都要序列化整份数组。
    if(value.contains("favorites")&&value["favorites"].is_array())
        for(const auto& entry:value["favorites"]) {
            if(static_cast<int>(settings.favorites.size())>=kFavoritesMax) break;
            if(entry.is_string()) settings.favorites.push_back(entry.get<std::string>());
        }
    if(value.contains("recents")&&value["recents"].is_array())
        for(const auto& entry:value["recents"]) {
            if(static_cast<int>(settings.recents.size())>=kRecentsMax) break;
            if(entry.is_string()) settings.recents.push_back(entry.get<std::string>());
        }
    settings.closeAction=StrOr(value,"closeAction",std::string{"hide"});
    settings.hotLoadedMtime=LongOr(value,"hotLoadedMtime",0);
    settings.hotkey=StrOr(value,"hotkey",std::string{"Control+Alt+H"});
    settings.previewDiff=BoolOr(value,"previewDiff",false);
    settings.autoReloadGateway=BoolOr(value,"autoReloadGateway",false);
    settings.latencyProbe=BoolOr(value,"latencyProbe",false);
    settings.latencyMinutes=IntOr(value,"latencyMinutes",5);
    if(settings.latencyMinutes!=5&&settings.latencyMinutes!=15&&settings.latencyMinutes!=60)
        settings.latencyMinutes=5;
    settings.widgetStyle=StrOr(value,"widgetStyle",std::string{"capsule"});
    // 只剩铭牌/胶囊两套（信标/旗标已删）；旧值就近映射：badge/halo→铭牌，
    // beacon/pulse/flag/magnet 与未知值→胶囊。
    if(settings.widgetStyle!="tag"&&settings.widgetStyle!="capsule")
        settings.widgetStyle=(settings.widgetStyle=="badge"||settings.widgetStyle=="halo")
            ?"tag":"capsule";
    settings.legacyMigrated=BoolOr(value,"legacyMigrated",false);
    if(settings.pollSeconds<5) settings.pollSeconds=5;
    if(settings.pollSeconds>3600) settings.pollSeconds=3600;
    if(settings.webuiPort<1) settings.webuiPort=1;
    if(settings.webuiPort>65535) settings.webuiPort=65535;
    if(settings.gatewayPort<1) settings.gatewayPort=1;
    if(settings.gatewayPort>65535) settings.gatewayPort=65535;
    return settings;
}
}
SettingsStore::SettingsStore(std::wstring path) : path_(std::move(path)),directory_(DirectoryOf(path_)) {
    if(directory_.empty()) return;
    EnsureDirectory(directory_);
    EnsureDirectory(directory_+L"\\logs");
}
Settings SettingsStore::Load() {
    std::ifstream input(path_);
    if(!input) return Settings{};
    try {
        nlohmann::json value;
        input>>value;
        if(value.is_object()) {
            // 审计 F-019：记录这一版不认识的键，保存时原样写回（向前兼容）。
            static const char* kKnown[]={"version","edge","tier","float","stripFloat","stripFloatPos",
                "anchor","height",
                "monitorId","topmost","pollSeconds","theme","distro","pythonExe","helperPath",
                "readHelperPath","webuiPort","gatewayPort","favorites","recents","closeAction",
                "hotLoadedMtime","hotkey","previewDiff","autoReloadGateway","latencyProbe",
                "latencyMinutes","widgetStyle","legacyMigrated"};
            extras_=nlohmann::json::object();
            for(const auto& item:value.items()) {
                bool known=false;
                for(const char* key:kKnown) if(item.key()==key) { known=true;break; }
                if(!known) extras_[item.key()]=item.value();
            }
        }
        return FromJson(value);
    } catch(const std::exception&) {
        // A corrupt file must never block startup: fall back to defaults and let the next save rewrite it.
        return Settings{};
    }
}
bool SettingsStore::WriteAtomically(const Settings& settings) {
    const auto temporary=path_+L".tmp";
    {
        std::ofstream output(temporary,std::ios::trunc|std::ios::binary);
        if(!output) return false;
        auto value=nlohmann::json::object();
        if(extras_.is_object()) value=extras_;          // 先写未知键，已知键随后覆盖
        // 注意：不能直接对 ToJson() 的临时对象取 items()（迭代器会悬垂，启动即 AV）。
        const auto known=ToJson(settings);
        for(const auto& item:known.items()) value[item.key()]=item.value();
        output<<value.dump(2)<<'\n';
        output.flush();
        if(!output) return false;
    }
    if(!MoveFileExW(temporary.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    ++writes_;
    lastWriteTick_=GetTickCount64();
    return true;
}
void SettingsStore::Save(const Settings& settings,bool force) {
    pending_=settings;
    dirty_=true;
    if(force||GetTickCount64()-lastWriteTick_>=kThrottleMilliseconds) {
        if(WriteAtomically(pending_)) dirty_=false;
    }
}
void SettingsStore::Flush() {
    if(!dirty_) return;
    if(WriteAtomically(pending_)) dirty_=false;
}
}
