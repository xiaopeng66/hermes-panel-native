#pragma once
#include <d2d1.h>
#include <string>
#include <vector>
#include "app/panel_view.h"

namespace hermes {
// Interactive controls. The renderer draws them and hit testing looks them up, so both always agree on
// where a control is.
enum class ControlId {
    None,Snap,Pin,Minimize,Close,
    TabModels,TabFallback,TabAdd,TabBackup,TabSettings,
    Refresh,Chip,ChipWarn,SearchBox,SearchClear,
    FilterAll,FilterFav,FilterRecent,
    // ---- H5 其余四页 ----
    FbProbe,FbFoldToggle,FbFoldClose,FbSubmit,FbSelect,   // 回退链页
    FbUp,FbDown,FbRemove,                                 // 阶梯行的三个按钮（配合 ListHitTest 的 row）
    FbProbeDot,                                           // 阶梯行的探测结果（悬停说明用，index = 行）
    SetRestartGateway,                                    // 设置页「手动重启网关」
    SetDiagnose,SetUsage,                                 // 设置页「诊断」「用量」
    LatencySeg,                                           // 设置页「延迟探测间隔」分段
    WidgetSeg,                                            // 设置页「贴边挂件」分段
    SetProfiles,                                          // 设置页「配置档案 · 管理」
    ModalProfileApply,ModalProfileDelete,                 // 配置档案模态里每行的两个动作（index = 档案下标）
    FormField,SelectOption,                               // 表单字段 / 下拉选项（用 ControlRect::index 区分）
    ModalConfirm,ModalCancel,                             // 模态的两个按钮
    // ---- H5b/H5c：添加 / 备份 / 设置 ----
    SubTabNew,SubTabModels,SubTabDel,                     // 添加页的子页分段
    ModelChip,ProbeButton,PrimaryButton,                  // 添加页的模型药丸 / 探测 / 保存
    SwitchRowControl,                                     // 开关行（用 ControlRect::index）
    ModeSeg,ThemeSeg,                                     // 接口模式 / 主题分段
    RestoreButton,BackupRefresh,OpenWebUiButton,          // 备份页刷新/回滚、设置页打开 WebUI
    ChipHelper,                                           // 「助手出错」chip（点一下重试）
    RegisterRestartGateway,                               // 寄存器行的「重启网关」胶囊（换模型后）
    StatusPill,                                           // 多状态合并胶囊（点开状态中心）
    LampWsl,LampWebui,LampGateway,                        // 三盏状态灯（点击 = 重试探活，审计 F-016）
    KeyReveal,                                            // 新建供应商页 API Key 的显隐按钮（审计 F-012）
    OfflineStart                                          // 离线卡上的「启动 WSL」
};
// `index` carries the option / field number for the repeating controls (select options, form fields).
struct ControlRect { ControlId id=ControlId::None;D2D1_RECT_F rect{};int index=-1; };

struct LampLayout {
    D2D1_RECT_F dot{},label{},detail{};
};
struct NavLayout {
    D2D1_RECT_F box{},icon{},label{},badge{};
};
// One drawn (and hit-testable) entry of the models list. Group headers stick to the top of the
// viewport (`sticky`), exactly like the old `.group-head` did.
enum class ListItemKind { GroupHead,Row,Sect,Empty };
struct ListItem {
    ListItemKind kind=ListItemKind::Row;
    int group=0,row=0;                 // indexes into PanelViewModel::groups / groups[].rows
    D2D1_RECT_F rect{};                // content box, already offset by the scroll position
    bool sticky=false;                 // pinned to the viewport top by this frame's scroll
    int groupEnd=0;                    // item index one past the last item of the same group
    std::string text;                  // section caption / empty-state caption
};
// Where a pointer landed inside the list. `star` is the 22px favourite button at the row's right end,
// `menu` the provider "…" button at the group header's right end.
struct ListHit {
    ListItemKind kind=ListItemKind::Empty;
    int group=-1,row=-1;
    bool inList=false,star=false,menu=false;
};
// Panel geometry for one frame. Width is fixed by the tier; height too, so the content area is whatever
// is left between the hero and the navigation (DESIGN §4).
struct PanelLayout {
    float scale=1.0f;
    D2D1_RECT_F window{};
    D2D1_RECT_F titleBar{},titleDivider{};
    D2D1_RECT_F brandMark{},brandText{};
    D2D1_RECT_F lampRow{},lampsDivider{};
    std::vector<LampLayout> lamps;
    D2D1_RECT_F chipHelper{},chip{},chipWarn{},busy{},sync{};
    D2D1_RECT_F hero{},heroDivider{},heroLamp{},heroProvider{},heroModel{},heroBadge{},heroModeTag{};
    D2D1_RECT_F content{};
    D2D1_RECT_F pageTitle{},pageNote{};
    D2D1_RECT_F search{},searchIcon{},searchText{},searchHint{},searchClear{};  // models-page search row
    // ---- models page: the filter row and the scrolling list (DESIGN §4.3 #9-#13) ----
    D2D1_RECT_F filters{},filterHint{};
    std::vector<ControlRect> segments;  // the three filters, left to right
    D2D1_RECT_F list{};                 // the list's outer box (below the filters, above the navigation)
    D2D1_RECT_F listViewport{};         // where rows are actually visible (list minus the scrollbar)
    std::vector<ListItem> items;        // in draw order, y already includes listScroll
    float listScroll=0,listMaxScroll=0;
    D2D1_RECT_F scrollTrack{},scrollThumb{};
    bool scrollVisible=false;
    // ---- H5 其余四页：页头 / 表单 / 阶梯 / 卡片 / 模态 / Toast ----
    D2D1_RECT_F pageHead{},pageHeadTitle{},pageHeadSub{},pageHeadAction{};
    D2D1_RECT_F ladder{};                     // 阶梯列表的外框
    std::vector<ListItem> ladderRows;         // 每行一个（kind=Row，row=序号）
    D2D1_RECT_F addLine{},fold{},foldClose{}; // 「追加一条」虚线 + 页内浮层表单卡片
    struct FieldBox { D2D1_RECT_F label,box;int kind=0;int index=-1;std::string note; };  // kind 0 文本 / 1 密码 / 2 数字 / 3 下拉 / 4 多行 / 5 分段；note 是标签右侧小字
    std::vector<FieldBox> fields;             // 当前页表单的字段（上到下）
    D2D1_RECT_F errorLine{},primaryButton{},secondaryButton{},restartGatewayButton{};
    D2D1_RECT_F registerRestartButton{};      // 寄存器行的「重启网关」胶囊（换模型后显示）
    // 多状态合并：一个胶囊 + 展开的状态中心（行整行可点，action 区只是视觉提示）。
    D2D1_RECT_F statusPill{},statusCard{};
    struct StatusRowBox { D2D1_RECT_F row{},action{}; };
    std::vector<StatusRowBox> statusRows;
    D2D1_RECT_F diagnoseButton{},usageButton{};   // 设置页「诊断」「用量」入口
    // 回退链阶梯的内部滚动（条目多于视口时）：与模型列表同一套做法。
    float fbScroll=0.0f,fbMaxScroll=0.0f;
    D2D1_RECT_F fbScrollTrack{},fbScrollThumb{};
    D2D1_RECT_F profilesButton{};                 // 设置页「配置档案 · 管理」入口
    // 配置档案模态的每行：整行 + 行尾「应用」「删除」两个按钮（渲染层与命中测试共用）。
    struct ProfileRowBox { D2D1_RECT_F row{},apply{},remove{}; };
    std::vector<ProfileRowBox> modalProfileRows;
    // 模态正文横向滚动（诊断/用量长行）：内容宽超出正文时出现，可拖动/滚轮。
    bool modalScrollVisible=false;
    float modalMaxScroll=0.0f,modalScrollX=0.0f;
    D2D1_RECT_F modalHScrollTrack{},modalHScrollThumb{};
    D2D1_RECT_F selectList{};                 // 展开的下拉列表
    std::vector<ControlRect> selectOptions;
    // 下拉展开时的项文案与选中项（回退链页用 sources/source，添加页用 providers/provider）：
    // 渲染层只认这两份数据，两个页面共用同一段绘制。
    std::vector<std::string> selectLabels;
    int selectActive=0;
    // 下拉列表滚动：条目多于可视行时（真机：18 个供应商放不进 8 行，后面的人选不到），
    // 显示滑块并且滚轮/拖动可滚。scroll=窗口内首项的下标。
    int selectScroll=0,selectTotal=0,selectVisible=0;
    D2D1_RECT_F selectBar{},selectThumb{};
    D2D1_RECT_F modalShade{},modalBox{},modalTitle{},modalBody{},modalConfirm{},modalCancel{};
    D2D1_RECT_F cards{},cardFoot{},toast{};
    // 页内滚动（添加 / 备份 / 设置页的正文比窗口高，旧版靠 clamp 后整页滚动）
    D2D1_RECT_F pageViewport{};
    float pageScroll=0,pageMaxScroll=0;
    D2D1_RECT_F pageScrollTrack{},pageScrollThumb{};
    bool pageScrollVisible=false;
    std::vector<ListItem> rows;         // 备份页的行（kind=Row，row=行号）
    D2D1_RECT_F subTabs{},footBar{},dangerCard{},chipRow{},noteLine{};
    // 说明行 / 小节标题：kind 0 = 普通说明，>0 = 设置页第 N 个小节标题（1..4）
    struct NoteLine { D2D1_RECT_F rect{};int kind=0;std::string text; };
    std::vector<NoteLine> notes;
    // .srow > label：行内标签（11.5px、文本色、左对齐）。主题 / WebUI 两行是「标签 + 控件」，
    // 不是小节标题（带底色的 .sect 才是标题）。
    std::vector<NoteLine> labels;
    D2D1_RECT_F offlineCard{},offlineIcon{},offlineTitle{},offlineBody{},offlineButton{};
    D2D1_RECT_F nav{},navDivider{};
    std::vector<NavLayout> navItems;
    std::vector<ControlRect> controls;
    float contentHeight=0;
    // 当前页面的自然高度（DIP，不含窗口边框）：标题栏 34 + 寄存器 27 + 页面内容 + 导航 47。
    // 旧 Electron 版是渲染层量出来再 app:fit，这里由布局直接算出来给外壳去 resize。
    float desiredHeight=0;
};
PanelLayout ComputeLayout(const PanelViewModel& view,float width,float height,float scale);
ControlId ControlAt(const PanelLayout& layout,float x,float y);
const D2D1_RECT_F* ControlRectFor(const PanelLayout& layout,ControlId id);
// The whole entry (id + rect + index) under a point, for the repeating controls.
const ControlRect* ControlEntryAt(const PanelLayout& layout,ControlId id,float x,float y);
// List hit testing (models page): the same items ComputeLayout built, so a click can never land
// somewhere the user did not see. Returns a kind=Empty, inList=false result for a miss.
ListHit ListHitTest(const PanelLayout& layout,float x,float y);
// The star button of a row, in the row's own coordinates (widget-tests and the click handler share it).
D2D1_RECT_F StarBox(const D2D1_RECT_F& row,float scale);
D2D1_RECT_F GroupMenuBox(const D2D1_RECT_F& head,float scale);
// 阶梯行（回退链页）：三个 22px 按钮排在行尾（.fb-btns 上移/下移/移除），返回 1..3，0 表示没命中按钮。
D2D1_RECT_F LadderButtonBox(const D2D1_RECT_F& row,int which,float scale);
int LadderButtonAt(const D2D1_RECT_F& row,float x,float scale);
}
