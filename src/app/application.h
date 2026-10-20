#pragma once
#include <windows.h>
#include <json.hpp>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "core/geometry_controller.h"
#include "core/state.h"
#include "data/log.h"
#include "data/settings.h"
#include "app/panel_view.h"
#include "render/layout.h"
#include "render/textfield.h"
#include "data/helper.h"
#include "data/latency.h"
#include "data/profiles.h"
#include "data/state.h"
#include "render/surface.h"
#include "render/theme.h"

namespace hermes {
using Json=nlohmann::json;
struct NativeMonitor { HMONITOR handle=nullptr; std::wstring id; Monitor geometry; };
NativeMonitor ReadMonitor(HMONITOR monitor);
NativeMonitor MonitorAt(Point point);

struct MenuEntry { unsigned id; const char* token; const wchar_t* text; };
const std::vector<MenuEntry>& MenuEntries();

enum class MenuCommand : unsigned {
    ToggleVisibility=1001,Dock=1002,Undock=1003,
    Refresh=1004,OpenWebUI=1005,RestartGateway=1006,CompactView=1007,AlwaysOnTop=1008,
    Poll30=1009,Poll60=1010,Poll120=1011,Poll300=1012,
    AutoStart=1013,OpenDataDir=1014,Quit=1099,
    CurrentInfo=1031,About=1032,RestartWebUi=1033,
    SwitchHeader=1030,SwitchBase=1100,      // 托盘「切换到」子菜单：1100 + 条目下标
    ThemeDark=1020,ThemeLight=1021,ThemeSystem=1022,ThemeSlate=1025,ThemeCarbon=1026,
    ThemePorcelain=1027,ThemeSand=1028,
    CloseHide=1023,CloseQuit=1024
};
// The hermes panel has one hotkey (Ctrl+Alt+H by default, configurable in the settings page).
enum class Hotkey : int { TogglePanel=1 };

// The native shell around the hermes model panel: two windows (panel + edge sliver), the system drag
// loop, tray, hotkeys, settings, theme, logging and the debug control plane. Everything hermes-specific
// lives behind PanelViewModel (render/app) and the helper bridge that fills it.
class Application {
public:
    bool Initialize(HINSTANCE instance,HWND control,SettingsStore* settings,Log* log);
    // 旧设置目录的覆盖（门禁夹具用，避免碰真实的 %APPDATA%\HermesModelPanel）。
    void SetLegacyDirectory(std::wstring directory) { legacyDirectory_=std::move(directory); }
    Settings SnapshotSettings() const;
    void PersistSettings(bool force=false);
    ~Application();
    void Wake();
    void TrayMessage(LPARAM event);
    void InvokeHotkey(int id);
    void InvalidateAll();
    // 状态中心（>=2 个状态时由 StatusPill 打开）：行点击走同一套动作分发。
    void InvokeStatusAction(StatusAction action);
    // H3 wires these to the WSL helper and the probes; until then they only log, so the shell can run.
    void RefreshData();
    void OpenWebUi();
    // PanelViewModel for the active page; the renderer only ever draws what this returns.
    PanelViewModel CurrentView();
    // The debug control plane: dump/place/state/controls/menu/hit…, used by the acceptance scripts.
    Json Command(const Json& request);
    unsigned WakeCount() const { return wakeCount_; }
    // 控制窗口（托盘/热键）里的动作不在面板 WindowProc 的 catch 范围内：这里统一收尾，
    // 记日志后正常关闭，绝不让异常穿出 DispatchMessage 变成无提示崩溃。
    void ReportControlFailure(const std::exception& error);
private:
    struct WindowContext { Application* owner;bool strip;bool card;Surface surface; };
    static LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
    LRESULT WindowMessage(HWND window,bool strip,UINT message,WPARAM wparam,LPARAM lparam);
    LRESULT HandleWindowFailure(HWND window,UINT message,const std::exception& error);
    bool AddTray();
    void RemoveTray();
    HICON CreateTrayIcon(COLORREF color) const;
    HMENU BuildMenu() const;
    void InvokeMenu(unsigned id);
    bool RegisterHotkeys();
    void SetTier(Tier tier);
    void SetTopmost(bool topmost);
    void InvokeControl(ControlId id);
    void TrackHover(HWND window,float x,float y);
    void SetPollSeconds(int seconds);
    void SetAutoStart(bool enabled);
    bool AutoStartEnabled() const;
    void SyncTopmostStyle();
    void Show();
    void Hide();
    void DockNearest();
    void Undock();
    void PointerEnteredStrip();
    void PointerMovedOnPanel(POINT messagePoint);
    void PointerLeftPanel(POINT messagePoint);
    void GraceExpired();
    void ApplyDpiChange(const RECT& suggested);
    void RefreshTopology(const char* reason);
    void BeginSystemDrag();
    void CommitDrop();
    // 贴条拖动：按住拖动时保持小挂件形态、不展开、不显示提示；松手吸附到最近的显示器边缘。
    void BeginStripPress(POINT screenPoint);
    void UpdateStripDrag(POINT screenPoint);
    void CommitStripDrop();
    // 贴条悬浮信息卡（独立分层小窗）：300ms 静止后出现，离开/按下/拖动/展开即隐藏。
    void ArmStripCard();
    void ShowHoverCard();
    void HideHoverCard();
    void PositionHoverCard();
    void RenderHoverCard();
    bool cardVisible_=false;
    // ---- 动效（移植自 balance-panel-native）----
    // 只影响绘制：统一的 ease-out 进度、一个只在动画期间存在的 16ms 计时器、
    // 跟随系统「显示动画」开关（SPI_GETCLIENTAREAANIMATION，1 秒缓存）。
    bool AnimationsEnabled() const;
    void KeepMotionTimer();
    void StartPanelMotion(bool enter);
    void StartCardMotion();
    void StartStripMotion();
    void StartTooltipMotion();
    void StartThemeMotion(const ThemePalette& target);
    void MotionTick();
    void SetHoverControl(int id);
    unsigned long long panelMotionStart_=0,cardMotionStart_=0,tooltipMotionStart_=0;
    unsigned long long hoverMotionStart_=0,themeMotionStart_=0;
    unsigned long long stripMotionStart_=0,cardExitStart_=0;
    unsigned long long capsuleDockStart_=0;   // 胶囊贴边↔浮起过渡起点（0=无动画）
    bool capsuleDocked_=true;                 // 目标态：true=贴边（直角侧）、false=浮起（椭圆）
    float capsuleDockOverride_=-1.0f;         // 拖动中按离边距离实时插值（<0=走动画状态）
    unsigned loggedBackdropGeneration_=0;     // 玻璃快照诊断日志去重（只在代次变化时记一行）
    unsigned long long stripDragRenderTick_=0; // 拖动重绘节流（16ms，位置仍逐条跟手）
    bool panelMotionEnter_=true;
    int hoverPrevious_=0;
    bool motionTimer_=false;
    mutable bool animCacheValid_=false;
    mutable bool animCache_=true;
    mutable unsigned long long animCacheTick_=0;
    // 主题交叉淡入的两端：切换时把当前调色板记为 from，目标是新主题；进度到 1 后不再混合。
    ThemePalette previousPalette_{},targetPalette_{};
    bool themeStartValid_=false;
    bool animOverride_=true;          // 控制面 configure animations=false → 门禁在无动画模式下跑
    bool stripPressed_=false;
    bool stripDragging_=false;
    Point stripPressScreen_{};
    Point stripDragOffset_{};
    // 手动拉伸（底部条带）：拖动期间窗口由 SetWindowPos 直接跟手，松手后按 DIP 落盘并 Place 一次。
    bool CanResizeHeight() const;
    void BeginPanelResize(POINT screenPoint);
    void UpdatePanelResize(POINT screenPoint);
    void CommitPanelResize();
    void SetPanelHeightDip(int dip);
    LRESULT HitTest(POINT screen);
    void ApplyState(const char* reason,State previous);
    Json Dump() const;
    HINSTANCE instance_=nullptr;
    HWND control_=nullptr;
    HWND panel_=nullptr;
    HWND strip_=nullptr;
    HWND hoverCard_=nullptr;
    WindowContext panelContext_{this,false,false,{}};
    WindowContext stripContext_{this,true,false,{}};
    WindowContext hoverCardContext_{this,false,true,{}};
    GeometryModel model_;
    NativeMonitor monitor_;
    State state_=State::Floating;
    GeometryController geometry_;
    bool hotkeysRegistered_=false;
    int hotkeyErrors_[3]{0,0,0};
    HICON trayIcon_=nullptr;
    bool trayRegistered_=false;
    UINT taskbarCreated_=0;      // Explorer 重启广播：重新挂回托盘图标
    unsigned trayReadds_=0;
    // 托盘图标 = logo（状态色 ok/warn/err）+ 旧版多行 tooltip：两者都在状态变化时按需刷新。
    // 撞上飞行中调用时排队的用户动作（托盘菜单点击）：落地后由 ApplyDataResult 取走执行。
    // 是 FIFO（两个不同的点击都要执行），但**相同**动作合并（连点同一个按钮不会排成一串）。
    struct PendingAction {
        std::string verb;
        Json args;
        std::string toast;
        bool refreshAfter=false;
        bool probeFallbacks=false;
    };
    std::vector<PendingAction> pendingActions_;
    COLORREF trayColour_=0;
    std::wstring trayTip_;
    COLORREF TrayColour() const;
    std::wstring TrayTooltip() const;
    bool GatewayStale() const;
    void UpdateTrayStatus();
    bool graceTimer_=false;
    bool floatingRemembered_=false;
    Point floatingBeforeDock_{};
    SettingsStore* settings_=nullptr;
    // Which page the content area shows, and the control the pointer is over (hover is paint only).
    int activePage_=0;                         // 0 models, 1 fallback, 2 add, 3 backup, 4 settings
    int hoverControl_=0;
    ThemePalette palette_;
    Log* log_=nullptr;
    Settings stored_;
    unsigned wakeCount_=0;
    unsigned drops_=0;              // completed drags; the only legitimate source of a rect change
    unsigned tierWrites_=0;
    unsigned stripMoves_=0;
    unsigned panelMoves_=0;
    unsigned panelLeaves_=0;
    unsigned graces_=0;
    bool dragging_=false;
    bool resizing_=false;
    int resizeStartHeight_=0;      // 物理像素：按下时的窗口高
    int resizeStartCursorY_=0;     // 物理屏幕坐标
    unsigned resizeDpi_=96;
    Point dragStartOrigin_{};
    // 抓取点相对窗口左上角的物理偏移与拖拽起始 DPI：跨 DPI 时系统会换锚点，浮动态落点
    // 需要按 DIP 语义还原抓取点，否则松手瞬间面板在光标下滑走一截。
    Point dragGrabPx_{};
    unsigned dragStartDpi_=96;
    // WM_ENTERSIZEMOVE 到达时指针可能已经走了一段（输入排队/快速拖拽），抓取点必须在
    // WM_NCLBUTTONDOWN 当场采样，否则落点会按一个过期的偏移还原。
    Point dragPressPx_{};
    bool dragPressValid_=false;
    struct DropRecord {
        bool valid=false;
        Rect start{};
        Rect finalRect{};
        Point cursor{};
        int decidedEdge=0;
        int decidedAnchor=0;
    } lastDrop_;
    struct HoverEvent { const char* name;unsigned tick;unsigned msgTime;int x;int y;int state;int hit; };
    // ---- H2c the self-drawn text fields (the models-page search box first; forms arrive with H4/H5).
    // One focused field at a time; WM_CHAR/WM_KEYDOWN/WM_IME_* are routed through HandleFieldKey.
    TextField search_;
    TextField* focusedField_=nullptr;
    bool caretOn_=false;
    // ---- H4 the models page: the helper state, its poll, and the switch path.
    HermesState hermesState_;      // helper state (distinct from state_, the shell state machine)
    HelperBridge helper_;
    std::string stateSignature_;
    int filterKind_=0;                     // 0 all / 1 fav / 2 recent
    std::string switchingKey_,switchingModel_;
    // The list's own view state: which groups the user opened, where the ↑↓ cursor is, how far the
    // list is scrolled (and whether the scrollbar thumb is being dragged).
    std::vector<std::string> expanded_;
    int cursor_=-1;
    float listScroll_=0;
    bool scrollDrag_=false;
    float scrollGrabOffset_=0;
    // 页内滚动条（设置/添加/备份）：拖动拇指或点轨道，与列表滚动条同语义。
    bool pageScrollDrag_=false;
    float pageScrollGrabOffset_=0;
    long long hotLoadedMtime_=0;           // settings.hotLoadedMtime leave-behind for gatewayHint
    // The helper is slow (WSL start-up + python) so reads and writes run on a worker thread; the result
    // comes back through a posted message and is applied on the UI thread. One job at a time, exactly
    // like the old panel's single-flight refresh.
    struct FallbackProbe {
        bool ok=false;
        int count=0;
        bool hasModel=false;
        std::string error;
    };
    struct BackupRow { std::string file,kind,stamp;int sizeKb=0; };
    // 逐条探测的单行结果：每测完一条就投递一次（真机反馈：不能等整条链测完才显示）。
    struct ProbeRow { int index=0;FallbackProbe probe; };
    struct DataResult {
        HermesState state;
        bool switched=false;
        std::string switchKey,switchError;
        // H5: a page write (fallback-add / fallback-remove / …) carries the toast it wants shown; a probe
        // run carries one entry per fallback row.
        bool write=false;
        std::string verb,toast,error;
        // 助手报错的原因随结果携带（此前轮询线程直写 Application::helperError_，UI 线程读，
        // 是跨线程竞态）。helperErrorValid 只由刷新路径置位：切换/写入结果不带这个语义，
        // 不该把上一次刷新留下的错误清掉。
        bool helperErrorValid=false;
        std::string helperError;
        // 写入预览（previewDiff）：助手对 dry_run 请求只回 diff；确认后按 preview* 重发真写。
        bool previewOnly=false;
        bool previewIgnored=false;     // 请求了 dry_run，但助手不支持、已直接写入（审计 F-003 的防线）
        std::vector<std::string> previewFacts;
        std::string previewVerb,previewToast;
        Json previewArgs;
        bool previewRefreshAfter=false,previewProbe=false;
        bool clearsGatewayRestart=false;   // 网关重启完成：收掉寄存器行的「重启网关」胶囊
        std::vector<FallbackProbe> probes;
        bool probed=false;
        std::vector<std::string> models;      // probe 的模型列表（添加页拿它填药丸）
        std::vector<BackupRow> backups;       // backup-list 的结果
        std::string readKind,readJson;        // 只读助手（diagnose/usage）的结果：kind 非空即读任务
        unsigned readToken=0;                 // 读任务代号：过期结果不许再打开模态
    };
    bool firstRefresh_=true;          // 第一次刷新：先 ping 捂热冷启动的 WSL，并放宽时限
    std::string helperError_;        // 助手最近一次报错的原因（显示在 chip 与空态里）
    // DESIGN v1.7：UNC 读路径的 mtime/size 缓存（未变化直接复用上次解析，零文件读取）。
    long long uncCacheMtime_=0,uncCacheSize_=0;
    bool uncCacheValid_=false;
    std::string uncCacheListJson_,uncCacheHealthJson_;
    int pollOverrideSeconds_=0;    // debug control plane `configure`: poll cadence for a measurement run
    int syntheticMods_=0;          // debug `vk mods`: synthetic Ctrl/Alt/Shift state for one keydown
    std::thread worker_;
    std::atomic<bool> workerBusy_{false},stopping_{false};
    // 逐条探测进度：worker 每完成一条就 PostMessage，UI 线程只读这些原子量，
    // 按钮显示「检测中 n/N」，当前行显示省略号（真机反馈：点了没反应，不知道跑到哪了）。
    std::atomic<bool> probeRunning_{false};
    std::atomic<int> probeDone_{0};
    std::atomic<int> probeTotal_{0};
    std::atomic<int> probeActive_{-1};
    // Last pointer position inside the panel (DIP), for list hover and the scrollbar thumb drag.
    float pointerX_=0,pointerY_=0;
    bool pointerInside_=false;
    void ApplyHelperState(const HermesState& state);
    // H5: one generic write action (verb + args) on the worker thread, with an optional state re-read
    // and an optional "probe every fallback row" pass.
    void StartHelperAction(const std::string& verb,Json args,const std::string& successToast,
        bool refreshAfter=true,bool probeFallbacks=false);
    bool HandleChromeClick(float x,float y);   // 底栏/标题栏控件在任何页面都先吃点击
    void FallbackPageClick(float x,float y);
    void SubmitFallback();
    TextField* FormFieldAt(int index);
    int SelectOptionAt(const PanelLayout& layout,float x,float y) const;
    void ShowToast(const std::string& text,int kind);
    void UpdateToastTimer();
    void CloseForm();
    void StartRefresh();
    void StartSwitch(const std::string& providerId,const std::string& model);
    void StartQueuedSwitch();      // 忙时点下的模型：当前调用落地后再切（审计 F-011）
    void ApplyDataResult(DataResult* result);
    void HandleProbeRow(ProbeRow* row);
    void UpdatePollTimer();
    // Models-page input: one hit test serves the click handler and the renderer's hover states.
    PanelLayout LayoutForInput() const;
    void ModelsPageClick(float x,float y);
    void ModelsPageWheel(int delta);
    void ScrollFromThumb(float y);
    void PageScrollFromThumb(float y);
    bool HandleListKey(WPARAM key);
    void ListMoveCursor(int delta);
    void ListSelectCursor();
    void ClearSearch();
    void CycleFilter(int kind);
    void ToggleFavorite(const std::string& key);
    void SwitchModel(const std::string& providerId,const std::string& model);
    void FocusField(TextField* field);
    void BlurField();
    bool HandleFieldKey(UINT message,WPARAM wparam,LPARAM lparam);
    void HandleFieldIme(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
    void UpdateCaretTimer();
    // 键盘焦点链（审计 F-010）：Tab/Shift+Tab 遍历当前页控件，Enter 激活，Esc 清除。
    int focusControl_=0;
    int focusControlIndex_=-1;
    void MoveFocus(int delta);
    void ActivateFocus();
    void ClearFocus();
    // ---- H5b/H5c 三页：表单字段、开关、备份列表、热键捕获 ----
    TextField addName_,addKeyEnv_,addUrl_,addKey_,addModels_,addDanger_,addNewModels_;
    int addSub_=0,addMode_=0,addProvider_=0;
    bool addSelectOpen_=false;      // 添加页的供应商下拉（加模型 / 删除）是否展开
    int addSelectField_=0;          // 展开的是第几个字段
    bool settingsSelectOpen_=false; // 设置页「关闭按钮」下拉的展开列表
    bool addKeyRevealed_=false,addProbeBefore_=true,addSetMainAfter_=false;
    bool addPurgeEnv_=true,addDropFallbacks_=false;
    std::string addError_,addInfo_;
    std::vector<std::string> addChips_;
    TextField setDistro_,setWebuiPort_,setPollSec_,setHotkey_;
    bool setHotkeyCapturing_=false;
    int backupFilter_=0;
    bool backupBusy_=false,backupLoaded_=false;
    std::string backupError_;
    std::vector<BackupRow> backups_;
    std::string pendingBackupFile_,pendingBackupKind_;   // 待确认的回滚目标
    float pageScroll_=0;
    // 模态事实行的横向滚动（F2/F3 长行）：拖动滑条或滚轮。
    float modalScrollX_=0;
    float fallbackScroll_=0;      // 回退链阶梯滚动（条目多时）
    bool modalScrollDrag_=false;
    float modalScrollGrab_=0;
    void ScrollModalFrom(float x);
    // G1 贴边挂件动画（声波/光环）：收起态下 120ms 一帧。
    float widgetPhase_=0.0f;
    void UpdateWidgetTimer();
    // 贴条玻璃的流动高光：只在收起态且贴条可见时跑 60fps 计时器，隐藏/展开立即停。
    void UpdateStripFlowTimer();
    float glassPhase_=0.0f;
    bool stripFlowTimer_=false;
    // 流动帧复用上一次的视图：60fps 每帧重建整个 PanelViewModel（含模型列表布局）
    // 是纯浪费；只改相位，另外每 60 帧强制重建一次兜底。
    PanelViewModel stripFlowView_;
    bool stripFlowViewValid_=false;
    unsigned stripFlowFrames_=0;
    bool HitStripShape(POINT client) const;
    // 分层窗口不靠 InvalidateRect/WM_PAINT 出画面：状态/动画变化时直接合成并 UpdateLayeredWindow。
    void RenderStrip();
    void RenderStripFlowFrame();   // 流动高光的单帧（复用缓存视图）
    void AddPageClick(float x,float y);
    void BackupPageClick(float x,float y);
    void SettingsPageClick(float x,float y);
    void SubmitAdd();
    void ProbeForAdd();
    void LoadBackups();
    void SetSetting(const std::string& key,const std::string& value);
    void CaptureHotkey(WPARAM key);
    TextField* AddFieldAt(int index);
    int FormFieldIndexOf(const TextField* field) const;
    // ---- H5 回退链页：折叠表单的字段、逐条探测的结果、待确认的动作与 Toast ----
    TextField fbModel_,fbProvider_,fbUrl_,fbKeyEnv_,fbKey_;
    bool fbFormOpen_=false,fbSelectOpen_=false;
    int fbSource_=0;
    std::string fbError_;
    std::vector<FallbackProbe> fbProbes_;      // 按回退链条目下标
    enum class Pending { None,RemoveFallback,RestoreBackup,DeleteProvider,ProviderMenu,SwitchModel,
        DiagnoseInfo,UsageInfo,ProfileMenu,ProfileApplyConfirm,ProfileDeleteConfirm,PreviewWrite };
    Pending pending_=Pending::None;
    int pendingIndex_=-1;
    std::string pendingProvider_;    // 供应商「…」菜单开着时指向谁（旧 openProviderMenu(name)）
    std::string pendingSwitchProvider_,pendingSwitchModel_;   // 切模型二次确认的目标
    std::string queuedSwitchProvider_,queuedSwitchModel_;     // 轮询在飞时点的模型（落地后执行）
    std::string lastSyncedModel_;    // 网关最后一次同步（重启/热读确认）时的模型
    bool pendingGatewayRestart_=false;  // 换模型后（自动重启关）寄存器行的「重启网关」胶囊
    bool statusCenterOpen_=false;       // 多状态合并胶囊展开的状态中心
    // 控制面演示用（Debug 门禁）：强制一组状态，验证合并胶囊/状态中心的布局与点击。
    std::vector<StatusItemView> demoStatuses_;
    // 写入预览的暂存（确认后重发原请求；previewArgs 里 dry_run 显式 false 表示真写）
    std::vector<std::string> previewFacts_;
    std::string previewVerb_,previewToast_;
    Json previewArgs_;
    bool previewRefreshAfter_=false,previewProbe_=false;
    float syncSpinAngle_=0.0f;          // 刷新进行中同步章图标的旋转角（度）
    float busySweep_=0.0f;              // 顶部不定进度条的流动相位（度，0..720 循环）
    bool syncCheckShown_=false;         // 刷新成功后的对勾（800ms 后恢复刷新图标）
    // 悬停提示（旧版的 title 属性）：悬停 450ms 后弹一个气泡，移开/点击即收。
    int tooltipHover_=0;             // 鼠标停住的控件（0 = ControlId::None，与 hoverControl_ 同一套）
    int tooltipHoverIndex_=-1;       // 重复控件的序号（分段/行），用于逐项说明
    bool tooltipVisible_=false;
    PanelViewModel::TooltipPopup tooltipView_;   // 带锚点的气泡（LayoutForInput 里填）
    std::string toastText_;
    int toastKind_=0;
    // 托盘「切换到」子菜单每一项对应的 key（provider|model）。BuildMenu 是 const（dump 也用它枚举），
    // 所以这张表是缓存式的 mutable。
    mutable std::vector<std::string> switchKeys_;
    void StartWsl();                        // 离线卡的主按钮：敲一次 WSL 再刷新
    // ---- F2/F3 诊断与用量：只读助手（diagnose/usage），结果落成模态里的事实行 ----
    void StartReadHelper(const std::string& cmd);
    void ShowReadResult(const std::string& kind,const std::string& json,const std::string& error,
        unsigned token);
    bool readBusy_=false;         // 只读助手的读取在飞：模态原地显示"正在读取…"，不再消失
    unsigned readToken_=0;        // 每次读取 +1；过期结果直接丢弃（用户已关掉模态）
    std::string readTitle_;
    std::vector<std::string> readFacts_;
    // ---- F4 延迟探测：当前模型 + 收藏（去重、≤8），串行经只读助手 probe，历史落 latency.json ----
    struct LatencyResult { std::string key,model;int ms=0;bool ok=false; };
    void StartLatencyProbe(bool manual=false);
    void HandleLatencyResults(std::vector<LatencyResult>* results);
    void UpdateLatencyTimer();
    std::unique_ptr<LatencyStore> latency_;
    std::thread latencyWorker_;
    std::atomic<bool> latencyBusy_{false};
    // ---- F5 配置档案：profiles.json（只存 provider/model/favorites），应用 = 串行写 + 失败即停 ----
    struct ProfileApplyResult { std::string name;bool ok=false;std::string failedStep,error; };
    void StartProfileApply(const std::string& name);
    void HandleProfileResult(ProfileApplyResult* result);
    void SaveCurrentProfile();
    std::unique_ptr<ProfileStore> profiles_;
    std::thread profileWorker_;
    std::atomic<bool> profileBusy_{false};
    std::string pendingProfileName_;
    std::string profileError_;
    bool keepModalOpen_=false;   // 「保存当前」后留在管理模态（否则 HandleModalClick 会清掉 pending）
    int selectScroll_=0;         // 展开中的下拉列表滚动量（首项下标；打开/关闭/选中后归零）
    // ---- F6 托盘通知：只在边沿触发；同事件 10 分钟抑制；探针失败不发 ----
    void Notify(const std::string& event,const std::string& text);
    void CheckNotificationEdges();
    int notificationCount_=0;
    bool lastNotifyOk_=false;      // 最近一次 Shell_NotifyIcon(NIM_MODIFY) 的返回值（门禁断言真的投递了）
    std::map<std::string,int> notificationKinds_;   // 每个事件实际发出的次数（抑制后不增）
    std::string lastNotificationText_;
    std::map<std::string,unsigned long long> notifiedAt_;
    bool notifyPrimed_=false;
    bool lastGatewayOk_=false,lastWebuiOk_=false,lastHelperFailed_=false,lastPendingRestart_=false;
    int consecutiveRefreshFailures_=0;
    bool lastRefreshFailing_=false;
    void HideTooltip();                     // 收起悬停气泡（点击/离开/滚动时）
    // 模态是全局浮层：确定/取消与当前页无关（旧版一个 #modal，谁都能开）。
    bool HandleModalClick(float x,float y);
    void RunPendingAction();                // 各 pending 的"确定"动作
    void BuildTrayMenu();                   // 状态变化后让托盘菜单的勾选/条目跟上
    void MigrateLegacySettings();           // 旧 Electron 版设置的一次性迁移（只读旧文件）
    std::wstring legacyDirectory_;          // 覆盖旧设置目录（门禁用；默认 %APPDATA%\HermesModelPanel）
    std::vector<HoverEvent> events_;
    void LogEvent(const char* name,POINT messagePoint=POINT{});
};
}
