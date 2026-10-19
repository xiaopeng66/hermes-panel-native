#include "panel_view.h"
#include "render/layout.h"
#include <algorithm>
#include <cctype>
#include <set>

namespace hermes {
// styles.css .hy0..hy7 - eight cold hues keyed by the provider name (and to the hero, the group dots
// and the row tints). The index is app.js hueCls(): h = 7; h = (h * 33 + charCode) & 0xffff; h % 8.
// It must hash exactly like the old panel, or the same provider shows a different identity colour.
int HueFor(const std::string& name) {
    static const int hues[8]={186,196,206,216,228,240,252,264};
    unsigned hash=7;
    for(const unsigned char character:name) hash=(hash*33u+character)&0xffffu;
    return hues[hash%8];
}
namespace {
// The five pages of the old panel, in the same order and with the same labels (DESIGN §4.8).
struct PageDef { const char* label;const char* title;const char* note; };
const PageDef kPages[]={
    {"\xE6\xA8\xA1\xE5\x9E\x8B","\xE6\xA8\xA1\xE5\x9E\x8B","\xE6\x90\x9C\xE7\xB4\xA2\xE3\x80\x81\xE7\xAD\x9B\xE9\x80\x89\xE4\xB8\x8E\xE6\xA8\xA1\xE5\x9E\x8B\xE6\xA0\x91\xEF\xBC\x9A\xE5\x9C\xA8 H4 \xE6\x8E\xA5\xE5\x85\xA5"},
    {"\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE","\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE","\xE8\xB6\x8A\xE9\x9D\xA0\xE5\x89\x8D\xE4\xBC\x98\xE5\x85\x88\xE7\xBA\xA7\xE8\xB6\x8A\xE9\xAB\x98\xEF\xBC\x9A\xE5\x9C\xA8 H5 \xE6\x8E\xA5\xE5\x85\xA5"},
    {"\xE6\xB7\xBB\xE5\x8A\xA0","\xE6\xB7\xBB\xE5\x8A\xA0","\xE6\x96\xB0\xE5\xBB\xBA\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86 / \xE5\x8A\xA0\xE6\xA8\xA1\xE5\x9E\x8B / \xE5\x88\xA0\xE9\x99\xA4\xEF\xBC\x9A\xE5\x9C\xA8 H5 \xE6\x8E\xA5\xE5\x85\xA5"},
    {"\xE5\xA4\x87\xE4\xBB\xBD","\xE5\xA4\x87\xE4\xBB\xBD","\xE6\xAF\x8F\xE6\xAC\xA1\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE5\x90\x84\xE7\x95\x99\xE4\xB8\x80\xE4\xBB\xBD\xEF\xBC\x9A\xE5\x9C\xA8 H5 \xE6\x8E\xA5\xE5\x85\xA5"},
    {"\xE8\xAE\xBE\xE7\xBD\xAE","\xE8\xAE\xBE\xE7\xBD\xAE","\xE8\xBF\x9E\xE6\x8E\xA5\xE3\x80\x81\xE8\xA1\x8C\xE4\xB8\xBA\xE4\xB8\x8E\xE5\xBF\xAB\xE6\x8D\xB7\xE9\x94\xAE\xEF\xBC\x9A\xE5\x9C\xA8 H5 \xE6\x8E\xA5\xE5\x85\xA5"}
};
std::string Lower(std::string value) {
    for(auto& character:value)
        character=static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return value;
}
std::string KeyOf(const std::string& providerId,const std::string& model) {
    return providerId+"|"+model;
}
bool Contains(const std::vector<std::string>& list,const std::string& value) {
    return std::find(list.begin(),list.end(),value)!=list.end();
}
// main.js providerLabel: the "custom:" prefix is never shown to the user.
std::string ProviderLabel(const std::string& providerId) {
    return providerId.starts_with("custom:")?providerId.substr(7):providerId;
}
// main.js does String(text).slice(11,16) on every timestamp - "YYYY-MM-DD HH:MM:SS" -> "HH:MM".
std::string ClockText(const std::string& stamp) {
    return stamp.size()>=16?stamp.substr(11,5):(stamp.empty()?"\xE2\x80\x94":stamp);
}
}

PanelViewModel BuildPanelView(const PanelViewInput& input) {
    PanelViewModel view;
    view.tier=input.tier;
    view.lightTheme=input.lightTheme;
    view.activePage=input.activePage<0?0:(input.activePage>4?4:input.activePage);
    view.hoverControl=input.hoverControl;
    view.pinned=input.pinned;
    view.motion=input.motion;
    view.lamps.push_back({"WSL",LampState::Idle,"\xE2\x80\x94"});
    view.lamps.push_back({"WebUI",LampState::Idle,"\xE2\x80\x94"});
    view.lamps.push_back({"\xE7\xBD\x91\xE5\x85\xB3",LampState::Idle,"\xE2\x80\x94"});
    for(int index=0;index<5;++index) {
        NavItem item;
        item.label=kPages[index].label;
        item.active=index==view.activePage;
        view.nav.push_back(item);
    }
    view.pageTitle=kPages[view.activePage].title;
    view.pageNote=kPages[view.activePage].note;
    return view;
}

namespace {
// One grid row from a provider + model id, with the marks the old modelRow carried.
PanelViewModel::ModelRow MakeRow(const ProviderView& provider,const std::string& model,
    const ModelsPageInput& input,const std::string& currentKey,bool insideActiveGroup) {
    // insideActiveGroup decides what "current" means: inside the active group the key is decisive,
    // in the flat view the active provider plus its default model wins (old modelRow).
    PanelViewModel::ModelRow row;
    row.provider=provider.name;
    row.providerId=provider.providerId;
    row.model=model;
    row.key=KeyOf(provider.providerId,model);
    row.hue=HueFor(provider.name);
    // 当前行：组内看 key 命中，扁平视图看「活跃供应商 + 该供应商的 default_model」。
    row.current=insideActiveGroup?row.key==currentKey
        :(provider.isActive&&provider.defaultModel==model);
    row.favorite=Contains(input.favorites,row.key);
    row.recent=Contains(input.recents,row.key);
    if(!input.switchingKey.empty()&&row.key==input.switchingKey) row.inlineStatus=input.switchingModel;
    return row;
}
}

void AttachModelsPage(PanelViewModel& view,const ModelsPageInput& input) {
    const HermesState empty;
    const HermesState& state=input.state?*input.state:empty;
    view.loading=state.loading;
    // 助手最近一次错误：空态优先显示它（旧面板同款），dump 也靠它断言。
    view.helperError=input.helperError;
    view.filters[0]={"\xE5\x85\xA8\xE9\x83\xA8",static_cast<int>(state.providers.size()),input.filterKind==0};
    view.filters[1]={"\xE6\x94\xB6\xE8\x97\x8F",static_cast<int>(input.favorites.size()),input.filterKind==1};
    view.filters[2]={"\xE6\x9C\x80\xE8\xBF\x91",static_cast<int>(input.recents.size()),input.filterKind==2};
    view.listScroll=input.listScroll<0?0:input.listScroll;
    const auto query=Lower(input.filterText);
    const bool grouped=input.filterKind==0&&query.empty();
    view.flatView=!grouped;
    view.groups.clear();
    view.visibleRows.clear();
    view.matches=0;
    const auto currentKey=KeyOf(state.model.provider,state.model.model);
    if(grouped) {
        // 当前供应商置顶、其余按 config 顺序，两组之间插一行「其他供应商」（旧 renderTree）。
        std::vector<const ProviderView*> actives,others;
        for(const auto& provider:state.providers) {
            const bool active=provider.isActive||provider.providerId==state.model.provider;
            (active?actives:others).push_back(&provider);
        }
        const auto append=[&](const ProviderView& provider) {
            PanelViewModel::ModelGroup group;
            group.title=provider.name;
            group.providerId=provider.providerId;
            group.hue=HueFor(provider.name);
            group.active=provider.isActive||provider.providerId==state.model.provider;
            // 旧 groupHtml：open = expanded.has(name) || isActive
            group.open=group.active||Contains(input.expanded,provider.name);
            group.codex=provider.apiMode=="codex_responses";
            group.keyWarn=!provider.keyEnv.empty()&&!provider.keySet;
            // 0 不显示：等宽字体的 0 中间带点，会被看成 Θ（与回退链探测计数同一个问题）。
            group.countText=provider.models.empty()?std::string{}
                :std::to_string(provider.models.size())+(provider.modelsDiscovered?"\xE2\x81\xBA":"");
            for(const auto& model:provider.models) {
                group.rows.push_back(MakeRow(provider,model,input,currentKey,group.active));
                ++view.matches;
            }
            view.groups.push_back(group);
        };
        for(const auto* provider:actives) append(*provider);
        for(const auto* provider:others) append(*provider);
    } else {
        // 扁平视图：查询 / 收藏 / 最近的结果带固定宽度供应商列（旧 .pcol），去重后按顺序渲染。
        std::vector<std::pair<std::string,std::string>> keys;
        if(!query.empty()) {
            for(const auto& provider:state.providers) {
                const auto groupHit=Lower(provider.name).find(query)!=std::string::npos
                    ||Lower(provider.baseUrl).find(query)!=std::string::npos;
                for(const auto& model:provider.models)
                    if(groupHit||Lower(model).find(query)!=std::string::npos)
                        keys.emplace_back(provider.providerId,model);
            }
        } else {
            const auto& source=input.filterKind==1?input.favorites:input.recents;
            for(const auto& key:source) {
                const auto split=key.find('|');
                if(split!=std::string::npos) keys.emplace_back(key.substr(0,split),key.substr(split+1));
            }
        }
        std::set<std::string> seen;
        PanelViewModel::ModelGroup flat;
        flat.hue=200;
        flat.open=true;                        // a flat result list is never collapsed
        for(const auto& entry:keys) {
            if(!seen.insert(KeyOf(entry.first,entry.second)).second) continue;
            const ProviderView* provider=nullptr;
            for(const auto& candidate:state.providers)
                if(candidate.providerId==entry.first) { provider=&candidate;break; }
            if(!provider||entry.second.empty()) continue;   // 供应商已被删
            flat.rows.push_back(MakeRow(*provider,entry.second,input,currentKey,false));
            ++view.matches;
        }
        if(!flat.rows.empty()) view.groups.push_back(flat);
    }
    // 可见行序列：折叠的组不参与 ↑↓ 与 ↵（旧面板只把已渲染的 .mrow 放进游标序列）。
    for(int groupIndex=0;groupIndex<static_cast<int>(view.groups.size());++groupIndex) {
        auto& group=view.groups[static_cast<std::size_t>(groupIndex)];
        if(!group.open) {                        // 组头本身仍可见，所以组号照样在序列里占位
            for(auto& row:group.rows) row.index=-1;
            continue;
        }
        for(int rowIndex=0;rowIndex<static_cast<int>(group.rows.size());++rowIndex) {
            group.rows[static_cast<std::size_t>(rowIndex)].index=static_cast<int>(view.visibleRows.size());
            view.visibleRows.push_back({groupIndex,rowIndex});
        }
    }
    view.cursor=input.cursor>=0&&input.cursor<static_cast<int>(view.visibleRows.size())?input.cursor:-1;
    // ".hint"（旧 renderTree 末尾）：查询或非「全部」过滤时显示命中数与快捷键。
    view.showHint=!query.empty()||input.filterKind!=0;
    if(view.showHint)
        view.hintText=std::to_string(view.matches)+" \xE6\x9D\xA1\xE7\xBB\x93\xE6\x9E\x9C \xC2\xB7 "
            "\xE2\x86\x91\xE2\x86\x93 \xE9\x80\x89\xE6\x8B\xA9 \xC2\xB7 \xE2\x86\xB5 \xE5\x88\x87\xE6\x8D\xA2";
    else
        view.hintText.clear();
    // 旧页面的空态文案，逐字照搬。
    if(view.matches==0) {
        if(!query.empty())
            view.listEmpty="\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8C\xB9\xE9\x85\x8D\xE7\x9A\x84\xE6\xA8\xA1\xE5\x9E\x8B\xE6\x88\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86";
        else if(input.filterKind==1)
            view.listEmpty="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x94\xB6\xE8\x97\x8F\xE4\xBB\xBB\xE4\xBD\x95\xE6\xA8\xA1\xE5\x9E\x8B";
        else if(input.filterKind==2)
            view.listEmpty="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE5\x88\x87\xE6\x8D\xA2\xE8\xAE\xB0\xE5\xBD\x95";
        else if(state.loading)
            view.listEmpty="\xE6\xAD\xA3\xE5\x9C\xA8\xE8\xAF\xBB WSL \xE9\x87\x8C\xE7\x9A\x84 config.yaml\xE2\x80\xA6";
        else if(!input.helperError.empty())
            view.listEmpty="\xE5\x8A\xA9\xE6\x89\x8B\xE5\x87\xBA\xE9\x94\x99\xEF\xBC\x9A"
                +input.helperError+"\xEF\xBC\x88\xE7\x82\xB9\xE4\xB8\x8A\xE6\x96\xB9"
                "\xE7\x9A\x84\xE5\x9C\x86\xE7\x82\xB9\xE9\x87\x8D\xE8\xAF\x95\xEF\xBC\x89";
        else
            view.listEmpty="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE8\x87\xAA\xE5\xAE\x9A\xE4\xB9\x89\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xEF\xBC\x8C\xE5\x8E\xBB\xE3\x80\x8C\xE6\xB7\xBB\xE5\x8A\xA0\xE3\x80\x8D\xE9\xA1\xB5\xE5\xBB\xBA\xE4\xB8\x80\xE4\xB8\xAA";
    } else {
        view.listEmpty.clear();
    }
}

void AttachStatusView(PanelViewModel& view,const StatusViewInput& input) {
    const HermesState empty;
    const HermesState& state=input.state?*input.state:empty;
    const bool wslUp=state.wsl=="up",wslDown=state.wsl=="down";
    const bool webuiOk=state.webui.reachable,gatewayOk=state.gateway.reachable;
    // 三盏灯（旧 renderStatus）：up 亮 ok、down 亮 err、未回（unknown）画 idle - 不画成离线卡。
    view.lamps.clear();
    view.lamps.push_back({"WSL",wslUp?LampState::Ok:(wslDown?LampState::Err:LampState::Idle),
        wslUp?"\xE5\x9C\xA8\xE7\xBA\xBF":(wslDown?"\xE7\xA6\xBB\xE7\xBA\xBF":"\xE2\x80\xA6")});
    view.lamps.push_back({"WebUI",webuiOk?LampState::Ok:LampState::Warn,
        webuiOk?":"+std::to_string(state.webui.port):"\xE2\x80\x94"});
    view.lamps.push_back({"\xE7\xBD\x91\xE5\x85\xB3",gatewayOk?LampState::Ok:LampState::Warn,
        gatewayOk?":"+std::to_string(state.gateway.port):"\xE2\x80\x94"});
    // chip：WebUI 不可达（err，左）与「配置改过但网关没重启」（warn，右）；两条可以同时出现，
    // 只有 stale 才提示（main.js gatewayHint 的四挡里仅 stale 上 chip）。
    // 助手报错（脚本路径不对 / python 缺依赖 / 配置读不了）：一个 warn chip 点一下重试，
    // 列表的空态也会直接写原因（比"还没有自定义供应商"有用得多）。
    view.chipHelper.present=!state.error.empty()&&wslUp;
    view.chipHelper.kind=ChipKind::Warn;
    view.chipHelper.text="\xE5\x8A\xA9\xE6\x89\x8B\xE5\x87\xBA\xE9\x94\x99";
    view.chipHelper.icon="refresh-cw";
    view.chip.present=!webuiOk;
    view.chip.kind=ChipKind::Err;
    view.chip.text="WebUI \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE";
    view.chip.icon="wifi-off";
    const auto stale=state.config.mtime>0&&state.gateway.sinceUnix>0
        &&state.config.mtime>state.gateway.sinceUnix+2
        &&state.config.mtime>input.hotLoadedMtime;
    // 面板自己换模型（自动重启关）后的待重启态：寄存器行显示「重启网关」胶囊（网关灯后），
    // 不再显示「网关待重启」警告 chip——同一信息，胶囊是可点的动作。
    view.pendingGatewayRestart=input.pendingGatewayRestart;
    view.chipWarn.present=stale&&!input.pendingGatewayRestart;
    view.chipWarn.kind=ChipKind::Warn;
    view.chipWarn.text="\xE7\xBD\x91\xE5\x85\xB3\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF";
    view.chipWarn.icon="refresh-cw";
    // 寄存器行的状态项（优先级：待重启 > 配置待重启 > WebUI > 助手）。
    // 0/1 项时保持原来的单个胶囊；>=2 项交给状态中心，避免多个 chip 挤在同一行重叠。
    view.statuses.clear();
    if(view.pendingGatewayRestart)
        view.statuses.push_back({"\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3",StatusAction::RestartGateway});
    else if(view.chipWarn.present)
        view.statuses.push_back({view.chipWarn.text,StatusAction::RestartConfig});
    if(view.chip.present)
        view.statuses.push_back({view.chip.text,StatusAction::RetryWebUi});
    if(view.chipHelper.present)
        view.statuses.push_back({view.chipHelper.text,StatusAction::RetryHelper});
    view.statusCenterOpen=input.statusCenterOpen&&view.statuses.size()>=2;
    view.syncTime=ClockText(state.config.mtimeText);
    // 贴条悬浮信息卡：当前模型 + 三个服务状态 + 待处理提示 + 操作说明。
    {
        auto& card=view.hoverCard;
        card.rows.clear();
        card.model=(state.model.provider.empty()?std::string{}:state.model.provider+"/")
            +(state.model.model.empty()?"—":state.model.model);
        // 灯色语义三处同源（贴条/面板/信息卡）：ok 绿、warn 黄、err 红、**unknown 灰（探测中）**。
        card.rows.push_back({"WSL",wslUp?"在线":(wslDown?"离线":"探测中"),
            wslUp?0:(wslDown?2:3)});
        card.rows.push_back({"WebUI",webuiOk?("在线 :"+std::to_string(state.webui.port)):"不可达",
            webuiOk?0:2});
        card.rows.push_back({"网关",gatewayOk?("在线 :"+std::to_string(state.gateway.port)):"不可达",
            gatewayOk?0:2});
        card.note=view.pendingGatewayRestart?"网关待重启：点开面板后可重启"
            :(!state.error.empty()?"助手出错：点开面板后重试"
            :(view.chipWarn.present?"配置已改，网关待重启":std::string{}));
        card.sync=view.syncTime;
    }
    // 悬停提示（旧 renderStatus 里的 element.title）：**全部一句话、不换行**（用户要求）。
    view.tooltips.clear();
    {
        const auto wslTitle=wslUp?std::string("WSL：在线")
            :(wslDown?std::string("WSL：离线"):std::string("WSL：正在探测…"));
        view.tooltips.push_back({static_cast<int>(ControlId::LampWsl),-1,wslTitle});
        view.tooltips.push_back({static_cast<int>(ControlId::LampWebui),-1,
            webuiOk?std::string("WebUI：")+std::to_string(state.webui.port)+" 在线"
                :std::string("WebUI：不可达（不影响写配置）")});
        view.tooltips.push_back({static_cast<int>(ControlId::LampGateway),-1,
            gatewayOk?(stale?"网关：在线，配置改过但尚未重启":"网关：在线，配置已热加载")
                :"网关：不可达"});
        view.tooltips.push_back({static_cast<int>(ControlId::Refresh),-1,"重新读取配置并探活"});
        view.tooltips.push_back({static_cast<int>(ControlId::Chip),-1,"WebUI 不可达，点一下重新探活"});
        view.tooltips.push_back({static_cast<int>(ControlId::ChipWarn),-1,"配置改过但网关尚未重启"});
        view.tooltips.push_back({static_cast<int>(ControlId::Pin),-1,"始终置顶"});
        view.tooltips.push_back({static_cast<int>(ControlId::Snap),-1,"贴边收起成小挂件"});
        view.tooltips.push_back({static_cast<int>(ControlId::Minimize),-1,"最小化"});
        view.tooltips.push_back({static_cast<int>(ControlId::Close),-1,"隐藏到托盘"});
        view.tooltips.push_back({static_cast<int>(ControlId::ChipHelper),-1,"助手报错，点一下重试"});
        // 导航 / 模型页 / 回退链 / 添加 / 备份 / 设置：每个可点控件一句短说明。
        view.tooltips.push_back({static_cast<int>(ControlId::TabModels),-1,"模型列表：点行切换当前模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::TabFallback),-1,"回退链：主模型不可用时按顺序接管"});
        view.tooltips.push_back({static_cast<int>(ControlId::TabAdd),-1,"添加供应商或模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::TabBackup),-1,"备份与回滚配置"});
        view.tooltips.push_back({static_cast<int>(ControlId::TabSettings),-1,"设置、诊断与用量"});
        view.tooltips.push_back({static_cast<int>(ControlId::SearchBox),-1,"搜索模型或供应商（/ 聚焦）"});
        view.tooltips.push_back({static_cast<int>(ControlId::SearchClear),-1,"清空搜索"});
        view.tooltips.push_back({static_cast<int>(ControlId::FilterAll),-1,"显示全部模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::FilterFav),-1,"只显示收藏的模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::FilterRecent),-1,"只显示最近用过的模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbProbe),-1,"逐条检查回退是否可用（不消耗 token）"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbFoldToggle),-1,"追加一条回退"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbFoldClose),-1,"收起追加表单"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbSubmit),-1,"保存这条回退"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbSelect),-1,"选择来源供应商"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbUp),-1,"上移"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbDown),-1,"下移"});
        view.tooltips.push_back({static_cast<int>(ControlId::FbRemove),-1,"移除这条回退"});
        view.tooltips.push_back({static_cast<int>(ControlId::RestoreButton),-1,"回滚到这份备份"});
        view.tooltips.push_back({static_cast<int>(ControlId::BackupRefresh),-1,"重新扫描备份"});
        view.tooltips.push_back({static_cast<int>(ControlId::OpenWebUiButton),-1,"在浏览器打开 WebUI"});
        view.tooltips.push_back({static_cast<int>(ControlId::SetRestartGateway),-1,"重启网关，让机器人用上新配置"});
        view.tooltips.push_back({static_cast<int>(ControlId::SetDiagnose),-1,"查看网关、通道与日志"});
        view.tooltips.push_back({static_cast<int>(ControlId::SetUsage),-1,"查看近期 token 用量"});
        view.tooltips.push_back({static_cast<int>(ControlId::SetProfiles),-1,"保存或套用整套模型配置"});
        view.tooltips.push_back({static_cast<int>(ControlId::SubTabNew),-1,"新建供应商"});
        view.tooltips.push_back({static_cast<int>(ControlId::SubTabModels),-1,"给已有供应商加模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::SubTabDel),-1,"删除供应商"});
        view.tooltips.push_back({static_cast<int>(ControlId::ProbeButton),-1,"探测供应商的模型列表"});
        view.tooltips.push_back({static_cast<int>(ControlId::KeyReveal),-1,"显示或隐藏 API Key"});
        // 审计 F-016：异常时点灯 = 重试（DESIGN §4.2 #4）。
        view.tooltips.push_back({static_cast<int>(ControlId::LampWsl),-1,wslTitle});
        view.tooltips.push_back({static_cast<int>(ControlId::PrimaryButton),-1,"保存"});
        view.tooltips.push_back({static_cast<int>(ControlId::SwitchRowControl),-1,"点一下切换"});
        view.tooltips.push_back({static_cast<int>(ControlId::ModeSeg),-1,"选择接口模式"});
        view.tooltips.push_back({static_cast<int>(ControlId::ThemeSeg),-1,"选择主题"});
        view.tooltips.push_back({static_cast<int>(ControlId::WidgetSeg),-1,"选择贴边挂件样式"});
        view.tooltips.push_back({static_cast<int>(ControlId::LatencySeg),-1,"选择探测间隔"});
        view.tooltips.push_back({static_cast<int>(ControlId::RegisterRestartGateway),-1,"重启网关，让机器人用上新模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::OfflineStart),-1,"启动 WSL"});
        view.tooltips.push_back({static_cast<int>(ControlId::ModelChip),-1,"选中这个模型"});
        view.tooltips.push_back({static_cast<int>(ControlId::FormField),-1,"点一下编辑"});
        view.tooltips.push_back({static_cast<int>(ControlId::ModalConfirm),-1,"确定"});
        view.tooltips.push_back({static_cast<int>(ControlId::ModalCancel),-1,"取消"});
    }
    // 当前模型铭牌（旧 renderCurrent）：供应商身份色、供应商名（去掉 custom:）、模型 id 与 api_mode。
    view.heroHue=static_cast<float>(HueFor(ProviderLabel(state.model.provider).empty()
        ?"x":ProviderLabel(state.model.provider)));
    view.currentProvider=state.model.provider.empty()?"\xE2\x80\x94":ProviderLabel(state.model.provider);
    view.currentModel=input.switchingModel.empty()
        ?(state.model.model.empty()?"\xEF\xBC\x88\xE6\x9C\xAA\xE8\xAE\xBE\xE7\xBD\xAE\xEF\xBC\x89":state.model.model)
        :input.switchingModel;         // 切换中：铭牌画「→ 目标模型」，箭头由渲染层上色
    view.currentSwap=!input.switchingModel.empty();
    view.currentMode=state.model.apiMode=="codex_responses"?"codex":(state.model.apiMode.empty()?"":"chat");
    view.currentBadge=wslUp&&gatewayOk?"\xE2\x97\x8F":"";
    // 底部导航的「回退链」计数徽标（旧 renderTree 里的 nav-fb-n）。
    if(view.nav.size()==5) view.nav[1].badge=static_cast<int>(state.fallbacks.size());
    view.offlineReason=wslDown?(state.error.empty()?"WSL \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE":state.error)
        :std::string();
}

// ---- 回退链页（旧 renderFallback/fbAdd 的语义）----
namespace {
// 表单错误文案与按钮文案逐字照搬旧页（fbAdd / renderFallback）。
constexpr const char* kFbEmpty="\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE\xE6\x98\xAF\xE7\xA9\xBA\xE7\x9A\x84\xE3\x80\x82"
    "\xE4\xB8\x8B\xE9\x9D\xA2\xE3\x80\x8C\xE8\xBF\xBD\xE5\x8A\xA0\xE4\xB8\x80\xE6\x9D\xA1\xE3\x80\x8D\xE5\x8A\xA0\xE8\xBF\x9B\xE5\x8E\xBB\xE3\x80\x82";
}
// ---- 回退链页（旧 renderFallback/fbAdd 的语义）----
void AttachFallbackPage(PanelViewModel& view,const FallbackPageInput& input) {
    const HermesState empty;
    const HermesState& state=input.state?*input.state:empty;
    auto& page=view.fallback;
    page.rows.clear();
    page.title="\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE";
    page.subtitle="\xE8\xB6\x8A\xE9\x9D\xA0\xE5\x89\x8D\xE4\xBC\x98\xE5\x85\x88\xE7\xBA\xA7\xE8\xB6\x8A\xE9\xAB\x98";
    page.probeLabel="\xE9\x80\x90\xE6\x9D\xA1\xE6\x8E\xA2\xE6\xB5\x8B";
    page.addLabel="\xE8\xBF\xBD\xE5\x8A\xA0\xE4\xB8\x80\xE6\x9D\xA1";
    page.buttonLabel="\xE8\xBF\xBD\xE5\x8A\xA0\xE5\x88\xB0\xE9\x93\xBE\xE5\xB0\xBE";
    page.emptyText="\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE\xE6\x98\xAF\xE7\xA9\xBA\xE7\x9A\x84\xE3\x80\x82"
        "\xE4\xB8\x8B\xE9\x9D\xA2\xE3\x80\x8C\xE8\xBF\xBD\xE5\x8A\xA0\xE4\xB8\x80\xE6\x9D\xA1\xE3\x80\x8D\xE5\x8A\xA0"
        "\xE8\xBF\x9B\xE5\x8E\xBB\xE3\x80\x82";
    page.note="\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE8\x87\xAA\xE5\x8A\xA8\xE5\xA4\x87\xE4\xBB\xBD config.yaml "
        "\xE4\xB8\x8E .env";
    page.formOpen=input.formOpen;
    page.selectOpen=input.selectOpen;
    page.busy=input.busy;
    page.error=input.error;
    page.modelValue=input.model;
    page.providerValue=input.provider;
    page.urlValue=input.baseUrl;
    page.keyEnvValue=input.keyEnv;
    page.keyValue=input.key;
    for(const auto& entry:state.fallbacks) {
        FallbackRowView row;
        row.provider=entry.provider;
        row.model=entry.model;
        row.baseUrl=entry.baseUrl;
        row.keyEnv=entry.keyEnv;
        row.keyWarn=!entry.keyEnv.empty()&&!entry.keySet;
        page.rows.push_back(row);
    }
    // 下拉：首项是「手填 provider + base_url」，其余是当前 config 里的供应商（旧 providerOptions）。
    page.sources.clear();
    page.sources.push_back("\xE2\x80\x94 \xE6\x89\x8B\xE5\xA1\xAB provider + base_url \xE2\x80\x94");
    for(const auto& provider:state.providers) page.sources.push_back(provider.name);
    page.source=std::clamp(input.source,0,static_cast<int>(page.sources.size())-1);
    page.sourceScroll=std::max(0,input.sourceScroll);
}
// ---- H5b/H5c：添加 / 备份 / 设置三页（旧 panel-add / panel-backup / panel-settings）----
namespace {
// 标签与文案逐字照搬旧页。
constexpr const char* kSubNew="\xE6\x96\xB0\xE5\xBB\xBA\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86";
constexpr const char* kSubModels="\xE5\x8A\xA0\xE6\xA8\xA1\xE5\x9E\x8B";
constexpr const char* kSubDel="\xE5\x88\xA0\xE9\x99\xA4\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86";
constexpr const char* kNoProbe="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x8E\xA2\xE6\xB5\x8B";
}
bool ProviderDeletable(const HermesState& state,const std::string& name) {
    if(name.empty()) return false;
    for(const auto& provider:state.providers) {
        if(provider.name!=name) continue;
        // 名字大小写可能与 id 不同；provider_id / model.provider 两边都过 ProviderLabel。
        const auto current=Lower(ProviderLabel(state.model.provider));
        return !provider.isActive&&Lower(ProviderLabel(provider.providerId))!=current;
    }
    return false;
}
void AttachAddPage(PanelViewModel& view,const AddPageInput& input) {
    auto& page=view.add;
    page=AddPageView{};
    page.title="\xE6\xB7\xBB\xE5\x8A\xA0";
    page.subs={kSubNew,kSubModels,kSubDel};
    page.sub=std::clamp(input.sub,0,2);
    page.busy=input.busy;
    page.error=input.error;
    page.info=input.info;
    page.keyRevealed=input.keyRevealed;
    page.chips.clear();
    for(const auto& model:input.chips) page.chips.push_back({model});
    page.countText=input.chips.empty()?kNoProbe:
        (std::to_string(input.chips.size())+" \xE4\xB8\xAA\xE6\xA8\xA1\xE5\x9E\x8B");
    page.summary="\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE8\x87\xAA\xE5\x8A\xA8\xE5\xA4\x87\xE4\xBB\xBD config.yaml "
        "\xE4\xB8\x8E .env";
    const HermesState empty;
    const HermesState& state=input.state?*input.state:empty;
    page.providers.clear();
    for(const auto& provider:state.providers) page.providers.push_back(provider.name);
    page.provider=std::clamp(input.provider,0,std::max(0,static_cast<int>(page.providers.size())-1));
    page.providerScroll=std::max(0,input.providerScroll);
    // 供应商下拉的展开态（加模型 / 删除两个子页共用同一个控件）。
    page.selectOpen=input.selectOpen;
    page.selectField=input.selectField;
    if(page.sub==0) {
        // 新建供应商（旧 sub-new）：名称/Key 变量名 一行两列，Base URL，API Key + 眼睛，
        // 接口模式分段，模型药丸 + 手动输入，两个开关，底部探测/保存。
        page.fields={
            {"\xE5\x90\x8D\xE7\xA7\xB0",input.name,"my-relay",{},0,false,0,1},
            {"Key \xE5\x8F\x98\xE9\x87\x8F\xE5\x90\x8D",input.keyEnv,"\xE8\x87\xAA\xE5\x8A\xA8\xE6\x8E\xA8\xE5\xAF\xBC",{},0,false,1,2},
            {"Base URL",input.url,"https://api.example.com/v1",{},0,false,2,0},
            {"API Key",input.key,"sk-\xE2\x80\xA6",
                input.keyEnv.empty()?"\xE5\x8F\xAA\xE5\x86\x99 .env\xEF\xBC\x8C\xE4\xB8\x8D\xE5\x9B\x9E\xE6\x98\xBE"
                    :"\xE5\x86\x99\xE5\x85\xA5 .env \xE7\x9A\x84 "+input.keyEnv,
                1,!input.keyRevealed,3,0},
            // 审计 F-007：旧版 #np-models 的「手动输入/编辑」多行框。探测结果也写进这里，可继续编辑；
            // 药丸只是同一份数据的快捷删除视图。
            {"\xE6\xA8\xA1\xE5\x9E\x8B",input.modelsNew,"deepseek-v4-flash",
                "\xE6\xAF\x8F\xE8\xA1\x8C\xE4\xB8\x80\xE4\xB8\xAA",4,false,4,0}};
        page.keyEnvPreview=page.fields[3].note;
        // showCount=false：接口模式是模式切换不是筛选，计数没有意义（显示成 0 像个怪符号）。
        page.modes={{"chat_completions",0,input.mode==0,false},{"codex_responses",0,input.mode==1,false}};
        page.mode=std::clamp(input.mode,0,1);
        page.switches={{"\xE4\xBF\x9D\xE5\xAD\x98\xE5\x89\x8D\xE6\x8E\xA2\xE6\xB5\x8B\xE6\xA8\xA1\xE5\x9E\x8B"
            "\xE5\x88\x97\xE8\xA1\xA8",input.probeBefore,0},
            {"\xE4\xBF\x9D\xE5\xAD\x98\xE5\x90\x8E\xE8\xAE\xBE\xE4\xB8\xBA\xE5\xBD\x93\xE5\x89\x8D\xE6\xA8\xA1\xE5\x9E\x8B",
                input.setMainAfter,1}};
        if(page.chips.empty())
            page.chipNote="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x8E\xA2\xE6\xB5\x8B\xE3\x80\x82\xE7\x82\xB9\xE3\x80\x8C"
                "\xE6\x8E\xA2\xE6\xB5\x8B\xE3\x80\x8D\xE6\x8B\x89\xE6\xA8\xA1\xE5\x9E\x8B\xE5\x88\x97\xE8\xA1\xA8"
                "\xEF\xBC\x8C\xE6\x88\x96\xE6\x89\x8B\xE5\x8A\xA8\xE8\xBE\x93\xE5\x85\xA5\xE3\x80\x82";
    } else if(page.sub==1) {
        // 加模型（旧 sub-models）：供应商下拉 + 模型 id 多行 + 从供应商探测填充 / 追加
        page.fields={
            {"\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",
                page.providers.empty()?"":page.providers[static_cast<std::size_t>(page.provider)],
                {},{},3,false,0,0},
            {"\xE6\xA8\xA1\xE5\x9E\x8B id",input.modelsText,"deepseek-v4-flash",
                "\xE6\xAF\x8F\xE8\xA1\x8C\xE4\xB8\x80\xE4\xB8\xAA",4,false,1,0}};
    } else {
        // 删除供应商：危险卡片 + **供应商下拉**（用户要求：不要手输名字）。旧版是「输入名称确认」，
        // 那道防误删的护栏现在由「确定删除」模态承担（选谁删谁，弹窗里再确认一次）。
        page.danger=true;
        const auto selected=std::find(page.providers.begin(),page.providers.end(),input.dangerName)
            !=page.providers.end()?input.dangerName:std::string{};
        page.fields={{"\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",selected,
            "\xE9\x80\x89\xE6\x8B\xA9\xE8\xA6\x81\xE5\x88\xA0\xE9\x99\xA4\xE7\x9A\x84\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",
            {},3,false,0,0}};
        page.switches={{"\xE5\x90\x8C\xE6\x97\xB6\xE6\xB8\x85\xE9\x99\xA4 .env \xE9\x87\x8C\xE7\x9A\x84 API Key",
            input.purgeEnv,0},
            {"\xE4\xB8\x80\xE5\xB9\xB6\xE7\xA7\xBB\xE9\x99\xA4\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE\xE9\x87\x8C\xE5\xAF\xB9"
                "\xE5\xAE\x83\xE7\x9A\x84\xE5\xBC\x95\xE7\x94\xA8",input.dropFallbacks,1}};
        page.chipNote="\xE5\xBD\x93\xE5\x89\x8D\xE6\xA8\xA1\xE5\x9E\x8B\xE6\xAD\xA3\xE5\x9C\xA8\xE7\x94\xA8"
            "\xE7\x9A\x84\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xE5\x88\xA0\xE4\xB8\x8D\xE6\x8E\x89\xEF\xBC\x8C"
            "\xE5\x85\x88\xE5\x88\x87\xE8\xB5\xB0\xE3\x80\x82";
        // 删除按钮只在**选中了某个供应商**时才可用（旧版是"名字输对了才可用"）：当前正在用的
        // 那个也禁用（旧注释「当前模型正在用的供应商删不掉，先切走」）。
        page.primaryDisabled=!ProviderDeletable(state,selected);
    }
}
void AttachBackupPage(PanelViewModel& view,const BackupPageInput& input) {
    auto& page=view.backup;
    page=BackupPageView{};
    page.title="\xE5\xA4\x87\xE4\xBB\xBD";
    page.subtitle="\xE6\xAF\x8F\xE6\xAC\xA1\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE5\x90\x84\xE7\x95\x99\xE4\xB8\x80\xE4\xBB\xBD";
    page.refreshLabel="\xE5\x88\xB7\xE6\x96\xB0";
    page.busy=input.busy;
    page.emptyText="\xE8\xBF\x98\xE6\xB2\xA1\xE6\x9C\x89\xE5\xA4\x87\xE4\xBB\xBD\xEF\xBC\x88\xE9\x9D\xA2"
        "\xE6\x9D\xBF\xE6\xAF\x8F\xE6\xAC\xA1\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE9\x83\xBD\xE4\xBC\x9A\xE5\x90\x84"
        "\xE7\x95\x99\xE4\xB8\x80\xE4\xBB\xBD\xEF\xBC\x89";
    if(!input.error.empty()) page.emptyText=input.error;
    page.footNote="\xE5\x9B\x9E\xE6\xBB\x9A\xE5\x89\x8D\xE4\xBC\x9A\xE6\x8A\x8A\xE5\xBD\x93\xE5\x89\x8D"
        "\xE5\x86\x85\xE5\xAE\xB9\xE5\x86\x8D\xE5\xA4\x87\xE4\xBB\xBD\xE4\xB8\x80\xE4\xBB\xBD\xEF\xBC\x8C"
        "\xE6\x89\x80\xE4\xBB\xA5\xE5\x8F\xAF\xE4\xBB\xA5\xE5\x86\x8D\xE5\x9B\x9E\xE6\xBB\x9A\xE5\x9B\x9E"
        "\xE6\x9D\xA5 \xC2\xB7 \xE6\xAF\x8F\xE7\xB1\xBB\xE6\x9C\x80\xE5\xA4\x9A\xE4\xBF\x9D\xE7\x95\x99 100 \xE4\xBB\xBD";
    page.total=static_cast<int>(input.entries.size());
    page.filter=std::clamp(input.filter,0,2);
    page.filters.push_back({"\xE5\x85\xA8\xE9\x83\xA8",0,page.filter==0});
    page.filters.push_back({"config",0,page.filter==1});
    page.filters.push_back({".env",0,page.filter==2});
    int counts[3]{0,0,0};
    for(const auto& entry:input.entries) {
        ++counts[0];
        if(entry.kind=="env") ++counts[2];
        else ++counts[1];
        const auto wanted=page.filter==0||(page.filter==1&&entry.kind!="env")
            ||(page.filter==2&&entry.kind=="env");
        if(!wanted) continue;
        BackupPageView::Row row;
        row.file=entry.file;
        row.kind=entry.kind;
        row.stamp=entry.stamp;
        row.sizeKb=entry.sizeKb;
        // 旧 renderBackups 的 note：去掉「config-YYYYMMDD_HHMMSS-」前缀后剩下的就是备注，空则是"手动"。
        row.note="\xE6\x89\x8B\xE5\x8A\xA8";
        page.rows.push_back(row);
    }
    // 旧备份页的筛选只有「全部 <b>N</b>」带计数：config / .env 是纯文字（多出来的数字不对版）。
    page.filters[0].count=counts[0];
    page.filters[0].showCount=true;
    page.filters[1].count=counts[1];
    page.filters[1].showCount=false;
    page.filters[2].count=counts[2];
    page.filters[2].showCount=false;
    if(page.rows.size()>60) page.rows.resize(60);
}
void AttachSettingsPage(PanelViewModel& view,const SettingsPageInput& input) {
    auto& page=view.settings;
    page=SettingsPageView{};
    page.title="\xE8\xAE\xBE\xE7\xBD\xAE";
    page.sections={"\xE8\xBF\x9E\xE6\x8E\xA5","\xE8\xA1\x8C\xE4\xB8\xBA",
        "\xE5\xBF\xAB\xE6\x8D\xB7\xE9\x94\xAE","\xE5\x85\xB6\xE4\xBB\x96"};
    page.fields={
        {"WSL \xE5\x8F\x91\xE8\xA1\x8C\xE7\x89\x88",input.distro,{}, {},0,false,0,0},
        {"WebUI \xE7\xAB\xAF\xE5\x8F\xA3",std::to_string(input.webuiPort),{},
            "\xE4\xBE\x9D\xE6\xAC\xA1\xE8\xAF\x95 8787 \xE2\x86\x92 9119",2,false,1,0},
        {"\xE7\x8A\xB6\xE6\x80\x81\xE5\x88\xB7\xE6\x96\xB0\xE9\x97\xB4\xE9\x9A\x94",std::to_string(input.pollSeconds),
            {}, "\xE7\xA7\x92",2,false,2,0},
        {"\xE5\x85\xB3\xE9\x97\xAD\xE6\x8C\x89\xE9\x92\xAE",
            input.closeAction=="quit"?"\xE9\x80\x80\xE5\x87\xBA\xE7\xA8\x8B\xE5\xBA\x8F":"\xE9\x9A\x90\xE8\x97\x8F\xE5\x88\xB0\xE6\x89\x98\xE7\x9B\x98",
            {}, {}, 3,false,3,0},
        {"\xE5\x91\xBC\xE5\x87\xBA / \xE6\x94\xB6\xE8\xB5\xB7\xE9\x9D\xA2\xE6\x9D\xBF",input.hotkey,"Control+Alt+H",
            input.hotkeyCapturing?"\xE7\x8E\xB0\xE5\x9C\xA8\xE6\x8C\x89\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE\xE2\x80\xA6"
                :"\xE7\x82\xB9\xE8\xBE\x93\xE5\x85\xA5\xE6\xA1\x86\xE5\x90\x8E\xE6\x8C\x89\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE",
            0,false,4,0}};
    // 关闭按钮下拉的展开列表（真机反馈：点一下就直接换值了，应该像别的下拉一样弹出列表再选）。
    page.closeOptions={"\xE9\x9A\x90\xE8\x97\x8F\xE5\x88\xB0\xE6\x89\x98\xE7\x9B\x98",
        "\xE9\x80\x80\xE5\x87\xBA\xE7\xA8\x8B\xE5\xBA\x8F"};
    page.closeSelectOpen=input.closeSelectOpen;
    page.closeActive=input.closeAction=="quit"?1:0;
    page.switches={
        {"\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE5\x85\x88\xE7\x9C\x8B diff","dry_run \xE9\xA2\x84\xE6\xBC\x94",
            input.previewDiff,0},
        {"\xE8\x87\xAA\xE5\x8A\xA8\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3",
            "\xE5\xBC\x80\xEF\xBC\x9A\xE5\x88\x87\xE6\x8D\xA2\xE5\x90\x8E"
            "\xE8\x87\xAA\xE5\x8A\xA8\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3\xEF\xBC\x9B"
            "\xE5\x85\xB3\xEF\xBC\x9A\xE6\xA0\x87\xE8\xAE\xB0\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF",
            input.autoReloadGateway,1},
        {"\xE5\xA7\x8B\xE7\xBB\x88\xE7\xBD\xAE\xE9\xA1\xB6",{},input.alwaysOnTop,2},
        {"\xE5\xBB\xB6\xE8\xBF\x9F\xE6\x8E\xA2\xE6\xB5\x8B",
            "\xE6\xAF\x8F\xE9\x9A\x94\xE4\xB8\x80\xE6\xAE\xB5\xE7\xBB\x8F\xE7\xBD\x91\xE5\x85\xB3"
            "\xE5\x8F\x91\xE4\xB8\x80\xE6\xAC\xA1 1-token \xE8\xAF\xB7\xE6\xB1\x82",
            input.latencyProbe,3}};
    // 延迟探测间隔（5/15/60 分钟）：三选一，只在开关打开时生效。
    page.latencySegments={{"5 \xE5\x88\x86\xE9\x92\x9F",0,input.latencyMinutes==5,false},
        {"15 \xE5\x88\x86\xE9\x92\x9F",0,input.latencyMinutes==15,false},
        {"60 \xE5\x88\x86\xE9\x92\x9F",0,input.latencyMinutes==60,false}};
    page.latency=input.latencyMinutes==15?1:(input.latencyMinutes==60?2:0);
    // G11 贴边挂件：三套窄型 + 胶囊（与几何的 WidgetStyle 顺序一致）。
    page.widgetSegments={{"\xE4\xBF\xA1\xE6\xA0\x87",0,input.widgetStyle=="beacon",false},
        {"\xE9\x93\xAD\xE7\x89\x8C",0,input.widgetStyle=="tag",false},
        {"\xE6\x97\x97\xE6\xA0\x87",0,input.widgetStyle=="flag",false},
        {"\xE8\x83\xB6\xE5\x9B\x8A",0,input.widgetStyle=="capsule",false}};
    page.widget=input.widgetStyle=="tag"?1:(input.widgetStyle=="flag"?2
        :(input.widgetStyle=="capsule"?3:0));
    // 主题分段：旧面板三档（深色/浅色/跟随系统）+ 两套拟态主题（用户要求"高级拟态"）。
    // 六套主题 + 跟随系统：深色/浅色是旧面板原样，后四套是现代化候选（用户要求"四套都要"）。
    // 顺序固定：深色 · 浅色 · 跟随系统 · 石板 · 碳黑 · 瓷白 · 沙丘。
    // showCount=false：主题是选择器不是筛选，计数 0 显示出来像多余的符号（真机反馈）。
    page.themes={{"\xE6\xB7\xB1\xE8\x89\xB2",0,input.theme=="dark",false},
        {"\xE6\xB5\x85\xE8\x89\xB2",0,input.theme=="light",false},
        {"\xE8\xB7\x9F\xE9\x9A\x8F\xE7\xB3\xBB\xE7\xBB\x9F",0,input.theme=="system"||input.theme.empty(),false},
        {"\xE7\x9F\xB3\xE6\x9D\xBF",0,input.theme=="slate",false},
        {"\xE7\xA2\xB3\xE9\xBB\x91",0,input.theme=="carbon",false},
        {"\xE7\x93\xB7\xE7\x99\xBD",0,input.theme=="porcelain",false},
        {"\xE6\xB2\x99\xE4\xB8\x98",0,input.theme=="sand",false}};
    page.theme=input.theme=="dark"?0:(input.theme=="light"?1:(input.theme=="slate"?3
        :(input.theme=="carbon"?4:(input.theme=="porcelain"?5:(input.theme=="sand"?6:2)))));
    page.hotkeyHint=input.hotkeyCapturing?"\xE7\x8E\xB0\xE5\x9C\xA8\xE6\x8C\x89\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE\xE2\x80\xA6"
        :(!input.hotkeyRegistered
            ?"\xE6\xB3\xA8\xE5\x86\x8C\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A\xE5\x8F\xAF\xE8\x83\xBD\xE8\xA2\xAB"
                "\xE5\x85\xB6\xE4\xBB\x96\xE7\xA8\x8B\xE5\xBA\x8F\xE5\x8D\xA0\xE7\x94\xA8\xEF\xBC\x88\xE9\x94\x99"
                "\xE8\xAF\xAF "+std::to_string(input.hotkeyError)+"\xEF\xBC\x89"
            :"\xE7\x82\xB9\xE8\xBE\x93\xE5\x85\xA5\xE6\xA1\x86\xE5\x90\x8E\xE6\x8C\x89\xE7\xBB\x84\xE5\x90\x88\xE9\x94\xAE");
    page.hotkeyCapturing=input.hotkeyCapturing;
    page.openWebUiLabel="\xE5\x9C\xA8\xE6\xB5\x8F\xE8\xA7\x88\xE5\x99\xA8\xE6\x89\x93\xE5\xBC\x80";
    page.webUiRowLabel="WebUI";    // 旧 <label>WebUI</label>：这一行是「标签 + ghost 按钮」
    page.meta=input.meta;
    page.footNote="\xE8\xAE\xBE\xE7\xBD\xAE\xE4\xB8\x8E\xE6\x97\xA5\xE5\xBF\x97\xE5\x9C\xA8\xE6\x95\xB0"
        "\xE6\x8D\xAE\xE7\x9B\xAE\xE5\xBD\x95\xEF\xBC\x9B\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE9\x83\xBD\xE4\xBC\x9A"
        "\xE8\x87\xAA\xE5\x8A\xA8\xE5\xA4\x87\xE4\xBB\xBD\xE3\x80\x82";
}
}
