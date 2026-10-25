#include "app/application.h"
#include <psapi.h>
#include <tlhelp32.h>
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
#include "render/widgets.h"
#include <windowsx.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hermes {
namespace {
// 控件名表：controls/audit 两个命令共用（重复控件的名字只到 id，index 单独带）。
std::string ControlName(ControlId id) {
switch(id) {
            case ControlId::Snap: return std::string("snap");
            case ControlId::Pin: return std::string("pin");
            case ControlId::Minimize: return std::string("minimize");
            case ControlId::Close: return std::string("close");
            case ControlId::TabModels: return std::string("tabModels");
            case ControlId::TabFallback: return std::string("tabFallback");
            case ControlId::TabAdd: return std::string("tabAdd");
            case ControlId::TabBackup: return std::string("tabBackup");
            case ControlId::TabSettings: return std::string("tabSettings");
            case ControlId::Refresh: return std::string("refresh");
            case ControlId::Chip: return std::string("chip");
            case ControlId::ChipWarn: return std::string("chipWarn");
            case ControlId::ChipHelper: return std::string("chipHelper");
            case ControlId::RegisterRestartGateway: return std::string("registerRestartGateway");
            case ControlId::StatusPill: return std::string("statusPill");
            case ControlId::LampWsl: return std::string("lampWsl");
            case ControlId::LampWebui: return std::string("lampWebui");
            case ControlId::LampGateway: return std::string("lampGateway");
            case ControlId::KeyReveal: return std::string("keyReveal");
            case ControlId::SearchBox: return std::string("searchBox");
            case ControlId::SearchClear: return std::string("searchClear");
            case ControlId::SyncModels: return std::string("syncModels");
            case ControlId::FilterAll: return std::string("filterAll");
            case ControlId::FilterFav: return std::string("filterFav");
            case ControlId::FilterRecent: return std::string("filterRecent");
            case ControlId::FbProbe: return std::string("fbProbe");
            case ControlId::FbUp: return std::string("fbUp");
            case ControlId::FbDown: return std::string("fbDown");
            case ControlId::FbRemove: return std::string("fbRemove");
            case ControlId::FbProbeDot: return std::string("fbProbeDot");
            case ControlId::FbFoldToggle: return std::string("fbFoldToggle");
            case ControlId::FbFoldClose: return std::string("fbFoldClose");
            case ControlId::FbSubmit: return std::string("fbSubmit");
            case ControlId::FbSelect: return std::string("fbSelect");
            case ControlId::FbModelSelect: return std::string("fbModelSelect");
            case ControlId::ModalConfirm: return std::string("modalConfirm");
            case ControlId::ModalCancel: return std::string("modalCancel");
            case ControlId::SubTabNew: return std::string("subTabNew");
            case ControlId::SubTabDel: return std::string("subTabDel");
            case ControlId::ProbeButton: return std::string("probeButton");
            case ControlId::PrimaryButton: return std::string("primaryButton");
            case ControlId::BackupRefresh: return std::string("backupRefresh");
            case ControlId::RestoreButton: return std::string("restoreButton");
            case ControlId::OpenWebUiButton: return std::string("openWebUiButton");
            case ControlId::SetRestartGateway: return std::string("setRestartGateway");
            case ControlId::SetDiagnose: return std::string("setDiagnose");
            case ControlId::SetUsage: return std::string("setUsage");
            case ControlId::LatencySeg: return std::string("latencySeg");
            case ControlId::WidgetSeg: return std::string("widgetSeg");
            case ControlId::SetProfiles: return std::string("setProfiles");
            case ControlId::ModalProfileApply: return std::string("modalProfileApply");
            case ControlId::ModalProfileDelete: return std::string("modalProfileDelete");
            case ControlId::ModeSeg: return std::string("modeSeg");
            case ControlId::ThemeSeg: return std::string("themeSeg");
            case ControlId::ModelChip: return std::string("modelChip");
            case ControlId::SwitchRowControl: return std::string("switchRow");
            case ControlId::OfflineStart: return std::string("offlineStart");
            default: return std::string("none");
            }
}
}

Json Application::Dump() const {
    // The models-page block below reports what CurrentView actually produces (lamps, filters, rows), so
    // the acceptance run can assert on the same view model the renderer draws.
    auto* viewOwner=const_cast<Application*>(this);
    const auto view=viewOwner->CurrentView();
    const auto expected=ComputeRect(model_,monitor_.geometry);
    // 托盘菜单按**真实菜单句柄**导出（含分隔线、子菜单、禁用态与标签原文）：门禁直接断言
    // 「结构 + 标签无乱码」，而不是断言一份与实现无关的清单。
    Json menu=Json::array();
    if(const auto handle=BuildMenu()) {
        // 递归读一行（子菜单靠 std::function 回调自身，lambda 不能在自己的初始化里引用自己）。
        std::function<Json(HMENU,UINT)> readItem;
        readItem=[&](HMENU owner,UINT position) {
            MENUITEMINFOW info{sizeof(info)};
            info.fMask=MIIM_ID|MIIM_STATE|MIIM_STRING|MIIM_FTYPE|MIIM_SUBMENU;
            wchar_t text[160]{};
            info.dwTypeData=text;info.cch=static_cast<UINT>(std::size(text));
            if(!GetMenuItemInfoW(owner,position,TRUE,&info)) return Json{};
            const bool separator=(info.fType&MFT_SEPARATOR)!=0;
            Json item{{"id",separator?0:info.wID},{"separator",separator},{"label",Narrow(text)},
                {"checked",(info.fState&MFS_CHECKED)!=0},{"enabled",(info.fState&MFS_DISABLED)==0},
                {"submenu",info.hSubMenu!=nullptr}};
            const MenuEntry* entry=nullptr;
            for(const auto& candidate:MenuEntries()) if(candidate.id==info.wID) entry=&candidate;
            if(entry) item["token"]=entry->token;
            if(info.hSubMenu) {
                Json children=Json::array();
                const int childCount=GetMenuItemCount(info.hSubMenu);
                for(int child=0;child<childCount;++child) {
                    const auto childItem=readItem(info.hSubMenu,static_cast<UINT>(child));
                    if(!childItem.is_null()) children.push_back(childItem);
                }
                item["children"]=children;
            }
            return item;
        };
        const int count=GetMenuItemCount(handle);
        for(int index=0;index<count;++index) {
            const auto item=readItem(handle,static_cast<UINT>(index));
            if(!item.is_null()) menu.push_back(item);
        }
        DestroyMenu(handle);
    }
    const auto rect=GeometryController::Read(panel_);
    const auto strip=GeometryController::Read(strip_);
    return Json{
        {"instance",Narrow(control_?[this]{wchar_t name[128]{};GetWindowTextW(control_,name,128);return std::wstring(name);}():L"")},
        {"state",static_cast<int>(state_)},{"edge",static_cast<int>(model_.edge)},
        {"anchor",model_.anchor},{"compact",false},   // 紧凑视图已删除
        {"pinned",stored_.topmost},{"activePage",activePage_},
        {"search",{{"text",search_.TextUtf8()},{"focused",focusedField_==&search_},
            {"composing",search_.Composing()},{"composingText",Narrow(search_.composition)}}},
        {"pollSeconds",stored_.pollSeconds},{"topmost",stored_.topmost},
        {"panel",{rect.x,rect.y,rect.width,rect.height}},
        {"strip",{strip.x,strip.y,strip.width,strip.height}},
        {"expectedPanel",{expected.panel.x,expected.panel.y,expected.panel.width,expected.panel.height}},
        {"expectedStrip",expected.strip?Json{expected.strip->x,expected.strip->y,expected.strip->width,expected.strip->height}:Json{}},
        {"float",{model_.floating.x,model_.floating.y}},{"tier",static_cast<int>(model_.tier)},
        {"autostart",AutoStartEnabled()},{"hotkeys",hotkeysRegistered_},
        {"hotkeyErrors",{hotkeyErrors_[0],hotkeyErrors_[1],hotkeyErrors_[2]}},
        {"monitorId",Narrow(monitor_.id)},
        {"trayCallbackHwnd",reinterpret_cast<std::uintptr_t>(control_)},
        {"tray",{{"registered",trayRegistered_},{"readds",trayReadds_},{"callbackMessage",kTrayMessage},
            {"iconLoaded",trayIcon_!=nullptr},{"trayId",kTrayId},
            // 门禁据此断言「托盘图标是 logo、颜色按状态、tooltip 是旧版那四行」。
            {"colour",static_cast<int>(trayColour_)},{"tooltip",Narrow(trayTip_)}}},
        {"lastDrop",{{"valid",lastDrop_.valid},
            {"start",{lastDrop_.start.x,lastDrop_.start.y,lastDrop_.start.width,lastDrop_.start.height}},
            {"final",{lastDrop_.finalRect.x,lastDrop_.finalRect.y,lastDrop_.finalRect.width,lastDrop_.finalRect.height}},
            {"cursor",{lastDrop_.cursor.x,lastDrop_.cursor.y}},
            {"decidedEdge",lastDrop_.decidedEdge},{"decidedAnchor",lastDrop_.decidedAnchor}}},
        {"events",[this]{Json list=Json::array();for(const auto& entry:events_)list.push_back(
            {{"name",entry.name},{"tick",entry.tick},{"msgTime",entry.msgTime},{"x",entry.x},{"y",entry.y},
             {"state",entry.state},{"hit",entry.hit}});return list;}()},
        {"work",{monitor_.geometry.work.x,monitor_.geometry.work.y,monitor_.geometry.work.width,monitor_.geometry.work.height}},
        {"dpi",static_cast<int>(monitor_.geometry.dpi)},
        {"visible",state_!=State::Hidden},{"stripVisible",IsWindowVisible(strip_)!=0},
        {"panelVisible",IsWindowVisible(panel_)!=0},
        {"clickThrough",(GetWindowLongPtrW(panel_,GWL_EXSTYLE)&WS_EX_TRANSPARENT)!=0},
        {"panelHwnd",reinterpret_cast<std::uintptr_t>(panel_)},
        {"stripHwnd",reinterpret_cast<std::uintptr_t>(strip_)},
        {"writes",geometry_.Writes()},{"violations",geometry_.Violations()},
        {"backdrop",{{"generation",stripContext_.surface.BackdropGeneration()},
            {"pixels",static_cast<int>(stripContext_.surface.BackdropPixels().size())},
            {"mean",stripContext_.surface.BackdropMean()},
            {"size",{stripContext_.surface.BackdropWidth(),stripContext_.surface.BackdropHeight()}}}},
        {"drops",drops_},{"tierWrites",tierWrites_},
        // 动效状态（门禁断言「展开会启动入场、动画结束后计时器停掉」；不影响几何）。
        {"motion",{{"enabled",animOverride_},{"panelStart",panelMotionStart_},
            {"cardStart",cardMotionStart_},{"stripStart",stripMotionStart_},
            {"cardExit",cardExitStart_},{"themeStart",themeMotionStart_},{"timer",motionTimer_}}},
        {"stripMoves",stripMoves_},{"panelMoves",panelMoves_},{"panelLeaves",panelLeaves_},{"graces",graces_},
        {"wakeCount",wakeCount_},{"themeLight",palette_.light},
        // 真实调色板（0xRRGGBB）：门禁据此断言"切换主题后**画出来的颜色**真的变了"。
        // 以前只暴露 light 布尔，宿系统恰好是浅色时它恒为 true，主题切换失效也能全绿
        // （真机 2026-10-20 的「主题切换失效」正是这样漏掉的）。
        {"palette",[this] {
            const auto hex=[](const D2D1_COLOR_F& colour) {
                const auto channel=[](float value) {
                    return static_cast<int>(std::lround(std::clamp(value,0.0f,1.0f)*255.0f));
                };
                return (channel(colour.r)<<16)|(channel(colour.g)<<8)|channel(colour.b);
            };
            return Json{{"ink",hex(palette_.ink)},{"surf",hex(palette_.surf)},
                {"text",hex(palette_.text)},{"stripTop",hex(palette_.stripTop)},
                {"light",palette_.light}};
        }()},
        {"settingsPath",Narrow(settings_?settings_->Path():L"")},
        {"stored",{{"webuiPort",stored_.webuiPort},{"pollSeconds",stored_.pollSeconds},
            {"theme",stored_.theme},{"hotkey",stored_.hotkey},{"distro",stored_.distro},
            {"previewDiff",stored_.previewDiff},{"autoReloadGateway",stored_.autoReloadGateway},
            {"latencyProbe",stored_.latencyProbe},{"latencyMinutes",stored_.latencyMinutes},
            {"widgetStyle",stored_.widgetStyle},{"heightDip",stored_.heightDip},
            {"topmost",stored_.topmost},{"closeAction",stored_.closeAction},
            {"writes",settings_?settings_->Writes():0}}},
        {"logPath",Narrow(log_?log_->Path():L"")},
        // ---- H4 models page: what the gate asserts on (state, filters, list, switch bookkeeping) ----
        {"data",{{"busy",workerBusy_.load()},{"signature",stateSignature_},
            {"wsl",hermesState_.wsl},{"providers",hermesState_.providers.size()},
            // 供应商逐条（名称/地址/密钥变量名/模型数）：门禁要断言"某个动作发的是这个供应商的地址"，
            // 只给个数不够（v1.15 的供应商菜单探测路径就是这么漏覆盖的）。
            {"providerList",[this]{Json list=Json::array();
                for(const auto& provider:hermesState_.providers)
                    list.push_back({{"name",provider.name},{"providerId",provider.providerId},
                        {"baseUrl",provider.baseUrl},{"keyEnv",provider.keyEnv},
                        {"models",provider.models.size()},{"isActive",provider.isActive}});
                return list;}()},
            {"fallbacks",hermesState_.fallbacks.size()},
            {"currentProvider",hermesState_.model.provider},{"currentModel",hermesState_.model.model},
            {"webuiReachable",hermesState_.webui.reachable},{"gatewayReachable",hermesState_.gateway.reachable},
            {"configMtime",hermesState_.config.mtime},{"gatewaySince",hermesState_.gateway.sinceUnix},
            {"hotLoadedMtime",stored_.hotLoadedMtime},
            {"pendingGatewayRestart",pendingGatewayRestart_},
            {"chipWarnPresent",view.chipWarn.present},{"chipPresent",view.chip.present},
            {"statuses",[&view]{Json list=Json::array();
                for(const auto& item:view.statuses) list.push_back({{"label",item.label},
                    {"action",static_cast<int>(item.action)}});
                return list;}()},
            {"statusCenterOpen",view.statusCenterOpen},
            {"hoverCard",{{"visible",cardVisible_},
                {"windowVisible",hoverCard_?IsWindowVisible(hoverCard_)!=0:false},
                {"window",[this]{RECT rect{};if(hoverCard_) GetWindowRect(hoverCard_,&rect);
                    return Json::array({rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top});}()},
                {"model",view.hoverCard.model},{"note",view.hoverCard.note},
                {"hint",view.hoverCard.hint},
                {"rows",[&view]{Json list=Json::array();
                    for(const auto& row:view.hoverCard.rows) list.push_back({{"label",row.label},
                        {"detail",row.detail},{"state",row.state}});
                    return list;}()}}},
            {"statusRows",[&view,this]{Json list=Json::array();
                if(!panel_) return list;
                RECT client{};GetClientRect(panel_,&client);
                const auto layout=ComputeLayout(view,static_cast<float>(client.right),
                    static_cast<float>(client.bottom),static_cast<float>(WindowDpi())/96.0f);
                POINT origin{0,0};ClientToScreen(panel_,&origin);
                for(const auto& row:layout.statusRows)
                    list.push_back({{"x",static_cast<int>(std::lround(origin.x+row.row.left))},
                        {"y",static_cast<int>(std::lround(origin.y+row.row.top))},
                        {"width",static_cast<int>(std::lround(row.row.right-row.row.left))},
                        {"height",static_cast<int>(std::lround(row.row.bottom-row.row.top))}});
                return list;}()},
            {"loading",hermesState_.loading}}},
        // 展开中的下拉（供应商 / 来源）：可见项 + 滚动量（真机：18 个供应商放不进 8 行，
        // 门禁据此断言"后面的选项滚得到、点得到"）。
        {"select",[this,&view]{
            RECT client{};GetClientRect(panel_,&client);
            const auto layout=ComputeLayout(view,static_cast<float>(client.right),
                static_cast<float>(client.bottom),static_cast<float>(WindowDpi())/96.0f);
            Json labels=Json::array();
            for(const auto& label:layout.selectLabels) labels.push_back(label);
            return Json{{"labels",labels},{"scroll",layout.selectScroll},
                {"total",layout.selectTotal},{"visible",layout.selectVisible}};
        }()},
        // ---- H5：回退链页 / 模态 / Toast 的可断言状态 ----
        {"fallback",{{"title",view.fallback.title},{"subtitle",view.fallback.subtitle},
            {"addLabel",view.fallback.addLabel},{"emptyText",view.fallback.emptyText},
            {"probeLabel",view.fallback.probeLabel},{"probeRunning",view.fallback.probeRunning},
            {"formOpen",view.fallback.formOpen},{"selectOpen",view.fallback.selectOpen},
            {"source",view.fallback.source},{"sources",view.fallback.sources},
            {"busy",view.fallback.busy},
            {"error",view.fallback.error},{"modelValue",view.fallback.modelValue},
            {"modelSelectOpen",view.fallback.modelSelectOpen},
            {"modelOptions",view.fallback.modelOptions},
            {"rows",[&view]{Json list=Json::array();
                for(const auto& row:view.fallback.rows) list.push_back({{"provider",row.provider},
                    {"model",row.model},{"baseUrl",row.baseUrl},{"keyEnv",row.keyEnv},
                    {"keyWarn",row.keyWarn},{"hasProbe",row.hasProbe},{"probeOk",row.probeOk},
                    {"probeHasModel",row.probeHasModel},{"probePending",row.probePending},
                    {"probeCount",row.probeCount},{"probeDetail",row.probeDetail}});
                return list;}()}}},
        {"helperError",view.helperError},
        // 悬停提示表（审计用：每条都必须是一句话、不含换行）。
        {"tooltips",[&view]{Json list=Json::array();
            for(const auto& tip:view.tooltips)
                list.push_back({{"control",tip.control},{"name",ControlName(static_cast<ControlId>(tip.control))},
                    {"index",tip.index},{"text",tip.text}});
            return list;}()},
        // ---- F4：延迟历史（每个模型最近一次 + 近 5 次可用率）与探测状态 ----
        {"latency",[this]{Json map=Json::object();
            if(latency_) for(const auto& key:latency_->Keys()) {
                int ms=0;bool ok=false;
                latency_->Last(key,&ms,&ok);
                map[key]={{"ms",ms},{"ok",ok},{"availability",latency_->Availability(key)}};
            }
            return map;}()},
        {"latencyBusy",latencyBusy_.load()},
        // ---- F5：配置档案（列表 / 待确认档案 / 失败步） ----
        {"profiles",[this]{Json list=Json::array();
            if(profiles_) for(const auto& profile:profiles_->All())
                list.push_back({{"name",profile.name},{"provider",profile.mainProvider},
                    {"main",profile.mainModel},{"fallbacks",profile.fallbacks.size()},
                    {"favorites",profile.favorites.size()}});
            return list;}()},
        {"profileBusy",profileBusy_.load()},
        {"pendingProfile",pendingProfileName_},
        {"profileError",profileError_},
        // ---- F6：托盘通知计数与最后一条文案（门禁断言边沿与抑制） ----
        {"notifications",{{"count",notificationCount_},{"last",lastNotificationText_},
            {"lastOk",lastNotifyOk_},{"kinds",notificationKinds_}}},
        {"modal",{{"title",view.modal.title},{"body",view.modal.body},
            {"facts",view.modal.facts},
            {"profiles",[&view]{Json rows=Json::array();
                for(const auto& row:view.modal.profiles)
                    rows.push_back({{"name",row.name},{"detail",row.detail}});
                return rows;}()},
            {"confirmLabel",view.modal.confirmLabel},{"cancelLabel",view.modal.cancelLabel},
            {"danger",view.modal.danger}}},
        {"toast",{{"text",view.toast.text},{"kind",view.toast.kind}}},
        // ---- H5b/H5c：添加 / 备份 / 设置页的可断言状态 ----
        {"add",{{"selectOpen",addSelectOpen_},{"selectField",addSelectField_},{"sub",view.add.sub},{"title",view.add.title},{"mode",view.add.mode},
            {"error",view.add.error},{"info",view.add.info},{"summary",view.add.summary},
            {"chips",[&view]{Json list=Json::array();
                for(const auto& chip:view.add.chips) list.push_back(chip.model);
                return list;}()},
            {"chipsSelected",[&view]{Json list=Json::array();
                for(const auto& chip:view.add.chips) list.push_back(chip.selected);
                return list;}()},
            {"modelScroll",view.add.modelScroll},
            {"countText",view.add.countText},{"chipNote",view.add.chipNote},
            {"keyRevealed",view.add.keyRevealed},
            {"primaryDisabled",view.add.primaryDisabled},
            {"providers",view.add.providers},{"provider",view.add.provider},
            {"subs",view.add.subs},
            {"modes",[&view]{Json list=Json::array();
                for(const auto& seg:view.add.modes) list.push_back({{"label",seg.label},{"active",seg.active}});
                return list;}()},
            {"fields",[&view]{Json list=Json::array();
                for(const auto& field:view.add.fields) list.push_back({{"label",field.label},
                    {"value",field.value},{"hint",field.hint},{"note",field.note},{"kind",field.kind},
                    {"masked",field.masked},{"index",field.index}});
                return list;}()},
            {"switches",[&view]{Json list=Json::array();
                for(const auto& row:view.add.switches) list.push_back({{"label",row.label},
                    {"on",row.on},{"index",row.index}});
                return list;}()}}},
        {"backup",{{"title",view.backup.title},{"subtitle",view.backup.subtitle},
            {"filter",view.backup.filter},{"total",view.backup.total},{"busy",view.backup.busy},
            {"emptyText",view.backup.emptyText},{"footNote",view.backup.footNote},
            {"filters",[&view]{Json list=Json::array();
                for(const auto& seg:view.backup.filters) list.push_back({{"label",seg.label},
                    {"count",seg.count},{"active",seg.active}});
                return list;}()},
            {"rows",[&view]{Json list=Json::array();
                for(const auto& row:view.backup.rows) list.push_back({{"file",row.file},
                    {"kind",row.kind},{"note",row.note},{"stamp",row.stamp},{"sizeKb",row.sizeKb}});
                return list;}()}}},
        {"settings",{{"title",view.settings.title},{"theme",view.settings.theme},
            {"hotkeyHint",view.settings.hotkeyHint},{"meta",view.settings.meta},
            {"themes",[&view]{Json list=Json::array();
                for(const auto& seg:view.settings.themes) list.push_back({{"label",seg.label},
                    {"active",seg.active}});
                return list;}()},
            {"sections",view.settings.sections},
            {"fields",[&view]{Json list=Json::array();
                for(const auto& field:view.settings.fields) list.push_back({{"label",field.label},
                    {"value",field.value},{"note",field.note},{"kind",field.kind},{"index",field.index}});
                return list;}()},
            {"switches",[&view]{Json list=Json::array();
                for(const auto& row:view.settings.switches) list.push_back({{"label",row.label},
                    {"note",row.note},{"on",row.on},{"index",row.index}});
                return list;}()}}},
        {"focus",{{"field",view.focusedFormField},{"caret",view.focusedFormCaret},
            {"native",[this]{const auto* native=NativeFor(focusedField_);
                return Json(native?native->Focused():false);}()},
            {"multiline",view.focusedFormMultiline},{"hotkeyCapturing",setHotkeyCapturing_},
            {"control",focusControl_},{"index",focusControlIndex_}}},
        {"fallback",{{"formOpen",fbFormOpen_},{"selectOpen",fbSelectOpen_},{"source",fbSource_}}},
        {"probeAll",{{"busy",allProbeBusy_.load()},{"done",allProbeDone_.load()},
            {"total",allProbeTotal_.load()},{"ok",allProbeOk_.load()},
            {"providers",[this]{Json list=Json::array();std::lock_guard<std::mutex> lock(probedMutex_);
                for(const auto& entry:probedModels_)
                    list.push_back({{"name",entry.first},{"count",entry.second.size()}});
                return list;}()}}},
        {"tooltip",{{"visible",view.tooltip.visible},{"text",view.tooltip.text},
            {"control",tooltipHover_}}},
        {"tooltipHoverIndex",tooltipHoverIndex_},
        // 悬停高亮状态（排查"悬停没反应/闪过虚影"用：hovered 是命中控件，amount 是过渡进度）。
        {"hover",{{"control",view.hoverControl},{"amount",view.motion.hoverAmount},
            {"previous",view.motion.hoverPrevious}}},
        // 同步章角度（排查"不转/抖动"用）：按时间算，正常应随每次 dump 递增。
        {"spin",{{"angle",view.syncSpinAngle},{"busy",view.busy}}},
        // v1.24：忙碌条是独立子窗口——门禁断言"忙碌期间它持续重绘且单帧很便宜"。
        {"syncAll",{{"busy",syncAllBusy_.load()},{"done",syncAllDone_.load()},
            {"total",syncAllTotal_.load()},{"changed",syncAllChanged_.load()}}},
        {"paintFrames",paintFrames_},
        {"hoverLayout",{{"hits",hoverLayoutHits_},{"misses",hoverLayoutMisses_}}},
        {"measure",{{"hits",panelContext_.surface.MeasureHits()},
            {"misses",panelContext_.surface.MeasureMisses()}}},
        {"busyBar",{{"visible",busyBar_.Visible()},{"paints",busyBar_.Paints()},
            {"lastPaintMs",busyBar_.LastPaintMs()}}},
        {"tooltipPopup",{{"visible",tooltipView_.visible},{"text",tooltipView_.text},
            {"left",tooltipView_.left},{"top",tooltipView_.top}}},
        {"providerMenu",{{"open",pending_==Pending::ProviderMenu},
            {"provider",pendingProvider_.empty()?std::string{}:pendingProvider_}}},
        {"pageScroll",pageScroll_},
        {"fit",{{"contentHeight",model_.heightDip},{"heightDip",model_.heightDip},
            {"desiredPhysical",[this,&view]{RECT client{};GetClientRect(panel_,&client);
                const auto scale=static_cast<float>(WindowDpi())/96.0f;
                return ComputeLayout(view,static_cast<float>(client.right),
                    static_cast<float>(client.bottom),scale).desiredHeight;}()},
            {"scale",static_cast<double>(WindowDpi())/96.0}}},
        {"models",{{"filter",filterKind_},{"cursor",cursor_},{"scroll",listScroll_},
            {"pointer",{{"x",pointerX_},{"y",pointerY_},{"inside",pointerInside_}}},
            {"expanded",expanded_},{"search",search_.TextUtf8()},
            {"favorites",stored_.favorites},{"recents",stored_.recents},
            {"switchingKey",switchingKey_},{"switchingModel",switchingModel_},
            {"lights",[&view]{Json list=Json::array();
                for(const auto& lamp:view.lamps) list.push_back({{"label",lamp.label},
                    {"state",static_cast<int>(lamp.state)},{"detail",lamp.detail}});
                return list;}()},
            {"chip",{{"present",view.chip.present},{"kind",static_cast<int>(view.chip.kind)},
                {"text",view.chip.text}}},
            {"chipWarn",{{"present",view.chipWarn.present},{"kind",static_cast<int>(view.chipWarn.kind)},
                {"text",view.chipWarn.text}}},
            {"hero",{{"provider",view.currentProvider},{"model",view.currentModel},
                {"mode",view.currentMode},{"hue",view.heroHue},{"swap",view.currentSwap},
                {"badge",view.currentBadge}}},
            {"sync",view.syncTime},
            {"filters",[&view]{Json list=Json::array();
                for(const auto& seg:view.filters) list.push_back({{"label",seg.label},{"count",seg.count},
                    {"active",seg.active}});
                return list;}()},
            {"matches",view.matches},
            {"hint",view.showHint?view.hintText:std::string{}},
            {"offlineReason",view.offlineReason},
            {"listEmpty",view.listEmpty},
            {"rows",[&view]{Json list=Json::array();
                for(const auto& ref:view.visibleRows) {
                    const auto& group=view.groups[static_cast<std::size_t>(ref.group)];
                    const auto& row=group.rows[static_cast<std::size_t>(ref.row)];
                    list.push_back({{"key",row.key},{"provider",row.provider},{"model",row.model},
                        {"current",row.current},{"favorite",row.favorite},
                        {"index",row.index},{"group",group.title},{"open",group.open}});
                }
                return list;}()},
            {"groups",[&view]{Json list=Json::array();
                for(const auto& group:view.groups) list.push_back({{"title",group.title},
                    {"providerId",group.providerId},{"active",group.active},{"open",group.open},
                    {"countText",group.countText},{"rows",group.rows.size()}});
                return list;}()}}},
        {"menu",menu}};
}

Json Application::Command(const Json& request) {
    const auto command=request.at("command").get<std::string>();
    if(command=="place") {
        // Debug control plane contract (DESIGN §8): deterministic absolute placement without input.
        const auto requestedState=request.contains("state")?static_cast<State>(request["state"].get<int>()):State::Floating;
        if(requestedState==State::Dragging) throw std::invalid_argument("place cannot set the dragging state");
        if(request.contains("edge")) model_.edge=static_cast<Edge>(request["edge"].get<int>());
        if(request.contains("anchor")) model_.anchor=request["anchor"].get<int>();
        if(request.contains("tier")) model_.tier=static_cast<Tier>(request["tier"].get<int>());
        if(request.contains("float")) model_.floating={request["float"][0].get<int>(),request["float"][1].get<int>()};
        if(request.contains("monitorPoint")) {
            monitor_=MonitorAt({request["monitorPoint"][0].get<int>(),request["monitorPoint"][1].get<int>()});
        } else {
            const auto size=TierSize(model_,monitor_.geometry.dpi);
            const auto interest=model_.edge==Edge::None
                ?Rect{model_.floating.x,model_.floating.y,size.width,size.height}
                :GeometryController::Read(panel_);
            RECT native{interest.x,interest.y,interest.right(),interest.bottom()};
            monitor_=ReadMonitor(MonitorFromRect(&native,MONITOR_DEFAULTTONEAREST));
        }
        if(request.contains("state")) state_=requestedState;
        else state_=NextState(state_,Event::Show,model_.edge);
        geometry_.Place(model_,monitor_.geometry,state_,"place");
        geometry_.Visibility(state_);
        geometry_.Validate(model_,monitor_.geometry,state_);
        // 位置变了桌面就变了：与真实拖动落点同一条重抓规则。
        stripContext_.surface.InvalidateBackdrop();
        if(state_==State::Collapsed) RenderStrip();
        return Dump();
    }
    if(command=="state") return Dump();
    if(command=="process") {
        // 全量审查指标：资源泄漏（GDI/USER/句柄/线程）、内存与 CPU 时间。门禁按轮次采样比较。
        PROCESS_MEMORY_COUNTERS counters{};
        counters.cb=sizeof(counters);
        K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters));
        FILETIME create{},exit{},kernel{},user{};
        GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user);
        const auto ticks=[](const FILETIME& value) {
            return (static_cast<unsigned long long>(value.dwHighDateTime)<<32)|value.dwLowDateTime;
        };
        DWORD handles=0;
        GetProcessHandleCount(GetCurrentProcess(),&handles);
        int threads=0;
        if(const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);snapshot!=INVALID_HANDLE_VALUE) {
            THREADENTRY32 entry{sizeof(entry)};
            if(Thread32First(snapshot,&entry)) {
                do {
                    if(entry.th32OwnerProcessID==GetCurrentProcessId()) ++threads;
                } while(Thread32Next(snapshot,&entry));
            }
            CloseHandle(snapshot);
        }
        return {{"gdi",GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)},
            {"user",GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS)},
            {"handles",static_cast<unsigned long long>(handles)},
            {"threads",threads},
            {"workingSetKb",static_cast<unsigned long long>(counters.WorkingSetSize/1024)},
            {"privateKb",static_cast<unsigned long long>(counters.PagefileUsage/1024)},
            {"cpuMs",(ticks(kernel)+ticks(user))/10000ull},
            {"busy",workerBusy_.load()},{"pending",pending_!=Pending::None}};
    }
    if(command=="audit") {
        // 布局自查：控件/分段/字段/阶梯行/模态行的越界与退化。全页×主题扫描靠它断言"一个都不许有"。
        RECT client{};
        GetClientRect(panel_,&client);
        const auto scale=static_cast<float>(WindowDpi())/96.0f;
        const auto view=CurrentView();
        const auto layout=ComputeLayout(view,static_cast<float>(client.right),
            static_cast<float>(client.bottom),scale);
        Json issues=Json::array();
        const float width=static_cast<float>(client.right),height=static_cast<float>(client.bottom);
        // 纵向判定：窗口被工作区夹短时（pageMaxScroll>0），页面控件允许落在视口下方的可滚动区
        // ——那是"滚动才能看到"，不是缺陷；横向仍然严格贴窗口。
        const float scrollTop=layout.content.top-layout.pageScroll;
        const float scrollBottom=scrollTop+(layout.content.bottom-layout.content.top)
            +layout.pageMaxScroll;
        const auto check=[&](const char* kind,const std::string& name,const D2D1_RECT_F& rect,
                bool allowScroll=false) {
            if(rect.right<=rect.left||rect.bottom<=rect.top) {
                issues.push_back({{"kind",kind},{"name",name},{"issue","degenerate"}});
                return;
            }
            const bool scrollable=allowScroll&&(layout.pageMaxScroll>0.5f||layout.fbMaxScroll>0.5f);
            const float top=scrollable?scrollTop:-0.5f;
            const float bottom=scrollable?(scrollBottom+layout.fbMaxScroll):height+0.5f;
            if(rect.left<-0.5f||rect.right>width+0.5f||rect.top<top||rect.bottom>bottom) {
                issues.push_back({{"kind",kind},{"name",name},
                    {"issue",rect.right>width+0.5f||rect.left<-0.5f?"outside-horizontal":"outside"},
                    {"rect",{rect.left,rect.top,rect.right,rect.bottom}}});
            }
        };
        // 顶栏/底栏/状态灯这类"外壳件"永远必须在窗口里；只有页面正文可以随滚动落在视口下方。
        const auto chrome=[](ControlId id) {
            switch(id) {
            case ControlId::Snap: case ControlId::Pin: case ControlId::Minimize:
            case ControlId::Close: case ControlId::Refresh: case ControlId::Chip:
            case ControlId::ChipWarn: case ControlId::ChipHelper:
            case ControlId::LampWsl: case ControlId::LampWebui: case ControlId::LampGateway:
            case ControlId::TabModels: case ControlId::TabFallback: case ControlId::TabAdd:
            case ControlId::TabBackup: case ControlId::TabSettings:
            case ControlId::RegisterRestartGateway: case ControlId::OfflineStart:
            case ControlId::StatusPill:
            case ControlId::ModalConfirm: case ControlId::ModalCancel:
            case ControlId::ModalProfileApply: case ControlId::ModalProfileDelete:
                return true;
            default: return false;
            }
        };
        for(const auto& control:layout.controls) {
            if(control.id==ControlId::None) continue;
            const auto id=control.id;
            const bool ladderControl=id==ControlId::FbUp||id==ControlId::FbDown
                ||id==ControlId::FbRemove||id==ControlId::FbProbeDot;
            const auto name=ControlName(id)+(control.index>=0?("#"+std::to_string(control.index))
                :std::string{});
            if(ladderControl) {
                // 阶梯按钮随内部滚动上下移动：只查退化与横向越界。
                if(control.rect.right<=control.rect.left||control.rect.bottom<=control.rect.top)
                    issues.push_back({{"kind","control"},{"name",name},{"issue","degenerate"}});
                else if(control.rect.left<-0.5f||control.rect.right>width+0.5f)
                    issues.push_back({{"kind","control"},{"name",name},{"issue","outside-horizontal"},
                        {"rect",{control.rect.left,control.rect.top,control.rect.right,control.rect.bottom}}});
                continue;
            }
            check("control",name,control.rect,!chrome(id));
        }
        for(std::size_t index=0;index<layout.segments.size();++index)
            check("segment","segment#"+std::to_string(index),layout.segments[index].rect,true);
        for(const auto& field:layout.fields) check("field","field#"+std::to_string(field.index),field.box,true);
        // 阶梯行随内部滚动上下移动：纵向由 fbMaxScroll 保证，只查退化与横向越界。
        for(const auto& row:layout.ladderRows) {
            if(row.rect.right<=row.rect.left||row.rect.bottom<=row.rect.top)
                issues.push_back({{"kind","ladderRow"},{"name","ladder"},{"issue","degenerate"}});
            else if(row.rect.left<-0.5f||row.rect.right>width+0.5f)
                issues.push_back({{"kind","ladderRow"},{"name","ladder"},{"issue","outside-horizontal"},
                    {"rect",{row.rect.left,row.rect.top,row.rect.right,row.rect.bottom}}});
        }
        for(const auto& row:layout.modalProfileRows) check("modalProfileRow","profile",row.row);
        if(view.activePage==0) check("listViewport","viewport",layout.listViewport,true);
        // 滚动条只在需要时画（不可见时不进审计：0 尺寸是"没有"的正常表示）。
        if(layout.pageScrollVisible) {
            check("pageScrollbar","track",layout.pageScrollTrack);
            check("pageScrollbar","thumb",layout.pageScrollThumb);
        }
        if(layout.listMaxScroll>0.0f) {
            check("listScrollbar","track",layout.scrollTrack);
            check("listScrollbar","thumb",layout.scrollThumb);
        }
        if(layout.modalScrollVisible) {
            check("modalScrollbar","track",layout.modalHScrollTrack);
            check("modalScrollbar","thumb",layout.modalHScrollThumb);
        }
        return {{"issues",issues},{"count",static_cast<int>(issues.size())},
            {"page",view.activePage},{"scroll",layout.pageScroll},{"listScroll",layout.listScroll},
            {"client",{width,height}},{"scale",scale},{"pageMaxScroll",layout.pageMaxScroll},
            {"content",{layout.content.left,layout.content.top,layout.content.right,layout.content.bottom}},
            {"navTop",layout.nav.top},{"hero",{layout.hero.top,layout.hero.bottom}},
            {"listViewport",{layout.listViewport.top,layout.listViewport.bottom}}};
    }
    if(command=="latency-probe") {
        // F4 门禁入口：立即跑一轮探测（与计时器同一路径），结果异步落到 latency.json。
        StartLatencyProbe(true);
        return Dump();
    }
    if(command=="wheel") {
        // 页面门禁入口：合成一次滚轮（设置页现在比视口高，需要滚到底才能点到「其他」区）。
        const auto delta=request.value("delta",-120);
        SendMessageW(panel_,WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<short>(delta)),0);
        return Dump();
    }
    if(command=="tray-tip") {
        // 门禁用：托盘悬停提示必须装进 NIF_TIP 的 128 个宽字符（超了会被系统截断）。
        const auto tip=TrayTooltip();
        return {{"text",Narrow(tip)},{"length",static_cast<int>(tip.size())}};
    }
    if(command=="tray-click") {
        // F6 门禁入口：走托盘气泡点击的真实回调路径。
        TrayMessage(NIN_BALLOONUSERCLICK);
        return Dump();
    }
    if(command=="controls") {
        RECT client{};
        GetClientRect(panel_,&client);
        POINT origin{0,0};
        ClientToScreen(panel_,&origin);
        const auto scale=static_cast<float>(WindowDpi())/96.0f;
        const auto view=CurrentView();
        const auto layout=ComputeLayout(view,static_cast<float>(client.right),
            static_cast<float>(client.bottom),scale);

        Json controls=Json::object();
        controls["_client"]={{"width",client.right},{"height",client.bottom}};
        controls["_origin"]={{"x",origin.x},{"y",origin.y}};
        controls["_scale"]=scale;
        controls["_content"]={{"left",layout.content.left},{"right",layout.content.right},
            {"top",layout.content.top},{"bottom",layout.content.bottom}};
        // The models list, item by item, in screen coordinates: the page gates click real pixels and
        // assert what that click did, without re-deriving the layout on the test side.
        // _list.items 覆盖模型页的 layout.items，以及备份/回退链页的 layout.rows（渲染层画的就是后者）。
        Json items=Json::array();
        const auto translate=[&](const D2D1_RECT_F& rect) {
            return Json{{"x",static_cast<int>(std::lround(origin.x+rect.left))},
                {"y",static_cast<int>(std::lround(origin.y+rect.top))},
                {"width",static_cast<int>(std::lround(rect.right-rect.left))},
                {"height",static_cast<int>(std::lround(rect.bottom-rect.top))}};
        };
        for(const auto& item:layout.items) {
            Json row{{"kind",static_cast<int>(item.kind)},{"group",item.group},{"row",item.row},
                {"sticky",item.sticky},{"rect",translate(item.rect)}};
            if(item.kind==ListItemKind::Row) row["star"]=translate(StarBox(item.rect,scale));
            if(item.kind==ListItemKind::GroupHead) row["menu"]=translate(GroupMenuBox(item.rect,scale));
            if(!item.text.empty()) row["text"]=item.text;
            items.push_back(row);
        }
        for(const auto& row:layout.rows) {
            Json entry{{"kind",static_cast<int>(row.kind)},{"row",row.row},{"group",row.group},
                {"rect",translate(row.rect)}};
            if(!row.text.empty()) entry["text"]=row.text;
            items.push_back(entry);
        }
        controls["_list"]={{ "items",items},{"scroll",layout.listScroll},{"maxScroll",layout.listMaxScroll},
            {"scrollVisible",layout.scrollVisible},{"thumb",translate(layout.scrollThumb)},
            {"track",translate(layout.scrollTrack)},{"viewport",translate(layout.listViewport)}};
        Json filters=Json::array();
        for(const auto& segment:layout.segments) filters.push_back(translate(segment.rect));
        controls["_filters"]=filters;
        if(const auto* start=ControlRectFor(layout,ControlId::OfflineStart)) controls["_offline"]=translate(*start);
        // The 回退链 page: one entry per ladder row with its three button boxes, the select options and
        // the form fields (index-ordered, because they share one control id).
        Json ladder=Json::array();
        for(const auto& row:layout.ladderRows) {
            ladder.push_back({{"rect",translate(row.rect)},
                {"up",translate(LadderButtonBox(row.rect,1,scale))},
                {"down",translate(LadderButtonBox(row.rect,2,scale))},
                {"remove",translate(LadderButtonBox(row.rect,3,scale))}});
        }
        Json segments=Json::array();
        for(const auto& segment:layout.segments) segments.push_back(translate(segment.rect));
        controls["_segments"]=segments;
        Json probeDots=Json::array();
        for(const auto& control:layout.controls)
            if(control.id==ControlId::FbProbeDot) probeDots.push_back(translate(control.rect));
        controls["_probeDots"]=probeDots;
        Json switchRows=Json::array();
        for(const auto& control:layout.controls)
            if(control.id==ControlId::SwitchRowControl) switchRows.push_back(translate(control.rect));
        controls["_switches"]=switchRows;
        // 模型药丸（新建供应商页的探测结果）：门禁要能逐个点掉。
        Json chipRects=Json::array();
        for(const auto& control:layout.controls)
            if(control.id==ControlId::ModelChip) chipRects.push_back(translate(control.rect));
        controls["_chips"]=chipRects;
        // v1.21：模型列表的行（索引与 dump.add.chips 对齐）。
        Json modelRowRects=Json::array();
        for(const auto& row:layout.modelRows)
            modelRowRects.push_back({{"index",row.index},{"rect",translate(row.rect)}});
        controls["_modelRows"]=modelRowRects;
        if(const auto* all=ControlRectFor(layout,ControlId::ModelRowAll))
            controls["modelRowAll"]=translate(*all);
        Json restore=Json::array();
        for(const auto& control:layout.controls)
            if(control.id==ControlId::RestoreButton) restore.push_back(translate(control.rect));
        controls["_restore"]=restore;
        Json select=Json::array();
        for(const auto& option:layout.selectOptions) select.push_back(translate(option.rect));
        Json fields=Json::array();
        for(const auto& field:layout.fields) fields.push_back(translate(field.box));
        controls["_list"]["ladder"]=ladder;
        controls["_select"]=select;
        controls["_selectCount"]=static_cast<int>(layout.selectOptions.size());
        controls["_fields"]=fields;
        controls["_page"]=view.activePage;
        // 控件表的顺序（ControlAt 就是按这个顺序取第一个命中）——门禁据此判断"画出来的都点得到"。
        {
            Json order=Json::array();
            for(const auto& control:layout.controls) {
                if(control.id==ControlId::None) continue;
                const auto key=ControlName(control.id);
                if(key!="none") order.push_back(key);
            }
            controls["_controlsOrder"]=order;
        }
        // 诊断：备份/回退链页的行与视口（渲染层用的是 layout.rows，_list.items 只覆盖模型页）。
        controls["_rowsCount"]=static_cast<int>(layout.rows.size());
        controls["_listViewport"]={{ "x",static_cast<int>(std::lround(origin.x+layout.listViewport.left))},
            {"y",static_cast<int>(std::lround(origin.y+layout.listViewport.top))},
            {"top",static_cast<int>(std::lround(layout.listViewport.top))},
            {"bottom",static_cast<int>(std::lround(layout.listViewport.bottom))}};
        controls["_layoutScroll"]=view.pageScroll;
        controls["_pageMaxScroll"]=layout.pageMaxScroll;
        controls["_tier"]=static_cast<int>(model_.tier);
        for(const auto& control:layout.controls) {
            if(control.id==ControlId::None) continue;
            const auto key=ControlName(control.id);
            if(key=="none") continue;
            const auto rect=control.rect;
            controls[key]={{ "x",static_cast<int>(std::lround(origin.x+rect.left))},
                {"y",static_cast<int>(std::lround(origin.y+rect.top))},
                {"width",static_cast<int>(std::lround(rect.right-rect.left))},
                {"height",static_cast<int>(std::lround(rect.bottom-rect.top))}};
        }
        return controls;
    }
    if(command=="strip-drag") {
        // 门禁：合成一次贴条拖动（不依赖真实指针）。走与鼠标相同的 Begin/Update/Commit，
        // 验证拖动中不展开、松手只吸附到最近边。
        if(!strip_||state_!=State::Collapsed) throw std::invalid_argument("strip-drag requires a collapsed strip");
        const auto rect=GeometryController::Read(strip_);
        const POINT start{rect.x+rect.width/2,rect.y+rect.height/2};
        const POINT target{request.at("x").get<int>(),request.at("y").get<int>()};
        BeginStripPress(start);
        for(int step=1;step<=10;++step) {
            const POINT point{start.x+(target.x-start.x)*step/10,start.y+(target.y-start.y)*step/10};
            UpdateStripDrag(point);
        }
        CommitStripDrop();
        return Dump();
    }
    if(command=="strip-tap") {
        // 门禁：合成一次贴条单击（悬停不展开、单击展开由 PointerEnteredStrip 决定）。
        if(!strip_||state_!=State::Collapsed) throw std::invalid_argument("strip-tap requires a collapsed strip");
        PointerEnteredStrip();
        return Dump();
    }
    if(command=="strip-hover") {
        // 门禁：直接臂上 300ms 计时器（不伪造 WM_MOUSEMOVE——真实光标不在贴条上时
        // TrackMouseEvent 会立刻回 WM_MOUSELEAVE，把卡片马上隐藏，测不出计时器路径）。
        if(!strip_||state_!=State::Collapsed) throw std::invalid_argument("strip-hover requires a collapsed strip");
        ArmStripCard();
        return Dump();
    }
    if(command=="strip-card") {
        // 门禁：直接显示/隐藏贴条悬浮信息卡（计时器路径之外的确定性入口）。
        if(request.value("show",true)) {
            if(state_!=State::Collapsed) throw std::invalid_argument("strip-card requires a collapsed strip");
            ShowHoverCard();
        } else HideHoverCard();
        return Dump();
    }
    if(command=="strip-click") {
        // 门禁：走真实的 WM_LBUTTONDOWN/UP 分支（不依赖真实光标位置），验证点击/拖动的分流。
        if(!strip_||state_!=State::Collapsed) throw std::invalid_argument("strip-click requires a collapsed strip");
        SendMessageW(strip_,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(2,2));
        SendMessageW(strip_,WM_LBUTTONUP,0,MAKELPARAM(2,2));
        return Dump();
    }
    if(command=="status-demo") {
        // 门禁：强制两个状态，验证「N 项状态」合并胶囊/状态中心（不依赖真实服务状态）。
        demoStatuses_={{"WebUI \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE",StatusAction::RetryWebUi},
            {"\xE5\x8A\xA9\xE6\x89\x8B\xE5\x87\xBA\xE9\x94\x99",StatusAction::RetryHelper}};
        statusCenterOpen_=request.value("open",false);
        InvalidateAll();
        return Dump();
    }
    if(command=="status-clear") {
        demoStatuses_.clear();
        statusCenterOpen_=false;
        InvalidateAll();
        return Dump();
    }
    if(command=="tap") {
        // Deterministic pointer synthesis for the page gates: the messages go through the real window
        // procedure (WM_LBUTTONDOWN/UP with client coordinates), so the app's own hit testing and click
        // handlers run exactly as they would for a real mouse. The desktop's real mouse input is not
        // usable here - a foreign topmost window intermittently owns the point (measured 2026-10-15,
        // see PROGRESS), the same environment limit the drag gate documents.
        RECT client{};
        GetClientRect(panel_,&client);
        auto x=request.at("x").get<int>();
        auto y=request.at("y").get<int>();
        if(request.value("screen",false)) {
            POINT origin{0,0};
            ClientToScreen(panel_,&origin);
            x-=origin.x;y-=origin.y;
        }
        if(x<0||y<0||x>=client.right||y>=client.bottom)
            throw std::invalid_argument("tap point is outside the panel: ("+std::to_string(x)+","
                +std::to_string(y)+") client "+std::to_string(client.right)+"x"
                +std::to_string(client.bottom)+" page "+std::to_string(activePage_)
                +" scroll "+std::to_string(pageScroll_));
        const auto point=MAKELPARAM(x,y);
        SendMessageW(panel_,WM_MOUSEMOVE,0,point);
        SendMessageW(panel_,WM_LBUTTONDOWN,MK_LBUTTON,point);
        SendMessageW(panel_,WM_LBUTTONUP,0,point);
        return {{"x",x},{"y",y}};
    }
    if(command=="key"||command=="vk") {
        // Keyboard twin of `tap`: WM_CHAR / WM_KEYDOWN + WM_KEYUP go into the panel's own window
        // procedure, so the field editing, the list cursor and the clear-on-Esc paths run for real.
        // (Real SendInput for the same keys lives in the h-ime gate, which skips when the desktop is
        // busy; this desktop's foreground window is often held by another application.)
        // v1.23：焦点在原生输入框时，字符/按键要送进那个 EDIT（系统控件自己处理光标与选区）。
        HWND target=panel_;
        if(auto* native=NativeFor(focusedField_))
            if(native->Handle()) target=native->Handle();
        if(command=="key") {
            for(const auto character:request.at("text").get<std::string>())
                SendMessageW(target,WM_CHAR,static_cast<WPARAM>(static_cast<unsigned char>(character)),0);
            return {{"chars",request.at("text").get<std::string>().size()}};
        }
        const auto key=request.at("vk").get<UINT>();
        // `mods` 是给门禁用的合成修饰键状态（真实键盘由 GetKeyState 提供，合成按键不会改它）。
        syntheticMods_=request.value("mods",0);
        SendMessageW(target,WM_KEYDOWN,key,0);
        SendMessageW(target,WM_KEYUP,key,0);
        syntheticMods_=0;
        return {{"vk",key}};
    }
    if(command=="configure") {
        // Debug knob the A7 CPU scenario uses: drive the data poll cadence directly (seconds). It is
        // deliberately not persisted - it exists so a gate can measure the idle and the polling cost
        // without writing a settings file.
        if(request.contains("interval")) {
            pollOverrideSeconds_=std::clamp(request["interval"].get<int>(),1,3600);
            if(log_) log_->Info("data","poll override "+std::to_string(pollOverrideSeconds_)+"s");
            UpdatePollTimer();
        }
        // F4 门禁：直接驱动探测开关/间隔（不落盘），避免依赖设置页滚动后的点击位置。
        if(request.contains("latencyProbe")) {
            stored_.latencyProbe=request["latencyProbe"].get<bool>();
            UpdateLatencyTimer();
        }
        if(request.contains("latencyMinutes")) {
            const auto minutes=request["latencyMinutes"].get<int>();
            stored_.latencyMinutes=minutes==15?15:(minutes==60?60:5);
            UpdateLatencyTimer();
        }
        // G1 门禁/截图：直接切换贴边挂件样式（与设置页同一条 SetSetting 路径）。
        if(request.contains("widgetStyle")) {
            SetSetting("widgetStyle",request["widgetStyle"].get<std::string>());
        }
        if(request.contains("theme")) {
            SetSetting("theme",request["theme"].get<std::string>());
        }
        // 动效开关（门禁用确定性模式）：animations=false 时所有进度恒为 1，即无动画。
        if(request.contains("animations")) {
            animOverride_=request["animations"].get<bool>();
            animCacheValid_=false;
            if(!animOverride_) {
                // 关掉动效时把任何"进行到一半"的状态收尾：主题直接落到目标调色板，
                // 卡片退场直接隐藏，贴条 alpha 归 1 —— 门禁看到的必须是稳定帧。
                if(themeStartValid_) palette_=targetPalette_;
                panelMotionStart_=cardMotionStart_=tooltipMotionStart_=0;
                hoverMotionStart_=themeMotionStart_=0;
                stripMotionStart_=0;
                cardExitStart_=0;
                capsuleDockStart_=0;
                themeStartValid_=false;
                if(hoverCard_&&!cardVisible_) ShowWindow(hoverCard_,SW_HIDE);
            }
            InvalidateAll();
            if(state_==State::Collapsed) RenderStrip();
        }
        // 固定高度/手动拉伸门禁：与拖底边落盘走同一条 SetPanelHeightDip（夹取 + 一次 Place）。
        if(request.contains("height")) {
            SetPanelHeightDip(request["height"].get<int>());
        }
        return {{"interval",pollOverrideSeconds_},{"latencyProbe",stored_.latencyProbe},
            {"latencyMinutes",stored_.latencyMinutes},{"widgetStyle",stored_.widgetStyle},
            {"heightDip",model_.heightDip},{"animations",animOverride_}};
    }
    if(command=="focus") {
        // Page gates need the panel to be the top window before they click it: the desktop may have
        // another topmost window (a running browser) that owns the point. Debug surface only.
        SetWindowPos(panel_,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        // 置顶带里可能还有其他 topmost（用户自己的悬浮面板）：再抬到带顶。
        SetWindowPos(panel_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        if(strip_&&state_==State::Collapsed)
            SetWindowPos(strip_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetForegroundWindow(panel_);
        SetActiveWindow(panel_);
        return {{"foreground",reinterpret_cast<std::uintptr_t>(GetForegroundWindow())==
                reinterpret_cast<std::uintptr_t>(panel_)},
            {"panel",reinterpret_cast<std::uintptr_t>(panel_)},
            {"zTop",reinterpret_cast<std::uintptr_t>(GetWindow(panel_,GW_HWNDFIRST))==
                reinterpret_cast<std::uintptr_t>(panel_)}};
    }
    if(command=="poll") {
        RefreshData();
        return Dump();
    }
    if(command=="log") {
        // Debug surface: lets the acceptance run push a canary through the real logging path.
        if(log_) log_->Info("canary",request.value("text",std::string{}));
        return {{"logged",log_!=nullptr},{"path",log_?Narrow(log_->Path()):std::string{}}};
    }
    if(command=="settings") {
        // Debug surface for the C1 acceptance scenario: what is persisted, where, and how often.
        if(!settings_) return {{"available",false}};
        if(command=="settings"&&request.contains("save")) PersistSettings(request["save"].get<bool>());
        return {{"available",true},{"path",Narrow(settings_->Path())},{"writes",settings_->Writes()},
            {"edge",static_cast<int>(model_.edge)},{"anchor",model_.anchor},{"float",{model_.floating.x,model_.floating.y}},
            {"tier",static_cast<int>(model_.tier)},{"monitorId",Narrow(monitor_.id)}};
    }
    if(command=="tray-icon") {
        // Debug surface（设计 §8：控制面只在 Debug 编译进来）：把**当前托盘图标的真实像素**
        // 以十六进制 BGRA 交出来（门禁/真机证据用它核对"托盘图标就是 logo"）。十六进制而不是
        // base64：省一个编码器，20×20 也就 3 KB。
        if(!trayIcon_) return {{"available",false}};
        ICONINFO info{};
        if(!GetIconInfo(trayIcon_,&info)) return {{"available",false}};
        const auto cleanup=[&] { if(info.hbmColor) DeleteObject(info.hbmColor);
            if(info.hbmMask) DeleteObject(info.hbmMask); };
        BITMAP description{};
        if(!info.hbmColor||!GetObjectW(info.hbmColor,sizeof(description),&description)) {
            cleanup();return {{"available",false}};
        }
        BITMAPV5HEADER header{};
        header.bV5Size=sizeof(header);header.bV5Width=description.bmWidth;
        header.bV5Height=-description.bmHeight;header.bV5Planes=1;header.bV5BitCount=32;
        header.bV5Compression=BI_BITFIELDS;header.bV5RedMask=0x00FF0000;
        header.bV5GreenMask=0x0000FF00;header.bV5BlueMask=0x000000FF;header.bV5AlphaMask=0xFF000000;
        std::vector<std::uint32_t> pixels(static_cast<std::size_t>(description.bmWidth)
            *static_cast<std::size_t>(description.bmHeight),0);
        HDC screen=GetDC(nullptr);
        const auto lines=GetDIBits(screen,info.hbmColor,0,static_cast<UINT>(description.bmHeight),
            pixels.data(),reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS);
        ReleaseDC(nullptr,screen);
        cleanup();
        if(lines==0) return {{"available",false}};
        static const char* digits="0123456789abcdef";
        std::string hex;
        hex.reserve(pixels.size()*8);
        for(const auto pixel:pixels) {
            for(int shift=28;shift>=0;shift-=4) hex.push_back(digits[(pixel>>shift)&0xF]);
        }
        return {{"available",true},{"width",description.bmWidth},{"height",description.bmHeight},
            {"colour",static_cast<int>(trayColour_)},{"bgra",hex}};
    }
    if(command=="hover") {
        // 合成悬停（与 WM_MOUSEMOVE 同一条路径）：门禁据此验证"悬停 450ms 弹提示"，
        // 不依赖真实指针（真机上 SetCursorPos 不一定会给非前台窗口发 WM_MOUSEMOVE）。
        const auto x=request.at("x").get<int>(),y=request.at("y").get<int>();
        // 不调 PointerMovedOnPanel：它会 arm TME_LEAVE，而真实指针在面板外时系统会立刻回一个
        // WM_MOUSELEAVE，把刚点起来的气泡又收掉（合成悬停不该受真实指针影响）。
        syntheticHover_=true;      // 忽略紧随其后的真实 WM_MOUSELEAVE（指针在面板外）
        // 2 秒保护窗：真实指针可能正停在面板上别处（或系统补发 WM_MOUSEMOVE），
        // 那会把合成悬停立刻覆盖成"指针下的控件"，提示永远弹不出来。
        syntheticHoverUntil_=GetTickCount64()+2000;
        TrackHover(panel_,static_cast<float>(x),static_cast<float>(y));
        return {{"hovered",hoverControl_},{"index",tooltipHoverIndex_}};
    }
    if(command=="control-at") {
        // 逐控件点击门禁用它问"这个点命中哪个控件"：返回控件表下标（-1 = 没有控件）+ 名字。
        POINT client{request.at("x").get<LONG>(),request.at("y").get<LONG>()};
        ScreenToClient(panel_,&client);
        const auto laid=LayoutForInput();
        const auto nameOf=[&](ControlId id) {
            static const ControlId ids[]={ControlId::Snap,ControlId::Pin,ControlId::Minimize,ControlId::Close,
                ControlId::TabModels,ControlId::TabFallback,ControlId::TabAdd,ControlId::TabBackup,
                ControlId::TabSettings,ControlId::Refresh,ControlId::Chip,ControlId::ChipWarn,
                ControlId::SearchBox,ControlId::SearchClear,ControlId::FilterAll,ControlId::FilterFav,
                ControlId::FilterRecent,ControlId::FbProbe,ControlId::FbFoldToggle,ControlId::FbFoldClose,
                ControlId::FbSubmit,ControlId::FbSelect,ControlId::FbModelSelect,ControlId::FbUp,ControlId::FbDown,ControlId::FbRemove,
                ControlId::FormField,
                ControlId::SelectOption,ControlId::ModalConfirm,ControlId::ModalCancel,ControlId::SubTabNew,
                ControlId::SubTabDel,ControlId::ModelChip,
                ControlId::ProbeButton,ControlId::PrimaryButton,
                ControlId::SwitchRowControl,ControlId::ModeSeg,ControlId::ThemeSeg,ControlId::RestoreButton,
                ControlId::BackupRefresh,ControlId::OpenWebUiButton,ControlId::ChipHelper,ControlId::OfflineStart};
            for(std::size_t index=0;index<std::size(ids);++index) if(ids[index]==id) return std::string("id")
                +std::to_string(index);
            return std::string("none");
        };
        int hit=-1;std::string label;
        for(std::size_t index=0;index<laid.controls.size();++index) {
            const auto& control=laid.controls[index];
            if(control.id==ControlId::None) continue;
            if(client.x<control.rect.left||client.x>=control.rect.right) continue;
            if(client.y<control.rect.top||client.y>=control.rect.bottom) continue;
            hit=static_cast<int>(index);
            label=nameOf(control.id);
            break;
        }
        return {{"index",hit},{"label",label},{"page",activePage_}};
    }
    if(command=="hide") Hide();
    else if(command=="show") Show();
    else if(command=="menu") InvokeMenu(request.at("id").get<unsigned>());
    else if(command=="hit") {
        POINT point{request.at("x").get<int>(),request.at("y").get<int>()};
        const auto hit=WindowFromPoint(point);
        const auto root=GetAncestor(hit,GA_ROOT);
        const auto describe=[this](HWND window) {
            wchar_t cls[128]={};
            if(window) GetClassNameW(window,cls,static_cast<int>(std::size(cls)));
            DWORD pid=0;
            if(window) GetWindowThreadProcessId(window,&pid);
            const auto style=window?GetWindowLongPtrW(window,GWL_EXSTYLE):0;
            // v1.23：命中我们自己的原生输入框（面板的子窗口）也算"点得到"——
            // 它就在面板内部，不是遮挡。
            return Json{{"hwnd",reinterpret_cast<std::uintptr_t>(window)},{"class",Narrow(cls)},
                {"pid",pid},{"topmost",(style&WS_EX_TOPMOST)!=0},
                {"isPanel",window==panel_},
                {"isPanelChild",window&&panel_&&(window==panel_||GetAncestor(window,GA_ROOT)==panel_)},
                {"isStrip",window==strip_}};
        };
        GUITHREADINFO info{sizeof(info)};
        const auto haveInfo=GetGUIThreadInfo(0,&info)!=0;
        return {{"point",{point.x,point.y}},{"hit",describe(hit)},{"root",describe(root)},
            {"capture",haveInfo?reinterpret_cast<std::uintptr_t>(info.hwndCapture):0},
            {"foreground",haveInfo?reinterpret_cast<std::uintptr_t>(info.hwndActive):0},
            {"panel",reinterpret_cast<std::uintptr_t>(panel_)}};
    }
    else if(command=="dpi-probe") {
        SIZE size{};
        const auto handled=SendMessageW(panel_,WM_GETDPISCALEDSIZE,
            static_cast<WPARAM>(request.at("dpi").get<unsigned>()),reinterpret_cast<LPARAM>(&size));
        return {{"handled",handled==TRUE},{"width",size.cx},{"height",size.cy}};
    }
    else if(command=="monitors") {
        Json list=Json::array();
        EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,PRECT,LPARAM param) -> BOOL {
            auto& output=*reinterpret_cast<Json*>(param);
            MONITORINFOEXW info{};info.cbSize=sizeof(info);
            if(!GetMonitorInfoW(monitor,&info)) return TRUE;
            UINT x=96,y=96;
            if(FAILED(GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&x,&y))) return TRUE;
            const auto rect=[](const RECT& value){return Json::array({value.left,value.top,value.right-value.left,value.bottom-value.top});};
            output.push_back({{"device",Narrow(info.szDevice)},{"primary",(info.dwFlags&MONITORINFOF_PRIMARY)!=0},
                {"bounds",rect(info.rcMonitor)},{"work",rect(info.rcWork)},{"dpi",x}});
            return TRUE;
        },reinterpret_cast<LPARAM>(&list));
        return {{"monitors",list}};
    }
    else if(command=="quit") PostMessageW(control_,WM_CLOSE,0,0);
    else if(command!="dump") throw std::invalid_argument("Unknown control command");
    return Dump();
}
}
