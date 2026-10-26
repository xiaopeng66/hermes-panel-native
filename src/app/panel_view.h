#pragma once
#include <string>
#include <vector>
#include "core/geometry.h"
#include "data/state.h"

namespace hermes {
// What the panel shows. Pure: no window, no D2D, no clock and no WSL of its own - the caller passes the
// helper's answers and the pointer state in, so the whole presentation contract is unit testable.
//
// Structure mirrors the old page (DESIGN §4): a title bar, a lamp register (WSL / WebUI / 网关), the
// current-model hero, the active page, and the bottom navigation. H2a built the chrome; H4 fills the
// models page (search row, filter segments, grouped list, star, click-to-switch).
// app.js hueCls(): the provider identity hue for styles.css .hy0..hy7.
int HueFor(const std::string& name);
enum class LampState { Idle,Ok,Warn,Err };
enum class ChipKind { Ok,Warn,Err };
// 悬停文字提示（旧版每个元素一个 title 属性）：id + index 指明是哪个控件，text 是气泡文案。
// 文本里的换行按行画；矩形由外壳显示时从布局取，渲染层只负责画。
struct TooltipView {
    int control=0;                 // ControlId 的数值
    int index=-1;                  // 重复控件的序号（列表行/分段/字段）
    std::string text;
};
struct LampView {
    std::string label;                 // WSL / WebUI / 网关
    LampState state=LampState::Idle;
    std::string detail;                // short right-hand text, e.g. port or "—"
};
struct ChipView { bool present=false;ChipKind kind=ChipKind::Err;std::string text;std::string icon; };
struct NavItem { std::string label;int badge=-1;bool active=false; };
// 一个分段控件（.seg）：文案 + 计数 + 选中态。
struct FilterSeg {
    std::string label;
    int count=0;
    bool active=false;
    // 旧备份页的筛选只有「全部 <b>N</b>」带计数，config / .env 是纯文字：显示与否是视图的决定，
    // 计数本身仍然算（测试断言的是数据）。
    bool showCount=true;
};
// ---- 回退链页（DESIGN §4.4 #14-#17）：优先级阶梯 + 折叠追加表单 + 逐条探测 ----
struct FallbackRowView {
    std::string provider,model,baseUrl,keyEnv;     // provider / model / base_url / key_env
    bool keyWarn=false;                            // key_env 设了但 key_set 为假：缺 Key
    bool hasProbe=false,probeOk=false;             // 逐条探测的结果（旧 fbProbe 数组）
    bool probeHasModel=false;                      // 连上了，且列表里含这条回退的模型
    bool probePending=false;                       // 正在探测这一行（显示 … 而不是空）
    int probeCount=0;
    std::string probeDetail;                       // "N 个模型，含该模型" / 错误文本
};
struct FallbackPageInput {
    const HermesState* state=nullptr;
    bool formOpen=false;                           // 追加表单（页内浮层卡片）
    bool selectOpen=false;                         // 来源供应商下拉是否展开
    std::string model;                             // 模型下拉的值
    int source=0;                                  // 下拉选中：直接是 providers 的下标（v1.25 起无手填项）
    int sourceScroll=0;                            // 下拉列表滚动量（选项多于可视行数时）
    bool modelSelectOpen=false;                    // v1.16：模型 ▾ 展开
    std::vector<std::string> modelOptions;         // v1.16：模型 ▾ 候选（一键探测缓存 → 供应商 models）
    std::string error;                             // 表单错误行（旧 #fb-err）
    bool busy=false;                               // 追加/探测进行中
};
struct FallbackPageView {
    std::vector<FallbackRowView> rows;
    std::string title,subtitle,probeLabel,addLabel,emptyText,note,buttonLabel,error;
    std::string modelValue;   // 模型下拉的选中项
    bool formOpen=false,selectOpen=false,busy=false;
    bool probeRunning=false;                       // 逐条探测进行中：按钮文案显示进度并禁用重复触发
    int source=0,sourceScroll=0;
    std::vector<std::string> sources;              // 下拉选项，首项是「— 手填 provider + base_url —」
    bool modelSelectOpen=false;                    // v1.16：模型 ▾ 展开态
    std::vector<std::string> modelOptions;         // v1.16：模型 ▾ 选项（空 = 还没探测）
};
// 一个可编辑的表单字段（新建供应商 / 加模型 / 删除 / 设置共用）：标签 + 值 + 提示 + 右侧小字。
struct FormFieldView {
    std::string label,value,hint,note;             // note 是标签右侧的小字（旧 <code>）
    int kind=0;                                     // 0 文本 / 1 密码 / 2 数字 / 3 下拉 / 4 多行 / 5 分段
    bool masked=false;
    int index=-1;                                   // 字段在页内的序号（点击/键盘路由用）
    int column=0;                                   // 0 独占一行，1/2 是 .with 里的左右两列
};
struct AddPageView {
    std::string title;
    int sub=0;                                     // 子页：0 新建供应商 / 1 加模型 / 2 删除供应商
    bool selectOpen=false;                         // 供应商下拉是否展开
    int selectField=0;                             // 展开的是第几个字段（0 = 供应商）
    std::vector<std::string> subs;                 // 子页标签（新建供应商 / 加模型 / 删除）
    std::vector<FormFieldView> fields;
    std::vector<std::string> providers;            // 下拉里的供应商名（加模型用）
    int provider=0,providerScroll=0;
    struct ChipRef { std::string model;bool selected=true; };   // 模型列表的一行（点一下切换选中）
    std::vector<ChipRef> chips;
    int modelScroll=0;                             // 列表滚动量（首行下标）
    int hoverIndex=-1;                             // 悬停的列表行号
    struct SwitchRow { std::string label;bool on=false;int index=-1; };
    std::vector<SwitchRow> switches;
    std::vector<FilterSeg> modes;                  // api_mode 分段（chat_completions / codex_responses）
    int mode=0;
    std::string chipNote,summary,countText,keyEnvPreview,error,info,footNote;
    bool keyRevealed=false,busy=false,danger=false;
    bool primaryDisabled=false;                    // 删除页：名称没匹配上时按钮不可用（旧 disabled）
};
struct BackupPageView {
    struct Row { std::string file,kind,note,stamp;int sizeKb=0; };
    std::vector<Row> rows;
    std::vector<FilterSeg> filters;                    // 全部 / config / env（带计数）
    int filter=0,total=0;
    std::string title,subtitle,refreshLabel,note,emptyText,busyText,footNote;
    bool busy=false;
};
struct SettingsPageView {
    std::vector<std::string> sections;                 // 连接 / 行为 / 快捷键 / 其他
    std::vector<FormFieldView> fields;                 // 每行的输入（kind 3 = 下拉；note 是右侧小字）
    struct SwitchRow { std::string label,note;bool on=false;int index=-1; };
    std::vector<SwitchRow> switches;
    std::vector<FilterSeg> themes;                     // 深色 / 浅色 / 跟随系统
    int theme=2;
    // F4：延迟探测间隔（5/15/60 分钟），与主题分段同一套绘制。
    std::vector<FilterSeg> latencySegments;
    int latency=0;
    // G1：贴边挂件样式（胶囊/徽章/光环/声波/磁贴）。
    std::vector<FilterSeg> widgetSegments;
    int widget=0;
    // 关闭按钮下拉的展开列表（旧 select 点开是个列表，不是点一下就换值）。
    bool closeSelectOpen=false;
    std::vector<std::string> closeOptions;             // 隐藏到托盘 / 退出程序
    int closeActive=0;                                 // 当前值在 closeOptions 里的下标
    std::string title,hotkeyHint,openWebUiLabel,meta,footNote,error;
    bool hotkeyCapturing=false;                         // 热键框正在"按组合键"（绘制层据此高亮）
    std::string webUiRowLabel;                          // 「其他」里那一行的行标签（旧 <label>WebUI</label>）
};
// 模态（DESIGN §4.8 #33）：宽 min(304, 面板宽−22)，Enter 确定 / Esc 取消 / 点遮罩不关。
struct ModalView {
    std::string title,body,confirmLabel="\xE7\xA1\xAE\xE5\xAE\x9A";
    std::string cancelLabel="\xE5\x8F\x96\xE6\xB6\x88";
    bool danger=false;                 // 危险确认：确认按钮走 danger 皮肤
    // 分点行（旧版信息卡就是多行 <div>）：每条一行、左对齐，渲染层按行画。
    // 比把 base_url / 密钥 / 模型数挤成一段换行清楚得多（真机反馈"供应商界面很丑，能不能分点换行"）。
    std::vector<std::string> facts;
    // F5 配置档案管理：每个档案一行（名称 + 摘要），行尾是可点的「应用 / 删除」。
    struct ProfileRow { std::string name,detail; };
    std::vector<ProfileRow> profiles;
};
// Toast（DESIGN §4.8 #34）：显示 2.6s，条内文案 + ok/err 边框色。
struct ToastView { int kind=0;std::string text; };   // 0 普通 / 1 ok / 2 err
// 寄存器行的状态项：0/1 个时保持原来的单个胶囊（重启网关/单个 chip），>=2 个合并成
// 「N 项状态」胶囊并弹出状态中心，彻底避免多个 chip 互相重叠。
enum class StatusAction { None, RestartGateway, RestartConfig, RetryWebUi, RetryHelper };
struct StatusItemView { std::string label; StatusAction action=StatusAction::None; };
// 贴条悬浮信息卡（独立分层小窗）：一行一个服务状态 + 当前模型 + 操作提示。
struct HoverCardRowView { std::string label, detail; int state=0; };  // state 0 ok / 1 warn / 2 err / 3 idle
struct HoverCardView {
    std::string model;                 // provider/model
    std::string hint;                  // 单击展开 · 拖动移动
    std::string note;                  // 待重启/助手出错（可空）
    std::string sync;                  // 上次同步时间
    std::vector<HoverCardRowView> rows;
};
// 动效状态（只影响绘制，不影响模型/几何/命中）。alpha=1、offset=0 即"无动画"。
// 移植自 balance-panel-native：统一的 ease-out 进度 + 动效计时器 + 跟随系统"显示动画"开关。
struct MotionView {
    float panelAlpha=1.0f;        // 面板内容透明度（入场/内容切换）
    float panelOffsetDip=0.0f;    // 面板内容垂直位移（DIP，正=向下）
    float cardAlpha=1.0f;         // 贴条悬浮卡透明度
    float cardOffsetDip=0.0f;     // 悬浮卡内容垂直位移（DIP，正=向下）
    float stripAlpha=1.0f;        // 贴条淡入（收起瞬间贴条出现）
    float capsuleDocked=1.0f;     // 胶囊"贴边度"0..1：1=贴边（贴边侧直角）、0=浮起（完整椭圆）
    float tooltipAlpha=1.0f;      // 面板内提示气泡透明度
    int hoverPrevious=0;          // 上一个 hover 控件（淡出）
    float hoverAmount=1.0f;       // hover 过渡进度（0..1）
};
// 液态玻璃的背景快照（窗口像素，BGRA，已模糊）：只给胶囊贴条用。
// 指针/尺寸由 Surface 持有，视图只借用；generation 变化时渲染层重建 D2D 位图。
struct BackdropView {
    const unsigned int* pixels=nullptr;
    int width=0;
    int height=0;
    unsigned generation=0;
};
struct PanelViewModel {
    Tier tier=Tier::Full;
    bool lightTheme=false;
    std::string title="Hermes";
    // lamp register
    std::vector<LampView> lamps;
    std::vector<TooltipView> tooltips;   // 悬停提示（chrome + 当前页的控件）
    std::vector<StatusItemView> statuses;
    bool statusCenterOpen=false;         // 状态中心浮层（>=2 项时由 StatusPill 打开）
    HoverCardView hoverCard;             // 贴条悬浮信息卡（独立小窗）
    BackdropView backdrop;               // 胶囊贴条的玻璃背景（可空：空=退回不透明渐变）
    MotionView motion;                   // 动效状态（绘制用；不影响模型/几何/命中）
    // 当前正在显示的气泡（外壳填 visible/text/anchor，渲染层画）。锚点用 4 个 float，
    // 这里不引 D2D1 头（panel_view 是纯视图层，widget-tests 也编它）。
    struct TooltipPopup {
        bool visible=false;
        std::string text;
        float left=0,top=0,right=0,bottom=0;
    };
    TooltipPopup tooltip;
    ChipView chipHelper;               // "助手出错" (warn, leftmost): the helper answered badly
    ChipView chip;                     // "WebUI 不可达" (err, left of the warn chip)
    ChipView chipWarn;                 // "网关待重启" (warn, right, next to the sync stamp)
    bool busy=false;
    std::string busyLabel;
    std::string syncTime="\xE2\x80\x94";   // em dash until the first refresh
    // 换模型后（自动重启关）寄存器行上的「重启网关」胶囊：点了才重启，机器人立即用上新模型。
    bool pendingGatewayRestart=false;
    // 刷新进行中同步章图标的旋转角（度，绕图标中心）；刷新成功后短暂显示对勾。
    float syncSpinAngle=0.0f;
    float busySweep=0.0f;               // 顶部不定进度条的流动相位（度，0..720 循环）
    bool syncCheck=false;
    // 同步章（右上角的刷新+探活）**自己的**动作在跑：只有它才把图标换成 accent 色，
    // 别的动作（逐条探测/备份刷新…）不许借用这个戳的任何状态（用户："根本不是一个功能"）。
    bool refreshBusy=false;
    // G1 贴边挂件：样式、贴边方向与动画相位（收起态绘制用）。
    std::string widgetStyle="capsule";   // 只剩铭牌/胶囊两套（设置层会给出真实值）
    int widgetEdge=0;                  // 0 none / 1 left / 2 right / 3 top / 4 bottom
    bool widgetVertical=true;
    // 键盘焦点环（审计 F-010）：focusControl 为 ControlId 的整数值，0 = 无焦点。
    int focusControl=0;
    int focusIndex=-1;
    // current model (hero)
    LampState heroLamp=LampState::Idle;
    float heroHue=200.0f;              // provider identity hue (.hy0..hy7); the radial hero tint
    std::string currentProvider="\xE2\x80\x94";
    std::string currentModel="\xE2\x80\x94";
    std::string currentMode;           // api_mode tag, hidden when empty
    std::string currentBadge;          // e.g. from WebUI discovery
    bool currentSwap=false;            // a set-main is in flight: the plate plays 旧 → 新
    // F4 延迟徽章：最近一次探测 + 近 5 次可用率（没有历史时 hasLatency=false）。
    bool hasLatency=false;
    int latencyMs=0;
    bool latencyOk=false;
    int latencyAvailability=-1;
    // pages
    int activePage=0;                  // 0 models, 1 fallback, 2 add, 3 backup, 4 settings
    std::vector<NavItem> nav;
    std::string pageTitle;             // shown while a page is still a placeholder
    std::string pageNote;
    // ---- models page (DESIGN §4.3, 9-13): built by AttachModelsPage from the helper state ----
    struct ModelRow {
        std::string provider,providerId,model,key;
        int hue=200;                   // provider identity hue (.hy0..hy7), inherited from the group
        bool current=false,favorite=false,recent=false;
        int index=-1;                  // position among the visible rows (the ↑↓ cursor walks these)
        std::string inlineStatus;      // e.g. "切换中…" while set-main is in flight for this row
        // F4：行尾延迟徽章（最近一次探测）。
        bool hasLatency=false;
        int latencyMs=0;
        bool latencyOk=false;
    };
    struct ModelGroup {
        std::string title,providerId;
        int hue=200;
        bool active=false,open=true,keyWarn=false,codex=false;
        std::string countText;         // gcount: models length + ⁺ when discovered
        std::vector<ModelRow> rows;
    };
    using FilterSeg=hermes::FilterSeg; // 段控件（过滤行与备份页的类型过滤共用一份定义）
    FilterSeg filters[3];              // 全部 / 收藏 / 最近，带计数
    std::vector<ModelGroup> groups;    // grouped view; a flat search result is one pseudo-group
    bool flatView=false;               // query/fav/recent rows carry a provider column
    std::string listEmpty;             // the old .empty texts
    int matches=0;
    bool searchHasText=false;          // the search box has text: the clear button shows, the / kbd hides
    // 「刷新同步」进行中的反馈：以前点了只有顶部忙碌条在动、按钮毫无变化，用户以为"卡住了"。
    bool syncBusy=false;
    int syncDone=0,syncTotal=0;        // 进度 n/N（同步是逐供应商串行/并行的）
    std::string helperError;           // 助手最近一次错误（列表空态优先显示它）
    // The visible rows in draw order: the ↑↓ cursor and ↵ walk this list, so a collapsed group's rows
    // are not reachable (the old panel only ever put rendered .mrow elements in its cursor sequence).
    struct RowRef { int group=0;int row=0; };
    std::vector<RowRef> visibleRows;
    int cursor=-1;                     // index into visibleRows, -1 = no cursor
    bool showHint=false;               // ".hint" - query or a non-全部 filter is active
    std::string hintText;              // "N 条结果 · ↑↓ 选择 · ↵ 切换"
    float listScroll=0;                // pixels scrolled inside the list viewport
    // offline replaces the whole content area (DESIGN §4.8)
    std::string offlineReason;
    bool loading=true;                 // first frame: write actions are disabled
    float pageScroll=0;                // 添加/备份/设置页的正文滚动偏移（旧 clamp 后整页滚动）
    float modalScrollX=0;              // F2/F3 模态事实行的横向滚动偏移（长行不再被省略号吃掉）
    float fallbackScroll=0;            // 回退链阶梯的内部滚动偏移（条目多时）
    int focusedFormField=-1;           // 当前获得焦点的表单字段序号（光晕 + 插入符）
    int focusedFormCaret=-1;           // 插入符在字段文本里的位置（UTF-16）；-1 = 不画
    bool focusedFormMultiline=false;   // 多行字段：插入符按行/列定位
    bool focusedFormNative=false;      // v1.23：该字段由原生 EDIT 承载（不画自绘插入符）
    // ---- 模态与 Toast：两者都是"整块浮层"，任何页面都能有 ----
    ModalView modal;
    ToastView toast;
    // ---- 其余四页的视图（H5）：每帧只填当前页的那一个 ----
    FallbackPageView fallback;
    AddPageView add;
    BackupPageView backup;
    SettingsPageView settings;
    int hoverControl=0;
    int hoverIndex=-1;      // 悬停控件的行号（模型列表这类重复控件）
    bool pinned=true;
    // Pointer position in DIP inside the panel window (for list hover: rows, group heads, stars). The
    // renderer and the click handler share one hit test, so hover and click can never disagree.
    float pointerX=0,pointerY=0;
    bool pointerInside=false;
};
struct PanelViewInput {
    Tier tier=Tier::Full;
    bool lightTheme=false;
    int activePage=0;
    int hoverControl=0;
    int hoverIndex=-1;
    bool pinned=true;
    MotionView motion;                 // 动效状态（外壳按计时器算好传进来，纯视图层不碰时钟）
};
PanelViewModel BuildPanelView(const PanelViewInput& input);
// Old renderTree semantics, measured from app.js: grouped view for 全部+无查询 (active provider first,
// then config order, "其他供应商" divider), a flat provider-column view for query/fav/recent, counts =
// providers length / favorites length / recents length, row key = provider_id|model.
struct ModelsPageInput {
    const HermesState* state=nullptr;
    std::string filterText;
    int filterKind=0;                  // 0 all / 1 fav / 2 recent
    std::vector<std::string> favorites,recents;
    std::vector<std::string> expanded; // provider names the user opened by hand (old `expanded` set)
    int cursor=-1;                     // index into visibleRows
    float listScroll=0;
    std::string switchingKey;          // the row that is mid-switch (inline status)
    std::string switchingModel;
    std::string helperError;           // 助手最近一次错误（有错时列表空态显示它）
};
void AttachModelsPage(PanelViewModel& view,const ModelsPageInput& input);
// The lamp register, the action chip, the current-model hero and the config stamp, straight off the
// helper state (old renderStatus/renderCurrent). Called for every page: the register and the hero are
// panel chrome, not part of the models page.
struct StatusViewInput {
    const HermesState* state=nullptr;
    std::string switchingModel;        // non-empty while set-main is in flight: the hero plays 旧 → 新
    long long hotLoadedMtime=0;        // settings.hotLoadedMtime, for the 网关待重启 hint
    bool pendingGatewayRestart=false;  // 换模型后（自动重启关）寄存器行上的「重启网关」胶囊
    bool statusCenterOpen=false;       // 状态中心浮层是否展开
};
void AttachStatusView(PanelViewModel& view,const StatusViewInput& input);
// The page-list host: the five pages share one content area, and each page owns exactly one view.
void AttachFallbackPage(PanelViewModel& view,const FallbackPageInput& input);
void AttachAddPage(PanelViewModel& view,const AddPageView& page);
void AttachBackupPage(PanelViewModel& view,const BackupPageView& page);
void AttachSettingsPage(PanelViewModel& view,const SettingsPageView& page);
// 添加页的输入（两子表单的字段值、开关、模型药丸、错误行；v1.18 起「加模型」子页删除）。
struct AddPageInput {
    const HermesState* state=nullptr;
    int sub=0;                                      // 0 新建供应商 / 1 删除供应商
    int mode=0;
    std::string name,keyEnv,url,key;                // 新建供应商的字段（keyEnv 只是推导结果的提示）
    bool keyRevealed=false;
    bool setMainAfter=false;                        // 「保存后设为当前模型」
    std::string dangerName;                         // 删除页选中的供应商名（下拉选中项）
    int provider=0;                                 // 删除页的供应商下拉
    int providerScroll=0;                           // 下拉列表滚动量（18 个供应商放不进 8 行）
    bool selectOpen=false;                          // 下拉是否展开
    int selectField=0;                              // 展开的是第几个字段（0 = 供应商）
    bool purgeEnv=true,dropFallbacks=false;
    std::string error,info;
    std::vector<std::string> chips;                 // 模型列表（探测结果 + 手动添加）
    std::vector<bool> chipSelected;                 // 与 chips 平行：这一行会不会写入配置
    int modelScroll=0;                              // 模型列表滚动量（首行下标）
    int hoverIndex=-1;                              // 悬停的列表行号（视图用来高亮）
    bool busy=false;
};
void AttachAddPage(PanelViewModel& view,const AddPageInput& input);
// 「这个供应商能不能删」：唯一一份规则（视图的禁用态与提交时的守卫共用，两处不许各写一套）。
// 不能删 = 找不到这个名字、或是当前模型正在用的那个（is_active / provider_id 命中都算）。
bool ProviderDeletable(const HermesState& state,const std::string& name);
// 备份页的输入（列表 + 类型过滤 + 忙态）。
struct BackupPageInput {
    struct Entry { std::string file,kind,stamp;int sizeKb=0; };
    std::vector<Entry> entries;                     // 全部备份（按新到旧）
    int filter=0;                                   // 0 全部 / 1 config / 2 env
    bool busy=false;
    std::string error;
};
void AttachBackupPage(PanelViewModel& view,const BackupPageInput& input);
// 设置页的输入（值来自 Settings + 状态）。
struct SettingsPageInput {
    std::string distro,hotkey,closeAction,theme,meta;
    int webuiPort=8787,pollSeconds=60;
    bool previewDiff=false,autoReloadGateway=false,alwaysOnTop=false;
    bool latencyProbe=false;                        // F4：延迟探测开关
    int latencyMinutes=5;                           // F4：5/15/60
    std::string widgetStyle="capsule";   // 只剩铭牌/胶囊两套（设置层会给出真实值）               // G11：贴边挂件样式
    bool hotkeyCapturing=false;                     // 热键输入框在"按组合键"状态
    bool hotkeyRegistered=true;                     // 注册是否成功（失败时设置页给出原因）
    int hotkeyError=0;                              // Win32 错误码（1409 = 已被占用）
    bool closeSelectOpen=false;                     // 关闭按钮下拉的展开列表
};
void AttachSettingsPage(PanelViewModel& view,const SettingsPageInput& input);
}
