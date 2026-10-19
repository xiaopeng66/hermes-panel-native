#include "app/application.h"
#include "app/application_internal.h"
#include "control_plane.h"
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdexcept>
#include "data/http.h"
#include "data/paths.h"
#include "data/probe.h"
#include "data/unc_snapshot.h"
#include "render/widgets.h"
#include <windowsx.h>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hermes {
// 解析助手回复：解析器本身已做逐字段类型检查，这里再兜一层——畸形数据最多变成一条错误状态，
// 绝不能让异常从工作线程逃出去（std::terminate）。
HermesState ParseStateSafe(const std::string& list,const std::string& health,
    const std::string& webui,Log* log) {
    try { return ParseState(list,health,webui); }
    catch(const std::exception& error) {
        if(log) log->Info("data","state parse failed: "+std::string(error.what()));
        HermesState state;
        state.loading=false;
        state.error=std::string("解析助手回复失败：")+error.what();
        state.wsl="up";
        return state;
    }
}

// DESIGN v1.7：health 的 config/env 部分由 UNC 文件系统事实合成；systemd 属性
// （gateway.since/active）UNC 不可得，留空 —— UI 对空 since 的文案是「网关尚未重启」。
std::string UncHealthJson() {
    Json health{
        {"config",Json{{"mtime",0},{"size",0},{"parses",true}}},
        {"env_file",Json{{"exists",true}}},
        {"webui",Json{{"port",0},{"reachable",false}}},
        {"gateway",Json{{"port",0},{"reachable",false},{"since",""},{"sinceUnix",0},{"active",""}}},
    };
    return health.dump();
}
// WebUI/网关的 TCP 现探（纯 Windows 侧，与 wsl.exe 路径同一套规则）。
void FillReachability(HermesState& state,int webuiPort,int gatewayPort) {
    std::vector<int> candidates{webuiPort,8787,9119};
    std::sort(candidates.begin(),candidates.end());
    candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
    for(const int port:candidates) {
        if(TcpReachable("127.0.0.1",static_cast<unsigned>(port),1200)) {
            state.webui.port=port;
            state.webui.reachable=true;
            break;
        }
    }
    state.gateway.port=gatewayPort;
    if(TcpReachable("127.0.0.1",static_cast<unsigned>(gatewayPort),1200))
        state.gateway.reachable=true;
}

void Application::ApplyHelperState(const HermesState& state) {
    hermesState_=state;
    const auto signature=Signature(state);
    if(signature!=stateSignature_) {
        stateSignature_=signature;
        if(log_) log_->Info("data","state changed: providers="+std::to_string(state.providers.size())
            +" fallbacks="+std::to_string(state.fallbacks.size()));
    }
    // 托盘图标/悬浮提示跟着状态走（旧 updateTrayStatus 在每次 renderStatus 后调用）。
    UpdateTrayStatus();
    CheckNotificationEdges();
    // 信标挂件的呼吸只应该在"有故障"时跑：链路恢复后必须停掉计时器，否则会一直重绘。
    UpdateWidgetTimer();
    InvalidateAll();
}
void Application::Notify(const std::string& event,const std::string& text) {
    // 边沿已由 CheckNotificationEdges 判定；这里只做 10 分钟抑制与投递。
    const auto now=GetTickCount64();
    const auto found=notifiedAt_.find(event);
    if(found!=notifiedAt_.end()&&now-found->second<kNotifySuppressionMs) return;
    notifiedAt_[event]=now;
    ++notificationCount_;
    ++notificationKinds_[event];
    lastNotificationText_=text;
    if(log_) log_->Info("notify",event);   // 只记事件名，不记文案（可能含错误首行/端口）
    if(!trayRegistered_||!panel_) return;
    NOTIFYICONDATAW data{};
    data.cbSize=sizeof(data);
    // 图标注册在 control_ 上（AddTray）：NIM_MODIFY 必须用同一个 hWnd+uID 才能定位图标。
    // 审计 F-008：这里曾用 panel_，Win32 实测返回 FALSE，气泡永远不会出现（计数却照加）。
    data.hWnd=control_;
    data.uID=kTrayId;
    data.uFlags=NIF_INFO;
    data.dwInfoFlags=NIIF_INFO;
    data.uTimeout=10000;
    const auto title=Widen("Hermes 面板");
    wcsncpy_s(data.szInfoTitle,title.c_str(),_TRUNCATE);
    const auto body=Widen(text);
    wcsncpy_s(data.szInfo,body.c_str(),_TRUNCATE);
    lastNotifyOk_=Shell_NotifyIconW(NIM_MODIFY,&data)!=FALSE;
}
void Application::CheckNotificationEdges() {
    // F6：只在状态翻转时发（第一次拿到的状态只用来打底，不通知）。
    const bool gatewayOk=hermesState_.gateway.reachable;
    const bool webuiOk=hermesState_.webui.reachable;
    const bool helperFailed=!helperError_.empty();
    if(notifyPrimed_) {
        if(lastGatewayOk_&&!gatewayOk&&hermesState_.wsl=="up")
            Notify("gateway",Narrow(L"网关不可达（端口 ")+std::to_string(hermesState_.gateway.port)
                +Narrow(L"）"));
        if(lastWebuiOk_&&!webuiOk)
            Notify("webui",Narrow(L"WebUI 不可达（端口 ")+std::to_string(hermesState_.webui.port)
                +Narrow(L"）"));
        if(!lastHelperFailed_&&helperFailed)
            Notify("helper",Narrow(L"助手出错：")+TruncateUtf8(helperError_,120));
        if(!lastPendingRestart_&&pendingGatewayRestart_)
            Notify("restart",Narrow(L"已切换模型，网关待重启"));
        if(!lastRefreshFailing_&&consecutiveRefreshFailures_>=3)
            Notify("refresh",Narrow(L"连续刷新失败，请检查 WSL"));
    }
    notifyPrimed_=true;
    lastGatewayOk_=gatewayOk;
    lastWebuiOk_=webuiOk;
    lastHelperFailed_=helperFailed;
    lastPendingRestart_=pendingGatewayRestart_;
    lastRefreshFailing_=consecutiveRefreshFailures_>=3;
}
void Application::ToggleFavorite(const std::string& key) {
    // main.js `favorite`: prepend (cap 200) or drop when it is already there.
    const auto found=std::find(stored_.favorites.begin(),stored_.favorites.end(),key);
    if(found==stored_.favorites.end()) {
        stored_.favorites.insert(stored_.favorites.begin(),key);
        if(stored_.favorites.size()>kFavoritesMax) stored_.favorites.resize(kFavoritesMax);
    } else {
        stored_.favorites.erase(found);
    }
    PersistSettings(true);
    InvalidateAll();
}
void Application::SwitchModel(const std::string& providerId,const std::string& model) {
    if(providerId==hermesState_.model.provider&&model==hermesState_.model.model) {
        if(log_) log_->Info("data","already the current model");
        return;
    }
    if(hermesState_.loading||hermesState_.wsl!="up") {
        if(log_) log_->Info("data",std::string("switch refused: ")+(hermesState_.loading?"loading":"wsl down"));
        return;
    }
    // 真机反馈：微信/QQ 机器人要等网关重启后才用上新模型 —— 切换前弹二次确认，确认后按
    // 「自动重启网关」开关决定自动重启还是挂「网关待重启」状态（见 StartSwitch 完成块）。
    if(log_) log_->Info("data","switch confirm requested: "+providerId+" / "+model);
    pending_=Pending::SwitchModel;
    pendingSwitchProvider_=providerId;
    pendingSwitchModel_=model;
    InvalidateAll();
}
void Application::StartRefresh() {
    // Single-flight, like the old panel's tickRunning guard: a poll landing on top of a manual refresh
    // is dropped instead of queueing a second WSL round trip.
    if(workerBusy_.exchange(true)||stopping_.load()) return;
    // The previous job has finished (it clears the busy flag as it leaves), so its thread object can be
    // reaped here: assigning to a still-joinable std::thread calls std::terminate.
    if(worker_.joinable()) worker_.join();
    // 刷新进行中：同步章的图标绕中心旋转（真机反馈：不要「读取 WSL 配置…」的文字，改成图标转）。
    if(panel_) { syncSpinAngle_=0.0f;SetTimer(panel_,kSpinTimer,50,nullptr); }
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.helperPath;
    const auto webuiPort=stored_.webuiPort,gatewayPort=stored_.gatewayPort;
    const bool first=firstRefresh_;
    firstRefresh_=false;
    if(log_) log_->Info("data","refresh started"+(first?std::string(" (first: warming WSL)"):std::string{}));
    // DESIGN v1.7：UNC 读路径优先。快照成功 → 不起任何 wsl.exe；不可达/首次 → 原路径兜底。
    // mtime+size 与上次一致 → 直接复用缓存解析结果，连读都不读。
    // HERMES_DISABLE_UNC=1：门禁夹具强制走假助手。harness 的硬约束是夹具隔离——而真机发行版
    // 里恰好有一个 .hermes，不禁止的话测试实例会经 UNC 读到真机配置，污染夹具断言。
    // 带控制面的构建（Debug/门禁）默认禁用：测试实例可能经 geometry-probe 转发启动，
    // 环境变量不一定穿透；UNC 是真机（Release）专属优化路径。
#if HERMES_CONTROL_PLANE
    const auto home=std::string{};
#else
    wchar_t uncFlag[4]{};
    const auto uncDisabled=GetEnvironmentVariableW(L"HERMES_DISABLE_UNC",uncFlag,4)!=0
        &&uncFlag[0]==L'1';
    // 真机探测：helperPath 推不出 home（脚本在 /mnt/* 下，常态）时枚举发行版 /home/ 下唯一
    // 用户目录拼 .hermes。只在产品（Release，无控制面）构建里做——门禁夹具绝不摸真配置。
    auto home=uncDisabled
        ?std::string{}:WslHomeFromHelperPath(stored_.helperPath);
    if(!uncDisabled&&home.empty()) {
        WIN32_FIND_DATAW data{};
        std::wstring homeGlob=L"//wsl$/";
        for(const char ch:stored_.distro) homeGlob.push_back(static_cast<wchar_t>(ch));
        homeGlob+=L"/home/*";
        HANDLE find=FindFirstFileW(homeGlob.c_str(),&data);
        if(find!=INVALID_HANDLE_VALUE) {
            std::vector<std::wstring> users;
            do {
                if(wcscmp(data.cFileName,L".")&&wcscmp(data.cFileName,L".."))
                    users.push_back(data.cFileName);
            } while(FindNextFileW(find,&data));
            FindClose(find);
            if(users.size()==1) {
                home="/home/";
                for(const wchar_t ch:users[0]) home.push_back(static_cast<char>(ch));
                home+="/.hermes";
            }
        }
    }
#endif
    if(!first) {
        const auto snapshot=TakeUncSnapshot(distro,home,std::to_string(gatewayPort));
        if(snapshot.available&&snapshot.configMtime==uncCacheMtime_&&snapshot.configSize==uncCacheSize_
            &&!uncCacheListJson_.empty()) {
            if(log_) log_->Info("data","refresh from UNC cache (config unchanged)");
            auto* result=new DataResult;
            result->helperErrorValid=true;
            result->state=ParseStateSafe(uncCacheListJson_,uncCacheHealthJson_,std::string("{}"),log_);
            result->state.loading=false;
            // 灯照常现探：TCP 探活是纯 Windows 侧成本（<20ms），不吃 WSL。
            FillReachability(result->state,webuiPort,gatewayPort);
            workerBusy_=false;
            if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
            return;
        }
        if(snapshot.available) {
            if(log_) log_->Info("data","refresh from UNC snapshot (config mtime="+std::to_string(snapshot.configMtime)+")");
            uncCacheMtime_=snapshot.configMtime;
            uncCacheSize_=snapshot.configSize;
            uncCacheListJson_=snapshot.listJson;
            // health 的 config 部分从快照合成；gateway systemd 属性 UNC 不可得（DESIGN v1.7）。
            uncCacheHealthJson_=UncHealthJson();
            uncCacheValid_=true;
            auto* result=new DataResult;
            result->helperErrorValid=true;
            result->state=ParseStateSafe(uncCacheListJson_,uncCacheHealthJson_,std::string("{}"),log_);
            result->state.loading=false;
            FillReachability(result->state,webuiPort,gatewayPort);
            workerBusy_=false;
            if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
            return;
        }
        if(log_) log_->Info("data","UNC unavailable ("+snapshot.error+"), falling back to wsl.exe");
    }
    worker_=std::thread([this,distro,python,path,webuiPort,gatewayPort,first] {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
        bridge.SetLogSink([this](const std::string& line) { if(log_) log_->Info("helper",line); });
#if HERMES_CONTROL_PLANE
        // The acceptance runs point the bridge at tools/fake_hermes.cpp (see EnvironmentValue).
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        // 冷启动的 WSL 第一次调用可能要几十秒（发行版/虚拟机启动）：先 ping 一次把它捂热，第一次的
        // list 也放更长的时限。UI 侧同时显示「读取 WSL 配置…」，用户能看到在做什么而不是像卡死。
        if(first) bridge.WslReachable();
        const unsigned long listTimeout=first?60000u:20000u;
        const auto list=bridge.Call("{\"cmd\":\"list\"}",listTimeout);
        const auto health=bridge.Call("{\"cmd\":\"health\"}",first?30000u:15000u);
        if(stopping_.load()) { workerBusy_=false;return; }
        if(!list.ok) {
            // 助手报了错（脚本路径不对、python 缺依赖、配置读不了……）：把原因留下来给界面显示，
            // 否则用户只会看到"没有任何供应商"的空列表，完全不知道发生了什么。
            const auto helperError=list.error.empty()
                ?std::string("\xE5\x8A\xA9\xE6\x89\x8B\xE6\xB2\xA1\xE6\x9C\x89\xE5\x9B\x9E\xE7\xAD\x94")
                :list.error;
            if(log_) log_->Info("data","helper error: "+helperError);
            auto* failure=new DataResult;
            failure->helperErrorValid=true;
            failure->helperError=helperError;
            failure->state.wsl=bridge.WslReachable()?"up":"down";
            failure->state.loading=false;
            failure->state.error="\xE8\xAF\xBB\xE5\x8F\x96 WSL \xE9\x85\x8D\xE7\xBD\xAE\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A"
                +helperError;
            workerBusy_=false;
            if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(failure))==FALSE) delete failure;
            return;
        }
        // 成功：结果里的 helperError 默认空串，UI 侧据此清除（不再由轮询线程直写成员）。
        auto* result=new DataResult;
        result->helperErrorValid=true;
        result->state=ParseStateSafe(list.ok?list.data:std::string("{}"),
            health.ok?health.data:std::string("{}"),std::string("{}"),log_);
        // The helper answering at all is the WSL liveness signal the old panel used; only a failed
        // list falls back to `wsl.exe -- true` to tell a dead distro from a helper error.
        result->state.wsl=list.ok?"up":(bridge.WslReachable()?"up":"down");
        result->state.loading=false;
        // WebUI 探活：依次试设置里的端口 → 8787 → 9119，命中即用并写回 port。
        std::vector<int> candidates{webuiPort,8787,9119};
        std::sort(candidates.begin(),candidates.end());
        candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
        for(const int port:candidates) {
            if(TcpReachable("127.0.0.1",static_cast<unsigned>(port),1200)) {
                result->state.webui.port=port;
                result->state.webui.reachable=true;
                break;
            }
        }
        result->state.gateway.port=gatewayPort;
        if(TcpReachable("127.0.0.1",static_cast<unsigned>(gatewayPort),1200))
            result->state.gateway.reachable=true;
        // 先释放「有调用在飞」再投递结果：结果在 UI 线程里落地时，排队的用户动作
        // （托盘菜单点击撞上轮询刷新）才不会被当成第二次并发调用而丢掉（真机上点「重启网关」
        // 没反应就是这个静默丢包）。
        workerBusy_=false;
        if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
    });
}
void Application::ShowToast(const std::string& text,int kind) {
    toastText_=text;
    toastKind_=kind;
    UpdateToastTimer();
    InvalidateAll();
}
void Application::UpdateToastTimer() {
    if(!panel_) return;
    KillTimer(panel_,kToastTimer);
    if(!toastText_.empty()) SetTimer(panel_,kToastTimer,kToastMilliseconds,nullptr);
}
namespace {
// 助手真正实现 dry_run 的命令（apply 脚本里读取 payload["dry_run"] 的分支）：
// set-main / upsert-provider / add-models / fallback-add。其余写命令会**忽略**该字段直接落盘，
// 所以绝不能给它们注入 dry_run —— 否则用户以为"只是预览"，实际已经写盘（审计 F-003）。
bool SupportsDryRun(const std::string& cmd) {
    return cmd=="set-main"||cmd=="upsert-provider"||cmd=="add-models"||cmd=="fallback-add";
}
// 所有会改 config.yaml / .env 的命令（用于 DESIGN §5.1 的「解析失败则写操作全停」守卫）。
bool IsConfigWrite(const std::string& cmd) {
    return SupportsDryRun(cmd)||cmd=="delete-provider"||cmd=="fallback-remove"
        ||cmd=="fallback-move"||cmd=="restore";
}
}
void Application::StartHelperAction(const std::string& verb,Json args,const std::string& successToast,
    bool refreshAfter,bool probeFallbacks) {
    if(stopping_.load()) return;
    // DESIGN §5.1：config.yaml 解析失败时写操作全停（助手也会拒绝，但界面必须先说清楚）。
    // 只拦住真正的写命令；读/探测/备份/重启（不改 config）仍可用。
    if(hermesState_.config.parses==false&&IsConfigWrite(verb)) {
        ShowToast(Narrow(L"config.yaml \u89E3\u6790\u5931\u8D25\uFF0C\u5199\u64CD\u4F5C\u5DF2\u505C\u7528"),
            2);
        if(log_) log_->Warn("data","write refused: config.yaml does not parse ("+verb+")");
        return;
    }
    // 「写入前先看 diff」：只有助手支持 dry_run 的写操作才先跑一遍预演（旧版 previewDiff 的契约）。
    // 删除/移除/移动/恢复没有 dry_run 分支，它们各自有危险确认模态（删除、移除、恢复）或可逆（移动）。
    const bool wantsPreview=stored_.previewDiff&&!args.contains("dry_run")&&SupportsDryRun(verb);
    if(wantsPreview) args["dry_run"]=true;
    if(workerBusy_.exchange(true)) {
        // 用户点出来的动作（托盘菜单/面板按钮）**排队**而不是丢掉：正在飞的调用落地后按顺序执行。
        // 同一动作（verb + 参数一致）合并，只留最新的一份；不同动作都保留。自动轮询仍然直接丢弃
        // （StartRefresh 里的规则）。
        const auto signature=verb+" "+args.dump();
        const auto existing=std::find_if(pendingActions_.begin(),pendingActions_.end(),
            [&](const PendingAction& action) { return action.verb+" "+action.args.dump()==signature; });
        PendingAction queued{verb,args,successToast,refreshAfter,probeFallbacks};
        if(existing!=pendingActions_.end()) *existing=queued;
        else {
            pendingActions_.push_back(queued);
            if(pendingActions_.size()>8) pendingActions_.erase(pendingActions_.begin());
        }
        if(log_) log_->Info("data","action "+verb+" queued behind an in-flight call ("
            +std::to_string(pendingActions_.size())+" waiting)");
        return;
    }
    if(worker_.joinable()) worker_.join();
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.helperPath;
    if(log_) log_->Info("data","action "+verb+" started");
    worker_=std::thread([this,distro,python,path,verb,args,successToast,refreshAfter,probeFallbacks,wantsPreview] {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        auto* result=new DataResult;
        result->write=true;
        result->verb=verb;
        result->toast=successToast;
        // 手动重启网关的动作完成后清掉寄存器行的「重启网关」胶囊（机器人已用上新模型）。
        if(verb=="reload"&&args.value("targets",Json::array())==Json::array({"gateway"}))
            result->clearsGatewayRestart=true;
        if(!probeFallbacks) {
            // A single verb call. The probe pass below has its own loop, so it must not also fire the
            // placeholder call the page passed in.
            const auto reply=bridge.Call(args.dump(),60000);
            if(stopping_.load()) { workerBusy_=false;delete result;return; }
            if(!reply.ok) {
                result->error=reply.error.empty()?"helper exit "+std::to_string(reply.exitCode):reply.error;
                result->toast.clear();
            } else if(verb=="probe") {
                try {
                    const auto data=Json::parse(reply.data);
                    if(data.contains("models")&&data["models"].is_array())
                        for(const auto& item:data["models"])
                            if(item.is_string()) result->models.push_back(item.get<std::string>());
                } catch(const std::exception&) {}
            } else if(verb=="backup-list") {
                try {
                    const auto data=Json::parse(reply.data);
                    if(data.contains("backups")&&data["backups"].is_array()) {
                        for(const auto& item:data["backups"]) {
                            BackupRow row;
                            row.file=item.value("file",std::string{});
                            row.kind=item.value("kind",std::string{"config"});
                            row.stamp=item.value("mtime_text",std::string{});
                            row.sizeKb=static_cast<int>((item.value("size",0LL)+1023)/1024);
                            result->backups.push_back(row);
                        }
                    }
                } catch(const std::exception&) {}
            }
            // 预演回复（data.dry_run）：diff 交给确认卡；不算写入完成、不触发重读。
            if(reply.ok) {
                try {
                    const auto data=Json::parse(reply.data);
                    if(wantsPreview&&!data.value("dry_run",false)) {
                        // 请求了预演，助手却直接写了（契约漂移）：必须让用户知道，不能静默。
                        result->previewIgnored=true;
                        if(log_) log_->Warn("data","dry_run requested but the helper wrote directly: "+verb);
                    }
                    if(data.value("dry_run",false)) {
                        if(log_) log_->Info("data","dry run preview: "+verb);
                        result->previewOnly=true;
                        if(data.contains("diff")&&data["diff"].is_array()) {
                            for(const auto& block:data["diff"]) {
                                if(!block.is_object()) continue;
                                result->previewFacts.push_back(
                                    "区块："+block.value("block",std::string{}));
                                int shown=0;
                                if(block.contains("lines")&&block["lines"].is_array()) {
                                    for(const auto& line:block["lines"]) {
                                        if(!line.is_string()) continue;
                                        result->previewFacts.push_back(line.get<std::string>());
                                        if(++shown>=12) {
                                            result->previewFacts.push_back("…（其余行略）");
                                            break;
                                        }
                                    }
                                }
                                if(block.value("truncated",false))
                                    result->previewFacts.push_back(
                                        "…（该区块输出已截断）");
                            }
                        }
                        if(data.contains("env_will_write")&&data["env_will_write"].is_string()
                            &&!data["env_will_write"].get<std::string>().empty())
                            result->previewFacts.push_back(
                                "会写入 .env："+data["env_will_write"].get<std::string>());
                        if(result->previewFacts.empty())
                            result->previewFacts.push_back("（没有差异）");
                        result->previewVerb=verb;
                        result->previewToast=successToast;
                        result->previewRefreshAfter=refreshAfter;
                        result->previewProbe=probeFallbacks;
                        result->previewArgs=args;
                        result->previewArgs["dry_run"]=false;   // 确认后真写
                        result->toast.clear();
                    }
                } catch(const std::exception&) {}
            }
        }
        if(probeFallbacks) {
            // 逐条探测（旧 fbProbe）：一行一次调用，**测完一条立刻投递**（真机反馈：不能等整条链
            // 跑完才显示状态）。优先用只读的 `probe`（带该行的 base_url/key_env，助手自己从 .env
            // 取 key）——真助手的 `fallback-probe` 忽略 index 并把所有行一次性返回，既不支持逐行
            // 进度，它的 has_model 判定也和面板看到的模型名写法不一致（"明明有却说没有"）。
            // 没有 base_url 的老配置退回 `fallback-probe`（按 index）。
            const auto rows=hermesState_.fallbacks;
            result->probed=true;
            result->probes.assign(rows.size(),FallbackProbe{});
            probeTotal_.store(static_cast<int>(rows.size()));
            probeDone_.store(0);
            probeActive_.store(-1);
            probeRunning_.store(true);
            if(panel_) PostMessageW(panel_,kProbeProgressMessage,0,0);
            const auto pushRow=[&](std::size_t index,const FallbackProbe& entry) {
                result->probes[index]=entry;
                auto* payload=new ProbeRow{static_cast<int>(index),entry};
                if(panel_) {
                    if(PostMessageW(panel_,kProbeRowMessage,0,reinterpret_cast<LPARAM>(payload))==FALSE)
                        delete payload;
                } else delete payload;
            };
            for(std::size_t index=0;index<rows.size();++index) {
                if(stopping_.load()) break;
                probeActive_.store(static_cast<int>(index));
                if(panel_) PostMessageW(panel_,kProbeProgressMessage,0,0);
                FallbackProbe entry;
                if(!rows[index].baseUrl.empty()) {
                    // 逐行只读探测：助手自己从 .env 取 key，面板只看返回的模型列表。
                    Json probe;
                    probe["cmd"]="probe";
                    probe["base_url"]=rows[index].baseUrl;
                    if(!rows[index].keyEnv.empty()) probe["key_env"]=rows[index].keyEnv;
                    // 把助手给的三次候选尝试压成一句话，并告诉调用方是否发生过连接层失败
                    // （真机实测 s3ai 4 次里 1 次 connection reset，被"没有返回模型列表"盖住）。
                    const auto probeOnce=[&](FallbackProbe& target,bool& transportFailure) {
                        transportFailure=false;
                        const auto answer=bridge.Call(probe.dump(),30000);
                        Json data;
                        if(!answer.data.empty()) {
                            try { data=Json::parse(answer.data); } catch(const std::exception&) {}
                        }
                        int httpStatus=0;
                        if(data.contains("attempts")&&data["attempts"].is_array()) {
                            for(const auto& attempt:data["attempts"]) {
                                const auto status=attempt.value("status",0);
                                if(status==0) transportFailure=true;
                                else if(status>=400&&httpStatus==0) httpStatus=status;
                            }
                        }
                        const auto describe=[&]() -> std::string {
                            if(httpStatus>0) return "HTTP "+std::to_string(httpStatus);
                            if(transportFailure) return "\xE8\xBF\x9E\xE6\x8E\xA5\xE8\xA2\xAB\xE9\x87\x8D\xE7\xBD\xAE";
                            return {};
                        };
                        if(!answer.ok) {
                            const auto detail=describe();
                            target.error=detail.empty()
                                ?(answer.error.empty()?"\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE":answer.error)
                                :detail;
                            return false;
                        }
                        std::vector<std::string> models;
                        if(data.contains("models")&&data["models"].is_array())
                            for(const auto& item:data["models"])
                                if(item.is_string()) models.push_back(item.get<std::string>());
                        target.count=static_cast<int>(models.size());
                        target.hasModel=ModelListHasModel(rows[index].model,models);
                        // 空列表 = 连上了但这个供应商没有任何模型：按"不可用"处理，与旧语义一致。
                        target.ok=target.count>0;
                        if(!target.ok) {
                            const auto detail=describe();
                            target.error=detail.empty()
                                ?"\xE6\xB2\xA1\xE6\x9C\x89\xE6\xA8\xA1\xE5\x9E\x8B\xE5\x88\x97\xE8\xA1\xA8"
                                :detail;
                        }
                        return true;
                    };
                    bool transport=false;
                    probeOnce(entry,transport);
                    // 连接层抖动重试一次；HTTP 4xx/5xx 不重试（重试没有意义）。
                    if(!entry.ok&&transport) {
                        FallbackProbe retried;
                        bool ignored=false;
                        probeOnce(retried,ignored);
                        entry=retried;
                    }
                } else {
                    Json probe;
                    probe["cmd"]="fallback-probe";
                    probe["index"]=static_cast<int>(index);
                    const auto answer=bridge.Call(probe.dump(),30000);
                    entry.ok=answer.ok;
                    if(answer.ok) {
                        try {
                            const auto data=Json::parse(answer.data);
                            // 真助手的回复是 {"rows":[{index,count,has_model,...}]}；夹具是单行对象。
                            if(data.contains("rows")&&data["rows"].is_array()) {
                                for(const auto& row:data["rows"]) {
                                    if(row.value("index",-1)!=static_cast<int>(index)) continue;
                                    entry.count=row.value("count",0);
                                    entry.hasModel=row.value("has_model",false);
                                    entry.ok=entry.count>0;
                                    break;
                                }
                            } else {
                                entry.count=data.value("count",0);
                                entry.hasModel=data.value("has_model",false);
                            }
                        } catch(const std::exception&) {}
                    } else {
                        entry.error=answer.error.empty()?"\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE":answer.error;
                    }
                }
                pushRow(index,entry);
                probeDone_.store(static_cast<int>(index)+1);
                if(panel_) PostMessageW(panel_,kProbeProgressMessage,0,0);
            }
            probeActive_.store(-1);
            probeRunning_.store(false);
            if(panel_) PostMessageW(panel_,kProbeProgressMessage,0,0);
        }
        if(refreshAfter&&result->error.empty()&&!result->previewOnly) {
            const auto list=bridge.Call("{\"cmd\":\"list\"}",15000);
            const auto health=bridge.Call("{\"cmd\":\"health\"}",15000);
            if(stopping_.load()) { workerBusy_=false;delete result;return; }
            result->state=ParseStateSafe(list.ok?list.data:std::string("{}"),
                health.ok?health.data:std::string("{}"),std::string("{}"),log_);
            // 写操作后的重读也要给出 WSL 存活判定：只依赖 health 里的 status.wsl 时，助手没报这个
            // 字段就会变成 "unknown" —— 真机上就是"改完模型 WSL 的灯灭了"。
            result->state.wsl=list.ok?"up":(bridge.WslReachable()?"up":"down");
            result->state.loading=false;
        }
        // 先释放「有调用在飞」再投递结果：结果在 UI 线程里落地时，排队的用户动作
        // （托盘菜单点击撞上轮询刷新）才不会被当成第二次并发调用而丢掉（真机上点「重启网关」
        // 没反应就是这个静默丢包）。
        workerBusy_=false;
        if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
    });
}
void Application::StartQueuedSwitch() {
    if(queuedSwitchProvider_.empty()||queuedSwitchModel_.empty()) return;
    const auto provider=queuedSwitchProvider_,model=queuedSwitchModel_;
    queuedSwitchProvider_.clear();
    queuedSwitchModel_.clear();
    StartSwitch(provider,model);
}
void Application::StartSwitch(const std::string& providerId,const std::string& model) {
    if(hermesState_.config.parses==false) {
        ShowToast(Narrow(L"config.yaml \u89E3\u6790\u5931\u8D25\uFF0C\u5199\u64CD\u4F5C\u5DF2\u505C\u7528"),2);
        if(log_) log_->Warn("data","switch refused: config.yaml does not parse");
        return;
    }
    if(workerBusy_.exchange(true)||stopping_.load()) {
        // 审计 F-011：轮询/刷新在飞时点模型，原来直接丢弃（用户看到"点空"）。
        // 与其它写动作一致：记下目标，当前调用落地后立即执行（同一目标合并）。
        if(!stopping_.load()) {
            queuedSwitchProvider_=providerId;
            queuedSwitchModel_=model;
            if(log_) log_->Info("data","switch queued behind an in-flight call: "+providerId+" / "+model);
        }
        return;
    }
    if(worker_.joinable()) worker_.join();
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.helperPath;
    switchingKey_=providerId+"|"+model;
    switchingModel_=model;
    InvalidateAll();
    worker_=std::thread([this,distro,python,path,providerId,model] {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        Json request;
        request["cmd"]="set-main";
        request["provider"]=providerId;
        request["model"]=model;
        const auto reply=bridge.Call(request.dump(),60000);
        if(stopping_.load()) { workerBusy_=false;return; }
        auto* result=new DataResult;
        result->switched=true;
        result->switchKey=providerId+"|"+model;
        if(!reply.ok) result->switchError=reply.error.empty()
            ?"helper exit "+std::to_string(reply.exitCode):reply.error;
        if(reply.ok) {
            // A successful write is immediately re-read (PLAN §2.4) so the hero shows the new model.
            const auto list=bridge.Call("{\"cmd\":\"list\"}",15000);
            const auto health=bridge.Call("{\"cmd\":\"health\"}",15000);
            if(stopping_.load()) { workerBusy_=false;delete result;return; }
            result->state=ParseStateSafe(list.ok?list.data:std::string("{}"),
                health.ok?health.data:std::string("{}"),std::string("{}"),log_);
            // 写入后的重读同样给出 WSL 存活判定（health 里没有 status.wsl 时不能变 unknown：真机上
            // 「改完模型 WSL 的灯灭了」就是这一条）。
            result->state.wsl=list.ok?"up":(bridge.WslReachable()?"up":"down");
            result->state.loading=false;
            // The local probes are not part of this reply: the state merge keeps whatever the last
            // refresh found — 但**切换完成后**会紧接着跑一次完整刷新（含 WebUI/网关探活），
            // 旧版 afterPanelWrite 也是这么做的（refreshData({withWebui:true})）。
        }
        // 先释放「有调用在飞」再投递结果：结果在 UI 线程里落地时，排队的用户动作
        // （托盘菜单点击撞上轮询刷新）才不会被当成第二次并发调用而丢掉（真机上点「重启网关」
        // 没反应就是这个静默丢包）。
        workerBusy_=false;
        if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
    });
}
void Application::StartReadHelper(const std::string& cmd) {
    // F2/F3：只读助手（diagnose/usage）与 apply 助手共用 HelperBridge（同一条 wsl.exe + python 通路），
    // 但走独立的脚本路径，且**永不写**配置。单飞：撞上进行中的调用时给提示而不是排队（读操作
    // 用户会再点一次，排队反而让"刷新"按钮看起来卡住）。
    if(stopping_.load()) return;
    if(workerBusy_.exchange(true)) {
        // 只读模态的"刷新"：模态原地显示"正在读取…"，不要为了提示而把它关掉
        // （真机反馈：点刷新弹窗消失，读完才重新出现）。
        if(pending_==Pending::DiagnoseInfo||pending_==Pending::UsageInfo) return;
        pending_=Pending::None;
        ShowToast("\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xAF\xBB\xE5\x8F\x96\xEF\xBC\x8C\xE8\xAF\xB7"
            "\xE7\xA8\x8D\xE5\x80\x99\xE2\x80\xA6",1);   // 正在读取，请稍候…
        return;
    }
    if(worker_.joinable()) worker_.join();
    const auto token=++readToken_;
    readBusy_=true;
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.readHelperPath;
    if(log_) log_->Info("data","read "+cmd+" started");
    worker_=std::thread([this,distro,python,path,cmd,token] {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
        bridge.SetLogSink([this](const std::string& line) { if(log_) log_->Info("read",line); });
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        const auto reply=bridge.Call("{\"cmd\":\""+cmd+"\"}",60000);
        auto* result=new DataResult;
        result->readKind=cmd;
        result->readToken=token;
        if(reply.ok) result->readJson=reply.data;
        else result->error=reply.error.empty()?"helper exit "+std::to_string(reply.exitCode):reply.error;
        if(stopping_.load()) { workerBusy_=false;delete result;return; }
        workerBusy_=false;
        if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
    });
}
void Application::UpdateLatencyTimer() {
    if(!panel_) return;
    KillTimer(panel_,kLatencyTimer);
    // 默认关：只有开关打开才排计时器；间隔只允许 5/15/60（设置解析时已归一化）。
    if(stored_.latencyProbe)
        SetTimer(panel_,kLatencyTimer,
            static_cast<UINT>(stored_.latencyMinutes)*60u*1000u,nullptr);
}
void Application::StartLatencyProbe(bool manual) {
    if(!latency_) return;
    if(latencyBusy_.exchange(true)) return;
    if(latencyWorker_.joinable()) latencyWorker_.join();
    // 目标 = 当前模型 + 全部收藏；按模型去重，上限 8（文档约定，避免刷爆网关）。
    std::vector<LatencyResult> targets;
    const auto addTarget=[&](const std::string& key) {
        const auto bar=key.find('|');
        if(bar==std::string::npos||bar+1>=key.size()) return;
        const auto model=key.substr(bar+1);
        for(const auto& target:targets) if(target.model==model) return;
        if(targets.size()>=8) return;
        targets.push_back({key,model,0,false});
    };
    if(!hermesState_.model.model.empty())
        addTarget(hermesState_.model.provider+"|"+hermesState_.model.model);
    for(const auto& key:stored_.favorites) addTarget(key);
    if(targets.empty()) { latencyBusy_=false;return; }
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.readHelperPath;
    if(log_) log_->Info("data","latency probe started ("+(manual?std::string("manual"):std::string("timer"))
        +", "+std::to_string(targets.size())+" model(s))");
    latencyWorker_=std::thread([this,distro,python,path,targets] {
        std::vector<LatencyResult> results;
        for(const auto& target:targets) {
            HelperBridge bridge;
            bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
            if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
                bridge.SetWslExecutable(override);
#endif
            // 审计 F-009：模型名是外部数据，必须序列化转义（含 " 或 \ 时手拼会变成非法 JSON）。
            const auto request=Json{{"cmd","probe"},{"model",target.model}}.dump();
            const auto reply=bridge.Call(request,30000);
            int ms=0;
            bool ok=false;
            try {
                const auto data=Json::parse(reply.data);
                ms=data.value("ms",0);
                const auto status=data.value("status",0);
                ok=reply.ok&&status>=200&&status<300;
            } catch(const std::exception&) {
                ok=reply.ok;
            }
            results.push_back({target.key,target.model,ms,ok});
            if(stopping_.load()) { latencyBusy_=false;return; }
            Sleep(200);   // 串行 + 间隔，别让网关把探测当成流量尖峰
        }
        auto* heap=new std::vector<LatencyResult>(std::move(results));
        latencyBusy_=false;
        if(PostMessageW(panel_,kLatencyMessage,0,reinterpret_cast<LPARAM>(heap))==FALSE) delete heap;
    });
}
void Application::HandleLatencyResults(std::vector<LatencyResult>* results) {
    std::unique_ptr<std::vector<LatencyResult>> owned(results);
    if(!latency_||!owned) return;
    const auto now=static_cast<long long>(std::time(nullptr));
    for(const auto& result:*owned) {
        latency_->Record(result.key,result.ms,result.ok,now);
        if(log_) log_->Info("data","latency "+result.key+" "+std::to_string(result.ms)+"ms "
            +(result.ok?"ok":"failed"));
    }
    latency_->Save();
    InvalidateAll();
}
void Application::SaveCurrentProfile() {
    if(!profiles_) return;
    Profile profile;
    profile.name=hermesState_.model.model.empty()?"default":hermesState_.model.model;
    profile.mainProvider=hermesState_.model.provider;
    profile.mainModel=hermesState_.model.model;
    for(const auto& entry:hermesState_.fallbacks)
        profile.fallbacks.push_back({entry.provider,entry.model});
    profile.favorites=stored_.favorites;
    profile.updated=static_cast<long long>(std::time(nullptr));
    profiles_->Upsert(profile);
    profiles_->Save();
    profileError_.clear();
    keepModalOpen_=true;      // 保存后留在管理模态，列表立即能看到新档案
    pending_=Pending::ProfileMenu;
    ShowToast(Narrow(L"已保存配置档案「")+profile.name+Narrow(L"」"),1);
    InvalidateAll();
}
void Application::StartProfileApply(const std::string& name) {
    if(!profiles_) return;
    const auto* found=profiles_->Find(name);
    if(!found) return;
    if(profileBusy_.exchange(true)) {
        ShowToast(Narrow(L"正在执行，请稍候…"),1);
        return;
    }
    if(profileWorker_.joinable()) profileWorker_.join();
    const auto profile=*found;
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.helperPath;
    const int fallbackCount=static_cast<int>(hermesState_.fallbacks.size());
    if(log_) log_->Info("data","profile apply started: "+profile.name);
    profileWorker_=std::thread([this,distro,python,path,profile,fallbackCount] {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        auto* result=new ProfileApplyResult;
        result->name=profile.name;
        const auto call=[&](const std::string& step,const Json& args) {
            const auto reply=bridge.Call(args.dump(),60000);
            if(!reply.ok) {
                result->failedStep=step;
                result->error=reply.error.empty()?("helper exit "+std::to_string(reply.exitCode))
                    :reply.error;
                return false;
            }
            return true;
        };
        bool ok=call(Narrow(L"主模型"),Json{{"cmd","set-main"},{"provider",profile.mainProvider},
            {"model",profile.mainModel}});
        // 清空现有回退链（从后往前删，索引不会漂移），再按档案顺序追加。
        for(int index=fallbackCount-1;ok&&index>=0;--index)
            ok=call(Narrow(L"清空回退链"),Json{{"cmd","fallback-remove"},{"index",index}});
        for(const auto& entry:profile.fallbacks) {
            if(!ok) break;
            ok=call(Narrow(L"添加回退"),Json{{"cmd","fallback-add"},{"model",entry.model},
                {"provider",entry.provider}});
        }
        result->ok=ok;
        if(stopping_.load()) { profileBusy_=false;delete result;return; }
        profileBusy_=false;
        if(PostMessageW(panel_,kProfileMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
    });
}
void Application::HandleProfileResult(ProfileApplyResult* result) {
    std::unique_ptr<ProfileApplyResult> owned(result);
    if(!owned) return;
    if(!owned->ok) {
        profileError_=owned->failedStep+Narrow(L"：")+owned->error;
        if(log_) log_->Info("data","profile apply failed: "+profileError_);
        ShowToast(Narrow(L"应用失败（")+owned->failedStep+Narrow(L"）"),2);
        pending_=Pending::ProfileMenu;   // 回到管理模态，错误留在 facts 第一行
        InvalidateAll();
        return;
    }
    profileError_.clear();
    if(log_) log_->Info("data","profile applied: "+owned->name);
    ShowToast(Narrow(L"已应用配置档案「")+owned->name+Narrow(L"」"),1);
    if(stored_.autoReloadGateway) {
        pendingGatewayRestart_=false;
        StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
            Narrow(L"网关已重启，机器人已用上新配置"),true);
    } else {
        // 写操作完成但网关没重启：复用「待重启」胶囊（与换模型同一套语义）。
        pendingGatewayRestart_=true;
    }
    StartRefresh();
    InvalidateAll();
}
void Application::HandleProbeRow(ProbeRow* payload) {
    // 逐条探测的单行结果：只更新那一行，不等整条链。
    std::unique_ptr<ProbeRow> owned(payload);
    if(!owned) return;
    const auto index=static_cast<std::size_t>(std::max(0,owned->index));
    if(fbProbes_.size()<index+1) fbProbes_.resize(index+1,FallbackProbe{});
    fbProbes_[index]=owned->probe;
    InvalidateAll();
}
void Application::ApplyDataResult(DataResult* result) {
    // 助手错误从结果里取（轮询线程只写结果，不碰 UI 成员）；只有刷新结果带这个语义。
    if(result->helperErrorValid) {
        helperError_=result->helperError;
        // F6「连续刷新失败」：失败累加，成功清零。
        consecutiveRefreshFailures_=result->helperError.empty()?0:consecutiveRefreshFailures_+1;
        // 一次助手失败不代表 WebUI/网关真的掉了：保留上一帧的探活结果，避免三盏灯同时变红
        // （那会连带触发 gateway/webui 两条通知，属于误报）。
        if(!result->helperError.empty()) {
            result->state.webui=hermesState_.webui;
            result->state.gateway=hermesState_.gateway;
        }
    }
    std::unique_ptr<DataResult> owned(result);
    switchingKey_.clear();
    switchingModel_.clear();
    // 手动重启网关完成：收掉寄存器行的「重启网关」胶囊（机器人已用上新模型）。
    if(result->clearsGatewayRestart) {
        pendingGatewayRestart_=false;
        lastSyncedModel_=hermesState_.model.model;
    }
    // 排队中的用户动作现在可以起跑了（上一个调用已经收尾，workerBusy_ 已释放）：一次一个，
    // 每个动作自己的结果落地时会再取下一个。
    if(!pendingActions_.empty()) {
        const auto action=pendingActions_.front();
        pendingActions_.erase(pendingActions_.begin());
        StartHelperAction(action.verb,action.args,action.toast,action.refreshAfter,action.probeFallbacks);
    }
    // 写入预览（previewDiff）：助手对 dry_run 的回执落成确认卡，不写、不重读；确认由
    // RunPendingAction 用 dry_run:false 重发。
    if(owned->previewOnly) {
        previewFacts_=owned->previewFacts;
        previewVerb_=owned->previewVerb;
        previewArgs_=owned->previewArgs;
        previewToast_=owned->previewToast;
        previewRefreshAfter_=owned->previewRefreshAfter;
        previewProbe_=owned->previewProbe;
        pending_=Pending::PreviewWrite;
        InvalidateAll();
        StartQueuedSwitch();
        return;
    }
    // 只读助手的结果：不碰 hermesState_（它没有 state 字段），直接落成模态的事实行。
    if(!owned->readKind.empty()) {
        ShowReadResult(owned->readKind,owned->readJson,owned->error,owned->readToken);
        StartQueuedSwitch();
        return;
    }
    if(owned->write) {
        // H5 页面写：成功给 ok toast 并（按需）重读状态，失败给 err toast 并把错误留在表单里。
        if(!owned->error.empty()) {
            if(log_) log_->Info("data",owned->verb+" failed: "+owned->error);
            if(!fbFormOpen_) ShowToast(owned->error,2);
            else { fbError_=owned->error;InvalidateAll(); }
        } else if(!owned->toast.empty()) {
            // 预演被助手忽略（契约漂移）：覆盖成功 toast，明确告诉用户"已经写了"。
            ShowToast(owned->previewIgnored
                ?Narrow(L"\u8BE5\u64CD\u4F5C\u4E0D\u652F\u6301\u9884\u89C8\uFF0C\u5DF2\u76F4"
                    L"\u63A5\u5199\u5165")
                :owned->toast,owned->previewIgnored?2:1);
        }
        if(owned->probed) { fbProbes_=owned->probes;InvalidateAll(); }
        if(owned->verb=="probe") {
            // 审计 F-006：加模型页的"从供应商探测填充"要把结果写进「模型 id」多行框
            // （旧版 #am-models 的行为）；新建供应商页才是药丸列表。
            if(activePage_==2&&addSub_==1) {
                std::wstring joined;
                for(const auto& model:owned->models) {
                    if(!joined.empty()) joined+=L"\n";
                    joined+=Widen(model);
                }
                addModels_.SetText(joined);
                addChips_.clear();
                addInfo_=owned->models.empty()?std::string{}
                    :std::to_string(owned->models.size())
                        +Narrow(L" \u4E2A\u6A21\u578B\u5DF2\u586B\u5165\u4E0B\u65B9\u6587\u672C\u6846");
            } else if(activePage_==2&&addSub_==0) {
                // 审计 F-007：新建供应商页的探测结果写进「模型」多行框（可继续编辑）；
                // 药丸只是同一份数据的快捷删除视图。
                std::wstring joined;
                for(const auto& model:owned->models) {
                    if(!joined.empty()) joined+=L"\n";
                    joined+=Widen(model);
                }
                addNewModels_.SetText(joined);
                addChips_=owned->models;
            } else {
                addChips_=owned->models;
            }
            if(owned->models.empty()&&owned->error.empty())
                addInfo_="\xE8\xBF\x9E\xE4\xB8\x8A\xE4\xBA\x86\xEF\xBC\x8C\xE4\xBD\x86"
                    "\xE6\xB2\xA1\xE8\xBF\x94\xE5\x9B\x9E\xE6\xA8\xA1\xE5\x9E\x8B\xE5\x88\x97"
                    "\xE8\xA1\xA8\xEF\xBC\x88\xE5\x8F\xAF\xE4\xBB\xA5\xE6\x89\x8B\xE5\x8A\xA8"
                    "\xE5\xA1\xAB id\xEF\xBC\x89";
            InvalidateAll();
        }
        if(owned->verb=="backup-list") {
            backups_=owned->backups;
            backupBusy_=false;
            backupError_=owned->error;
            InvalidateAll();
        }
        if(!owned->state.providers.empty()||!owned->state.fallbacks.empty()) ApplyHelperState(owned->state);
        else InvalidateAll();
        StartQueuedSwitch();
        return;
    }
    if(!owned->switched) {
        ApplyHelperState(owned->state);
        StartQueuedSwitch();
        return;
    }
    if(!owned->switchError.empty()) {
        if(log_) log_->Info("data","set-main failed: "+owned->switchError);
        InvalidateAll();
        StartQueuedSwitch();
        return;
    }
    // The write's re-read does not probe ports: keep the last probe result, so a switch cannot make the
    // WebUI/gateway lamps flicker to "unreachable" on the way past.
    owned->state.webui=hermesState_.webui;
    owned->state.gateway=hermesState_.gateway;
    // A successful write is a recent (main.js addRecent): newest first, capped at 12.
    const auto& key=owned->switchKey;
    stored_.recents.erase(std::remove(stored_.recents.begin(),stored_.recents.end(),key),
        stored_.recents.end());
    stored_.recents.insert(stored_.recents.begin(),key);
    if(stored_.recents.size()>kRecentsMax) stored_.recents.resize(kRecentsMax);
    // 真机反馈：微信/QQ 机器人要等网关重启后才用上新模型（模型配置对 API 请求是热读，但机器人
    // 的回合走 agent，实测不重启不生效）。开了「自动重启网关」就接着重启；没开就不清
    // hotLoadedMtime，让「网关待重启」chip 一直亮着 —— 这就是待重启状态。
    const bool autoRestart=stored_.autoReloadGateway;
    // 只有真的自动重启了才记 hotLoadedMtime（压制 chip）；没重启就让「网关待重启」chip 亮着。
    if(autoRestart&&owned->state.config.mtime>0) stored_.hotLoadedMtime=owned->state.config.mtime;
    PersistSettings(true);
    if(log_) log_->Info("data","switched to "+key);
    // 网关最后一次同步的模型：首次拿到状态时登记（用**覆盖前**的 hermesState_，此后换模型/重启都会更新它）。
    if(lastSyncedModel_.empty()&&!hermesState_.model.model.empty())
        lastSyncedModel_=hermesState_.model.model;
    if(!owned->state.providers.empty()||!owned->state.model.provider.empty()) ApplyHelperState(owned->state);
    else InvalidateAll();
    // 切换完成后马上做一次**完整刷新**（list+health+WebUI/网关探活）：这样三盏灯立刻反映真实状态，
    // 而不是等下一个轮询周期（真机反馈"改完模型灯灭了，不能自动刷新探活吗"）。
    StartRefresh();
    {
        const auto split=key.find('|');
        const auto model=split==std::string::npos?key:key.substr(split+1);
        // 文案走 Narrow + Unicode 转义（源码纯 ASCII）：这一段被工具链二次编码过一次，门禁抓到后
        // 统一改成这个写法，中文不再有被改写的通道。
        if(autoRestart) {
            lastSyncedModel_=model;   // 网关重启后已同步到新模型
            // 排在刷新后面（StartHelperAction 自己会排队）：网关重启完再刷一次状态。
            StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
                Narrow(L"\u7F51\u5173\u5DF2\u91CD\u542F\uFF0C\u673A\u5668\u4EBA\u5DF2\u7528"
                    L"\u4E0A\u65B0\u6A21\u578B"),true);
            ShowToast(Narrow(L"\u5DF2\u5207\u6362\u4E3A ")+model
                +Narrow(L"\uFF1B\u7F51\u5173\u91CD\u542F\u4E2D\uFF0C\u673A\u5668\u4EBA\u7A0D\u540E"
                    L"\u751F\u6548"),1);
        } else {
            // 待重启状态：寄存器行（网关灯后）显示「重启网关」胶囊，点了才重启。
            // 目标模型与「网关最后一次同步的模型」一致（换回去）→ 网关无需重启，胶囊收起。
            if(model==lastSyncedModel_) {
                if(log_) log_->Info("data","target matches the last synced model; gateway restart no longer needed");
                pendingGatewayRestart_=false;
            } else {
                pendingGatewayRestart_=true;
            }
            ShowToast(Narrow(L"\u5DF2\u5207\u6362\u4E3A ")+model
                +Narrow(L"\uFF1B\u5FAE\u4FE1/QQ \u7B49\u673A\u5668\u4EBA\u9700\u91CD\u542F\u7F51\u5173"
                    L"\u540E\u751F\u6548"),1);
        }
    }
    StartQueuedSwitch();
}
void Application::UpdatePollTimer() {
    // The cadence is settings.pollSeconds (the tray menu already exposes it: 30/60/120/300), unless the
    // debug control plane overrode it for a measurement run.
    // DESIGN v1.7 可见性降频：展开 = 设置的秒数；收起（贴条）= 60s；隐藏 = 停轮询。
    // 任一可见性变化由 ApplyState 调这里；重新可见的那次刷新由 ApplyState 直接触发。
    if(!panel_) return;
    KillTimer(panel_,kPollTimer);
    if(state_==State::Hidden) return;
    UINT seconds=0;
    if(pollOverrideSeconds_>0) seconds=static_cast<UINT>(pollOverrideSeconds_);
    else seconds=static_cast<UINT>(std::clamp(stored_.pollSeconds,5,3600));
    if(state_==State::Collapsed) seconds=std::max<UINT>(seconds,60);
    SetTimer(panel_,kPollTimer,seconds*1000,nullptr);
}
bool Application::GatewayStale() const {
    const auto loaded=hermesState_.config.mtime;
    return loaded>0&&hermesState_.gateway.sinceUnix>0&&loaded>hermesState_.gateway.sinceUnix+2
        &&loaded>stored_.hotLoadedMtime;
}
void Application::RefreshData() {
    // The public entry stays synchronous in name only: the WSL round trip happens on a worker thread,
    // so a hover or a keystroke never waits for `wsl.exe` (PLAN §2.4).
    StartRefresh();
}
void Application::StartWsl() {
    // 离线卡的主按钮（旧 startWsl）：真的去敲一次 WSL，成功就立刻刷新。
    if(log_) log_->Info("data","start wsl requested");
    HelperBridge bridge;
    bridge.Configure(Widen(stored_.distro),Widen(stored_.pythonExe),Widen(stored_.helperPath));
#if HERMES_CONTROL_PLANE
    if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
        bridge.SetWslExecutable(override);
#endif
    if(bridge.WslReachable()) {
        ShowToast("WSL \xE5\xB7\xB2\xE5\xB0\xB1\xE7\xBB\xAA",1);
        RefreshData();
    } else {
        ShowToast("\xE5\x90\xAF\xE5\x8A\xA8 WSL \xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A\xE8\xAF\xB7\xE5\x85\x88"
            "\xE5\x9C\xA8\xE7\xBB\x88\xE7\xAB\xAF\xE6\x89\x93\xE5\xBC\x80\xE4\xB8\x80\xE4\xB8\xAA"
            "\xE5\x88\x86\xE5\x8F\x91\xE7\x89\x88",2);
    }
}
}
