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
    // 真机 2026-10-21「切换模型/重启网关时偶发卡一下」：状态落地的整条路径（托盘图标、
    // 气泡通知、重绘）都在 UI 线程上执行，任何一步撞上资源管理器/合成器就会顶住消息循环。
    // 与 busy tick gap / busy paint / tray update 三条日志一起，把"哪里卡"变成可判定的证据。
    LARGE_INTEGER applyFreq{},applyStart{};
    QueryPerformanceFrequency(&applyFreq);QueryPerformanceCounter(&applyStart);
    // 托盘图标/悬浮提示跟着状态走（旧 updateTrayStatus 在每次 renderStatus 后调用）。
    UpdateTrayStatus();
    CheckNotificationEdges();
    // 挂件已经完全静止：信标样式的异常呼吸随样式删除一起移除，这里不再需要挂件定时器。
    // v1.28（性能）：状态**没变**就不重绘。轮询每 20s 都会走到这里，即使内容一模一样也会整窗
    // 重画一次（实测一帧 7-16ms，且会和输入/悬停抢消息循环）。界面里没有随时间变化的元素
    // （同步时间戳来自 config mtime，不来自本机时钟），所以跳过是安全的；首帧仍必须画。
    if(signature!=stateSignature_||!statePainted_) {
        statePainted_=true;
        InvalidateAll();
    }
    LARGE_INTEGER applyDone{};
    QueryPerformanceCounter(&applyDone);
    if(applyFreq.QuadPart>0&&log_) {
        const auto ms=static_cast<double>(applyDone.QuadPart-applyStart.QuadPart)*1000.0
            /static_cast<double>(applyFreq.QuadPart);
        if(ms>=80.0) log_->Info("ui","state apply "+std::to_string(static_cast<int>(ms))+"ms");
    }
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
void Application::StartRefresh(bool quiet) {
    refreshQuiet_=quiet;
    // Single-flight, like the old panel's tickRunning guard: a poll landing on top of a manual refresh
    // is dropped instead of queueing a second WSL round trip.
    if(workerBusy_.exchange(true)||stopping_.load()) return;
    // 真的起跑了才算"这轮忙是同步章自己的手动刷新"：被丢弃的刷新不能抢所有权。
    refreshOwnsBusy_=!quiet;
    // The previous job has finished (it clears the busy flag as it leaves), so its thread object can be
    // reaped here: assigning to a still-joinable std::thread calls std::terminate.
    if(worker_.joinable()) worker_.join();
    // 刷新进行中：同步章的图标绕中心旋转（真机反馈：不要「读取 WSL 配置…」的文字，改成图标转）。
    // v1.24：忙碌条在独立子窗口里画（单帧 0.05ms），16ms 一拍 = 60fps。
    if(panel_) { spinGlyph_.Restart();lastSpinTick_=0;SetTimer(panel_,kSpinTimer,kSpinIntervalMs,nullptr); }
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
    // UNC 探测与读取全部放进 worker（//wsl$ 走 9P：网关重启时单次调用可能阻塞数百毫秒到数秒）。
    // 旧实现在 UI 线程上做 FindFirstFileW/TakeUncSnapshot，忙碌条与转圈动画会跟着卡住
    // （真机 2026-10-21 报「忙碌条偶尔卡住不动一会才开始动」）。
    const bool spawned=SpawnWorker(worker_,&workerBusy_,nullptr,nullptr,[this,distro,python,path,webuiPort,gatewayPort,first] {
        try {
        if(!first) {
            // HERMES_DISABLE_UNC=1：门禁夹具强制走假助手。harness 的硬约束是夹具隔离——而真机
            // 发行版里恰好有一个 .hermes，不禁止的话测试实例会经 UNC 读到真机配置，污染夹具断言。
            // 带控制面的构建（Debug/门禁）默认禁用：测试实例可能经 geometry-probe 转发启动，
            // 环境变量不一定穿透；UNC 是真机（Release）专属优化路径。
            std::string home;
#if !HERMES_CONTROL_PLANE
            wchar_t uncFlag[4]{};
            const auto uncDisabled=GetEnvironmentVariableW(L"HERMES_DISABLE_UNC",uncFlag,4)!=0
                &&uncFlag[0]==L'1';
            // 真机探测：helperPath 推不出 home（脚本在 /mnt/* 下，常态）时枚举发行版 /home/ 下
            // 唯一用户目录拼 .hermes。只在产品（Release，无控制面）构建里做——门禁夹具绝不摸真配置。
            home=uncDisabled?std::string{}:WslHomeFromHelperPath(path);
            if(!uncDisabled&&home.empty()) {
                WIN32_FIND_DATAW data{};
                std::wstring homeGlob=L"//wsl$/";
                for(const char ch:distro) homeGlob.push_back(static_cast<wchar_t>(ch));
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
            const auto snapshot=TakeUncSnapshot(distro,home,std::to_string(gatewayPort));
            if(snapshot.available&&snapshot.configMtime==uncCacheMtime_&&snapshot.configSize==uncCacheSize_
                &&!uncCacheListJson_.empty()) {
                if(log_) log_->Info("data","refresh from UNC cache (config unchanged)");
                auto* result=new DataResult;
                result->helperErrorValid=true;
                result->state=ParseStateSafe(uncCacheListJson_,uncCacheHealthJson_,std::string("{}"),log_);
                result->state.loading=false;
                result->state.wsl="up";   // 同快照路径：UNC 可达即 WSL 在线（health 无 status.wsl）
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
                auto* result=new DataResult;
                result->helperErrorValid=true;
                result->state=ParseStateSafe(uncCacheListJson_,uncCacheHealthJson_,std::string("{}"),log_);
                result->state.loading=false;
                // 与 wsl.exe 路径同款兜底：UNC 读到 config 本身就是 WSL 活着的信号——
                // health 里没有 status.wsl 字段，不补的话灯永远停在 unknown（灰"探测中"）。
                result->state.wsl="up";
                FillReachability(result->state,webuiPort,gatewayPort);
                workerBusy_=false;
                if(PostMessageW(panel_,kDataMessage,0,reinterpret_cast<LPARAM>(result))==FALSE) delete result;
                return;
            }
            if(log_) log_->Info("data","UNC unavailable ("+snapshot.error+"), falling back to wsl.exe");
        }
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("refresh: ")+error.what());
            workerBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","refresh: unknown exception");
            workerBusy_=false;
        }
    });
    if(!spawned) {
        // 起线程失败（SpawnWorker 已回滚 workerBusy_）：定时器还等着，下一拍会把它当成
        // "刷新成功"画一个对勾——那是假状态，这里直接停掉旋转与对勾。
        if(panel_) {
            KillTimer(panel_,kSpinTimer);
            syncCheckShown_=false;
            InvalidateRect(panel_,nullptr,FALSE);
        }
    }
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
    // 别的动作起跑 = 这轮忙不属于同步章（它自己的图标别点亮、结束也别打勾）。
    refreshOwnsBusy_=false;
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
    const int generation=probeFallbacks?++probeGeneration_:probeGeneration_;
    if(!SpawnWorker(worker_,&workerBusy_,nullptr,nullptr,[this,distro,python,path,verb,args,successToast,refreshAfter,probeFallbacks,wantsPreview,generation] {
        try {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        auto* result=new DataResult;
        result->write=true;
        result->probeGeneration=generation;
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
                auto* payload=new ProbeRow{static_cast<int>(index),generation,entry};
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("action: ")+error.what());
            workerBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","action: unknown exception");
            workerBusy_=false;
        }
    })) return;
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
    if(!SpawnWorker(worker_,&workerBusy_,nullptr,nullptr,[this,distro,python,path,providerId,model] {
        try {
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("switch: ")+error.what());
            workerBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","switch: unknown exception");
            workerBusy_=false;
        }
    })) return;
}
void Application::StartReadHelper(const std::string& cmd) {
    // F2/F3：只读助手（diagnose/usage）与 apply 助手共用 HelperBridge（同一条 wsl.exe + python 通路），
    // 但走独立的脚本路径，且**永不写**配置。单飞：撞上进行中的调用时给提示而不是排队（读操作
    // 用户会再点一次，排队反而让"刷新"按钮看起来卡住）。
    if(stopping_.load()) return;
    if(workerBusy_.exchange(true)) {
        // 只读模态的"刷新"：模态原地显示"正在读取…"，不要为了提示而把它关掉
        // （真机反馈：点刷新弹窗消失，读完才重新出现）。
        if(pending_==Pending::DiagnoseInfo||pending_==Pending::UsageInfo) {
            // 后台刷新正占着 worker：**记下来**等它结束补跑。以前这里直接 return，模态就
            // 永远停在「正在读取…」（真机点「查看」撞上轮询时的现象）。
            pendingReadCmd_=cmd;
            return;
        }
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
    if(!SpawnWorker(worker_,&workerBusy_,nullptr,&readBusy_,[this,distro,python,path,cmd,token] {
        try {
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("read: ")+error.what());
            workerBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","read: unknown exception");
            workerBusy_=false;
        }
    })) return;
}
void Application::UpdateLatencyTimer() {
    if(!panel_) return;
    KillTimer(panel_,kLatencyTimer);
    // 默认关：只有开关打开才排计时器；间隔只允许 5/15/60（设置解析时已归一化）。
    if(stored_.latencyProbe)
        SetTimer(panel_,kLatencyTimer,
            static_cast<UINT>(stored_.latencyMinutes)*60u*1000u,nullptr);
}
std::vector<std::string> Application::FallbackModelOptions() const {
    // v1.16：回退链「追加一条」的模型候选。顺序就是可用性顺序：
    //   ① 一键探测缓存里该供应商的模型（最新、最全，来自 /models 的只读 GET）；
    //   ② 供应商条目自带的 models（config.yaml 里声明过的列表）；
    //   ③ 都没有 → 空（点击 ▾ 时给一句"先探测"的 toast，不画空列表）。
    // v1.25：只有「选中的供应商」一种来源（手填模式已删）。
    std::string name;
    if(fbSource_>=0&&fbSource_<static_cast<int>(hermesState_.providers.size()))
        name=hermesState_.providers[static_cast<std::size_t>(fbSource_)].name;
    if(name.empty()) return {};
    {
        std::lock_guard<std::mutex> lock(probedMutex_);
        const auto found=probedModels_.find(name);
        if(found!=probedModels_.end()&&!found->second.empty()) return found->second;
    }
    for(const auto& provider:hermesState_.providers)
        if(provider.name==name&&!provider.models.empty()) return provider.models;
    return {};
}
void Application::HandleSyncAllDone() {
    if(log_) log_->Info("data","sync-all done changed="+std::to_string(syncAllChanged_.load())
        +" total="+std::to_string(syncAllTotal_.load()));
    // 一键同步写的是 config.yaml：落地后必须重新读取，并按 v1.17 的规则交代网关重启。
    StartRefresh();
    const int changed=syncAllChanged_.load();
    const int total=syncAllTotal_.load();
    if(changed>0) {
        if(stored_.autoReloadGateway) {
            pendingGatewayRestart_=false;
            StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
                Narrow(L"\u7F51\u5173\u5DF2\u91CD\u542F\uFF0C\u673A\u5668\u4EBA\u5DF2\u7528\u4E0A\u65B0\u914D\u7F6E"),true);
            ShowToast(std::to_string(changed)+Narrow(L" \u4E2A\u4F9B\u5E94\u5546\u5DF2\u540C\u6B65\uFF1B\u7F51\u5173\u91CD\u542F\u4E2D"),1);
        } else {
            pendingGatewayRestart_=true;
            ShowToast(std::to_string(changed)+Narrow(L" \u4E2A\u4F9B\u5E94\u5546\u5DF2\u540C\u6B65\uFF1B\u9700\u91CD\u542F\u7F51\u5173\u540E\u673A\u5668\u4EBA\u624D\u7528\u4E0A"),1);
        }
    } else {
        ShowToast(Narrow(L"\u6A21\u578B\u5217\u8868\u5DF2\u662F\u6700\u65B0\uFF08")
            +std::to_string(std::max(total,0))+Narrow(L" \u4E2A\u4F9B\u5E94\u5546\uFF09"),1);
    }
    InvalidateAll();
}
void Application::SyncAllModels() {
    // v1.25（用户要求）：模型页的「刷新同步」——对每个供应商只读探测，再把它配置里的
    // models 列表同步成探测结果（新增的写入、已下线的删除，写前备份）。整批串行，一次一条。
    if(syncAllBusy_.exchange(true)) return;
    if(syncAllWorker_.joinable()) syncAllWorker_.join();
    struct Target { std::string name,providerId,baseUrl,keyEnv;std::vector<std::string> models; };
    std::vector<Target> targets;
    for(const auto& provider:hermesState_.providers) {
        if(provider.baseUrl.empty()||provider.providerId.empty()) continue;
        targets.push_back({provider.name,provider.providerId,provider.baseUrl,provider.keyEnv,
            provider.models});
    }
    syncAllTotal_=static_cast<int>(targets.size());
    syncAllDone_=0;
    syncAllChanged_=0;
    if(targets.empty()) {
        syncAllBusy_=false;
        ShowToast(Narrow(L"没有可同步的供应商（都缺 base_url）"),2);
        return;
    }
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.readHelperPath;
    if(log_) log_->Info("data","sync-all started ("+std::to_string(targets.size())+" providers)");
    if(!SpawnWorker(syncAllWorker_,&syncAllBusy_,nullptr,nullptr,
        [this,distro,python,path,targets] {
        try {
        HelperBridge bridge;
        bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
        if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
            bridge.SetWslExecutable(override);
#endif
        int changed=0;
        for(const auto& target:targets) {
            if(stopping_.load()) break;
            Json probe{{"cmd","probe"},{"base_url",target.baseUrl},{"timeout_s",6}};
            if(!target.keyEnv.empty()) probe["key_env"]=target.keyEnv;
            const auto reply=bridge.Call(probe.dump(),20000);
            std::vector<std::string> probed;
            if(reply.ok) {
                try {
                    const auto data=Json::parse(reply.data);
                    if(data.contains("models")&&data["models"].is_array())
                        for(const auto& item:data["models"])
                            if(item.is_string()) probed.push_back(item.get<std::string>());
                } catch(const std::exception&) {}
            }
            if(!probed.empty()) {
                std::lock_guard<std::mutex> lock(probedMutex_);
                probedModels_[target.name]=probed;
            }
            bool differs=probed.size()!=target.models.size();
            if(!differs) {
                for(const auto& model:probed)
                    if(std::find(target.models.begin(),target.models.end(),model)==target.models.end()) {
                        differs=true;break;
                    }
            }
            bool wroteThisOne=false;
            if(!probed.empty()&&differs) {
                Json sync{{"cmd","add-models"},{"provider",target.providerId},
                    {"models",probed},{"prune",true}};
                const auto applied=bridge.Call(sync.dump(),20000);
                const bool ok=applied.ok;
                if(ok) ++changed;
                wroteThisOne=ok;
                if(log_) log_->Info("data","sync-all: "+target.name+" -> "
                    +std::to_string(probed.size())+" model(s) "+(ok?"synced":"failed"));
            } else if(log_) {
                log_->Info("data","sync-all: "+target.name+" unchanged");
            }
            syncAllDone_=syncAllDone_.load()+1;
            syncAllChanged_=changed;
            // 每完成一个就通知 UI 重绘：按钮上显示「同步 n/N」（否则整批跑完才有反馈，
            // 用户看到的就是"一直在加载"）。wparam=0 = 进度帧，1 = 结算帧（HandleSyncAllDone）。
            if(panel_) PostMessageW(panel_,kSyncAllMessage,0,0);
            // 间隔只为"别让网关把整批探测当成流量尖峰"：只有**这一条真的写了配置**才需要等，
            // 全部没变化时白等 120ms×N（17 个供应商 = 2 秒纯等待，正是"刷新很慢"的来源）。
            if(wroteThisOne) Sleep(120);
        }
        syncAllBusy_=false;
        if(panel_) PostMessageW(panel_,kSyncAllMessage,1,0);
        } catch(const std::exception& error) {
            if(log_) log_->Warn("data","sync-all crashed: "+std::string(error.what()));
            syncAllBusy_=false;
            if(panel_) PostMessageW(panel_,kSyncAllMessage,1,0);
        }
    })) {
        syncAllBusy_=false;
        ShowToast(Narrow(L"工作线程无法启动，稍后自动重试"),2);
    }
}
void Application::ProbeAllModels() {
    // 一键探测：把每个供应商（按 base_url 去重，最多 24 条）的模型列表**只读**拉一遍。
    // 只发 GET /models（助手 probe 的全部动作），不写 config.yaml、不碰 .env。
    if(allProbeBusy_.exchange(true)) return;
    if(allProbeWorker_.joinable()) allProbeWorker_.join();
    struct Target { std::string name,baseUrl,keyEnv; };
    std::vector<Target> targets;
    for(const auto& provider:hermesState_.providers) {
        if(provider.baseUrl.empty()) continue;
        bool seen=false;
        for(const auto& existing:targets) if(existing.baseUrl==provider.baseUrl) { seen=true;break; }
        if(seen) continue;
        if(targets.size()>=24) break;
        targets.push_back({provider.name,provider.baseUrl,provider.keyEnv});
    }
    allProbeTotal_=static_cast<int>(targets.size());
    allProbeDone_=0;
    allProbeOk_=0;
    if(targets.empty()) {
        allProbeBusy_=false;
        ShowToast(Narrow(L"\u6CA1\u6709\u53EF\u63A2\u6D4B\u7684\u4F9B\u5E94\u5546\uFF08\u90FD\u7F3A base_url\uFF09"),2);
        return;
    }
    const auto distro=stored_.distro,python=stored_.pythonExe,path=stored_.readHelperPath;
    if(log_) log_->Info("data","probe-all started ("+std::to_string(targets.size())+" providers)");
    if(!SpawnWorker(allProbeWorker_,&allProbeBusy_,nullptr,nullptr,
        [this,distro,python,path,targets] {
        try {
        int ok=0;
        for(const auto& target:targets) {
            if(stopping_.load()) break;
            HelperBridge bridge;
            bridge.Configure(Widen(distro),Widen(python),Widen(path));
#if HERMES_CONTROL_PLANE
            if(const auto override=EnvironmentValue(L"HERMES_WSL_EXE");!override.empty())
                bridge.SetWslExecutable(override);
#endif
            Json args{{"cmd","probe"},{"base_url",target.baseUrl},{"timeout_s",6}};
            if(!target.keyEnv.empty()) args["key_env"]=target.keyEnv;
            const auto reply=bridge.Call(args.dump(),20000);
            std::vector<std::string> models;
            if(reply.ok) {
                try {
                    const auto data=Json::parse(reply.data);
                    if(data.contains("models")&&data["models"].is_array())
                        for(const auto& item:data["models"])
                            if(item.is_string()) models.push_back(item.get<std::string>());
                } catch(const std::exception&) {}
            }
            {
                std::lock_guard<std::mutex> lock(probedMutex_);
                probedModels_[target.name]=models;
            }
            if(!models.empty()) ++ok;
            allProbeDone_=allProbeDone_.load()+1;
            allProbeOk_=ok;
            if(log_) log_->Info("data","probe-all: "+target.name+" -> "+std::to_string(models.size())+" model(s)");
            if(panel_) PostMessageW(panel_,kProbeAllMessage,0,0);
            Sleep(150);      // 串行 + 间隔：不让网关把探测当成流量尖峰
        }
        allProbeBusy_=false;
        if(panel_) PostMessageW(panel_,kProbeAllMessage,1,0);
        } catch(const std::exception& error) {
            allProbeBusy_=false;
            if(log_) log_->Error("data",std::string("probe-all failed: ")+error.what());
        }
    })) return;
}
void Application::ApplyProbeAll() { InvalidateAll(); }
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
    if(!SpawnWorker(latencyWorker_,&latencyBusy_,nullptr,nullptr,[this,distro,python,path,targets] {
        try {
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("latency: ")+error.what());
            latencyBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","latency: unknown exception");
            latencyBusy_=false;
        }
    })) return;
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
    if(!SpawnWorker(profileWorker_,&profileBusy_,nullptr,nullptr,[this,distro,python,path,profile,fallbackCount] {
        try {
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
        } catch(const std::exception& error) {
            // 任何异常都必须在这里落地：逃出线程会走 std::terminate → __fastfail，
            // 绕过 SetUnhandledExceptionFilter，表现为"无日志、无探针、无 WER"的闪退。
            if(log_) log_->Error("worker",std::string("profile: ")+error.what());
            profileBusy_=false;
        } catch(...) {
            if(log_) log_->Error("worker","profile: unknown exception");
            profileBusy_=false;
        }
    })) return;
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
    // 迟到的结果（属于上一轮探测）直接丢掉：否则它会把新一轮的状态覆盖回去
    // （实测：probe_missing 切琥珀后，上一轮的绿色结果晚到，界面自己变回绿色）。
    if(owned->generation!=probeGeneration_) return;
    const auto index=static_cast<std::size_t>(std::max(0,owned->index));
    if(fbProbes_.size()<index+1) fbProbes_.resize(index+1,FallbackProbe{});
    fbProbes_[index]=owned->probe;
    InvalidateAll();
}
void Application::ApplyDataResult(DataResult* result) {
    // 补跑被后台刷新挡下的只读请求（诊断/用量）：worker 退出时已释放 workerBusy_，
    // 此时若还停在对应的模态上就立刻发起。用户已经关掉模态的话就丢弃。
    if(!pendingReadCmd_.empty()&&!workerBusy_.load()) {
        const auto cmd=pendingReadCmd_;
        pendingReadCmd_.clear();
        const bool stillWanted=(cmd=="diagnose"&&pending_==Pending::DiagnoseInfo)
            ||(cmd=="usage"&&pending_==Pending::UsageInfo);
        if(stillWanted) StartReadHelper(cmd);
    }
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
        } else if(!owned->previewOnly) {
            // DESIGN v1.17：**任何**改 config.yaml/.env 的动作落盘后都要交代网关是否需要重启。
            // 热读只覆盖 API 请求；机器人的回合走 agent，实测不重启不生效——所以不能再笼统说
            // "网关热读生效"。分两路：设了自动重启就立刻重启，否则点亮「重启网关」胶囊并说清楚。
            std::string toast=owned->previewIgnored?Narrow(L"该操作不支持预览，已直接写入"):owned->toast;
            if(IsConfigWrite(owned->verb)) {
                if(stored_.autoReloadGateway) {
                    pendingGatewayRestart_=false;
                    toast+=Narrow(L"；网关重启中，机器人稍后生效");
                    StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
                        Narrow(L"网关已重启，机器人已用上新配置"),true);
                } else {
                    pendingGatewayRestart_=true;
                    toast+=Narrow(L"；需重启网关后机器人才用上（点网关灯后的「重启网关」）");
                }
            }
            if(!toast.empty()) ShowToast(toast,owned->previewIgnored?2:1);
        }
        // 整轮探测结果也带代次：旧一轮的最终结果晚到时会整块覆盖新一轮的单行状态
        // （实测：probe_missing 切琥珀后，旧轮结果把行刷回绿色）。
        if(owned->probed&&owned->probeGeneration==probeGeneration_) {
            fbProbes_=owned->probes;
            InvalidateAll();
        }
        if(owned->verb=="probe") {
            // v1.18（用户要求）：模型页供应商 ⋯ →「扫描并追加」。探测结果不再需要用户去「添加」
            // 页手动保存：这里直接把**配置里没有的**模型用 add-models 写回该供应商，刷新后
            // 模型列表就是最新的。没有新增就只提示"已是最新"，绝不空写。
            if(!probeAppendProvider_.empty()) {
                const auto name=probeAppendProvider_;
                probeAppendProvider_.clear();
                std::vector<std::string> known;
                std::string providerId;
                for(const auto& provider:hermesState_.providers) {
                    if(provider.name!=name) continue;
                    known=provider.models;
                    providerId=provider.providerId;
                }
                // 只有探测**成功且有结果**才更新缓存：失败时清空缓存会让回退链的模型选择器
                // 突然退回到"只有配置里的几个"（那个下拉的数据源就是这份缓存）。
                if(owned->error.empty()&&!owned->models.empty()) {
                    std::lock_guard<std::mutex> lock(probedMutex_);
                    probedModels_[name]=owned->models;
                }
                std::vector<std::string> delta, stale;
                for(const auto& model:owned->models)
                    if(std::find(known.begin(),known.end(),model)==known.end()) delta.push_back(model);
                // 配置里有、探测结果里没有的 = 供应商已下线的模型。v1.19 起同步会把它们删掉
                // （助手 add-models 的 prune 模式），不再只是"追加后假装已是最新"。
                for(const auto& model:known)
                    if(std::find(owned->models.begin(),owned->models.end(),model)==owned->models.end())
                        stale.push_back(model);
                if(!owned->error.empty()) {
                    // 探测失败：错误已由上面的 toast 给出，这里不再重复。
                } else if(owned->models.empty()) {
                    // 探测成功但没有任何模型：绝不能 prune（那会把配置里的列表清空）。
                    ShowToast(Narrow(L"\u300C")+name+Narrow(L"\u300D\u63A2\u6D4B\u6CA1\u6709"
                        L"\u8FD4\u56DE\u6A21\u578B\uFF0C\u672A\u6539\u52A8\u914D\u7F6E"),2);
                } else if((!delta.empty()||!stale.empty())&&!providerId.empty()) {
                    // v1.19（用户要求）：同步写回——把该供应商的 models 列表替换成探测结果
                    // （助手 add-models 的 prune 模式；只读探测 + 写前自动备份，默认追加语义未变）。
                    Json args{{"cmd","add-models"},{"provider",providerId},
                        {"models",owned->models},{"prune",true}};
                    std::string message=Narrow(L"\u300C")+name+Narrow(L"\u300D\u5DF2\u540C\u6B65\uFF1A")
                        +std::to_string(owned->models.size())+Narrow(L" \u4E2A\u6A21\u578B");
                    if(!delta.empty())
                        message+=Narrow(L"\uFF08\u65B0\u589E ")+std::to_string(delta.size())
                            +Narrow(L"\uFF09");
                    if(!stale.empty())
                        message+=Narrow(L"\uFF08\u5220\u9664 ")+std::to_string(stale.size())
                            +Narrow(L"\uFF09");
                    StartHelperAction("add-models",args,message);
                } else {
                    ShowToast(Narrow(L"\u300C")+name+Narrow(L"\u300D\u5DF2\u662F\u6700\u65B0\uFF08")
                        +std::to_string(owned->models.size())+Narrow(L" \u4E2A\u6A21\u578B\uFF09"),1);
                }
                InvalidateAll();
            } else {
                // 新建供应商页的探测：结果写进「模型」多行框（可继续编辑），药丸是同一份数据的
                // 快捷删除视图。v1.18 起没有「加模型」子页，这条分支只服务新建表单。
                if(!owned->models.empty()) {
                    // v1.21：探测结果直接成为**列表**（全部模型、默认全选），不再塞进文本框。
                    addChips_=owned->models;
                    addChipSelected_.assign(addChips_.size(),true);
                    addModelScroll_=0;
                    addInfo_=std::to_string(owned->models.size())
                        +Narrow(L" \u4E2A\u6A21\u578B\u5DF2\u5217\u51FA\uFF08\u9ED8\u8BA4\u5168\u9009\uFF09");
                }
                if(owned->models.empty()&&owned->error.empty())
                    addInfo_="\xE8\xBF\x9E\xE4\xB8\x8A\xE4\xBA\x86\xEF\xBC\x8C\xE4\xBD\x86"
                        "\xE6\xB2\xA1\xE8\xBF\x94\xE5\x9B\x9E\xE6\xA8\xA1\xE5\x9E\x8B\xE5\x88\x97"
                        "\xE8\xA1\xA8\xEF\xBC\x88\xE5\x8F\xAF\xE4\xBB\xA5\xE6\x89\x8B\xE5\x8A\xA8"
                        "\xE5\xA1\xAB id\xEF\xBC\x89";
                InvalidateAll();
            }
        }
        if(owned->verb=="backup-list") {
            backups_=owned->backups;
            backupBusy_=false;
            backupError_=owned->error;
            InvalidateAll();
        }
        // 回滚会「恢复前再把当前内容备份一份」：列表里会多出一条，必须重读，否则界面上看不到
        // 那份刚生成的备份（真机：回滚成功、toast 正常，但列表还是旧的 3 行）。
        if(owned->verb=="restore"&&owned->error.empty()) LoadBackups();
        // 说明：新建供应商保存后**有意保留**表单与模型列表（h-add 门禁的契约：可以在同一页
        // 连续保存第二个供应商；upsert 本身是幂等写入，重复保存不会产生重复条目）。
        // 旧 Electron 版会清空并跳回模型页，这里按本移植版的门禁用例保持不变——不是缺陷。
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
