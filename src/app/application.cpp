#include "application.h"
#include "app/application_internal.h"
#include "control_plane.h"
#include <algorithm>
#include <cmath>
#include "core/text.h"
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdexcept>
#include "data/http.h"
#include "data/paths.h"
#include "data/probe.h"
#include "render/widgets.h"
#include <windowsx.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hermes {
const std::vector<MenuEntry>& MenuEntries() {
    static const std::vector<MenuEntry> entries{
        {static_cast<unsigned>(MenuCommand::ToggleVisibility),"hide",L"显示/展开/隐藏面板"},
        {static_cast<unsigned>(MenuCommand::Refresh),"refresh",L"刷新状态"},
        {static_cast<unsigned>(MenuCommand::ProbeAllModels),"probe-all",L"探测全部供应商模型（只读）"},
        {static_cast<unsigned>(MenuCommand::OpenWebUI),"open-webui",L"打开 WebUI"},
        {static_cast<unsigned>(MenuCommand::RestartGateway),"restart-gateway",L"重启网关"},
        {static_cast<unsigned>(MenuCommand::AlwaysOnTop),"topmost",L"始终置顶"},
        {static_cast<unsigned>(MenuCommand::Dock),"dock",L"贴边收回"},
        {static_cast<unsigned>(MenuCommand::Undock),"undock",L"解除贴边"},
        {static_cast<unsigned>(MenuCommand::Poll30),"poll-30",L"30 秒"},
        {static_cast<unsigned>(MenuCommand::Poll60),"poll-60",L"60 秒"},
        {static_cast<unsigned>(MenuCommand::Poll120),"poll-120",L"120 秒"},
        {static_cast<unsigned>(MenuCommand::Poll300),"poll-300",L"300 秒"},
        {static_cast<unsigned>(MenuCommand::AutoStart),"autostart",L"开机自启"},
        {static_cast<unsigned>(MenuCommand::OpenDataDir),"open-data-dir",L"打开数据目录"},
        {static_cast<unsigned>(MenuCommand::Quit),"quit",L"退出"}};
    return entries;
}
// exe 自己的版本号（读 VERSIONINFO，不在代码里再抄一份）：关于对话框显示的就是文件属性里那一条。
std::wstring ExecutableVersion() {
    wchar_t path[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,path,MAX_PATH)) return {};
    const DWORD size=GetFileVersionInfoSizeW(path,nullptr);
    if(!size) return {};
    std::vector<BYTE> data(size);
    if(!GetFileVersionInfoW(path,0,size,data.data())) return {};
    VS_FIXEDFILEINFO* info=nullptr;
    UINT length=0;
    if(!VerQueryValueW(data.data(),L"\\",reinterpret_cast<void**>(&info),&length)||!info) return {};
    return std::to_wstring(HIWORD(info->dwFileVersionMS))+L"."
        +std::to_wstring(LOWORD(info->dwFileVersionMS))+L"."
        +std::to_wstring(HIWORD(info->dwFileVersionLS))+L"."
        +std::to_wstring(LOWORD(info->dwFileVersionLS));
}
NativeMonitor ReadMonitor(HMONITOR monitor) {    MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(!monitor || !GetMonitorInfoW(monitor,&info)) throw std::runtime_error("Monitor information unavailable");
    UINT x=96,y=96;
    if(FAILED(GetDpiForMonitor(monitor,MDT_EFFECTIVE_DPI,&x,&y))) throw std::runtime_error("Monitor DPI unavailable");
    const auto rect=[](const RECT& value){return Rect{value.left,value.top,value.right-value.left,value.bottom-value.top};};
    return {monitor,info.szDevice,{rect(info.rcMonitor),rect(info.rcWork),x}};
}
HMONITOR MonitorByDevice(const std::string& deviceUtf8) {
    if(deviceUtf8.empty()) return nullptr;
    const auto wanted=std::wstring(deviceUtf8.begin(),deviceUtf8.end());
    struct Search { const std::wstring* wanted;HMONITOR found; };
    Search search{&wanted,nullptr};
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,PRECT,LPARAM param) -> BOOL {
        auto& state=*reinterpret_cast<Search*>(param);
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info)&&*state.wanted==info.szDevice) { state.found=monitor;return FALSE; }
        return TRUE;
    },reinterpret_cast<LPARAM>(&search));
    return search.found;
}
NativeMonitor MonitorAt(Point point) { return ReadMonitor(MonitorFromPoint({point.x,point.y},MONITOR_DEFAULTTONEAREST)); }

bool Application::Initialize(HINSTANCE instance,HWND control,SettingsStore* settings,Log* log) {
    control_=control;settings_=settings;log_=log;
    // 表单字段的形态（旧 textarea 是多行；API Key 掩码由绘制层按 masked 处理）。
    // v1.21：模型区是"单行输入 + 列表"——单行才会让 Enter 走"失焦即添加"。
    RegisterNativeFieldClass(instance);
    RegisterBusyBarClass(instance);
    RegisterSpinGlyphClass(instance);
    if(log_) log_->Info("app","starting instance");
    palette_=ResolveTheme(stored_.theme);
    // The hermes panel has no stored session of its own: the WSL helper owns every secret.
    if(settings_) {
        const auto stored=settings_->Load();
        model_.edge=stored.edge;
        model_.tier=stored.tier;
        model_.anchor=stored.anchor;
        model_.floating=stored.floating;
        model_.heightDip=stored.heightDip;
        // v1.8 的"浮起胶囊"已废弃（松手必须回贴边）：旧设置里残留的 stripFloat 在这里
        // 折算成离它最近的边，用户升级后第一次启动就回到贴着边的收起态。
        if(stored.stripFloat) {
            const auto widget=WidgetSize(WidgetStyleFromName(stored.widgetStyle),Edge::Left,
                monitor_.geometry.dpi);
            const auto rect=Rect{stored.stripFloatPos.x,stored.stripFloatPos.y,widget.width,widget.height};
            model_.edge=NearestEdge(rect,monitor_.geometry);
            model_.anchor=model_.edge==Edge::Left||model_.edge==Edge::Right
                ?rect.y:rect.x;
        }
        stored_=stored;
        // G1：贴边挂件样式在创建窗口前就要定下来（ComputeRect 用它算 strip 尺寸）。
        model_.widget=WidgetStyleFromName(stored.widgetStyle);
        // F4：延迟历史与设置同目录（latency.json），坏文件按空历史处理。
        {
            auto directory=settings_->Path();
            const auto slash=directory.find_last_of(L'\\');
            if(slash!=std::wstring::npos) directory.resize(slash);
            latency_=std::make_unique<LatencyStore>(directory+L"\\latency.json");
            latency_->Load();
            // F5：配置档案与设置同目录（profiles.json）。
            profiles_=std::make_unique<ProfileStore>(directory+L"\\profiles.json");
            profiles_->Load();
        }
        // 主题必须在读到设置**之后**解析：原来先解析再被 stored_ 覆盖，于是「设置=深色 / 系统=浅色」
        // 时整个面板按浅色画（真机上就是设置页高亮着「深色」而页面是白的）。
        palette_=ResolveTheme(stored_.theme);
        // A saved monitor may be gone: resolve it by device name and fall back to the nearest one.
        const auto monitor=MonitorByDevice(stored.monitorId);
        monitor_=monitor?ReadMonitor(monitor):MonitorAt(model_.floating);
    } else {
        monitor_=MonitorAt(model_.floating);
    }
    model_.heightDip=ClampPanelHeightDip(model_.heightDip,monitor_.geometry.dpi,monitor_.geometry.work);
    const auto size=TierSize(model_,monitor_.geometry.dpi);
    model_.floating=ClampFloating(model_.floating,size,monitor_.geometry.work);
    {
        RECT native{model_.floating.x,model_.floating.y,model_.floating.x+size.width,model_.floating.y+size.height};
        monitor_=ReadMonitor(MonitorFromRect(&native,MONITOR_DEFAULTTONEAREST));
    }
    WNDCLASSEXW cls{sizeof(cls)};
    cls.hInstance=instance;cls.lpfnWndProc=WindowProc;cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    // 窗口类图标 = exe 里的 logo（assets/hermes.rc 的 ID 1）：任务栏/Alt-Tab 与标题栏同源。
    cls.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
    cls.hIconSm=static_cast<HICON>(LoadImageW(instance,MAKEINTRESOURCEW(1),IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_DEFAULTCOLOR));
    if(!cls.hIconSm) cls.hIconSm=cls.hIcon;
    cls.lpszClassName=L"HermesModelPanelNative.Panel";
    if(!RegisterClassExW(&cls)) return false;
    cls.lpszClassName=L"HermesModelPanelNative.Strip";
    if(!RegisterClassExW(&cls)) return false;
    cls.lpszClassName=L"HermesModelPanelNative.HoverCard";
    if(!RegisterClassExW(&cls)) return false;
    const auto rects=ComputeRect(model_,monitor_.geometry);
    const auto& rect=rects.panel;
    // v1.23：WS_CLIPCHILDREN —— 原生输入框是子窗口，父窗口绘制时必须让出它们的位置，
    // 否则每次重绘都会把输入框区域刷掉一帧（真机反馈"输入框一直在闪烁"）。
    panel_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"HermesModelPanelNative.Panel",L"Hermes \u6A21\u578B\u9762\u677F",
        WS_POPUP|WS_CLIPCHILDREN,rect.x,rect.y,rect.width,rect.height,nullptr,nullptr,instance,&panelContext_);
    strip_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED,L"HermesModelPanelNative.Strip",L"Hermes \u6A21\u578B\u9762\u677F",
        WS_POPUP,rect.x,rect.y,ScaleDip(8,monitor_.geometry.dpi),rect.height,nullptr,nullptr,instance,&stripContext_);
    hoverCard_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,
        L"HermesModelPanelNative.HoverCard",L"Hermes",WS_POPUP,0,0,8,8,nullptr,nullptr,instance,
        &hoverCardContext_);
    if(!panel_||!strip_||!hoverCard_) return false;
    ShowWindow(hoverCard_,SW_HIDE);
    CreateNativeFields();
    busyBar_.Create(panel_,9001);
    spinGlyph_.Create(panel_,9002);
    const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    DwmSetWindowAttribute(panel_,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    geometry_.Attach(panel_,strip_,log_);
    // The persisted presentation decides what the user sees at startup: a docked panel comes back as
    // the collapsed strip, exactly like the tray "show" path (DESIGN §5.2 row 1, §6.5 step 5).
    state_=model_.edge==Edge::None?State::Floating:State::Collapsed;
    geometry_.Place(model_,monitor_.geometry,state_,"startup");
    geometry_.Visibility(state_);
    geometry_.Validate(model_,monitor_.geometry,state_);
    if(state_==State::Collapsed) RenderStrip();
    SyncTopmostStyle();
    SetTimer(panel_,kTopmostGuardTimer,kTopmostGuardMs,nullptr);
    MigrateLegacySettings();
    RegisterHotkeys();
    if(!AddTray()) {
        // 没有托盘图标这个面板就等于失联（菜单、呼出、退出全在托盘上）。以前这里只 return false，
        // 进程以退出码 10 静默结束：用户看到"面板没启动"，日志里连一行都没有。
        if(log_) log_->Error("tray","no tray icon: refusing to start");
        return false;
    }
    UpdatePollTimer();
    UpdateLatencyTimer();
    RefreshData();
    return true;
}
Application::~Application() {
    RemoveTray();
    // The worker may be inside a `wsl.exe` call (up to 15s): waiting for it would keep the process alive
    // long after the user closed the panel, so it is detached instead. Nothing else is shared with it -
    // it owns its bridge, writes only through the posted result, and a post to a destroyed window just
    // fails (the result is deleted in that case).
    stopping_=true;
    if(worker_.joinable()) worker_.detach();
    if(latencyWorker_.joinable()) latencyWorker_.detach();
    if(profileWorker_.joinable()) profileWorker_.detach();
    // v1.26：一键探测 / 一键同步的 worker 也必须 detach——退出时还在跑的话，
    // joinable 的 std::thread 析构会直接 std::terminate（"关面板时进程崩一下"）。
    if(allProbeWorker_.joinable()) allProbeWorker_.detach();
    if(syncAllWorker_.joinable()) syncAllWorker_.detach();
    if(hoverCard_) DestroyWindow(hoverCard_);
    if(panel_) DestroyWindow(panel_);
    if(strip_) DestroyWindow(strip_);
}
HICON Application::CreateTrayIcon(COLORREF colour) const {
    // 托盘图标 = logo（标题栏那枚 mark）：深色圆角方块 + 状态色脉冲折线 + 柔光。
    const int size=std::max(16,GetSystemMetrics(SM_CXSMICON));
    BITMAPV5HEADER header{};
    header.bV5Size=sizeof(header);header.bV5Width=size;header.bV5Height=-size;
    header.bV5Planes=1;header.bV5BitCount=32;header.bV5Compression=BI_BITFIELDS;
    header.bV5RedMask=0x00FF0000;header.bV5GreenMask=0x0000FF00;header.bV5BlueMask=0x000000FF;header.bV5AlphaMask=0xFF000000;
    void* pixels=nullptr;
    HDC screen=GetDC(nullptr);
    HBITMAP bitmap=CreateDIBSection(screen,reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS,&pixels,nullptr,0);
    ReleaseDC(nullptr,screen);
    if(!bitmap||!pixels) { if(bitmap) DeleteObject(bitmap); return nullptr; }
    auto* rgba=static_cast<std::uint32_t*>(pixels);
    const TrayMarkGeometry shape;
    // 折线端点（归一化）。
    double points[7][2]{};
    for(int index=0;index<7;++index) {
        points[index][0]=0.5+(TrayMarkGeometry::grid[index][0]/24.0-0.5)*shape.span;
        points[index][1]=0.5+(TrayMarkGeometry::grid[index][1]/24.0-0.5)*shape.span;
    }
    const double strokeRadius=shape.stroke/2.0;
    const int glyphRed=GetRValue(colour),glyphGreen=GetGValue(colour),glyphBlue=GetBValue(colour);
    const int glowRed=glyphRed/3,glowGreen=glyphGreen/3,glowBlue=glyphBlue/3;
    constexpr int kSamples=4;
    for(int y=0;y<size;++y) {
        for(int x=0;x<size;++x) {
            double tileCoverage=0.0,glyphCoverage=0.0,glowCoverage=0.0;
            for(int sy=0;sy<kSamples;++sy) {
                for(int sx=0;sx<kSamples;++sx) {
                    const double px=(x+(sx+0.5)/kSamples)/size;
                    const double py=(y+(sy+0.5)/kSamples)/size;
                    if(InsideRoundedSquare(px,py,shape.inset,shape.radius)) tileCoverage+=1.0;
                    double distance=1e9;
                    for(int index=0;index+1<7;++index)
                        distance=std::min(distance,DistanceToSegment(px,py,points[index][0],points[index][1],
                            points[index+1][0],points[index+1][1]));
                    if(distance<=strokeRadius) glyphCoverage+=1.0;
                    else if(distance<=strokeRadius+shape.glow)
                        glowCoverage+=std::max(0.0,1.0-(distance-strokeRadius)/shape.glow);
                }
            }
            const double samples=kSamples*kSamples;
            tileCoverage/=samples;glyphCoverage/=samples;glowCoverage/=samples;
            // 自下而上合成（tile → 柔光 → 折线），得到非预乘颜色 + 覆盖度。
            double alpha=0.0,red=0.0,green=0.0,blue=0.0;
            const auto over=[&](double layerRed,double layerGreen,double layerBlue,double layerAlpha) {
                const double outAlpha=layerAlpha+alpha*(1.0-layerAlpha);
                if(outAlpha<=0.0) { alpha=0.0;return; }
                red=(layerRed*layerAlpha+red*alpha*(1.0-layerAlpha))/outAlpha;
                green=(layerGreen*layerAlpha+green*alpha*(1.0-layerAlpha))/outAlpha;
                blue=(layerBlue*layerAlpha+blue*alpha*(1.0-layerAlpha))/outAlpha;
                alpha=outAlpha;
            };
            over(0x15,0x1A,0x22,tileCoverage);                    // --hx-plate-b 的深色方块
            over(glowRed,glowGreen,glowBlue,glowCoverage*0.55);   // 折线后的柔光
            over(glyphRed,glyphGreen,glyphBlue,glyphCoverage);    // 状态色脉冲折线
            const auto clampChannel=[&](double value) { return std::clamp(static_cast<int>(std::lround(value)),0,255); };
            const int finalAlpha=clampChannel(alpha*255.0);
            // CreateIconIndirect 的 32bpp 颜色位图要预乘 alpha。
            const auto premultiply=[&](double value) {
                return static_cast<std::uint32_t>(clampChannel(value*(finalAlpha/255.0)));
            };
            rgba[y*size+x]=(static_cast<std::uint32_t>(finalAlpha)<<24)
                |(premultiply(red)<<16)|(premultiply(green)<<8)|premultiply(blue);
        }
    }
    HBITMAP mask=CreateBitmap(size,size,1,1,nullptr);
    ICONINFO info{TRUE,0,0,mask,bitmap};
    HICON icon=CreateIconIndirect(&info);
    DeleteObject(mask);DeleteObject(bitmap);
    return icon;
}
bool Application::AddTray() {
    // 图标 = logo（旧版是状态色圆环；用户要求改成 logo，颜色仍按状态变）。
    if(!taskbarCreated_) taskbarCreated_=RegisterWindowMessageW(L"TaskbarCreated");
    trayColour_=TrayColour();
    trayIcon_=CreateTrayIcon(trayColour_);
    if(!trayIcon_) return false;
    trayTip_=TrayTooltip();
    NOTIFYICONDATAW data{};
    data.cbSize=sizeof(data);data.hWnd=control_;data.uID=kTrayId;
    data.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    data.uCallbackMessage=kTrayMessage;data.hIcon=trayIcon_;
    wcsncpy_s(data.szTip,trayTip_.c_str(),_TRUNCATE);
    trayRegistered_=Shell_NotifyIconW(NIM_ADD,&data)!=FALSE;
    // 托盘是面板唯一的常驻入口，注册成败必须留下痕迹：以前这条路径一个字都不写，发布版既没法
    // 从日志确认图标在不在（Debug 版还能用控制面查 dump.tray.registered），失败时更是完全静默。
    if(log_) {
        if(trayRegistered_) log_->Info("tray","registered (id="+std::to_string(kTrayId)+")");
        else log_->Warn("tray","registration failed ("+std::to_string(GetLastError())+")");
    }
    return trayRegistered_;
}
COLORREF Application::TrayColour() const {
    // 旧 updateTrayStatus 的三挡：WSL 不在线 = 红；WebUI 或网关不可达 = 琥珀；全在线 = 绿。
    const bool wslUp=hermesState_.wsl=="up";
    const bool webuiOk=hermesState_.webui.reachable,gatewayOk=hermesState_.gateway.reachable;
    if(!wslUp) return RGB(0xF4,0x62,0x6D);                  // --hx-err
    if(!webuiOk||!gatewayOk) return RGB(0xF2,0xB1,0x3C);    // --hx-warn
    return RGB(0x3E,0xD5,0x98);                             // --hx-ok
}
std::wstring Application::TrayTooltip() const {
    // 旧 trayTooltip：四行以内 —— 当前模型 / 三灯 / 「配置改了但网关没重启」/ 最后一条错误。
    const auto label=ProviderLabelFor(hermesState_.model.provider);
    const auto shortModel=(label.empty()?std::string{}:label+"/")
        +(hermesState_.model.model.empty()?std::string("\xE2\x80\x94"):hermesState_.model.model);
    // NIF_TIP 只有 128 个宽字符：模型名与错误先各自收短，保证三盏灯那行永远看得见。
    const auto clip=[](const std::wstring& value,std::size_t limit) {
        return value.size()<=limit?value:value.substr(0,limit)+L"…";
    };
    std::wstring tip=L"当前："+clip(Widen(shortModel),36)+L"\n";
    const auto mark=[](bool ok) { return ok?L"✓":L"✗"; };
    tip+=L"WSL "+std::wstring(mark(hermesState_.wsl=="up"))
        +L" · WebUI "+mark(hermesState_.webui.reachable)
        +L" · 网关 "+mark(hermesState_.gateway.reachable);
    if(GatewayStale()) {
        // 旧版：'有外部改动，网关自 HH:MM 未重启'（时间取网关启动时间，本地 HH:MM）。
        auto since=hermesState_.gateway.since;
        const auto space=since.find(' ');
        if(space!=std::string::npos) since=since.substr(space+1);
        if(since.size()>5) since=since.substr(0,5);
        // 网关不可达/unit 信息缺失时 since 为空：不要拼出「网关自  未重启」的双空格（审计 F-017）。
        tip+=since.empty()?L"\n有外部改动，网关尚未重启"
            :L"\n有外部改动，网关自 "+Widen(since)+L" 未重启";
    }
    // 旧 lastError：界面上正在显示的那条错误（助手报错优先，其次是状态里的错误）。
    const auto lastError=!helperError_.empty()?helperError_:hermesState_.error;
    if(!lastError.empty()) tip+=L"\n⚠ "+clip(Widen(lastError),56);
    return tip.size()<=127?tip:tip.substr(0,126)+L"…";
}
void Application::UpdateTrayStatus() {
    if(!trayRegistered_) return;
    // 真机 2026-10-21「切换模型/重启网关时偶发卡一下」：Shell_NotifyIcon 在资源管理器忙时
    // 会同步阻塞几百毫秒，而它正好发生在状态翻转（网关灯变红）那一刻。>80ms 记一行，定性用。
    LARGE_INTEGER shellFreq{},shellStart{},shellDone{};
    QueryPerformanceFrequency(&shellFreq);QueryPerformanceCounter(&shellStart);
    const auto colour=TrayColour();
    // 图标只在状态色变化时重画（每次刷新重画会闪）。
    if(colour!=trayColour_||!trayIcon_) {
        auto* next=CreateTrayIcon(colour);
        if(next) {
            NOTIFYICONDATAW data{};
            data.cbSize=sizeof(data);data.hWnd=control_;data.uID=kTrayId;data.uFlags=NIF_ICON;
            data.hIcon=next;
            if(Shell_NotifyIconW(NIM_MODIFY,&data)) {
                if(trayIcon_) DestroyIcon(trayIcon_);
                trayIcon_=next;
                trayColour_=colour;
            } else {
                DestroyIcon(next);
            }
        }
    }
    auto tip=TrayTooltip();
    if(tip!=trayTip_) {
        trayTip_=tip;
        NOTIFYICONDATAW data{};
        data.cbSize=sizeof(data);data.hWnd=control_;data.uID=kTrayId;data.uFlags=NIF_TIP;
        wcsncpy_s(data.szTip,trayTip_.c_str(),_TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY,&data);
    }
    QueryPerformanceCounter(&shellDone);
    if(shellFreq.QuadPart>0&&log_) {
        const auto ms=static_cast<double>(shellDone.QuadPart-shellStart.QuadPart)*1000.0
            /static_cast<double>(shellFreq.QuadPart);
        if(ms>=80.0) log_->Info("ui","tray update "+std::to_string(static_cast<int>(ms))+"ms");
    }
}
void Application::RemoveTray() {
    if(trayRegistered_) {
        NOTIFYICONDATAW data{};
        data.cbSize=sizeof(data);data.hWnd=control_;data.uID=kTrayId;
        Shell_NotifyIconW(NIM_DELETE,&data);
        trayRegistered_=false;
    }
    if(trayIcon_) { DestroyIcon(trayIcon_);trayIcon_=nullptr; }
}
HMENU Application::BuildMenu() const {
    // 分类（2026-10-26 重排，用户要求"分类合理"）：
    //   ① 面板：显隐 / 当前 / 切换 / 贴边 / 置顶  ② 服务：刷新 + 三个服务动作
    //   ③ 外观与行为：主题 / 关闭按钮 / 自启 / 刷新间隔  ④ 系统：数据目录 / 关于 / 退出
    // 勾选项一律表示**当前状态**，文案表示**点击后的动作**（与 Windows 菜单惯例一致）；
    // 显隐项按状态写"隐藏面板 / 展开面板 / 显示面板"，不再用「显示 / 隐藏」这种二义写法。
    // 所有来自 std::string（UTF-8）的文案都必须过 Widen()：逐字节加宽会在托盘菜单里显示乱码。
    HMENU menu=CreatePopupMenu();
    if(!menu) return nullptr;
    const bool docked=model_.edge!=Edge::None;
    // 枚举 → 菜单 ID（旧版的 id 是固定数字：1001 显示/隐藏 … 1099 退出）。
    const auto label=[](MenuCommand command) { return static_cast<UINT_PTR>(command); };
    // 组 1：显隐 + 当前模型（信息行）+ 切换到 ▸ + 窗口行为（贴边 / 置顶）
    {
        // 三种状态三种动作：隐藏→显示（贴边时直接展开成面板）、贴条→展开、面板→隐藏。
        const wchar_t* text=state_==State::Hidden
            ?L"\u663E\u793A\u9762\u677F"                                  // 显示面板
            :(state_==State::Collapsed?L"\u5C55\u5F00\u9762\u677F"        // 展开面板
                :L"\u9690\u85CF\u9762\u677F");                            // 隐藏面板
        AppendMenuW(menu,MF_STRING,label(MenuCommand::ToggleVisibility),text);
    }
    {
        const auto provider=ProviderLabelFor(hermesState_.model.provider);
        const auto current=L"\u5F53\u524D\uFF1A"
            +Widen((provider.empty()?std::string("\xE2\x80\x94"):provider)+" / "
                +(hermesState_.model.model.empty()?std::string("\xE2\x80\x94"):hermesState_.model.model));
        AppendMenuW(menu,MF_STRING|MF_GRAYED,label(MenuCommand::CurrentInfo),current.c_str());
    }
    {
        HMENU submenu=CreatePopupMenu();
        switchKeys_.clear();
        const auto currentKey=hermesState_.model.provider+"|"+hermesState_.model.model;
        const auto pushModel=[&](const std::string& key) {
            const auto split=key.find('|');
            if(split==std::string::npos) return;
            const auto model=key.substr(split+1);
            if(model.empty()) return;
            auto text=model;
            if(key==currentKey) text+="  \xE2\x86\x90 \xE5\xBD\x93\xE5\x89\x8D";
            const auto id=label(MenuCommand::SwitchBase)+switchKeys_.size();
            AppendMenuW(submenu,MF_STRING|(key==currentKey?MF_CHECKED:0),id,Widen(text).c_str());
            switchKeys_.push_back(key);
        };
        const auto header=[&](const wchar_t* text) {
            AppendMenuW(submenu,MF_STRING|MF_GRAYED,label(MenuCommand::SwitchHeader),text);
        };
        int favorites=0;
        for(const auto& key:stored_.favorites) {
            if(favorites>=10) break;
            if(favorites==0) header(L"\u6536\u85CF");
            pushModel(key);
            ++favorites;
        }
        int recents=0;
        for(const auto& key:stored_.recents) {
            if(recents>=8) break;
            if(recents==0) header(L"\u6700\u8FD1\u5207\u6362");
            pushModel(key);
            ++recents;
        }
        if(switchKeys_.empty())
            AppendMenuW(submenu,MF_STRING|MF_GRAYED,label(MenuCommand::SwitchHeader),
                L"\uFF08\u5148\u5728\u9762\u677F\u91CC\u5207\u4E00\u6B21\uFF0C\u6216\u52A0\u6536\u85CF\uFF09");
        AppendMenuW(menu,MF_STRING|MF_POPUP,reinterpret_cast<UINT_PTR>(submenu),L"\u5207\u6362\u5230");
    }
    // 窗口行为跟面板放在同一组（以前挤在"主题/关闭按钮"那一组里，属于分类混乱）。
    AppendMenuW(menu,MF_STRING|(docked?MF_CHECKED:0),label(MenuCommand::Dock),
        L"\u8D34\u8FB9\u6536\u8D77");                                       // 贴边收起
    AppendMenuW(menu,MF_STRING|(stored_.topmost?MF_CHECKED:0),label(MenuCommand::AlwaysOnTop),
        L"\u59CB\u7EC8\u7F6E\u9876");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    // 组 2：刷新与三个服务动作（旧：立即刷新 / 打开 WebUI / 重启网关 / 重启 WebUI 服务）
    AppendMenuW(menu,MF_STRING,label(MenuCommand::Refresh),L"\u7ACB\u5373\u5237\u65B0");
    AppendMenuW(menu,MF_STRING,label(MenuCommand::OpenWebUI),L"\u6253\u5F00 WebUI");
    {
        auto text=std::wstring(L"\u91CD\u542F\u7F51\u5173");
        if(GatewayStale()) text+=L"\uFF08\u914D\u7F6E\u6709\u6539\u52A8\uFF09";
        AppendMenuW(menu,MF_STRING,label(MenuCommand::RestartGateway),text.c_str());
    }
    AppendMenuW(menu,MF_STRING,label(MenuCommand::RestartWebUi),L"\u91CD\u542F WebUI \u670D\u52A1");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    // 组 3：外观与行为（主题 / 关闭按钮 / 开机自启 / 刷新间隔）
    {
        HMENU theme=CreatePopupMenu();
        AppendMenuW(theme,MF_STRING|(stored_.theme=="dark"?MF_CHECKED:0),
            label(MenuCommand::ThemeDark),L"\u6DF1\u8272");
        AppendMenuW(theme,MF_STRING|(stored_.theme=="light"?MF_CHECKED:0),
            label(MenuCommand::ThemeLight),L"\u6D45\u8272");
        AppendMenuW(theme,MF_STRING|(stored_.theme=="system"||stored_.theme.empty()?MF_CHECKED:0),
            label(MenuCommand::ThemeSystem),L"\u8DDF\u968F\u7CFB\u7EDF");
        // 四套现代化主题（用户要求"四套都要"）：托盘子菜单与设置页的分段是同一份 id 表。
        AppendMenuW(theme,MF_STRING|(stored_.theme=="slate"?MF_CHECKED:0),
            label(MenuCommand::ThemeSlate),L"\u77F3\u677F");
        AppendMenuW(theme,MF_STRING|(stored_.theme=="carbon"?MF_CHECKED:0),
            label(MenuCommand::ThemeCarbon),L"\u78B3\u9ED1");
        AppendMenuW(theme,MF_STRING|(stored_.theme=="porcelain"?MF_CHECKED:0),
            label(MenuCommand::ThemePorcelain),L"\u74F7\u767D");
        AppendMenuW(theme,MF_STRING|(stored_.theme=="sand"?MF_CHECKED:0),
            label(MenuCommand::ThemeSand),L"\u6C99\u4E18");
        AppendMenuW(menu,MF_STRING|MF_POPUP,reinterpret_cast<UINT_PTR>(theme),L"\u4E3B\u9898");
    }
    {
        HMENU close=CreatePopupMenu();
        AppendMenuW(close,MF_STRING|(stored_.closeAction!="quit"?MF_CHECKED:0),
            label(MenuCommand::CloseHide),L"\u9690\u85CF\u5230\u6258\u76D8");
        AppendMenuW(close,MF_STRING|(stored_.closeAction=="quit"?MF_CHECKED:0),
            label(MenuCommand::CloseQuit),L"\u9000\u51FA\u7A0B\u5E8F");
        AppendMenuW(menu,MF_STRING|MF_POPUP,reinterpret_cast<UINT_PTR>(close),L"\u5173\u95ED\u6309\u94AE");
    }
    AppendMenuW(menu,MF_STRING|(AutoStartEnabled()?MF_CHECKED:0),label(MenuCommand::AutoStart),
        L"\u5F00\u673A\u81EA\u542F");
    {
        // 刷新间隔属于"行为"，跟主题/关闭按钮同一组（以前单独成组夹在中间，看着像第四个分类）。
        HMENU poll=CreatePopupMenu();
        const auto addPoll=[&](MenuCommand command,int seconds) {
            AppendMenuW(poll,MF_STRING|(stored_.pollSeconds==seconds?MF_CHECKED:0),label(command),
                (std::to_wstring(seconds)+L" \u79D2").c_str());
        };
        addPoll(MenuCommand::Poll30,30);
        addPoll(MenuCommand::Poll60,60);
        addPoll(MenuCommand::Poll120,120);
        addPoll(MenuCommand::Poll300,300);
        // 设置页的「刷新间隔」输入框允许任意 5..3600 秒。当前值不在上面四档里时（真机基线就是
        // 20 秒），整个子菜单一个勾都没有，用户看不出现在的间隔是多少 —— 补一条只读的当前值。
        const bool inPresets=stored_.pollSeconds==30||stored_.pollSeconds==60
            ||stored_.pollSeconds==120||stored_.pollSeconds==300;
        if(!inPresets) {
            AppendMenuW(poll,MF_STRING|MF_CHECKED|MF_GRAYED,0,
                (L"\u5F53\u524D\uFF1A"+std::to_wstring(stored_.pollSeconds)+L" \u79D2").c_str());
        }
        AppendMenuW(menu,MF_STRING|MF_POPUP,reinterpret_cast<UINT_PTR>(poll),L"\u5237\u65B0\u95F4\u9694");
    }
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    // 组 4：系统（数据目录 / 关于 / 退出）
    AppendMenuW(menu,MF_STRING,label(MenuCommand::OpenDataDir),L"\u6253\u5F00\u6570\u636E\u76EE\u5F55");
    AppendMenuW(menu,MF_STRING,label(MenuCommand::About),L"\u5173\u4E8E");
    AppendMenuW(menu,MF_STRING,label(MenuCommand::Quit),L"\u9000\u51FA");
    return menu;
}
void Application::TrayMessage(LPARAM event) {
    switch(LOWORD(event)) {
    case WM_LBUTTONUP: InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));return;
    case NIN_BALLOONUSERCLICK:
        // F6：点气泡把面板叫回来（用户从通知里直接进入）。
        Show();
        return;
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU: {
        HMENU menu=BuildMenu();
        if(!menu) return;
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(control_);
        const auto command=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,cursor.x,cursor.y,0,control_,nullptr);
        DestroyMenu(menu);
        if(command) InvokeMenu(static_cast<unsigned>(command));
        return;
    }
    default: return;
    }
}
void Application::InvokeMenu(unsigned id) {
    if(id>=static_cast<unsigned>(MenuCommand::SwitchBase)
        &&id<static_cast<unsigned>(MenuCommand::SwitchBase)+switchKeys_.size()) {
        // 托盘「切换到」条目：与面板里点行同一条写路径（旧的 setModelFromTray）。
        const auto& key=switchKeys_[id-static_cast<unsigned>(MenuCommand::SwitchBase)];
        const auto split=key.find('|');
        if(split!=std::string::npos) SwitchModel(key.substr(0,split),key.substr(split+1));
        return;
    }
    switch(static_cast<MenuCommand>(id)) {
    case MenuCommand::ToggleVisibility:
        // 三态语义（2026-10-26）：隐藏 → 显示成面板（贴边时直接展开，不能只亮出 30×136 的贴条
        // 让人以为没反应）、贴条 → 展开、面板 → 隐藏。托盘左键 / 热键 / 菜单第一项共用这一条。
        if(state_==State::Hidden) {
            const auto previous=state_;
            state_=NextState(state_,Event::Show,model_.edge);
            if(model_.edge!=Edge::None) state_=NextState(state_,Event::EnterStrip,model_.edge);
            ApplyState("show",previous);
        } else if(state_==State::Collapsed) {
            const auto previous=state_;
            state_=NextState(state_,Event::EnterStrip,model_.edge);
            ApplyState("expand",previous);
        } else {
            Hide();
        }
        return;
    // 旧版托盘只有一个「贴边收回 / 滑出」切换项（toggleSnap），所以同一个 ID 既是收起也是滑出。
    case MenuCommand::Dock: if(model_.edge!=Edge::None) Undock(); else DockNearest(); return;
    case MenuCommand::Undock: Undock(); return;
    case MenuCommand::Refresh: RefreshData();InvalidateAll();return;
    case MenuCommand::ProbeAllModels: ProbeAllModels();return;
    case MenuCommand::ThemeDark: SetSetting("theme","dark");return;
    case MenuCommand::ThemeLight: SetSetting("theme","light");return;
    case MenuCommand::ThemeSystem: SetSetting("theme","system");return;
    case MenuCommand::ThemeSlate: SetSetting("theme","slate");return;
    case MenuCommand::ThemeCarbon: SetSetting("theme","carbon");return;
    case MenuCommand::ThemePorcelain: SetSetting("theme","porcelain");return;
    case MenuCommand::ThemeSand: SetSetting("theme","sand");return;
    case MenuCommand::CloseHide: SetSetting("closeAction","hide");return;
    case MenuCommand::CloseQuit: SetSetting("closeAction","quit");return;
    case MenuCommand::OpenWebUI: OpenWebUi();return;
    case MenuCommand::RestartGateway:
        // 旧 doReload(['gateway'])：写一次 helper reload（mode auto = 优先 reload，失败再 restart），
        // 回来后重读状态并 toast。网关永远不会被自动重启，只有用户点才动。
        StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
            "\xE7\xBD\x91\xE5\x85\xB3\xE5\xB7\xB2\xE9\x87\x8D\xE5\x90\xAF",true);
        return;
    case MenuCommand::RestartWebUi:
        StartHelperAction("reload",{{"cmd","reload"},{"targets",{"webui"}},{"mode","auto"}},
            "WebUI \xE6\x9C\x8D\xE5\x8A\xA1\xE5\xB7\xB2\xE9\x87\x8D\xE5\x90\xAF",true);
        return;
    case MenuCommand::About: {
        const auto provider=ProviderLabelFor(hermesState_.model.provider);
        const auto model=hermesState_.model.model.empty()?std::string("\xE2\x80\x94"):hermesState_.model.model;
        const auto directory=settings_?DirectoryOfPath(settings_->Path()):std::wstring{};
        const auto version=ExecutableVersion();
        const std::wstring body=
            L"Hermes \u6A21\u578B\u9762\u677F\uFF08\u539F\u751F\u7248\uFF09\n"
            // 版本号取 exe 的 VERSIONINFO：安装包改名/换目录都不会和界面说的不一致。
            +(version.empty()?L"":(L"\u7248\u672C\uFF1A"+version+L"\n"))
            +L"\u5F53\u524D\u6A21\u578B\uFF1A"+Widen((provider.empty()?std::string("\xE2\x80\x94"):provider)+" / "+model)+L"\n"
            L"\u547C\u51FA\u70ED\u952E\uFF1A"+Widen(stored_.hotkey)+L"\n"
            L"\u6570\u636E\u76EE\u5F55\uFF1A"+directory+L"\n"
            L"WSL \u91CC\u7684 ~/.hermes/config.yaml \u4E0E .env \u53EA\u7531\u52A9\u624B\u811A\u672C\u5199\u5165\u3002";
        MessageBoxW(panel_,body.c_str(),L"\u5173\u4E8E Hermes \u6A21\u578B\u9762\u677F",MB_OK|MB_ICONINFORMATION);
        return;
    }
    case MenuCommand::AlwaysOnTop: SetTopmost(!stored_.topmost);return;
    case MenuCommand::Poll30: SetPollSeconds(30);return;
    case MenuCommand::Poll60: SetPollSeconds(60);return;
    case MenuCommand::Poll120: SetPollSeconds(120);return;
    case MenuCommand::Poll300: SetPollSeconds(300);return;
    case MenuCommand::AutoStart: SetAutoStart(!AutoStartEnabled());return;
    case MenuCommand::OpenDataDir:
        if(settings_) ShellExecuteW(nullptr,L"open",DirectoryOfPath(settings_->Path()).c_str(),nullptr,nullptr,SW_SHOWNORMAL);
        return;
    case MenuCommand::Quit: PersistSettings(true);PostMessageW(control_,WM_CLOSE,0,0);return;
    }
}
void Application::SetPollSeconds(int seconds) {
    stored_.pollSeconds=seconds;
    PersistSettings(true);
    if(log_) log_->Info("settings","poll interval "+std::to_string(seconds)+"s");
}
void Application::SetTopmost(bool topmost) {
    stored_.topmost=topmost;
    SyncTopmostStyle();
    PersistSettings(true);
    if(log_) log_->Info("settings",std::string("topmost ")+(topmost?"on":"off"));
}
void Application::SyncTopmostStyle() {
    const auto place=[](HWND window,bool topmost) {
        if(!window) return;
        SetWindowPos(window,topmost?HWND_TOPMOST:HWND_NOTOPMOST,0,0,0,0,
            SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    };
    place(panel_,stored_.topmost);
    place(strip_,stored_.topmost);
}
namespace {
// 置顶带的覆盖判定：从本窗口向上枚举 z-order（GW_HWNDPREV = 上面一个）。置顶带在
// z-order 顶部是连续的，所以走到第一个非 topmost 窗口就能断定带内没人压我们。
// 跳过的都不构成遮挡：自己进程的窗口（悬停卡/贴条本来就该在面板上面）、不可见窗口、
// 壳层任务栏、DWM cloaked 的壳窗口（在屏幕上根本不绘制，比如输入体验宿主）。
bool ObscuredInTopmostBand(HWND window) {
    DWORD pid=0;
    GetWindowThreadProcessId(window,&pid);
    RECT self{};
    if(!GetWindowRect(window,&self)) return false;
    for(HWND above=GetWindow(window,GW_HWNDPREV);above;above=GetWindow(above,GW_HWNDPREV)) {
        const auto style=GetWindowLongPtrW(above,GWL_EXSTYLE);
        if(!(style&WS_EX_TOPMOST)) return false;
        DWORD other=0;
        GetWindowThreadProcessId(above,&other);
        if(other==pid||!IsWindowVisible(above)) continue;
        wchar_t className[32]{};
        GetClassNameW(above,className,static_cast<int>(std::size(className)));
        if(wcscmp(className,L"Shell_TrayWnd")==0||wcscmp(className,L"Shell_SecondaryTrayWnd")==0) continue;
        BOOL cloaked=FALSE;
        if(SUCCEEDED(DwmGetWindowAttribute(above,DWMWA_CLOAKED,&cloaked,sizeof(cloaked)))&&cloaked) continue;
        RECT otherRect{};
        if(!GetWindowRect(above,&otherRect)) continue;
        RECT overlap{};
        if(!IntersectRect(&overlap,&self,&otherRect)) continue;
        return true;
    }
    return false;
}
}
void Application::EnforceTopmost() {
    if(!stored_.topmost) return;
    const auto enforce=[this](HWND window) {
        if(!window||!IsWindowVisible(window)) return;
        // 压顶才抬 + 兜底：极少数路径（第三方置顶工具/全屏切换）会把 WS_EX_TOPMOST 风格
        // 从窗口上剥掉，让风格与设置项重新一致。
        if(ObscuredInTopmostBand(window)||(GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)==0)
            SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    };
    enforce(panel_);
    enforce(strip_);
}
// ---- v1.23 原生输入框：创建 / 摆放 / 事件 ----
namespace {
// 字段 id = 绑定表下标 + 1（0 保留给"没有焦点"）。
constexpr int kNativeFieldBase=1;
}
void Application::CreateNativeFields() {
    if(!panel_||!nativeBindings_.empty()) return;
    // 顺序固定：id 与绑定表下标一一对应。只收**单行**文本字段；热键捕获框保持自绘
    // （它要截获组合键，原生 EDIT 做不了），供应商下拉不是文本。
    struct Entry { TextField* field;bool password;bool right; };
    const Entry entries[]={
        {&search_,false,false},
        {&addName_,false,false},{&addUrl_,false,false},{&addKey_,true,false},

        // v1.25：回退链只剩两个下拉，没有原生输入框。
        // 三个设置字段统一 ES_RIGHT：右侧那一列值统一靠右（真机反馈「WSL 发行版没和下面
        // 两个数字对齐」——以前只有端口/间隔是右对齐的）。
        {&setDistro_,false,true},{&setWebuiPort_,false,true},{&setPollSec_,false,true}};
    int id=kNativeFieldBase;
    for(const auto& entry:entries) {
        auto* native=new NativeField;
        native->Create(panel_,id,entry.password,entry.right);
        if(!native->Created()) { delete native;continue; }
        native->SetFontAndDpi(static_cast<UINT>(monitor_.geometry.dpi));
        // 原生控件的占位提示：自绘层的 hint 会被控件盖住，改用系统的 cue banner。
        if(entry.field==&search_) native->SetCue(L"搜索模型或供应商");
        // 发行版留空 = 用 WSL 默认发行版（helper 传 -d ""）：给个占位，免得空框让人以为坏了。
        else if(entry.field==&setDistro_) native->SetCue(L"默认发行版（留空）");
        else if(!entry.field->placeholder.empty())
            native->SetCue(Widen(entry.field->placeholder));
        nativeBindings_.push_back({entry.field,native});
        ++id;
    }
    if(log_) log_->Info("ui","native fields created: "+std::to_string(nativeBindings_.size()));
}
NativeField* Application::NativeFor(const TextField* field) const {
    if(!field) return nullptr;
    for(const auto& binding:nativeBindings_) if(binding.field==field) return binding.edit;
    return nullptr;
}
void Application::FocusNativeField(TextField* field) {
    if(auto* native=NativeFor(field)) native->Focus();
}
void Application::SyncNativeFields(const PanelLayout& layout) {
    if(nativeBindings_.empty()) return;
    // 原生 EDIT 是**真正显示文字的那一层**：自绘字段的 value 被它整个盖住，所以镜像里的文本
    // 必须推下去。之前只在创建时设过一次空文本，于是设置页三行（WSL 发行版 / WebUI 端口 /
    // 刷新间隔）一直显示为空框（真机反馈"设置界面上边几个输入框啥也看不见"）；添加页
    // 清空表单后残留的旧文本也是同一个原因。正在编辑的字段以控件为准（推文本会打断 IME）。
    // API Key 的「眼睛」：原生控件默认 ES_PASSWORD，切到明文必须真的改控件的掩码字符
    // （否则自绘层"已显示"、控件还是一片圆点，看起来像点了没反应）。
    for(auto& binding:nativeBindings_)
        if(binding.edit&&binding.field==&addKey_) binding.edit->SetPasswordVisible(addKeyRevealed_);
    // v1.29：摆放只依赖"页 / 子页 / 滚动 / 客户区 / DPI / 浮层态"。这些都没变时整段跳过——
    // 每帧重建可见字段 map 并逐个 Place 实测要 2-6ms，而绝大多数帧（轮询、悬停、忙碌条）
    // 布局根本没动。签名只做整数运算，比建 map 便宜一个量级。
    {
        unsigned long long signature=0x9E3779B97F4A7C15ull;
        const auto mix=[&signature](long long value) {
            signature^=static_cast<unsigned long long>(value)+0x9E3779B97F4A7C15ull
                +(signature<<6)+(signature>>2);
        };
        RECT client{};
        if(panel_) GetClientRect(panel_,&client);
        mix(activePage_);mix(addSub_);mix(fbFormOpen_?1:0);mix(fbSelectOpen_?1:0);
        mix(fbModelSelectOpen_?1:0);mix(addSelectOpen_?1:0);
        mix(static_cast<int>(pending_));mix(static_cast<int>(WindowDpi()));
        mix(client.right);mix(client.bottom);
        mix(static_cast<long long>(pageScroll_*4.0f));mix(static_cast<long long>(listScroll_*4.0f));
        // 浮层（Toast / 状态中心）的出现/消失也要触发这一帧的重摆：重叠的字段会被藏起来。
        mix(layout.toast.right>layout.toast.left?1:0);
        mix(layout.statusCard.right>layout.statusCard.left?1:0);
        mix(static_cast<long long>(fallbackScroll_*4.0f));
        // 离线/恢复会改变"哪些原生字段该显示"（搜索框在离线页必须收起），但它不改页/滚动/浮层，
        // 签名里漏掉它时布局摆放在离线切换那一帧被整段跳过 —— 搜索框就一直压在离线卡片上
        // （真机：离线页左上角浮着一个白色输入框）。
        mix(hermesState_.wsl=="down"?1:0);
        mix(hermesState_.error.empty()?0:1);
        if(signature==lastNativeLayoutSignature_) return;
        lastNativeLayoutSignature_=signature;
    }
    // 先算出"这一帧哪些字段可见、放在哪"，再逐个摆放一次。早先的实现先全部隐藏再逐个显示，
    // 每次 SetWindowPos/ShowWindow 都会让父窗口重绘 → 重绘里又调这里 → 125fps 自激。
    std::map<TextField*,RECT> visibleFields;
    const bool panelVisible=panel_&&IsWindowVisible(panel_);
    // 下拉列表（来源供应商 / 模型 ▾ / 添加页供应商）是自绘浮层，而原生 EDIT 是子窗口、
    // 永远画在父窗口之上：不隐藏的话列表会被输入框盖住，点列表也会点到输入框上
    // （真机反馈"下拉里能看到输入框、选不到模型"）。
    // v1.33：设置页「关闭按钮」下拉也是自绘浮层，漏在判定外时输入框会盖住选项。
    const bool overlayOpen=fbSelectOpen_||fbModelSelectOpen_||addSelectOpen_||settingsSelectOpen_;
    const bool blocked=pending_!=Pending::None||overlayOpen;
    const bool offline=!hermesState_.error.empty()||hermesState_.wsl=="down";
    if(panelVisible&&!blocked&&!offline) {
        // 内缩量按窗口 DPI（与绘制同源）：monitor_ 在拖动跨屏期间是旧显示器的倍率。
        const auto dpi=WindowDpi();
        // 原生控件是**子窗口**，不吃 D2D 的裁剪区：页内滚动后它会盖到页头、底部动作栏或
        // 底栏导航上（D2D 那层被裁掉了，子窗口不会）。只摆"整块都在当前页视口里"的字段；
        // 半截滚出视口的字段由自绘层照常显示（文字在原生控件底下，被遮住的部分本来也看不见）。
        const bool hasViewport=layout.pageViewport.right>layout.pageViewport.left
            &&layout.pageViewport.bottom>layout.pageViewport.top;
        // Toast / 状态中心卡片也是 D2D 画的浮层，会被子窗口（原生输入框）盖住：
        // 浮层出现时把与它重叠的字段藏起来（自绘层的框与值还在，视觉无缝；
        // 焦点字段不藏，否则正在输入的内容会掉）。浮层矩形为空时自动不生效。
        const auto intersects=[&](const RECT& box,const D2D1_RECT_F& rect) {
            return rect.right>rect.left&&rect.bottom>rect.top
                &&box.left<static_cast<LONG>(rect.right)&&static_cast<LONG>(rect.left)<box.right
                &&box.top<static_cast<LONG>(rect.bottom)&&static_cast<LONG>(rect.top)<box.bottom;
        };
        const auto underOverlay=[&](const RECT& box) {
            return intersects(box,layout.toast)||intersects(box,layout.statusCard);
        };
        const auto insideViewport=[&](const RECT& box) {
            if(!hasViewport) return true;
            constexpr LONG slack=1;   // 半像素舍入
            return box.left>=static_cast<LONG>(layout.pageViewport.left)-slack
                &&box.right<=static_cast<LONG>(layout.pageViewport.right)+slack
                &&box.top>=static_cast<LONG>(layout.pageViewport.top)-slack
                &&box.bottom<=static_cast<LONG>(layout.pageViewport.bottom)+slack;
        };
        if(activePage_==0&&layout.search.right>layout.search.left) {
            // 左边留出放大镜；右边让开 ✕ 清空按钮与「刷新同步」按钮（两者都在搜索框右端）。
            // （真机反馈：✕ / 同步按钮被原生输入框盖住，点不到、文字也看不到）。
            const auto leftInset=static_cast<LONG>(ScaleDip(30,dpi));
            // 右侧要让开 ✕（search.right-114..-94）与「刷新同步」按钮。原来给 100 DIP，只让到
            // ✕ 的中间，清空按钮被输入框盖住大半——真机上只剩紧贴边框的一条":"。
            const auto rightInset=static_cast<LONG>(ScaleDip(118,dpi));
            visibleFields[&search_]=RECT{static_cast<LONG>(layout.search.left)+leftInset,
                static_cast<LONG>(layout.search.top)+static_cast<LONG>(ScaleDip(3,dpi)),
                static_cast<LONG>(layout.search.right)-rightInset,
                static_cast<LONG>(layout.search.bottom)-static_cast<LONG>(ScaleDip(3,dpi))};
        }
        for(const auto& field:layout.fields) {
            TextField* target=nullptr;
            if(activePage_==1) target=FormFieldAt(field.index);
            else if(activePage_==2) target=AddFieldAt(field.index);
            else if(activePage_==4) {
                switch(field.index) {
                case 0: target=&setDistro_;break;
                case 1: target=&setWebuiPort_;break;
                case 2: target=&setPollSec_;break;
                default: break;
                }
            }
            if(!target||!NativeFor(target)) continue;
            const auto insetX=static_cast<float>(ScaleDip(9,dpi));
            const auto insetY=static_cast<float>(ScaleDip(3,dpi));
            // 密码字段右内侧有「眼睛」按钮（26 DIP + 右内边距 5 DIP）：输入框要让开，
            // 否则按钮被原生控件盖住，只剩贴着右边框的一条边（真机看到的"一个 > 符号"）。
            const LONG rightInset=static_cast<LONG>(insetX)
                +(field.kind==1?static_cast<LONG>(ScaleDip(24,dpi)):0);
            RECT box{static_cast<LONG>(field.box.left+insetX),static_cast<LONG>(field.box.top+insetY),
                static_cast<LONG>(field.box.right)-rightInset,
                static_cast<LONG>(field.box.bottom-insetY)};
            if(box.right<=box.left+4||box.bottom<=box.top) continue;
            if(!insideViewport(box)) continue;
            if(target!=focusedField_&&underOverlay(box)) continue;
            visibleFields[target]=box;
        }
    }
    for(auto& binding:nativeBindings_) {
        const auto found=visibleFields.find(binding.field);
        if(found==visibleFields.end()) binding.edit->Place(RECT{},false);
        else binding.edit->Place(found->second,true);
    }
    // 文本必须在**摆好并显示之后**再推：控件隐藏期间 SetWindowTextW 是不生效的
    // （窗口不可见时系统不更新控件文本），先推后显示会得到"字段是空的"。
    for(auto& binding:nativeBindings_) {
        if(!binding.edit||binding.field==focusedField_) continue;
        binding.edit->SetText(binding.field->Text());
    }
}

void Application::SyncBusyBar(const PanelLayout& layout,const D2D1_RECT_F& pageHeadIcon) {
    const bool busy=workerBusy_.load()||probeRunning_.load()||allProbeBusy_.load()
        ||syncAllBusy_.load();
    const bool show=busy&&panel_&&IsWindowVisible(panel_)&&pending_==Pending::None
        &&layout.content.right>layout.content.left;
    if(!show) {
        busyBar_.Place(RECT{},false);
        spinGlyph_.Place(RECT{},false);
        spinGlyphShown_=false;
        if(busyBarTimerArmed_) { KillTimer(panel_,kSpinTimer);busyBarTimerArmed_=false; }
        return;
    }
    if(!busyBarTimerArmed_&&panel_) { busyBarTimerArmed_=true;SetTimer(panel_,kSpinTimer,kSpinIntervalMs,nullptr); }
    const auto dpi=WindowDpi();
    RECT bar{static_cast<LONG>(layout.content.left),
        static_cast<LONG>(layout.lampRow.top),
        static_cast<LONG>(layout.content.right),
        static_cast<LONG>(layout.lampRow.top+ScaleDip(3,dpi))};
    const auto toColour=[](const D2D1_COLOR_F& colour) {
        return RGB(static_cast<BYTE>(std::lround(colour.r*255.0f)),
            static_cast<BYTE>(std::lround(colour.g*255.0f)),
            static_cast<BYTE>(std::lround(colour.b*255.0f)));
    };
    // 底色必须取忙碌条所在那一行的颜色（lampRow = surf）：取 ink 会在状态行顶部
    // 压出一条比周围更深的暗带（真机能看到一条深色横条）。
    busyBar_.SetColours(toColour(palette_.surf),toColour(palette_.accent));
    // 旋转图标只画在"它对应的按钮"里，位置一律取真实落点，绝不跨页复用坐标（G94 的教训：
    // layout.syncModels 挂在 layout.search 上、与页面无关地算出来，别的页面同一坐标正好落着
    // 页头动作按钮，圆环就压到了「检测中 1/2」文字上）。现在两张目标各自成立才画：
    //   模型页 + 面板忙  ⇒ 搜索框右端的「刷新同步」（图标格固定：距左边 9 DIP、画 12 DIP）
    //   备份页 + 列表加载 ⇒ 页头的「刷新」（图标格由渲染层回传：忙碌时文案换成「扫描中…」，
    //                       居中内容的宽度跟着变，格子会移动，只能用它算出来的那个）
    // 不用子窗口画的原因：主面板整窗重绘 10–170ms，拿它当动画时钟只会"不转、只在抖"。
    D2D1_RECT_F glyphCell{};
    bool glyphWanted=false;
    //   模型页 + 正在同步（SyncAllModels）⇒ 搜索框右端的「刷新同步」
    if(activePage_==0&&syncAllBusy_.load()&&layout.syncModels.right>layout.syncModels.left) {
        const auto dpiScale=static_cast<float>(dpi)/96.0f;
        const auto glyph=12.0f*dpiScale;
        const auto left=layout.syncModels.left+9.0f*dpiScale;
        const auto centreY=(layout.syncModels.top+layout.syncModels.bottom)/2.0f;
        glyphCell=D2D1::RectF(left,centreY-glyph/2.0f,left+glyph,centreY+glyph/2.0f);
        glyphWanted=true;
    } else if(activePage_==3&&backupBusy_&&pageHeadIcon.right>pageHeadIcon.left) {
        const auto dpiScale=static_cast<float>(dpi)/96.0f;
        const auto glyph=12.0f*dpiScale;
        const auto centreX=(pageHeadIcon.left+pageHeadIcon.right)/2.0f;
        const auto centreY=(pageHeadIcon.top+pageHeadIcon.bottom)/2.0f;
        glyphCell=D2D1::RectF(centreX-glyph/2.0f,centreY-glyph/2.0f,
            centreX+glyph/2.0f,centreY+glyph/2.0f);
        glyphWanted=true;
    }
    if(glyphWanted) {
        RECT glyphRect{static_cast<LONG>(std::lround(glyphCell.left)),
            static_cast<LONG>(std::lround(glyphCell.top)),
            static_cast<LONG>(std::lround(glyphCell.right)),
            static_cast<LONG>(std::lround(glyphCell.bottom))};
        spinGlyph_.SetColours(toColour(palette_.accentStrong));
        spinGlyph_.Place(glyphRect,true);
        spinGlyphShown_=true;
    } else if(spinGlyphShown_) {
        // 不画的这条分支必须显式收起：切到别的页面时它不会自己消失，会留在上一页放好的位置上。
        spinGlyph_.Place(RECT{},false);
        spinGlyphShown_=false;
    }
    busyBar_.Place(bar,true);
}
void Application::HandleNativeFieldEvent(int id,int event) {
    TextField* field=nullptr;
    const int index=id-kNativeFieldBase;
    if(index>=0&&index<static_cast<int>(nativeBindings_.size()))
        field=nativeBindings_[static_cast<std::size_t>(index)].field;
    if(!field) return;
    using Event=NativeField::Event;
    const auto kind=static_cast<Event>(event);
    if(kind==Event::Focus) {
        // 原生控件拿到焦点：只同步 focusedField_（文本由 EN_CHANGE 维护，不再用镜像覆盖控件）。
        if(focusedField_!=field) {
            if(focusedField_) focusedField_->Blur();
            focusedField_=field;
            field->Focus();
            caretOn_=true;
            UpdateCaretTimer();
            InvalidateAll();
        }
        return;
    }
    if(kind==Event::Blur) {
        // 文本同步与提交都在 BlurField 里（CommitFocusedField），这里只判是不是同一个字段。
        if(focusedField_==field) BlurField();
        return;
    }
    if(kind==Event::Pressed) {
        // 鼠标点进控件：控件已经持有键盘焦点时点击不会产生 WM_SETFOCUS，只靠 Focus 事件
        // 就学不到"用户又开始编辑了"，随后的输入会在失焦时被静默丢弃（真机：同一字段第二次
        // 编辑永远不提交）。这里只把焦点指回来——**不做 field->Focus()**，那会 EM_SETSEL 全选，
        // 把用户刚点出来的插入点冲掉。
        if(focusedField_!=field) {
            if(focusedField_) CommitFocusedField();   // 上一个字段还标着编辑：先写回，不抢焦点
            focusedField_=field;
            caretOn_=true;
            UpdateCaretTimer();
            InvalidateAll();
        }
        return;
    }
    if(kind==Event::Change) {
        // EN_CHANGE 在每次按键后到达：立刻镜像，否则"点了探测/保存但还没失焦"时读到旧文本。
        if(auto* native=NativeFor(field)) {
            const auto text=Narrow(native->Text());
            native->RefreshCache();      // 控件里的才是真的：缓存跟上，后续镜像推送才不会回写旧值
            if(field->TextUtf8()!=text) {
                field->SetTextUtf8(text);
                // 文本变了就要重画：搜索框的值决定列表（过滤后扁平视图、清除按钮、收藏/最近的计数），
                // 原生 EDIT 只更新自己的客户端，D2D 那半边不会自动跟上 —— 以前打字后列表一直是旧帧，
                // 直到鼠标移进面板触发一次悬停重绘（真机：搜索结果不出来）。
                InvalidateAll();
            }
        }
        return;
    }
    if(kind==Event::ArrowUp||kind==Event::ArrowDown) {
        // 搜索框的 ↑↓ = 移动列表光标（自绘字段时代由 HandleFieldKey 转发）。
        if(activePage_==0) HandleListKey(kind==Event::ArrowUp?VK_UP:VK_DOWN);
        return;
    }
    if(kind==Event::Enter||kind==Event::Escape||kind==Event::TabForward||kind==Event::TabBackward) {
        // 与自绘字段的键盘路径对齐：模型页的搜索框回车 = 切换选中行；其余单行字段回车/转义 = 失焦。
        if(activePage_==0&&kind==Event::Enter) { HandleListKey(VK_RETURN);return; }
        if(kind==Event::Escape&&field==&search_) { ClearSearch();return; }
        if(kind==Event::Escape) {
            if(fbSelectOpen_) { fbSelectOpen_=false;InvalidateAll();return; }
            if(fbFormOpen_) { CloseForm();return; }
        }
        if(kind==Event::TabForward) { MoveFocus(1);return; }
        if(kind==Event::TabBackward) { MoveFocus(-1);return; }
        if(focusedField_==field) {
            field->SetTextUtf8(Narrow(NativeFor(field)->Text()));
            BlurField();
        }
        return;
    }
}
void Application::ApplyState(const char* reason,State previous) {
    if(log_&&previous!=state_) log_->Info("state",std::string(reason)+" "+std::to_string(static_cast<int>(previous))
        +"->"+std::to_string(static_cast<int>(state_)));
    const bool visibilityChanged=VisibleFor(previous)!=VisibleFor(state_);
    const bool enteringCollapsed=visibilityChanged&&state_==State::Collapsed;
    if(visibilityChanged&&!enteringCollapsed) geometry_.Visibility(state_);
    if(enteringCollapsed) {
        // 收起路径（真机 2026-10-20/21：每次收起贴条都"闪一下+玻璃变色"）：
        // ① 面板先藏、贴条保持隐藏 —— 抓屏要的就是"面板不在"的那一帧；
        // ② **不再重抓玻璃快照**：快照本来拍的就是面板没展开时的桌面，收起后桌面恢复原样，
        //    复用即可。以前每次收起重抓（CaptureBackdrop 对可见窗口要 hide→DwmFlush→抓→show），
        //    那一步既是肉眼可见的闪烁，也是玻璃染上面板残影的原因；而同步等合成器（DwmFlush）
        //    还会把消息循环顶住 —— a-edge 门禁整轮失败就是它（实测 2026-10-21）。
        // ③ 需要新快照的场合各自显式失效：拖动落点（CommitStripDrop）、换显示器/DPI。
        ShowWindow(panel_,SW_HIDE);
        ShowWindow(strip_,SW_HIDE);
        if(model_.edge==Edge::None) {
            const auto widget=WidgetSize(model_.widget,Edge::Left,monitor_.geometry.dpi);
            const auto& work=monitor_.geometry.work;
            // 与几何层的 Clamp 同一防颠倒语义（work 比 widget 还窄时贴住左上角）。
            const int x=std::clamp(model_.floating.x,work.x,std::max(work.x,work.right()-widget.width));
            const int y=std::clamp(model_.floating.y,work.y,std::max(work.y,work.bottom()-widget.height));
            SetWindowPos(strip_,nullptr,x,y,widget.width,widget.height,SWP_NOACTIVATE|SWP_NOZORDER);
        }
    }
    // 动效触发：收起 → 贴条淡入（面板按纪律立即隐藏，收起方向只有贴条做动画）。
    // 展开方向**不做任何动效**（用户 2026-10-21 要求："展开能不能不要动画和效果了"）：
    // 不位移也不淡入，展开即完整显示。切页的内容淡入是另一回事，仍由 StartPanelMotion() 触发。
    if(previous!=State::Collapsed&&state_==State::Collapsed) StartStripMotion();
    // 外部窗口状态（Aero Snap / Win+↑ 最大化 / 系统调整）可能让实际矩形偏离模型；状态迁移时
    // 先把模型落到窗口，再校验。否则托盘/热键路径会抛 INV1（无 catch → 进程直接退出）。
    const auto expected=ComputeRect(model_,monitor_.geometry);
    if(GeometryController::Read(panel_)!=expected.panel
        ||(expected.strip&&GeometryController::Read(strip_)!=*expected.strip))
        geometry_.Place(model_,monitor_.geometry,state_,"reconcile");
    geometry_.Validate(model_,monitor_.geometry,state_);
    // DESIGN v1.7：可见性变化 → 轮询节奏跟着变（收起 60s / 隐藏停）；重新可见立即刷新一次，
    // 保证从贴条/托盘回到面板时看到的就是当前状态。刷新走 30ms 一次性定时器**延后触发**：
    // StartRefresh 会在 UI 线程 join 上一轮 worker（可能正卡在 wsl.exe 上百毫秒），在点击
    // 处理里同步调它会让「点贴条展开」体感卡顿甚至超时（a-pullout/a-edge 门禁抓到）。
    if(visibilityChanged) {
        UpdatePollTimer();
        if(state_!=State::Hidden) SetTimer(panel_,kPollNowTimer,30,nullptr);
    }
    if(state_==State::Collapsed) {
        // 贴条重新出现：先按缓存快照画好第一帧（贴条此刻仍隐藏，画在隐藏窗口上），再显示——
        // 不再在收起时重抓快照（见 enteringCollapsed 的注释：重抓才是"闪一下+变色"的来源）。
        if(enteringCollapsed) {
            RenderStrip();
            ShowWindow(strip_,SW_SHOWNOACTIVATE);
            InvalidateRect(strip_,nullptr,FALSE);
        } else RenderStrip();
    }
    HideHoverCard();   // 状态/几何变了：卡片内容和位置都作废，下次悬停重新计算
    (void)reason;
}
bool Application::HitStripShape(POINT client) const {
    // 分层窗口的透明角落不应吃鼠标：用与绘制同一份几何做命中（区域只用于测试，不设到窗口）。
    if(!strip_) return false;
    RECT bounds{};
    GetClientRect(strip_,&bounds);
    const int width=bounds.right-bounds.left,height=bounds.bottom-bounds.top;
    if(width<=0||height<=0) return false;
    if(client.x<0||client.y<0||client.x>=width||client.y>=height) return false;
    const auto metrics=WidgetMetricsFor(model_.widget,monitor_.geometry.dpi);
    // canonical → 窗口坐标（与渲染的旋转换算一致）：x 从贴边侧指向桌面，y 沿贴边方向。
    // 浮起胶囊没有贴边，窗口就是 canonical 方向（竖条）。
    const bool vertical=model_.edge==Edge::Left||model_.edge==Edge::Right||model_.edge==Edge::None;
    const int cw=vertical?width:height,ch=vertical?height:width;
    const auto map=[&](int x,int y) -> POINT {
        switch(model_.edge) {
        case Edge::Right: return {width-x,height-y};
        case Edge::Top: return {width/2-(y-ch/2),height/2+(x-cw/2)};
        case Edge::Bottom: return {width/2+(y-ch/2),height/2-(x-cw/2)};
        default: return {x,y};
        }
    };
    const auto polygon=[&](std::initializer_list<POINT> points) {
        std::vector<POINT> mapped;
        mapped.reserve(points.size());
        for(const auto& point:points) mapped.push_back(map(point.x,point.y));
        return CreatePolygonRgn(mapped.data(),static_cast<int>(mapped.size()),WINDING);
    };
    HRGN region=nullptr;
    switch(model_.widget) {
    case WidgetStyle::Tag:
        region=polygon({{1,1},{cw-1,1},{cw-1,ch-1},{1,ch-1}});
        break;
    case WidgetStyle::Capsule: {
        // 旧胶囊：桌面侧全圆角，贴边侧平直（窗口的贴边侧也在屏幕上，那里要能吃鼠标）。
        const int radius=std::min(cw,ch)/2;
        region=CreateRoundRectRgn(0,0,width+1,height+1,radius*2,radius*2);
        HRGN flat=nullptr;
        switch(model_.edge) {
        case Edge::Left: flat=CreateRectRgn(0,0,width/2+1,height+1);break;
        case Edge::Right: flat=CreateRectRgn(width/2,0,width+1,height+1);break;
        case Edge::Top: flat=CreateRectRgn(0,0,width+1,height/2+1);break;
        case Edge::Bottom: flat=CreateRectRgn(0,height/2,width+1,height+1);break;
        default: break;
        }
        if(flat) {
            CombineRgn(region,region,flat,RGN_OR);
            DeleteObject(flat);
        }
        break;
    }
    }
    const bool inside=region&&PtInRegion(region,client.x,client.y)!=0;
    if(region) DeleteObject(region);
    return inside;
}
void Application::RenderStrip() {
    if(!strip_||state_!=State::Collapsed) return;
    try {
        // 液态玻璃的背景：抓一次桌面缓存（贴条隐藏时抓，避免把自己的画面糊进去），
        // 之后轮询重绘直接复用；拖动中不抓（窗口每帧在动，抓取会闪），落手后失效重抓。
        if(!stripDragging_) stripContext_.surface.EnsureBackdrop(strip_);
        auto view=CurrentView();
        if(!stripDragging_) {
            view.backdrop.pixels=stripContext_.surface.BackdropPixels().data();
            view.backdrop.width=stripContext_.surface.BackdropWidth();
            view.backdrop.height=stripContext_.surface.BackdropHeight();
            view.backdrop.generation=stripContext_.surface.BackdropGeneration();
            // 这一帧没有玻璃快照 = 板子会画成不透明的实色（真机表现为"闪一下"）。抓屏失败或
            // 快照被清掉时不再静默：只有拖动中的帧允许没有快照。
            if(log_&&view.backdrop.width<=0)
                log_->Warn("paint","strip frame without a glass snapshot (state="
                    +std::to_string(static_cast<int>(state_))+" edge="
                    +std::to_string(static_cast<int>(model_.edge))+")");
        }
        // 玻璃快照只在代次变化时记一行：真机上"抓没抓到、抓到了什么"必须能从日志判断
        // （真机 2026-10-20 报"玻璃像噪点"，就是靠这行定位到快照缺失的）。
        // 拖动中的帧没有快照（generation=0）：那一帧不记日志，否则会刷出误导性的
        // "size=0x0" 和上一轮残留的 mean/noise（诊断日志只在真的抓到新快照时写一行）。
        if(log_&&view.backdrop.pixels&&view.backdrop.generation!=loggedBackdropGeneration_) {
            loggedBackdropGeneration_=view.backdrop.generation;
            log_->Info("paint",std::string("strip backdrop gen=")
                +std::to_string(view.backdrop.generation)+" size="
                +std::to_string(view.backdrop.width)+"x"+std::to_string(view.backdrop.height)
                +" mean=0x"+[this] {
                    char buffer[16]{};
                    std::snprintf(buffer,sizeof(buffer),"%06X",stripContext_.surface.BackdropMean());
                    return std::string(buffer);
                }()+" noise="+[this] {
                    char buffer[32]{};
                    std::snprintf(buffer,sizeof(buffer),"%.1f->%.1f",
                        stripContext_.surface.BackdropNoiseBefore(),stripContext_.surface.BackdropNoise());
                    return std::string(buffer);
                }());
        }
        stripFlowView_=view;      // 供拖动帧复用（只改预览边/圆角）
        stripFlowViewValid_=true;
        stripContext_.surface.PaintStrip(strip_,view,palette_);
    } catch(const std::exception& error) {
        if(log_) log_->Error("paint",std::string("strip: ")+error.what());
    }
}
void Application::RenderStripDragFrame() {
    if(!strip_||state_!=State::Collapsed) return;
    // 第一次拖动帧先全量建一次缓存视图（此时 stripDragging_ 已为 true，视图不带快照）。
    if(!stripFlowViewValid_) { RenderStrip();return; }
    auto& cached=stripFlowView_;
    cached.widgetEdge=static_cast<int>(previewEdge_);
    cached.widgetVertical=previewEdge_==1||previewEdge_==2;
    cached.backdrop={};                    // 拖动中不透背景（窗口在动，快照必过期）
    cached.motion.capsuleDocked=capsuleDockOverride_>=0.0f?capsuleDockOverride_:1.0f;
    cached.motion.stripAlpha=1.0f;
    try {
        stripContext_.surface.PaintStrip(strip_,cached,palette_);
    } catch(const std::exception& error) {
        stripFlowViewValid_=false;
        if(log_) log_->Error("paint",std::string("strip drag: ")+error.what());
    }
}
void Application::Show() {
    const auto previous=state_;
    state_=NextState(state_,Event::Show,model_.edge);
    ApplyState("show",previous);
}
void Application::Hide() {
    const auto previous=state_;
    state_=NextState(state_,Event::Hide,model_.edge);
    ApplyState("hide",previous);
}
void Application::DockNearest() {
    if(model_.edge!=Edge::None || state_==State::Hidden) return;
    const auto rect=GeometryController::Read(panel_);
    monitor_=MonitorAt(CenterOf(rect));
    const auto edge=NearestEdge(rect,monitor_.geometry);
    if(edge==Edge::None) return;
    floatingBeforeDock_={rect.x,rect.y};
    floatingRemembered_=true;
    const auto previous=state_;
    model_.edge=edge;
    model_.anchor=edge==Edge::Left||edge==Edge::Right?rect.y:rect.x;
    // 胶囊贴边过渡：浮起椭圆 → 贴边直角。
    if(stored_.widgetStyle=="capsule") {
        capsuleDocked_=true;
        if(AnimationsEnabled()) { capsuleDockStart_=GetTickCount64();KeepMotionTimer(); }
    }
    state_=NextState(state_,Event::DropDocked,edge);
    geometry_.Place(model_,monitor_.geometry,state_,"dock");
    // 贴条矩形变了（浮起→贴边）：玻璃快照作废，重抓（收起路径已不再每次重抓）。
    stripContext_.surface.InvalidateBackdrop();
    ApplyState("dock",previous);
    PersistSettings();
}
void Application::Undock() {
    if(model_.edge==Edge::None) return;
    const auto previous=state_;
    model_.edge=Edge::None;
    // 胶囊浮起过渡：贴边直角 → 完整椭圆（DESIGN v1.8 真机反馈）。
    if(stored_.widgetStyle=="capsule") {
        capsuleDocked_=false;
        if(AnimationsEnabled()) { capsuleDockStart_=GetTickCount64();KeepMotionTimer(); }
    }
    if(floatingRemembered_) {
        const auto size=TierSize(model_,monitor_.geometry.dpi);
        model_.floating=ClampFloating(floatingBeforeDock_,size,monitor_.geometry.work);
        floatingRemembered_=false;
    } else {
        model_.floating=ClampFloating(CenterOf(GeometryController::Read(panel_)),TierSize(model_,monitor_.geometry.dpi),monitor_.geometry.work);
    }
    state_=NextState(state_,Event::Undock,Edge::None);
    geometry_.Place(model_,monitor_.geometry,state_,"undock");
    ApplyState("undock",previous);
    PersistSettings();
}
void Application::LogEvent(const char* name,POINT messagePoint) {
    POINT cursor{messagePoint.x||messagePoint.y?messagePoint:POINT{}};
    if(!cursor.x&&!cursor.y) GetCursorPos(&cursor);
    const auto hit=WindowFromPoint(cursor);
    const int code=hit==panel_?1:hit==strip_?2:0;
    events_.push_back({name,static_cast<unsigned>(GetTickCount64()&0xFFFFFFFFu),static_cast<unsigned>(GetMessageTime()),cursor.x,cursor.y,
        static_cast<int>(state_),code});
    if(events_.size()>64) events_.erase(events_.begin(),events_.begin()+32);
}
void Application::PointerEnteredStrip() {
    // 贴条**单击**展开（WM_LBUTTONDOWN 调用）；悬停不展开。
    ++stripMoves_;
    HideHoverCard();
    if(dragging_) return;
    if(state_!=State::Collapsed || model_.edge==Edge::None) {
        if(log_) log_->Info("ui","strip-click ignored state="+std::to_string(static_cast<int>(state_))
            +" edge="+std::to_string(static_cast<int>(model_.edge))+" visible="
            +std::to_string(IsWindowVisible(strip_)!=0));
        return;
    }
    const auto previous=state_;
    state_=NextState(state_,Event::EnterStrip,model_.edge);
    ApplyState("strip-click",previous);
    if(state_!=State::Expanded) return;
    // Arm the single grace timer the moment the panel appears: the pointer may already be gone
    // (a single fast move out of the strip never produces an in-panel WM_MOUSEMOVE, so no
    // WM_MOUSELEAVE would ever arrive), and the containment check at expiry is the authority.
    LogEvent("grace-armed");
    graceTimer_=true;
    SetTimer(panel_,kTrayGraceTimer,kGraceMilliseconds,nullptr);
}
void Application::SetHoverControl(int id) {
    if(id==hoverControl_) return;
    if(AnimationsEnabled()) {
        hoverPrevious_=hoverControl_;
        hoverMotionStart_=GetTickCount64();
        KeepMotionTimer();
    }
    hoverControl_=id;
    if(panel_) InvalidateRect(panel_,nullptr,FALSE);
}
bool Application::AnimationsEnabled() const {
    // 跟随系统「显示动画」开关（SPI_GETCLIENTAREAANIMATION）；1 秒缓存，避免每帧都查。
    // 控制面 configure animations=false 可以让门禁在确定性（无动画）模式下跑。
    const auto now=GetTickCount64();
    if(!animCacheValid_||now-animCacheTick_>1000) {
        BOOL enabled=TRUE;
        const bool system=SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&enabled,0)==FALSE||enabled!=FALSE;
        animCache_=system;
        animCacheValid_=true;
        animCacheTick_=now;
    }
    return animCache_&&animOverride_;
}
void Application::KeepMotionTimer() {
    if(motionTimer_||!panel_) return;
    // SetTimer 失败就别把标志置上：否则之后不再重试，动画会停在中间值。
    motionTimer_=SetTimer(panel_,kMotionTimer,kMotionTickMs,nullptr)!=0;
}
void Application::StartPanelMotion() {
    // 只剩"切页内容淡入"这一个用途（展开入场已按用户要求移除，见 DESIGN v1.11）。
    if(!AnimationsEnabled()) { panelMotionStart_=0;return; }
    panelMotionStart_=GetTickCount64();
    KeepMotionTimer();
}
void Application::StartCardMotion() {
    cardExitStart_=0;
    if(!AnimationsEnabled()) { cardMotionStart_=0;return; }
    cardMotionStart_=GetTickCount64();
    KeepMotionTimer();
}
void Application::StartStripMotion() {
    if(!AnimationsEnabled()) { stripMotionStart_=0;return; }
    stripMotionStart_=GetTickCount64();
    KeepMotionTimer();
}
void Application::StartTooltipMotion() {
    if(!AnimationsEnabled()) { tooltipMotionStart_=0;return; }
    tooltipMotionStart_=GetTickCount64();
    KeepMotionTimer();
}
void Application::StartThemeMotion(const ThemePalette& target) {
    // 交叉淡入的契约：从**当前** palette_ 过渡到 target。目标只由调用方给出。
    // 以前这里写死 `targetPalette_=palette_`，把调用方刚解析的新主题覆盖回旧值 ——
    // 动画变成"旧→旧"，开动画时主题切换完全失效（真机 2026-10-20 报「主题切换失效」；
    // 关动画走的是下面这条立即落地分支，所以门禁一直是绿的，掩盖了它）。
    previousPalette_=palette_;
    targetPalette_=target;
    if(!AnimationsEnabled()) {
        themeMotionStart_=0;themeStartValid_=false;
        palette_=targetPalette_;          // 无动画：立即落到目标主题
        return;
    }
    themeMotionStart_=GetTickCount64();
    themeStartValid_=true;
    KeepMotionTimer();
}
void Application::MotionTick() {
    const auto now=GetTickCount64();
    const auto active=[&](unsigned long long start,unsigned duration) {
        return start!=0&&now-start<duration;
    };
    bool any=false;
    if(active(panelMotionStart_,kPanelMotionMs)) { any=true;InvalidateRect(panel_,nullptr,FALSE); }
    else panelMotionStart_=0;
    if(active(cardMotionStart_,kCardMotionMs)) { any=true;RenderHoverCard(); }
    else cardMotionStart_=0;
    if(active(stripMotionStart_,kStripEnterMs)) { any=true;if(strip_) InvalidateRect(strip_,nullptr,FALSE); }
    else stripMotionStart_=0;
    if(active(capsuleDockStart_,kCapsuleDockMs)) { any=true;if(strip_) InvalidateRect(strip_,nullptr,FALSE); }
    else capsuleDockStart_=0;
    if(active(cardExitStart_,kCardExitMs)) { any=true;if(hoverCard_) InvalidateRect(hoverCard_,nullptr,FALSE); }
    else if(cardExitStart_) {
        cardExitStart_=0;
        if(hoverCard_) ShowWindow(hoverCard_,SW_HIDE);
    }
    if(active(tooltipMotionStart_,kTooltipMotionMs)) { any=true;InvalidateRect(panel_,nullptr,FALSE); }
    else tooltipMotionStart_=0;
    if(active(hoverMotionStart_,kHoverMotionMs)) { any=true;InvalidateRect(panel_,nullptr,FALSE); }
    else hoverMotionStart_=0;
    if(active(themeMotionStart_,kThemeMotionMs)) { any=true;InvalidateAll(); }
    else {
        // 收尾必须落到目标调色板：最后一帧 CurrentView 可能停在 0.99，直接清标志会留下偏差。
        if(themeMotionStart_&&themeStartValid_) palette_=targetPalette_;
        themeMotionStart_=0;themeStartValid_=false;
    }
    if(!any&&motionTimer_) { KillTimer(panel_,kMotionTimer);motionTimer_=false; }
}
void Application::PointerMovedOnPanel(POINT messagePoint) {
    ++panelMoves_;
    LogEvent("panel-move",messagePoint);
    if(dragging_) return;
    TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,panel_,0};
    TrackMouseEvent(&track);
    if(graceTimer_) { KillTimer(panel_,kTrayGraceTimer);graceTimer_=false; }
}
void Application::PointerLeftPanel(POINT messagePoint) {
    ++panelLeaves_;
    LogEvent("panel-leave",messagePoint);
    if(dragging_) return;
    if(state_!=State::Expanded) return;
    ++graces_;
    graceTimer_=true;
    SetTimer(panel_,kTrayGraceTimer,kGraceMilliseconds,nullptr);
}
void Application::GraceExpired() {
    LogEvent("grace-expired");
    KillTimer(panel_,kTrayGraceTimer);
    graceTimer_=false;
    if(dragging_) return;
    if(state_!=State::Expanded) return;
    POINT cursor{};
    GetCursorPos(&cursor);
    if(Contains(GeometryController::Read(panel_),{cursor.x,cursor.y})) {
        // The system can post WM_MOUSELEAVE whose own coordinates are inside the panel when
        // real input is coalesced, so the collapse decision is made on geometry, never on that
        // notification alone (same containment rule as the reference implementation).
        LogEvent("grace-rearmed");
        graceTimer_=true;
        SetTimer(panel_,kTrayGraceTimer,kGraceMilliseconds,nullptr);
        return;
    }
    const auto previous=state_;
    state_=NextState(state_,Event::LeaveGrace,model_.edge);
    ApplyState("hover-collapse",previous);
}
UINT Application::WindowDpi() const {
    // 布局/命中/原生输入框的倍率必须和**绘制**用同一个 DPI：Surface::Paint 取
    // GetDpiForWindow(panel_)。monitor_.geometry.dpi 只在 ApplyDpiChange / CommitDrop 里更新，
    // 系统移动循环期间还停在上一个显示器上——拖动跨屏时若拿它算布局，搜索框会被按旧倍率
    // 摆到框外（真机：占位「搜索模型或供应商」跑到输入框下面且字号大一圈）。
    if(panel_) {
        const UINT dpi=GetDpiForWindow(panel_);
        if(dpi) return dpi;
    }
    const auto dpi=static_cast<UINT>(monitor_.geometry.dpi);
    return dpi?dpi:96;
}

void Application::RefreshNativeFieldMetrics() {
    // v1.26：跨显示器（DPI 变化）时原生 EDIT 的字号要重建，否则 125%→100% 后文字明显偏大/偏小。
    // 用窗口自己的 DPI：拖动跨屏时系统移动循环会先换窗口 DPI，monitor_ 要到松手才更新。
    const auto dpi=WindowDpi();
    for(auto& binding:nativeBindings_)
        if(binding.edit) binding.edit->SetFontAndDpi(dpi);
}

void Application::ApplyDpiChange(const RECT& suggested) {
    // 拖动/拉伸期间窗口几何归交互循环；但**字体倍率**必须立刻跟上窗口 DPI：
    // 窗口已经在另一台显示器上（GetDpiForWindow 已变），EDIT 还按旧 DPI 的字号画，
    // 拖动跨屏时占位文字就会比框大一圈、位置也对不上。
    RefreshNativeFieldMetrics();
    if(dragging_||resizing_) { if(panel_) InvalidateRect(panel_,nullptr,FALSE);return; }
    const auto resolved=MonitorFromRect(&suggested,MONITOR_DEFAULTTONEAREST);
    monitor_=ReadMonitor(resolved);
    if(model_.edge==Edge::None) model_.floating={suggested.left,suggested.top};
    const auto expected=ComputeRect(model_,monitor_.geometry);
    // Both windows are notified for one DPI change; skip when the model outcome is already applied.
    if(GeometryController::Read(panel_)==expected.panel&&
        (!expected.strip||GeometryController::Read(strip_)==*expected.strip)) return;
    // 换屏=换桌面：玻璃快照必须重抓（收起路径已不再每次重抓，见 ApplyState 的注释）。
    stripContext_.surface.InvalidateBackdrop();
    const auto previous=state_;
    geometry_.Place(model_,monitor_.geometry,state_,"dpi-changed");
    ApplyState("dpi-changed",previous);
}
void Application::RefreshTopology(const char* reason) {
    if(dragging_||resizing_) return;
    const auto rect=GeometryController::Read(panel_);
    RECT native{rect.x,rect.y,rect.right(),rect.bottom()};
    const auto resolved=MonitorFromRect(&native,MONITOR_DEFAULTTONULL);
    if(!resolved) {
        const auto primary=ReadMonitor(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY));
        monitor_=primary;
        if(model_.edge==Edge::None) {
            model_.floating=ClampFloating({rect.x,rect.y},TierSize(model_,primary.geometry.dpi),primary.geometry.work);
        }
        // 桌面/工作区变了：玻璃快照作废，重抓（收起路径已不再每次重抓）。
        stripContext_.surface.InvalidateBackdrop();
        geometry_.Place(model_,monitor_.geometry,state_,reason);
        ApplyState(reason,state_);
        return;
    }
    if(model_.edge==Edge::None) return;
    monitor_=ReadMonitor(resolved);
    stripContext_.surface.InvalidateBackdrop();
    geometry_.Place(model_,monitor_.geometry,state_,reason);
    ApplyState(reason,state_);
}
void Application::BeginSystemDrag() {
    LogEvent("begin-drag");
    if(dragging_ || state_==State::Hidden) return;
    dragging_=true;
    panelDragViewValid_=false;
    dragLayoutDpi_=0;dragLayoutWidth_=0;dragLayoutHeight_=0;   // 首帧按当前 DPI 重摆原生输入框
    const auto startRect=GeometryController::Read(panel_);
    dragStartOrigin_={startRect.x,startRect.y};
    if(dragPressValid_) {
        dragGrabPx_=dragPressPx_;
        dragPressValid_=false;
    } else {
        POINT grabCursor{};
        GetCursorPos(&grabCursor);
        dragGrabPx_={grabCursor.x-startRect.x,grabCursor.y-startRect.y};
    }
    const unsigned windowDpi=GetDpiForWindow(panel_);
    dragStartDpi_=windowDpi?windowDpi:monitor_.geometry.dpi;
    if(log_) log_->Info("ui","drag-begin rect=("+std::to_string(startRect.x)+","+std::to_string(startRect.y)+
        ") grab=("+std::to_string(dragGrabPx_.x)+","+std::to_string(dragGrabPx_.y)+
        ") dpi="+std::to_string(dragStartDpi_));
    lastDrop_.start=startRect;
    lastDrop_.valid=false;
    if(graceTimer_) { KillTimer(panel_,kTrayGraceTimer);graceTimer_=false; }
    const auto previous=state_;
    state_=NextState(state_,Event::BeginDrag,model_.edge);
    if(state_==previous) return;
    // S2 is the system move loop: no geometry write, no visibility change, no model update.
}
void Application::CommitDrop() {
    LogEvent("commit-drop");
    if(!dragging_) return;
    const auto finalRect=GeometryController::Read(panel_);
    POINT cursor{};
    GetCursorPos(&cursor);
    // 目标显示器按**窗口**选（与窗口矩形交叠面积最大的那台，Windows 自己的规则），不按鼠标所在：
    // 双屏并排时把面板推向右屏左缘，指针会滑到左屏去，按鼠标选就会去量错误的工作区。
    monitor_=ReadMonitor(MonitorFromWindow(panel_,MONITOR_DEFAULTTONEAREST));
    // 落点 = 光标 - 抓取偏移。同一 DPI 时就是系统移动循环维持的物理偏移；跨 DPI 时系统
    // 缩放窗口后会换锚点，按 DIP 比例重算，否则面板在松手瞬间从光标下滑走。
    // 贴边判定也用这个锚定矩形：它仍是**窗口**（不是鼠标位置），但不会受系统移动循环
    // 在排队输入下滞后一两步的影响。
    const int grabX=dragStartDpi_==0?dragGrabPx_.x
        :static_cast<int>(static_cast<long long>(dragGrabPx_.x)*monitor_.geometry.dpi/dragStartDpi_);
    const int grabY=dragStartDpi_==0?dragGrabPx_.y
        :static_cast<int>(static_cast<long long>(dragGrabPx_.y)*monitor_.geometry.dpi/dragStartDpi_);
    Rect anchored=finalRect;
    anchored.x=cursor.x-grabX;
    anchored.y=cursor.y-grabY;
    const auto snap=SnapDecision(anchored,lastDrop_.start,monitor_.geometry,model_.edge);
    const auto previous=state_;
    dragging_=false;
    panelDragViewValid_=false;
    dragPressValid_=false;
    lastDrop_.finalRect=anchored;
    lastDrop_.cursor={cursor.x,cursor.y};
    lastDrop_.decidedEdge=static_cast<int>(snap.edge);
    lastDrop_.decidedAnchor=snap.anchor;
    lastDrop_.valid=true;
    ++drops_;
    if(log_) log_->Info("ui","drag-commit cursor=("+std::to_string(cursor.x)+","+std::to_string(cursor.y)+
        ") final=("+std::to_string(finalRect.x)+","+std::to_string(finalRect.y)+
        ") anchored=("+std::to_string(anchored.x)+","+std::to_string(anchored.y)+
        ") grab=("+std::to_string(grabX)+","+std::to_string(grabY)+
        ") dpi="+std::to_string(monitor_.geometry.dpi)+" startDpi="+std::to_string(dragStartDpi_));
    if(snap.edge!=Edge::None) {
        if(model_.edge==Edge::None) { floatingBeforeDock_=dragStartOrigin_;floatingRemembered_=true; }
        model_.edge=snap.edge;
        model_.anchor=snap.anchor;
        state_=NextState(state_,Event::DropDocked,snap.edge);
        geometry_.Place(model_,monitor_.geometry,state_,"drop-docked");
    } else {
        model_.edge=Edge::None;
        model_.floating={anchored.x,anchored.y};
        state_=NextState(state_,Event::DropFloating,Edge::None);
        geometry_.Place(model_,monitor_.geometry,state_,"drop-floating");
    }
    ApplyState("drop",previous);
    PersistSettings();
    if(wakeCount_==0) InvalidateRect(panel_,nullptr,FALSE);
}
bool Application::CanResizeHeight() const {
    // 只有浮动状态可拉高：贴边/收起/隐藏的几何属于停靠状态，尺寸由档位给出。
    return panel_&&state_==State::Floating&&model_.edge==Edge::None&&model_.tier==Tier::Full
        &&pending_==Pending::None&&!dragging_;
}
void Application::BeginPanelResize(POINT screenPoint) {
    if(!CanResizeHeight()) return;
    RECT window{};
    if(!GetWindowRect(panel_,&window)) return;
    resizing_=true;
    resizeStartHeight_=window.bottom-window.top;
    resizeStartCursorY_=screenPoint.y;
    const unsigned dpi=GetDpiForWindow(panel_);
    resizeDpi_=dpi?dpi:monitor_.geometry.dpi;
    SetCapture(panel_);
    LogEvent("resize-begin",screenPoint);
}
void Application::UpdatePanelResize(POINT screenPoint) {
    if(!resizing_) return;
    RECT window{};
    if(!GetWindowRect(panel_,&window)) return;
    const auto work=monitor_.geometry.work;
    const int minPx=ScaleDip(std::min(kPanelMinHeightDip,PanelHeightMaxDip(resizeDpi_,work)),resizeDpi_);
    const int maxPx=ScaleDip(PanelHeightMaxDip(resizeDpi_,work),resizeDpi_);
    const int wanted=std::clamp<int>(
        resizeStartHeight_+static_cast<int>(screenPoint.y)-resizeStartCursorY_,minPx,maxPx);
    // 拖动期间由窗口管理器跟手（与系统拖拽同一语义：交互中不经过 GeometryController）；
    // 松手后 CommitPanelResize 再按 DIP 统一 Place 一次。
    SetWindowPos(panel_,nullptr,window.left,window.top,window.right-window.left,wanted,
        SWP_NOACTIVATE|SWP_NOZORDER);
}
void Application::CommitPanelResize() {
    if(!resizing_) return;
    resizing_=false;
    if(GetCapture()==panel_) ReleaseCapture();
    RECT window{};
    if(!GetWindowRect(panel_,&window)) return;
    const unsigned dpi=GetDpiForWindow(panel_);
    const unsigned effective=dpi?dpi:monitor_.geometry.dpi;
    const int dipHeight=static_cast<int>(std::lround(
        static_cast<double>(window.bottom-window.top)*96.0/static_cast<double>(effective)));
    monitor_=ReadMonitor(MonitorFromWindow(panel_,MONITOR_DEFAULTTONEAREST));
    const auto previous=state_;
    model_.heightDip=ClampPanelHeightDip(dipHeight,effective,monitor_.geometry.work);
    stored_.heightDip=model_.heightDip;
    if(model_.edge==Edge::None) model_.floating={window.left,window.top};
    geometry_.Place(model_,monitor_.geometry,state_,"resize");
    ApplyState("resize",previous);
    PersistSettings(true);
    LogEvent("resize-commit");
    InvalidateAll();
}
void Application::SetPanelHeightDip(int dip) {
    const int clamped=ClampPanelHeightDip(dip,monitor_.geometry.dpi,monitor_.geometry.work);
    if(clamped==model_.heightDip) return;
    const auto previous=state_;
    model_.heightDip=clamped;
    stored_.heightDip=clamped;
    geometry_.Place(model_,monitor_.geometry,state_,"height");
    ApplyState("height",previous);
    PersistSettings(true);
    InvalidateAll();
}
void Application::BeginStripPress(POINT screenPoint) {
    if(!strip_) return;
    RECT window{};
    if(!GetWindowRect(strip_,&window)) return;
    stripPressed_=true;
    stripDragging_=false;
    stripPressScreen_={screenPoint.x,screenPoint.y};
    stripDragOffset_={screenPoint.x-window.left,screenPoint.y-window.top};
    SetCapture(strip_);
    HideHoverCard();
    // 拖动/点击期间不显示任何信息框（面板可能还没展开，提示计时器也要停）。
    if(panel_) {
        KillTimer(panel_,kTooltipTimer);
        tooltipVisible_=false;
    }
    LogEvent("strip-press",screenPoint);
}
void Application::UpdateStripDrag(POINT screenPoint) {
    if(!stripPressed_) return;
    if(!stripDragging_) {
        const unsigned dpi=GetDpiForWindow(strip_);
        // 系统拖拽阈值与 6 DIP 取大：按下到抬起之间的几像素抖动不会被误判成拖动。
        const int threshold=std::max(GetSystemMetrics(SM_CXDRAG),ScaleDip(6,dpi?dpi:96));
        if(std::abs(screenPoint.x-stripPressScreen_.x)<=threshold
            &&std::abs(screenPoint.y-stripPressScreen_.y)<=threshold) return;
        stripDragging_=true;
        HideHoverCard();
        LogEvent("strip-drag-begin",screenPoint);
    }
    // 每 8ms 一次的鼠标移动里反复 ReadMonitor（GetMonitorInfo + GetDpiForMonitor）是纯浪费：
    // 只有跨到另一台显示器时才需要重读。真机实测这是拖动卡顿的主要成本之一。
    const auto targetHandle=MonitorFromPoint(screenPoint,MONITOR_DEFAULTTONEAREST);
    if(targetHandle!=monitor_.handle) monitor_=ReadMonitor(targetHandle);
    const auto& work=monitor_.geometry.work;
    // 预览边：跟着挂件中心选最近的边。**朝向在松手前就跟着变**（真机要求），
    // 这样向上拖到上边框附近时，内容与流动立刻按"上边"排；落点仍由 CommitStripDrop 决定。
    RECT window{};
    if(!GetWindowRect(strip_,&window)) return;
    const int centreX=window.left+(window.right-window.left)/2;
    const int centreY=window.top+(window.bottom-window.top)/2;
    const auto snap=StripSnapDecision({centreX,centreY},TierSize(model_,monitor_.geometry.dpi),
        monitor_.geometry);
    previewEdge_=static_cast<int>(snap.edge);
    // 朝向换了就把窗口转置（竖条 ↔ 横条），围绕当前中心，保证内容不被裁掉。
    const bool wantVertical=snap.edge==Edge::Left||snap.edge==Edge::Right;
    int width=window.right-window.left,height=window.bottom-window.top;
    const bool isVertical=height>=width;
    if(wantVertical!=isVertical) {
        const int cx=window.left+width/2,cy=window.top+height/2;
        std::swap(width,height);
        const int nx=cx-width/2,ny=cy-height/2;
        SetWindowPos(strip_,nullptr,nx,ny,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
        stripDragOffset_={screenPoint.x-nx,screenPoint.y-ny};
        if(!GetWindowRect(strip_,&window)) return;
    }
    // 拖动限制在目标显示器的工作区内（余额面板同款）：不会把挂件拖到屏幕外。
    const int maxX=std::max(work.x,work.x+work.width-width);
    const int maxY=std::max(work.y,work.y+work.height-height);
    const int x=std::clamp<int>(static_cast<int>(screenPoint.x)-stripDragOffset_.x,work.x,maxX);
    const int y=std::clamp<int>(static_cast<int>(screenPoint.y)-stripDragOffset_.y,work.y,maxY);
    SetWindowPos(strip_,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
    // 拖动中的胶囊形态：离最近边越远越圆（0=完整椭圆），贴边时收直。松手后由
    // kCapsuleDockMs 的过渡动画收回贴边直角（落点永远吸附最近边）。
    int nearest=std::min(x-work.x,work.right()-(x+width));
    nearest=std::min(nearest,std::min(y-work.y,work.bottom()-(y+height)));
    const int band=ScaleDip(96,monitor_.geometry.dpi);
    capsuleDockOverride_=1.0f-std::clamp(static_cast<float>(nearest)
        /static_cast<float>(std::max(1,band)),0.0f,1.0f);
    // 重绘限 60Hz。**关键**：拖动帧复用缓存视图，不再每帧重建整份 PanelViewModel
    // （真机实测：逐帧重建让胶囊拖动吃掉 43% CPU、肉眼可见卡顿）。
    // 重绘节奏：拖动时窗口位置由 SetWindowPos 逐条跟手，重绘只负责"形状/朝向"。
    // 60Hz 全帧重绘在真机上让拖动吃掉 30% CPU 且明显跟手变差；改成 ~30Hz + 事件驱动
    // （朝向切换、圆角变化超过 3.5% 立即重绘），视觉上仍是连续形变。
    const auto now=GetTickCount64();
    const bool edgeChanged=previewEdge_!=lastDragEdge_;
    const bool shapeChanged=std::abs(capsuleDockOverride_-lastDragDocked_)>0.035f;
    if(edgeChanged||shapeChanged||now-stripDragRenderTick_>=33) {
        stripDragRenderTick_=now;
        lastDragEdge_=previewEdge_;
        lastDragDocked_=capsuleDockOverride_;
        RenderStripDragFrame();
    }
}
void Application::CommitStripDrop() {
    if(!stripDragging_) { stripPressed_=false;return; }
    // 先清状态再 ReleaseCapture：释放会同步发 WM_CAPTURECHANGED，避免递归提交。
    stripDragging_=false;
    stripPressed_=false;
    if(GetCapture()==strip_) ReleaseCapture();
    RECT window{};
    if(!GetWindowRect(strip_,&window)) return;
    const int centreX=window.left+(window.right-window.left)/2;
    const int centreY=window.top+(window.bottom-window.top)/2;
    monitor_=ReadMonitor(MonitorFromWindow(strip_,MONITOR_DEFAULTTONEAREST));
    // 松手**永远回贴边**（真机 2026-10-20 纠正）：拖到中间只是拖动中的椭圆形态，
    // 不是"浮起并留在中间"。落点按挂件中心选最近的边，展开面板的锚点跟着夹进工作区。
    const auto snap=StripSnapDecision({centreX,centreY},TierSize(model_,monitor_.geometry.dpi),
        monitor_.geometry);
    model_.edge=snap.edge;
    model_.anchor=snap.anchor;
    capsuleDockOverride_=-1.0f;
    // 胶囊落点动画：拖动中的完整椭圆 → 贴边直角（240ms）。松手必贴边，目标恒为贴边。
    previewEdge_=0;      // 落点确定：预览边交还给 model_.edge
    if(stored_.widgetStyle=="capsule"&&!capsuleDocked_) {
        capsuleDocked_=true;
        if(AnimationsEnabled()) { capsuleDockStart_=GetTickCount64();KeepMotionTimer(); }
    }
    const auto previous=state_;
    state_=State::Collapsed;
    geometry_.Place(model_,monitor_.geometry,state_,"strip-drop");
    // 位置变了：玻璃背景必须重抓。落点后贴条可见，若直接重抓会走"隐藏→抓屏→恢复"
    // 的肉眼闪烁（与收起路径同一类问题）：先藏 → 抓屏+画帧 → 再显示。
    ShowWindow(strip_,SW_HIDE);
    stripContext_.surface.InvalidateBackdrop();
    stripContext_.surface.EnsureBackdrop(strip_);
    RenderStrip();
    ShowWindow(strip_,SW_SHOWNOACTIVATE);
    ApplyState("strip-drop",previous);
    PersistSettings();
    InvalidateAll();
    LogEvent("strip-drag-commit");
}
void Application::ArmStripCard() {
    if(!strip_||state_!=State::Collapsed
        ||stripPressed_||stripDragging_||dragging_) return;
    if(cardVisible_) return;   // 已显示：不再排计时器，避免重复入场
    SetTimer(strip_,kStripCardTimer,kStripCardDelayMs,nullptr);
}
void Application::PositionHoverCard() {
    if(!hoverCard_) return;
    const auto dpi=monitor_.geometry.dpi;
    const auto view=CurrentView();
    // 宽度**按内容自适应**：以前固定 252 DIP，长模型名一律截断（真机反馈"模型名称要显示完全"）。
    // 下界 252（短名字保持原来的观感）、上界 520：常见的 "custom:供应商/很长的模型 id"
    // 大约 400–500 DIP，340 的上限在真机上仍会截断。再宽由 HoverCardRect 夹进工作区。
    const auto modelLine="\xE5\xBD\x93\xE5\x89\x8D\xEF\xBC\x9A"+view.hoverCard.model;
    const int wantedDip=static_cast<int>(EstimateTextWidth(modelLine,12.0f))+28;
    const int width=ScaleDip(std::clamp(wantedDip,252,520),dpi);
    // 高度跟着内容走：62（头+模型行）+ 每行 20 + note 20 + 底部 10。删掉 hint 后不再留空。
    const int contentDip=62+20*static_cast<int>(view.hoverCard.rows.size())
        +(view.hoverCard.note.empty()?0:20)+10;
    const int height=ScaleDip(contentDip,dpi);
    const auto stripRect=GeometryController::Read(strip_);
    const auto card=HoverCardRect(stripRect,model_.edge,monitor_.geometry.work,width,height,
        ScaleDip(8,dpi));
    SetWindowPos(hoverCard_,HWND_TOPMOST,card.x,card.y,card.width,card.height,SWP_NOACTIVATE);
}
void Application::ShowHoverCard() {
    KillTimer(strip_,kStripCardTimer);
    if(!hoverCard_||state_!=State::Collapsed
        ||stripPressed_||stripDragging_||dragging_) return;
    // 已经在显示：只按当前内容重排/重绘，**绝不重放入场动画**。鼠标在贴条上每动一下都会
    // 重新 ArmStripCard，如果这里再 StartCardMotion，alpha 会被打回 0 → 卡片一直闪。
    if(cardVisible_) { PositionHoverCard();RenderHoverCard();return; }
    PositionHoverCard();
    ShowWindow(hoverCard_,SW_SHOWNOACTIVATE);
    cardVisible_=true;
    StartCardMotion();
    RenderHoverCard();
    LogEvent("card-show");
}
void Application::HideHoverCard() {
    if(strip_) KillTimer(strip_,kStripCardTimer);
    if(!cardVisible_||!hoverCard_) return;
    // 逻辑上立即"不再显示"（模型与门禁看到的就是隐藏），窗口再多留 80ms 做淡出，
    // 淡完由 MotionTick 真正 SW_HIDE。卡片是独立小窗，不参与任何几何不变量。
    cardVisible_=false;
    cardMotionStart_=0;
    if(AnimationsEnabled()) { cardExitStart_=GetTickCount64();KeepMotionTimer(); }
    else { cardExitStart_=0;ShowWindow(hoverCard_,SW_HIDE); }
    LogEvent("card-hide");
}
void Application::RenderHoverCard() {
    if(!cardVisible_||!hoverCard_) return;
    // 入场动画期间**不重新定位**：PositionHoverCard 内部要 CurrentView()（重建整份视图）
    // 再 SetWindowPos，而 120ms 的淡入里内容根本不会变 —— 每帧重算只会让动画掉帧，
    // 表现为"弹出时文字一顿一顿的"。动画结束后再定尺（内容变化由调用方显式触发）。
    if(!cardMotionStart_) PositionHoverCard();
    try {
        auto view=CurrentView();
        hoverCardContext_.surface.PaintHoverCard(hoverCard_,view,palette_);
    } catch(const std::exception& error) {
        if(log_) log_->Error("paint",std::string("hover-card: ")+error.what());
    }
}
LRESULT Application::HitTest(POINT screen) {
    POINT client=screen;
    if(!ScreenToClient(panel_,&client)) return HTCLIENT;
    RECT bounds{};
    GetClientRect(panel_,&bounds);
    if(client.x<0||client.y<0||client.x>=bounds.right||client.y>=bounds.bottom) return HTNOWHERE;
    const auto dpi=monitor_.geometry.dpi;
    // 系统移动循环里每动一下都会发一次 WM_NCHITTEST：每次重建整份视图 + 布局是
    // 真机上"面板拖动很卡"的根因（实测 36.7% CPU）。拖动/拉伸期间窗口已经抓在手里，
    // 直接按标题栏处理，不做任何布局。
    if(dragging_||resizing_) return HTCAPTION;
    // 判 Hidden 用的是**几何状态**，但真正决定用户能否拖动的是窗口可见性。两者曾经脱节过
    // （最小化后按热键隐藏：窗口被系统恢复出来而 state_ 仍是 Hidden），后果是整条标题栏
    // 恒返回 HTCLIENT、面板再也拖不动。这里按"可见就按可见处理"，状态机出岔子也不至于
    // 把拖动功能整个关掉。
    if(state_!=State::Hidden||IsWindowVisible(panel_)) {
        // Buttons inside the title bar must receive the click, the rest of the bar drags the window.
        const auto layout=ComputeLayout(CurrentView(),static_cast<float>(bounds.right),
            static_cast<float>(bounds.bottom),static_cast<float>(dpi)/96.0f);
        if(ControlAt(layout,static_cast<float>(client.x),static_cast<float>(client.y))!=ControlId::None)
            return HTCLIENT;
        const int titleBar=ScaleDip(36,dpi);
        if(client.y<titleBar) return HTCAPTION;
        if(model_.edge!=Edge::None&&state_==State::Expanded) {
            const int innerBand=ScaleDip(12,dpi);
            const bool hit=model_.edge==Edge::Left?client.x<innerBand
                :model_.edge==Edge::Right?client.x>=bounds.right-innerBand
                :model_.edge==Edge::Top?client.y<innerBand
                :client.y>=bounds.bottom-innerBand;
            if(hit) return HTCAPTION;
        }
    }
    return HTCLIENT;
}
void Application::Wake() { ++wakeCount_;Show(); }
Settings Application::SnapshotSettings() const {
    Settings settings=stored_;
    settings.edge=model_.edge;
    settings.tier=model_.tier;
    settings.floating=model_.floating;
    settings.anchor=model_.anchor;
    settings.heightDip=model_.heightDip;
    settings.stripFloat=false;          // v1.9：松手必贴边，浮起胶囊状态不再存在
    settings.monitorId=Narrow(monitor_.id);
    return settings;
}
bool Application::RegisterHotkeys() {
    // One chord (default Control+Alt+H, editable on the settings page). Another process may already own
    // it, so the failure is recorded instead of assumed away.
    struct Chord { UINT modifiers=MOD_CONTROL|MOD_ALT;UINT key='H'; };
    const auto parse=[&](const std::string& text) {
        Chord chord;
        chord.modifiers=0;
        chord.key=0;
        std::string remainder=text;
        const auto take=[&](const char* token) {
            const std::string needle=std::string(token)+"+";
            if(remainder.rfind(needle,0)!=0) return false;
            remainder=remainder.substr(needle.size());
            return true;
        };
        if(take("Control")) chord.modifiers|=MOD_CONTROL;
        if(take("Alt")) chord.modifiers|=MOD_ALT;
        if(take("Shift")) chord.modifiers|=MOD_SHIFT;
        if(remainder.size()==1) chord.key=static_cast<UINT>(remainder[0]);
        else if(remainder.size()>=2&&remainder[0]=='F'&&remainder[1]>='1'&&remainder[1]<='9')
            chord.key=VK_F1+static_cast<UINT>(remainder[1]-'1');
        if(chord.modifiers==0||chord.key==0) { chord.modifiers=MOD_CONTROL|MOD_ALT;chord.key='H'; }
        return chord;
    };
    const auto chord=parse(stored_.hotkey);
    for(int index=0;index<1;++index) UnregisterHotKey(control_,static_cast<int>(Hotkey::TogglePanel));
    hotkeysRegistered_=true;
    for(int index=0;index<1;++index) {
        const auto ok=RegisterHotKey(control_,static_cast<int>(Hotkey::TogglePanel),chord.modifiers,chord.key)!=FALSE;
        hotkeyErrors_[index]=ok?0:static_cast<int>(GetLastError());
        if(!ok) hotkeysRegistered_=false;
    }
    if(log_) {
        if(hotkeysRegistered_) log_->Info("hotkeys","registered "+stored_.hotkey);
        else log_->Info("hotkeys","registration failed, errors "+std::to_string(hotkeyErrors_[0])+"/"
            +std::to_string(hotkeyErrors_[1])+"/"+std::to_string(hotkeyErrors_[2]));
    }
    return hotkeysRegistered_;
}
void Application::InvokeHotkey(int id) {
    switch(static_cast<Hotkey>(id)) {
    case Hotkey::TogglePanel: InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));return;
    }
}
bool Application::AutoStartEnabled() const {
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS) return false;
    wchar_t value[1024]{};
    DWORD size=sizeof(value);
    DWORD type=0;
    const auto status=RegQueryValueExW(key,L"HermesModelPanelNative",nullptr,&type,reinterpret_cast<BYTE*>(value),&size);
    RegCloseKey(key);
    if(status!=ERROR_SUCCESS||type!=REG_SZ) return false;
    // 必须**指向当前这个 exe** 才算开启：换目录/装新版后，注册表里残留的旧路径不该继续显示成
    // "已开启"（否则勾是亮的、开机启动的却是旧版本）。去掉引号、忽略大小写比较。
    std::wstring stored=value;
    if(stored.size()>=2&&stored.front()==L'"'&&stored.back()==L'"') stored=stored.substr(1,stored.size()-2);
    wchar_t self[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,self,MAX_PATH)) return false;
    return _wcsicmp(stored.c_str(),self)==0;
}
void Application::SetAutoStart(bool enabled) {
    HKEY key=nullptr;
    if(RegCreateKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return;
    if(enabled) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr,path,MAX_PATH);
        const std::wstring command=L"\""+std::wstring(path)+L"\"";
        RegSetValueExW(key,L"HermesModelPanelNative",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key,L"HermesModelPanelNative");
    }
    RegCloseKey(key);
    PersistSettings(true);
    if(log_) log_->Info("settings",std::string("autostart ")+(enabled?"on":"off"));
}
PanelViewModel Application::CurrentView() {
    PanelViewInput input;
    input.tier=model_.tier;
    input.lightTheme=palette_.light;
    input.activePage=activePage_;
    input.hoverControl=hoverControl_;
    input.hoverIndex=tooltipHoverIndex_;
    input.pinned=stored_.topmost;
    input.motion=MotionView();
    {
        // 统一进度：ease-out cubic；AnimationsEnabled 关掉时所有进度恒为 1（即"无动画"）。
        const auto now=GetTickCount64();
        const auto ease=[](float t) { return 1.0f-(1.0f-t)*(1.0f-t)*(1.0f-t); };
        const auto progress=[&](unsigned long long start,unsigned duration) -> float {
            if(start==0||!AnimationsEnabled()) return 1.0f;
            const auto elapsed=now-start;
            if(elapsed>=duration) return 1.0f;
            return ease(static_cast<float>(elapsed)/static_cast<float>(duration));
        };
        // 只剩切页的内容淡入（展开入场已移除）：不透明度吃进度，位移恒为 0（走视图默认值）。
        input.motion.panelAlpha=progress(panelMotionStart_,kPanelMotionMs);
        // 退场淡出优先：卡片逻辑上已隐藏（cardVisible_=false），窗口还会多留 80ms 把 alpha 画到 0。
        if(cardExitStart_) input.motion.cardAlpha=1.0f-progress(cardExitStart_,kCardExitMs);
        else input.motion.cardAlpha=progress(cardMotionStart_,kCardMotionMs);
        input.motion.cardOffsetDip=0.0f;   // 位移入场已移除（见 DrawHoverCard：亚像素平移会让文字发虚）
        input.motion.stripAlpha=progress(stripMotionStart_,kStripEnterMs);
        // 胶囊贴边度：目标态向 0/1 过渡（1=贴边直角、0=浮起椭圆）。动画中逐帧插值。
        {
            // 形状过渡用 smoothstep（t*t*(3-2t)）而不是 ease-out cubic：椭圆与直边的起止
            // 两端都软、中段更快，视觉上更接近真实弹性体（ease-out 前段太急）。
            const float target=capsuleDocked_?1.0f:0.0f;
            float value=target;
            if(capsuleDockStart_!=0) {
                const float elapsed=std::clamp(static_cast<float>(now-capsuleDockStart_)
                    /static_cast<float>(kCapsuleDockMs),0.0f,1.0f);
                const float eased=elapsed*elapsed*(3.0f-2.0f*elapsed);
                value=capsuleDocked_?eased:1.0f-eased;
            }
            input.motion.capsuleDocked=capsuleDockOverride_>=0.0f?capsuleDockOverride_:value;
        }
        input.motion.tooltipAlpha=progress(tooltipMotionStart_,kTooltipMotionMs);
        input.motion.hoverPrevious=hoverPrevious_;
        input.motion.hoverAmount=progress(hoverMotionStart_,kHoverMotionMs);
        // 主题交叉淡入：新旧调色板按进度混合，绘制层只认一份 palette。
        const auto theme=progress(themeMotionStart_,kThemeMotionMs);
        if(theme<0.999f&&themeStartValid_) palette_=BlendPalette(previousPalette_,targetPalette_,theme);
    }
    auto view=BuildPanelView(input);
    // The lamp register, the chips, the hero and the sync stamp are panel chrome: every page shows them.
    StatusViewInput status;
    status.state=&hermesState_;
    status.switchingModel=switchingModel_;
    status.hotLoadedMtime=stored_.hotLoadedMtime;
    status.pendingGatewayRestart=pendingGatewayRestart_;
    status.statusCenterOpen=statusCenterOpen_;
    AttachStatusView(view,status);
    if(!demoStatuses_.empty()) {
        view.statuses=demoStatuses_;
        view.statusCenterOpen=statusCenterOpen_;
    }
    // F4：hero 的延迟徽章（当前模型最近一次 + 近 5 次可用率）。
    if(latency_&&!hermesState_.model.model.empty()) {
        const auto key=hermesState_.model.provider+"|"+hermesState_.model.model;
        int ms=0;
        bool ok=false;
        if(latency_->Last(key,&ms,&ok)) {
            view.hasLatency=true;
            view.latencyMs=ms;
            view.latencyOk=ok;
            view.latencyAvailability=latency_->Availability(key);
        }
    }
    view.busy=workerBusy_.load();
    view.refreshBusy=workerBusy_.load()&&refreshOwnsBusy_;
    // 同步章图标已经搬到独立子窗口（SpinGlyph，单帧 <0.2ms），不再由主面板重绘驱动：
    // 主面板整窗重绘在本机实测 10–170ms，把它当动画时钟只会得到"不转、只在抖"。
    // 这里只把窗口视图里那个位置留空（Icon 由子窗口自己画）。
    view.syncSpinAngle=spinGlyph_.Angle();
    if(view.busy) {
        // 忙碌条相位改为**按时间计算**（0..1 单向循环）：以前靠 50ms 定时器每次 +12° 累加，
        // 定时器被整窗重绘挤晚时相位就停住、攒够了一次跳过去——真机看到的"卡一下再动"。
        const auto now=GetTickCount64();
        if(!busySweepRunning_) { busySweepRunning_=true;busySweepStart_=now; }
        constexpr double kBusySweepPeriodMs=1600.0;
        view.busySweep=static_cast<float>(std::fmod(
            static_cast<double>(now-busySweepStart_)/kBusySweepPeriodMs,1.0));
    } else {
        busySweepRunning_=false;
        view.busySweep=0.0f;
    }
    view.syncCheck=syncCheckShown_;
    view.pointerX=pointerX_;view.pointerY=pointerY_;view.pointerInside=pointerInside_;
    view.focusControl=focusControl_;
    view.focusIndex=focusControlIndex_;
    if(!switchingModel_.empty())
        view.busyLabel="\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xAD\xE2\x80\xA6";        // 切换中…
    else if(workerBusy_.load())
        view.busyLabel="\xE8\xAF\xBB\xE5\x8F\x96 WSL \xE9\x85\x8D\xE7\xBD\xAE\xE2\x80\xA6"; // 读取 WSL 配置…
    else
        view.busyLabel.clear();
    // Toast 与模态是任何页面都能有的浮层（DESIGN §4.8 #33/#34）。
    if(!toastText_.empty()) {
        view.toast.text=toastText_;
        view.toast.kind=toastKind_;
    }
    if(pending_==Pending::ProviderMenu) {
        // 旧 openProviderMenu(name)：一张"信息 + 动作"的卡。旧版是底部 action sheet（动作各占一个
        // 按钮），原生版用已有的模态承载：正文给信息，确定 = 第一项主操作（v1.18 起为「扫描并追加」），
        // 其余动作从「添加」页/托盘可达。差异记在 PROGRESS。
        const auto found=std::find_if(hermesState_.providers.begin(),hermesState_.providers.end(),
            [&](const ProviderView& provider) { return provider.name==pendingProvider_; });
        if(found!=hermesState_.providers.end()) {
            view.modal.title="\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xEF\xBC\x9A"+found->name;
            // 分点：base_url / 接口模式 / 密钥 / 模型数各占一行（真机反馈「供应商界面很丑，能不能分点换行」）。
            // 固定文案用 Unicode 转义写：源码保持纯 ASCII，避免工具链改写成乱码。
            view.modal.facts.clear();
            view.modal.facts.push_back(found->baseUrl.empty()
                ?Narrow(L"\uFF08\u672A\u586B Base URL\uFF09")
                :found->baseUrl);
            view.modal.facts.push_back(Narrow(L"\u63A5\u53E3\uFF1A")
                +(found->apiMode.empty()?"chat":found->apiMode));
            view.modal.facts.push_back(Narrow(L"\u5BC6\u94A5\uFF1A")
                +(found->keyEnv.empty()?Narrow(L"\u65E0"):found->keyEnv+(found->keySet
                    ?Narrow(L"\uFF08\u5DF2\u8BBE\u7F6E\uFF09")
                    :Narrow(L"\uFF08\u7F3A\u5931\uFF09"))));
            view.modal.facts.push_back(Narrow(L"\u6A21\u578B\uFF1A")
                +std::to_string(found->models.size())+" "+Narrow(L"\u4E2A")
                +(found->modelsDiscovered?Narrow(L"\uFF08\u5DF2\u63A2\u6D4B\u8FC7\uFF09"):std::string{}));
            view.modal.confirmLabel="\xE6\x89\xAB\xE6\x8F\x8F\xE5\xB9\xB6"
                "\xE5\x90\x8C\xE6\xAD\xA5";
        }
    }
    if(pending_==Pending::SyncAll) {
        // 一键刷新同步：只读探测每个供应商，再把配置里的模型列表同步成最新（写前自动备份）。
        view.modal.title=Narrow(L"刷新并同步全部供应商的模型列表？");
        view.modal.body=Narrow(L"逐个供应商只读探测，然后同步写回；新增的写入、已下线的删除。");
        view.modal.facts.clear();
        view.modal.facts.push_back(std::to_string(hermesState_.providers.size())
            +Narrow(L" 个供应商；写入前自动备份 config.yaml"));
        view.modal.confirmLabel=Narrow(L"同步");
        view.modal.cancelLabel=Narrow(L"取消");
        view.modal.danger=false;
    }
    if(pending_==Pending::DeleteProvider) {
        // 删除供应商的危险确认（旧 confirmBox('删除供应商？') + 说明写盘影响）
        view.modal.title="\xE5\x88\xA0\xE9\x99\xA4\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xEF\xBC\x9F";
        view.modal.body=addDanger_.TextUtf8()
            +"\xEF\xBC\x88\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE8\x87\xAA\xE5\x8A\xA8\xE5\xA4\x87"
            "\xE4\xBB\xBD config.yaml \xE4\xB8\x8E .env\xEF\xBC\x89";
        view.modal.confirmLabel="\xE5\x88\xA0\xE9\x99\xA4";
        view.modal.danger=true;
    }
    if(pending_==Pending::SwitchModel) {
        // 切模型前确认（真机反馈：微信/QQ 机器人要等网关重启才用上新模型，误点代价高）。
        view.modal.title=Narrow(L"\u5207\u6362\u6A21\u578B\uFF1F");
        view.modal.facts.clear();
        view.modal.facts.push_back(Narrow(L"\u76EE\u6807\uFF1A")+pendingSwitchModel_);
        view.modal.facts.push_back(Narrow(L"\u5FAE\u4FE1/QQ \u7B49\u673A\u5668\u4EBA\u9700\u91CD\u542F"
            L"\u7F51\u5173\u540E\u624D\u7528\u4E0A\u65B0\u6A21\u578B"));
        view.modal.confirmLabel=Narrow(L"\u5207\u6362");
    }
    if(pending_==Pending::PreviewWrite) {
        // 写入预览（旧版 confirmBox(\'写入预览：…\', renderDiff(diff)) 的等价物）：分点行即 diff。
        view.modal.title=Narrow(L"\u5199\u5165\u9884\u89c8");
        view.modal.facts=previewFacts_;
        view.modal.confirmLabel=Narrow(L"\u786e\u8ba4\u5199\u5165");
        view.modal.cancelLabel=Narrow(L"\u53d6\u6d88");
    }
    if(pending_==Pending::DiagnoseInfo||pending_==Pending::UsageInfo) {
        // F2/F3：只读助手的模态。确定 = 重新读取（同一张卡原地刷新），取消 = 关闭。
        view.modal.title=readTitle_;
        view.modal.facts=readFacts_;
        view.modal.confirmLabel=Narrow(L"刷新");
        view.modal.cancelLabel=Narrow(L"关闭");
    }
    if(pending_==Pending::ProfileMenu) {
        // F5 管理模态：档案列表（每行可点「应用 / 删除」），确定 = 保存当前。
        view.modal.title=Narrow(L"配置档案");
        view.modal.confirmLabel=Narrow(L"保存当前");
        view.modal.cancelLabel=Narrow(L"关闭");
        if(!profileError_.empty())
            view.modal.facts.push_back(Narrow(L"上一步失败：")+profileError_);
        if(profiles_) {
            int shown=0;
            for(const auto& profile:profiles_->All()) {
                if(shown++>=5) break;
                auto detail=profile.mainModel.empty()?Narrow(L"（未设置主模型）"):profile.mainModel;
                detail+=Narrow(L" · ")+std::to_string(profile.fallbacks.size())+Narrow(L" 条回退");
                view.modal.profiles.push_back({profile.name,detail});
            }
        }
        if(view.modal.profiles.empty()&&profileError_.empty())
            view.modal.facts.push_back(Narrow(L"还没有档案：点「保存当前」把当前模型与回退链存成一份"));
    }
    if(pending_==Pending::ProfileApplyConfirm) {
        view.modal.title=Narrow(L"应用配置档案？");
        view.modal.facts.clear();
        view.modal.facts.push_back(Narrow(L"档案：")+pendingProfileName_);
        if(profiles_) {
            if(const auto* profile=profiles_->Find(pendingProfileName_)) {
                view.modal.facts.push_back(Narrow(L"主模型：")+profile->mainProvider+" / "+profile->mainModel);
                view.modal.facts.push_back(Narrow(L"回退链：")
                    +std::to_string(profile->fallbacks.size())+Narrow(L" 条"));
            }
        }
        view.modal.facts.push_back(Narrow(L"写入前会自动备份 config.yaml"));
        view.modal.confirmLabel=Narrow(L"应用");
    }
    if(pending_==Pending::ProfileDeleteConfirm) {
        view.modal.title=Narrow(L"删除配置档案？");
        view.modal.body=pendingProfileName_;
        view.modal.confirmLabel=Narrow(L"删除");
        view.modal.danger=true;
    }
    if(pending_==Pending::RestoreBackup) {
        // 回滚确认（旧 confirmBox('回滚这份备份？')）：写盘前会把当前内容再备份一份。
        view.modal.title="\xE5\x9B\x9E\xE6\xBB\x9A\xE8\xBF\x99\xE4\xBB\xBD\xE5\xA4\x87\xE4\xBB\xBD\xEF\xBC\x9F";
        view.modal.body=pendingBackupFile_
            +"\n\xE5\x9B\x9E\xE6\xBB\x9A\xE5\x89\x8D\xE4\xBC\x9A\xE6\x8A\x8A\xE5\xBD\x93\xE5\x89\x8D"
            "\xE5\x86\x85\xE5\xAE\xB9\xE5\x86\x8D\xE5\xA4\x87\xE4\xBB\xBD\xE4\xB8\x80\xE4\xBB\xBD\xEF\xBC\x8C"
            "\xE6\x89\x80\xE4\xBB\xA5\xE5\x8F\xAF\xE4\xBB\xA5\xE5\x86\x8D\xE5\x9B\x9E\xE6\xBB\x9A\xE5\x9B\x9E"
            "\xE6\x9D\xA5\xE3\x80\x82";
        view.modal.confirmLabel="\xE5\x9B\x9E\xE6\xBB\x9A";
        view.modal.danger=true;
    }
    if(pending_==Pending::RemoveFallback) {
        const auto index=pendingIndex_;
        if(index>=0&&index<static_cast<int>(hermesState_.fallbacks.size())) {
            const auto& entry=hermesState_.fallbacks[static_cast<std::size_t>(index)];
            view.modal.title="\xE4\xBB\x8E\xE5\x9B\x9E\xE9\x80\x80\xE9\x93\xBE\xE7\xA7\xBB\xE9\x99\xA4\xEF\xBC\x9F";
            view.modal.body=ProviderLabelFor(entry.provider)+" / "+entry.model;
            view.modal.confirmLabel="\xE7\xA7\xBB\xE9\x99\xA4";
            view.modal.danger=true;
        }
    }
    if(activePage_==1) {
        FallbackPageInput page;
        page.state=&hermesState_;
        page.formOpen=fbFormOpen_;
        page.selectOpen=fbSelectOpen_;
        page.source=fbSource_;
        page.sourceScroll=selectScroll_;
        page.model=fbModel_.TextUtf8();

        page.error=fbError_;
        page.busy=workerBusy_.load();
        page.modelSelectOpen=fbModelSelectOpen_;
        page.modelOptions=FallbackModelOptions();
        AttachFallbackPage(view,page);
        // 逐条探测进度：按钮文案显示 检测中 n/N，当前行显示 …（每条最多 30s，没进度会以为卡死）。
        const bool probeRunning=probeRunning_.load();
        view.fallback.probeRunning=probeRunning;
        if(probeRunning) {
            const int total=probeTotal_.load();
            const int done=probeDone_.load();
            view.fallback.probeLabel=total>0
                ?std::string("\xE6\xA3\x80\xE6\xB5\x8B\xE4\xB8\xAD ")+std::to_string(std::min(done+1,total))
                    +"/"+std::to_string(total)
                :std::string("\xE6\xA3\x80\xE6\xB5\x8B\xE4\xB8\xAD\xE2\x80\xA6");
        }
        const int probeActive=probeActive_.load();
        for(std::size_t index=0;index<view.fallback.rows.size();++index) {
            if(probeRunning&&static_cast<int>(index)==probeActive)
                view.fallback.rows[index].probePending=true;
        }
        for(std::size_t index=0;index<view.fallback.rows.size();++index) {
            auto& row=view.fallback.rows[index];
            if(index<fbProbes_.size()) {
                const auto& probe=fbProbes_[index];
                row.hasProbe=true;
                row.probeOk=probe.ok;
                row.probeHasModel=probe.hasModel;
                row.probeCount=probe.count;
                row.probeDetail=probe.ok
                    ?std::to_string(probe.count)+" \xE4\xB8\xAA\xE6\xA8\xA1\xE5\x9E\x8B"
                        +(probe.hasModel?"\xEF\xBC\x8C\xE5\x90\xAB\xE8\xAF\xA5\xE6\xA8\xA1\xE5\x9E\x8B":
                            "\xEF\xBC\x8C\xE4\xB8\x8D\xE5\x90\xAB\xE8\xAF\xA5\xE6\xA8\xA1\xE5\x9E\x8B")
                    :(probe.error.empty()?"\xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE":probe.error);
            }
            // 探测结果的悬停说明：一句话、不换行（绿=含该模型 / 琥珀=不含 / 红=连不上）。
            std::string tip;
            if(!row.hasProbe) tip=Narrow(L"逐条探测：检查这条回退是否可用");
            else if(!row.probeOk) tip=Narrow(L"连不上：")+TruncateUtf8(row.probeDetail,60);
            else if(row.probeHasModel) tip=Narrow(L"已连上，含该模型（")
                +std::to_string(row.probeCount)+Narrow(L" 个模型）");
            else tip=Narrow(L"已连上，但不含该模型（这条回退不可用）");
            view.tooltips.push_back({static_cast<int>(ControlId::FbProbeDot),
                static_cast<int>(index),tip});
        }
    }
    if(activePage_==2) {
        AddPageInput page;
        page.state=&hermesState_;
        page.sub=addSub_;
        page.mode=addMode_;
        page.name=addName_.TextUtf8();
        // v1.18：Key 变量名不再由用户填，这里只把助手会推导出的名字作为提示显示。
        page.keyEnv=DeriveKeyEnv(addName_.TextUtf8());
        page.url=addUrl_.TextUtf8();
        page.key=addKey_.TextUtf8();
        page.dangerName=addDanger_.TextUtf8();
        // 删除页下拉的选中下标（按名字回查）：dump/门禁靠它断言"选中的是哪一个"。
        page.provider=0;
        for(std::size_t index=0;index<hermesState_.providers.size();++index)
            if(hermesState_.providers[index].name==page.dangerName)
                page.provider=static_cast<int>(index);
        page.keyRevealed=addKeyRevealed_;
        page.setMainAfter=addSetMainAfter_;
        page.purgeEnv=addPurgeEnv_;
        page.dropFallbacks=addDropFallbacks_;
        page.providerScroll=selectScroll_;
        page.selectOpen=addSelectOpen_;
        page.selectField=addSelectField_;
        page.error=addError_;
        page.info=addInfo_;
        page.chips=addChips_;
        page.chipSelected=addChipSelected_;
        page.modelScroll=addModelScroll_;
        page.hoverIndex=tooltipHoverIndex_;
        page.busy=workerBusy_.load();
        AttachAddPage(view,page);
    } else if(activePage_==3) {
        BackupPageInput page;
        for(const auto& row:backups_)
            page.entries.push_back({row.file,row.kind,row.stamp,row.sizeKb});
        page.filter=backupFilter_;
        page.busy=backupBusy_;
        page.error=backupError_;
        AttachBackupPage(view,page);
    } else if(activePage_==4) {
        // 设置行的显示值是字段自己的文本（旧面板每次 render 也把 input.value 写成设置值）：
        // 没在编辑的字段跟着设置走，正在编辑的字段保留用户输入。
        const auto sync=[&](TextField& field,const std::string& value) {
            if(focusedField_==&field) return;
            if(field.TextUtf8()!=value) field.SetTextUtf8(value);
        };
        sync(setDistro_,stored_.distro);
        sync(setWebuiPort_,std::to_string(stored_.webuiPort));
        sync(setPollSec_,std::to_string(stored_.pollSeconds));
        sync(setHotkey_,stored_.hotkey);
        SettingsPageInput page;
        page.distro=setDistro_.TextUtf8();
        page.webuiPort=atoi(setWebuiPort_.TextUtf8().c_str())>0?atoi(setWebuiPort_.TextUtf8().c_str())
            :stored_.webuiPort;
        page.pollSeconds=atoi(setPollSec_.TextUtf8().c_str())>0?atoi(setPollSec_.TextUtf8().c_str())
            :stored_.pollSeconds;
        page.hotkey=setHotkey_.TextUtf8();
        page.closeAction=stored_.closeAction;
        page.closeSelectOpen=settingsSelectOpen_;
        page.theme=stored_.theme;
        page.previewDiff=stored_.previewDiff;
        page.autoReloadGateway=stored_.autoReloadGateway;
        page.latencyProbe=stored_.latencyProbe;
        page.latencyMinutes=stored_.latencyMinutes;
        page.widgetStyle=stored_.widgetStyle;
        page.alwaysOnTop=stored_.topmost;
        page.hotkeyCapturing=setHotkeyCapturing_;
        page.hotkeyRegistered=hotkeysRegistered_;
        page.hotkeyError=hotkeyErrors_[0];
        page.meta="\xE8\xAE\xBE\xE7\xBD\xAE\xE4\xB8\x8E\xE6\x97\xA5\xE5\xBF\x97\xEF\xBC\x9A"
            +Narrow(settings_?settings_->Path():L"");
        // 审计 F-015 / DESIGN §5.1：config.yaml 解析失败必须醒目提示（写操作已在数据层停用）。
        if(!hermesState_.config.parses)
            page.meta=Narrow(L"config.yaml \u89E3\u6790\u5931\u8D25\uFF0C\u5199\u64CD\u4F5C\u5DF2\u505C"
                L"\u7528\uFF1B")+page.meta;
        AttachSettingsPage(view,page);
    }
    view.pageScroll=pageScroll_;
    view.modalScrollX=modalScrollX_;
    view.fallbackScroll=fallbackScroll_;
    // G1：收起态挂件（strip 窗口）用同一份视图绘制。
    view.widgetStyle=stored_.widgetStyle;
    // 拖动中按**即将贴上的边**渲染（真机要求：向上拖到上边框附近就该按上边流动，
    // 而不是等贴上去才变）。previewEdge_ 只影响绘制，落点仍由 CommitStripDrop 决定。
    const auto shownEdge=previewEdge_?static_cast<Edge>(previewEdge_):model_.edge;
    view.widgetEdge=static_cast<int>(shownEdge);
    view.widgetVertical=shownEdge==Edge::Left||shownEdge==Edge::Right;
    view.focusedFormField=FormFieldIndexOf(focusedField_);
    // v1.21（真机反馈"输入框应该有光标"）：把插入符位置一起交给渲染层。此前只有搜索框画插入符，
    // 表单字段只有一圈高亮，用户看不出光标在哪。
    view.focusedFormCaret=focusedField_?focusedField_->caret:-1;
    view.focusedFormMultiline=focusedField_?focusedField_->multiline:false;
    view.focusedFormNative=NativeFor(focusedField_)!=nullptr;
    if(activePage_==0) {
        ModelsPageInput page;
        page.state=&hermesState_;
        page.helperError=helperError_;
        page.filterText=search_.TextUtf8();
        page.filterKind=filterKind_;
        page.favorites=stored_.favorites;
        page.recents=stored_.recents;
        page.expanded=expanded_;
        page.cursor=cursor_;
        page.listScroll=listScroll_;
        page.switchingKey=switchingKey_;
        page.switchingModel=switchingModel_;
        AttachModelsPage(view,page);
        // F4：每行尾部的延迟徽章（只给有历史记录的模型）。
        if(latency_) {
            for(auto& group:view.groups) {
                for(auto& row:group.rows) {
                    int ms=0;
                    bool ok=false;
                    if(latency_->Last(row.key,&ms,&ok)) {
                        row.hasLatency=true;
                        row.latencyMs=ms;
                        row.latencyOk=ok;
                    }
                }
            }
        }
        view.searchHasText=!search_.Text().empty();
        view.syncBusy=syncAllBusy_.load();
        view.syncDone=syncAllDone_.load();
        view.syncTotal=syncAllTotal_.load();
        // The scroll offset is clamped against the real viewport, so the wheel/drag and the renderer
        // always agree on where the list is (the clamp needs the layout, hence the client rect here).
        RECT client{};
        if(panel_&&GetClientRect(panel_,&client)&&client.bottom>client.top) {
            // 与绘制同源的倍率（WindowDpi）：跨屏拖动时 monitor_ 还是旧显示器。
            const auto laid=ComputeLayout(view,static_cast<float>(client.right),
                static_cast<float>(client.bottom),static_cast<float>(WindowDpi())/96.0f);
            view.listScroll=laid.listScroll;
            if(laid.listScroll!=listScroll_) listScroll_=laid.listScroll;
        }
    }
    // 悬停气泡的文案查找放在最后：页面级提示（如回退链每行的探测说明）是在各页 attach 时
    // 才进 tooltips 表的，早于它查会漏掉这些条目。
    // 模态打开时不显示提示：提示现在是最后绘制的浮层，不挡住模态（DESIGN v1.12）。
    // 下拉展开时同样不显示：气泡画在最上层，会正好盖住列表里靠近指针的那一项
    // （真机实测：设置页「关闭按钮」下拉的「退出程序」被"点一下编辑"整块挡住，看不见也点不准）。
    auto anySelectOpen=fbSelectOpen_||fbModelSelectOpen_||addSelectOpen_||settingsSelectOpen_;
    if(tooltipVisible_&&tooltipHover_!=0&&pending_==Pending::None&&!anySelectOpen) {
        for(const auto& tip:view.tooltips)
            if(tip.control==tooltipHover_&&(tip.index<0||tip.index==tooltipHoverIndex_)) {
                view.tooltip.visible=true;view.tooltip.text=tip.text;break;
            }
    }
    // 原生 EDIT 的占位（cue banner）要跟着视图走：创建时只设过一次，而添加页的
    // 「名称 / Base URL / API Key」三处占位只存在于视图的 hint 里 —— 空框一直是全空的
    // （真机：输入框看着像坏了，也看不出该填什么）。只在文案真的变化时下发一次。
    if(activePage_==2) {
        const auto setCue=[&](TextField* field,const std::string& hint) {
            if(!field||field->placeholder==hint) return;
            field->placeholder=hint;
            if(auto* native=NativeFor(field)) native->SetCue(Widen(hint));
        };
        for(std::size_t index=0;index<view.add.fields.size()&&index<3;++index)
            setCue(AddFieldAt(static_cast<int>(index)),view.add.fields[index].hint);
    }
    return view;
}
namespace {
std::string FormatAge(long long seconds) {
    if(seconds<60) return Narrow(L"刚刚");
    if(seconds<3600) return std::to_string(seconds/60)+Narrow(L" 分钟前");
    if(seconds<86400) return std::to_string(seconds/3600)+Narrow(L" 小时前");
    return std::to_string(seconds/86400)+Narrow(L" 天前");
}
std::string FormatTokens(long long value) {
    const auto scaled=[&](long long unit,const char* suffix) {
        return std::to_string(value/unit)+"."+std::to_string((value/(unit/10))%10)+suffix;
    };
    if(value>=1000000000LL) return scaled(1000000000LL,"B");
    if(value>=1000000LL) return scaled(1000000LL,"M");
    if(value>=1000LL) return scaled(1000LL,"K");
    return std::to_string(value);
}
}
void Application::ShowReadResult(const std::string& kind,const std::string& json,const std::string& error,
    unsigned token) {
    // F2 诊断 / F3 用量：只读助手返回的是 {"ok":...,"data":{...}} 里的 data（HelperBridge 已剥掉外壳）。
    // 代号不匹配 = 用户已经关掉这张卡（或又点了一次刷新）：结果直接丢掉，不许把模态再弹回来。
    if(token!=readToken_) {
        if(log_) log_->Info("data","discarded a stale read result ("+std::to_string(token)
            +" != "+std::to_string(readToken_)+")");
        return;
    }
    readBusy_=false;
    if(pending_!=Pending::DiagnoseInfo&&pending_!=Pending::UsageInfo) {
        // 读取期间把模态关掉了：只更新缓存，不再弹回来（真机反馈：关掉又自己冒出来）。
        if(log_) log_->Info("data","read result arrived after the modal was closed");
        return;
    }
    const bool usage=kind=="usage";
    readTitle_=usage?Narrow(L"用量统计"):Narrow(L"诊断");
    readFacts_.clear();
    modalScrollX_=0;
    if(!error.empty()) {
        readFacts_.push_back(Narrow(L"读取失败：")+TruncateUtf8(error,140));
    } else if(!usage) {
        try {
            const auto data=Json::parse(json);
            const auto gateway=data.contains("gateway")&&data["gateway"].is_object()
                ?data["gateway"]:Json::object();
            const auto state=gateway.value("active_state",std::string{"unknown"});
            readFacts_.push_back(Narrow(L"网关：")+state
                +(gateway.value("listening",false)?Narrow(L" · 端口在监听")
                    :Narrow(L" · 端口未监听")));
            const auto since=gateway.value("active_since",std::string{});
            if(!since.empty()) readFacts_.push_back(Narrow(L"启动：")+TruncateUtf8(since,64));
            if(data.contains("channels")&&data["channels"].is_array()) {
                int shown=0;
                for(const auto& channel:data["channels"]) {
                    if(shown++>=6) break;
                    std::string line=channel.value("name",std::string{"?"})+Narrow(L"：")
                        +(channel.value("connected",false)?Narrow(L"已连接"):Narrow(L"未连接"));
                    auto stamp=channel.value("last_activity",std::string{});
                    // 模态是单行绘制：时间戳只留 "MM-DD HH:MM"，否则相对时间会被省略号吃掉。
                    if(stamp.size()>=16) stamp=stamp.substr(5,11);
                    if(!stamp.empty())
                        line+=Narrow(L" · 最后活动 ")+stamp
                            +Narrow(L"（")+FormatAge(channel.value("stale_seconds",0LL))
                            +Narrow(L"）");
                    // 微信失联判据（F1 实测）：sync.json 在更新、context-tokens.json 不动。
                    const auto sync=channel.value("sync_mtime",0LL);
                    const auto context=channel.value("context_tokens_mtime",0LL);
                    if(sync>0&&context>0&&sync>context+600) line+=Narrow(L" · 疑似不再投递");
                    readFacts_.push_back(TruncateUtf8(line,160));
                }
            }
            if(data.contains("logs")&&data["logs"].is_object()) {
                const auto& logs=data["logs"];
                const auto tail=logs.contains("gateway_tail")&&logs["gateway_tail"].is_array()
                    ?logs["gateway_tail"]:Json::array();
                if(!tail.empty()) {
                    readFacts_.push_back(Narrow(L"—— 网关日志 ——"));
                    const int start=std::max(0,static_cast<int>(tail.size())-6);
                    for(int index=start;index<static_cast<int>(tail.size());++index)
                        readFacts_.push_back(TruncateUtf8(
                            tail[static_cast<std::size_t>(index)].get<std::string>(),120));
                }
            }
        } catch(const std::exception& exception) {
            readFacts_.push_back(Narrow(L"解析失败：")+TruncateUtf8(exception.what(),140));
        }
    } else {
        try {
            const auto data=Json::parse(json);
            if(data.contains("ok")&&data["ok"].is_boolean()&&!data["ok"].get<bool>()) {
                readFacts_.push_back(Narrow(L"不可用：")
                    +TruncateUtf8(data.value("error",std::string{}),140));
            } else {
                const auto overview=data.contains("overview")&&data["overview"].is_object()
                    ?data["overview"]:Json::object();
                readFacts_.push_back(Narrow(L"最近 ")+std::to_string(data.value("days",7))
                    +Narrow(L" 天"));
                readFacts_.push_back(Narrow(L"会话：")
                    +std::to_string(overview.value("total_sessions",0LL))
                    +Narrow(L" · 消息：")+std::to_string(overview.value("total_messages",0LL)));
                readFacts_.push_back(Narrow(L"输入 ")+FormatTokens(overview.value("input_tokens",0LL))
                    +Narrow(L" · 输出 ")+FormatTokens(overview.value("output_tokens",0LL))
                    +Narrow(L" · 缓存读 ")+FormatTokens(overview.value("cache_read_tokens",0LL)));
                const auto models=data.contains("models")&&data["models"].is_array()
                    ?data["models"]:Json::array();
                if(!models.empty()) readFacts_.push_back(Narrow(L"—— 按模型 ——"));
                int shown=0;
                for(const auto& row:models) {
                    if(shown++>=6) break;
                    readFacts_.push_back(TruncateUtf8(
                        row.value("model",std::string{"?"})+Narrow(L"：")
                        +std::to_string(row.value("sessions",0LL))+Narrow(L" 会话 · ")
                        +FormatTokens(row.value("total_tokens",0LL))+Narrow(L" tokens"),140));
                }
            }
        } catch(const std::exception& exception) {
            readFacts_.push_back(Narrow(L"解析失败：")+TruncateUtf8(exception.what(),140));
        }
    }
    pending_=usage?Pending::UsageInfo:Pending::DiagnoseInfo;
    InvalidateAll();
}
void Application::MigrateLegacySettings() {
    // 旧 Electron 版的数据目录（%APPDATA%\HermesModelPanel）**只读**：把与面板行为有关的键搬一次，
    // 搬过就记 legacyMigrated，之后不再搬（旧版与其数据始终可回退）。
    //
    // 例外：如果新设置里的 helperPath 还是内置默认值（早期版本迁移时漏搬了这两个键），而旧文件里有
    // 真实值，就只补这两个键——否则面板会一直拿默认路径去跑不存在的助手脚本（真机上表现为"读不到配置"）。
    const Settings defaults;
    const auto helperLooksUnset=stored_.helperPath.empty()||stored_.helperPath==defaults.helperPath;
    const auto onlyHelperKeys=stored_.legacyMigrated&&helperLooksUnset;
    if(stored_.legacyMigrated&&!helperLooksUnset) return;
    std::wstring directory=legacyDirectory_.empty()
        ?LegacyDefaultDirectory():legacyDirectory_;
    if(directory.empty()) return;
    const auto path=directory+L"\\settings.json";
    std::ifstream input(path);
    if(!input) {
        if(log_) log_->Info("settings","no legacy settings at the expected path");
        stored_.legacyMigrated=true;
        PersistSettings(true);
        return;
    }
    try {
        Json legacy;
        input>>legacy;
        const auto number=[&](const char* key,int fallback) {
            return legacy.contains(key)&&legacy[key].is_number()?legacy[key].get<int>():fallback;
        };
        const auto boolean=[&](const char* key,bool fallback) {
            return legacy.contains(key)&&legacy[key].is_boolean()?legacy[key].get<bool>():fallback;
        };
        const auto text=[&](const char* key,const std::string& fallback) {
            return legacy.contains(key)&&legacy[key].is_string()?legacy[key].get<std::string>():fallback;
        };
        if(onlyHelperKeys) {
            // 只补连接信息，不动用户在新面板里改过的收藏/最近/主题等。
            stored_.pythonExe=text("pythonExe",stored_.pythonExe);
            stored_.helperPath=text("helperPath",stored_.helperPath);
            if(stored_.helperPath!=defaults.helperPath&&log_)
                log_->Info("settings","helper path adopted from the legacy settings");
            stored_.legacyMigrated=true;
            PersistSettings(true);
            return;
        }
        stored_.distro=text("distro",stored_.distro);
        // pythonExe / helperPath 是桥能否工作的两个键（旧版把它们写在 settings.json 里）：不搬就会出现
        // "读不到 WSL 配置"——面板拿着自己的默认路径去跑不存在的助手脚本。
        stored_.pythonExe=text("pythonExe",stored_.pythonExe);
        stored_.helperPath=text("helperPath",stored_.helperPath);
        stored_.webuiPort=number("webuiPort",stored_.webuiPort);
        stored_.gatewayPort=number("gatewayPort",stored_.gatewayPort);
        stored_.pollSeconds=std::clamp(number("pollSec",stored_.pollSeconds),5,3600);
        stored_.hotkey=text("hotkey",stored_.hotkey);
        stored_.theme=text("theme",stored_.theme);
        stored_.closeAction=text("closeAction",stored_.closeAction);
        stored_.topmost=boolean("alwaysOnTop",stored_.topmost);
        stored_.previewDiff=boolean("previewDiff",stored_.previewDiff);
        stored_.autoReloadGateway=boolean("autoReloadGateway",stored_.autoReloadGateway);
        if(legacy.contains("favorites")&&legacy["favorites"].is_array()) {
            stored_.favorites.clear();
            for(const auto& entry:legacy["favorites"])
                if(entry.is_string()) stored_.favorites.push_back(entry.get<std::string>());
        }
        if(legacy.contains("recents")&&legacy["recents"].is_array()) {
            stored_.recents.clear();
            for(const auto& entry:legacy["recents"])
                if(entry.is_string()) stored_.recents.push_back(entry.get<std::string>());
            if(stored_.recents.size()>kRecentsMax) stored_.recents.resize(kRecentsMax);
        }
        if(log_) log_->Info("settings","legacy settings migrated from "+Narrow(path));
    } catch(const std::exception& error) {
        // 旧设置坏了不影响使用：记一笔，继续用默认值（不覆盖旧文件）。
        if(log_) log_->Info("settings",std::string("legacy settings ignored: ")+error.what());
    }
    stored_.legacyMigrated=true;
    PersistSettings(true);
    palette_=ResolveTheme(stored_.theme);
    RegisterHotkeys();
    UpdatePollTimer();
}
void Application::BuildTrayMenu() {
    // 菜单在右键时现建（旧 refreshTrayMenu），这里只把缓存的条目表清掉，保证下次构建是最新的。
    switchKeys_.clear();
}
void Application::CloseForm() {
    fbFormOpen_=false;
    fbSelectOpen_=false;
    fbModelSelectOpen_=false;
    selectScroll_=0;
    fbError_.clear();
    if(focusedField_==&fbModel_) BlurField();
    InvalidateAll();
}
// ---- 模型页的输入：一次命中测试同时服务点击与悬停（列表行/星标/组头/过滤段/滚动条）----
// ---- H2c the self-drawn text fields: one focused field at a time; keyboard, IME and clipboard all
// enter through here. The models-page search box is the first host (DESIGN #10/#36).
void Application::InvalidateAll() {
    // v1.30：视觉状态变了，悬停布局缓存作废（下一次悬停重建一次即可）。
    hoverLayoutValid_=false;
    // 窗口高度是用户固定的，不再按页面内容 fit：切页只重排正文，底栏与窗口尺寸不动。
    if(panel_) InvalidateRect(panel_,nullptr,FALSE);
    // v1.26：原生输入框的底色/字色由 WM_CTLCOLOREDIT 现取调色板——换主题后必须让它们重绘。
    for(auto& binding:nativeBindings_)
        if(binding.edit&&binding.edit->Handle()) InvalidateRect(binding.edit->Handle(),nullptr,TRUE);
    if(state_==State::Collapsed) RenderStrip();
    else if(strip_) InvalidateRect(strip_,nullptr,FALSE);
    if(cardVisible_) RenderHoverCard();
}
void Application::PersistSettings(bool force) {
    if(!settings_) return;
    settings_->Save(SnapshotSettings(),force);
    if(log_) log_->Info("settings","saved edge="+std::to_string(static_cast<int>(model_.edge))
        +" anchor="+std::to_string(model_.anchor)+" writes="+std::to_string(settings_->Writes()));
}
LRESULT CALLBACK Application::WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* context=reinterpret_cast<WindowContext*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        context=static_cast<WindowContext*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(context));
    }
    if(context) {
        try { return context->owner->WindowMessage(window,context->strip,message,wparam,lparam); }
        catch(const std::exception& error) { return context->owner->HandleWindowFailure(window,message,error); }
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
LRESULT Application::HandleWindowFailure(HWND window,UINT message,const std::exception& error) {    // A failed paint must not take the panel down with it: log the frame, validate the region and keep
    // the message loop alive. Any other failure is a state error we cannot recover from safely.
    OutputDebugStringA(error.what());
    if(log_) log_->Error(message==WM_PAINT?"paint":"message",error.what());
    if(message==WM_PAINT) { ValidateRect(window,nullptr);return 0; }
    if(log_) log_->Error("message","fatal, closing");
    PostMessageW(control_,WM_CLOSE,0,0);
    return 0;
}
void Application::ReAddTrayAfterExplorerRestart() {
    // Explorer 重启后旧图标句柄失效：先删一次（不存在也无妨），再重新注册并刷新提示。
    ++trayReadds_;
    NOTIFYICONDATAW stale{};
    stale.cbSize=sizeof(stale);
    stale.hWnd=control_;
    stale.uID=kTrayId;
    Shell_NotifyIconW(NIM_DELETE,&stale);
    trayRegistered_=false;
    if(AddTray()) UpdateTrayStatus();
    if(log_) log_->Info("tray","re-added after explorer restart (readds="
        +std::to_string(trayReadds_)+")");
}
void Application::ReportControlFailure(const std::exception& error) {
    // 托盘菜单/热键在 ControlProc 里同步执行，那里没有面板 WindowProc 的 try/catch。
    // 不接住的话异常会穿出 DispatchMessage，由未处理异常过滤器兜底（表现为“突然消失”）。
    OutputDebugStringA(error.what());
    if(log_) log_->Error("control","fatal, closing: "+std::string(error.what()));
    if(control_) PostMessageW(control_,WM_CLOSE,0,0);
}
LRESULT Application::WindowMessage(HWND window,bool strip,UINT message,WPARAM wparam,LPARAM lparam) {
    const bool card=window==hoverCard_;
    if(taskbarCreated_&&message==taskbarCreated_) {
        ReAddTrayAfterExplorerRestart();
        return 0;
    }
    auto& surface=card?hoverCardContext_.surface:(strip?stripContext_.surface:panelContext_.surface);
    switch(message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};BeginPaint(window,&paint);
        if(!card&&!strip) ++paintFrames_;
        // 忙碌期间量一帧面板重绘的耗时：忙碌条"卡一下"要么是 UI 线程被别处挡住（kSpinTimer
        // 的间隔探针会记 gap），要么就是这一帧画得太慢。两条日志一起才能定性（真机 2026-10-21）。
        LARGE_INTEGER paintFreq{},paintStart{},paintDone{};
        const bool timing=!card&&!strip&&workerBusy_.load();
        if(timing) { QueryPerformanceFrequency(&paintFreq);QueryPerformanceCounter(&paintStart); }
        // The search field is handed to the renderer even when it is not focused: an unfocused box must
        // still show its own text (the TextField's `focused` flag drives the caret and the glow).
        try {
            // 悬停气泡挂在控件下方：锚点由 LayoutForInput 从布局里取（它同时把 tooltipView_ 填好）。
            // 先跑一次布局，再把带锚点的气泡并进这一帧的视图 —— 直接读上一步缓存的 tooltipView_
            // 会拿到"气泡还没显示时"的空锚点（真机上就是 dump 说可见、画面上却没有）。
            if(!card&&!strip&&dragging_&&panelDragViewValid_) {
                // 系统移动循环里每一帧都完整重建视图（含列表布局）会明显拖慢拖动
                // （真机实测面板拖动 14% CPU 且跟手变差）；拖动期间内容不变，直接复用。
                surface.Paint(window,false,panelDragView_,palette_,&search_);
                // 跨显示器拖动会换窗口 DPI（WM_DPICHANGED 在拖动中被推迟，绘制倍率却已经变了）：
                // 原生输入框与忙碌条必须按同一倍率重摆一次，否则占位文字会留在旧坐标、
                // 字号还按旧 DPI 画——真机看到的"搜索模型或供应商跑出输入框"。
                RECT client{};
                GetClientRect(window,&client);
                const auto dpi=WindowDpi();
                if(dpi!=dragLayoutDpi_||client.right!=dragLayoutWidth_
                    ||client.bottom!=dragLayoutHeight_) {
                    dragLayoutDpi_=dpi;dragLayoutWidth_=client.right;dragLayoutHeight_=client.bottom;
                    const auto laid=ComputeLayout(panelDragView_,static_cast<float>(client.right),
                        static_cast<float>(client.bottom),static_cast<float>(dpi)/96.0f);
                    SyncNativeFields(laid);
                    SyncBusyBar(laid,surface.PageHeadActionIcon());
                }
                EndPaint(window,&paint);
                return 0;
            }
            LARGE_INTEGER sub1{},sub2{},subFreq{};
            if(timing) { QueryPerformanceFrequency(&subFreq);QueryPerformanceCounter(&sub1); }
            auto view=CurrentView();
            LARGE_INTEGER sub3{}; if(timing) QueryPerformanceCounter(&sub2);
            if(card) {
                surface.PaintHoverCard(window,view,palette_);
            } else if(strip) {
                // 贴条走分层窗口：形状由 D2D 抗锯齿绘制；没有提示气泡，也不需要面板输入布局。
                surface.PaintStrip(window,view,palette_);
            } else {
                // 复用这一帧已经建好的 view：LayoutForInput() 内部会再 CurrentView() 一次，
                // 等于每帧把 60+ 条 tooltip 文案和整棵列表模型建两遍（实测 layout 阶段 0–3ms/帧）。
                const auto laid=LayoutForInput(view);
                if(timing) QueryPerformanceCounter(&sub3);
                if(dragging_) { panelDragView_=view;panelDragViewValid_=true; }
                surface.Paint(window,false,std::move(view),palette_,&search_);
                // v1.23：原生输入框跟着这一帧的布局走（滚动/切页/缩放后位置都对）。
                LARGE_INTEGER phFreq{},ph0{},ph1{},ph2{};
                QueryPerformanceFrequency(&phFreq);
                QueryPerformanceCounter(&ph0);
                SyncNativeFields(laid);
                QueryPerformanceCounter(&ph1);
                SyncBusyBar(laid,surface.PageHeadActionIcon());
                QueryPerformanceCounter(&ph2);
                if(phFreq.QuadPart>0) {
                    const auto toMs=[&](const LARGE_INTEGER& a,const LARGE_INTEGER& b) {
                        return static_cast<double>(b.QuadPart-a.QuadPart)*1000.0
                            /static_cast<double>(phFreq.QuadPart);
                    };
                    lastSyncNativeMs_=toMs(ph0,ph1);
                    lastSyncBusyMs_=toMs(ph1,ph2);
                }
            }
            if(timing&&subFreq.QuadPart>0) {
                const auto msOf=[](const LARGE_INTEGER& a,const LARGE_INTEGER& b,long long f) {
                    return static_cast<double>(b.QuadPart-a.QuadPart)*1000.0/static_cast<double>(f);
                };
                LARGE_INTEGER sub4{};QueryPerformanceCounter(&sub4);
                paintSubMs_[0]=msOf(sub1,sub2,subFreq.QuadPart);   // CurrentView
                paintSubMs_[1]=msOf(sub2,sub3,subFreq.QuadPart);   // LayoutForInput
                paintSubMs_[2]=msOf(sub3,sub4,subFreq.QuadPart);   // surface.Paint
            }
        } catch(...) { EndPaint(window,&paint);throw; }
        LARGE_INTEGER endFreq{},end0{},end1{};
        QueryPerformanceFrequency(&endFreq);
        QueryPerformanceCounter(&end0);
        EndPaint(window,&paint);
        QueryPerformanceCounter(&end1);
        if(timing&&endFreq.QuadPart>0)
            lastEndPaintMs_=static_cast<double>(end1.QuadPart-end0.QuadPart)*1000.0
                /static_cast<double>(endFreq.QuadPart);
        if(timing) {
            QueryPerformanceCounter(&paintDone);
            const auto now=GetTickCount64();
            if(paintFreq.QuadPart>0&&now-lastPaintLog_>500) {
                const auto ms=static_cast<double>(paintDone.QuadPart-paintStart.QuadPart)*1000.0
                    /static_cast<double>(paintFreq.QuadPart);
                if(log_&&ms>=25.0) log_->Info("ui","paint phases begin="
                    +std::to_string(static_cast<int>(panelContext_.surface.LastBeginMs()))
                    +"ms draw="+std::to_string(static_cast<int>(panelContext_.surface.LastDrawMs()))
                    +"ms end="+std::to_string(static_cast<int>(panelContext_.surface.LastEndMs()))
                    +"ms target="+(panelContext_.surface.TargetKind()==2?"software":"hardware")
                    +" syncNative="+std::to_string(static_cast<int>(lastSyncNativeMs_))
                    +"ms syncBusy="+std::to_string(static_cast<int>(lastSyncBusyMs_))
                    +"ms endPaint="+std::to_string(static_cast<int>(lastEndPaintMs_))+"ms");
                if(ms>=25.0) {
                    lastPaintLog_=now;
                    if(log_) log_->Info("ui","busy paint "+std::to_string(static_cast<int>(ms))+"ms (view "
                        +std::to_string(static_cast<int>(paintSubMs_[0]))+" + layout "
                        +std::to_string(static_cast<int>(paintSubMs_[1]))+" + draw "
                        +std::to_string(static_cast<int>(paintSubMs_[2]))+")");
                }
            }
        }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if(strip) {
            ++stripMoves_;
            if(stripPressed_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                UpdateStripDrag(cursor);
            } else {
                LogEvent("strip-move");
                // 静止 300ms 才出现信息卡；每次移动重置计时器。
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window,0};
                TrackMouseEvent(&track);
                ArmStripCard();
            }
            return 0;
        }
        if(resizing_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            UpdatePanelResize(cursor);
            return 0;
        }
        PointerMovedOnPanel(MessagePoint(window,lparam));
        if(modalScrollDrag_) { ScrollModalFrom(static_cast<float>(GET_X_LPARAM(lparam)));return 0; }
        if(scrollDrag_) { ScrollFromThumb(static_cast<float>(GET_Y_LPARAM(lparam)));return 0; }
        if(pageScrollDrag_) { PageScrollFromThumb(static_cast<float>(GET_Y_LPARAM(lparam)));return 0; }
        // 合成悬停保护窗内不吃真实移动（门禁的 hover → 立刻被系统真实指针覆盖是历史偶发失败源）。
        if(syntheticHover_&&GetTickCount64()<syntheticHoverUntil_) return 0;
        syntheticHover_=false;
        TrackHover(window,static_cast<float>(GET_X_LPARAM(lparam)),static_cast<float>(GET_Y_LPARAM(lparam)));
        return 0;
    case WM_LBUTTONUP:
        if(strip) {
            // 拖动过 → 吸附到最近边缘（保持贴条，不展开）；没拖动 → 这一次就是单击展开。
            // up 是"点击展开"的关键一步：把"收到 up 但状态不对"也记进事件环，
            // 门禁失败时能一眼分辨"up 没到"还是"up 到了但被忽略"（真机/门禁 2026-10-21）。
            LogEvent(stripDragging_?"strip-up-drag":(stripPressed_?"strip-up-click":"strip-up-stale"));
            if(stripDragging_) { CommitStripDrop();return 0; }
            if(stripPressed_) {
                stripPressed_=false;
                if(GetCapture()==strip_) ReleaseCapture();
                PointerEnteredStrip();
            }
            return 0;
        }
        if(resizing_) { CommitPanelResize();return 0; }
        if(modalScrollDrag_) { modalScrollDrag_=false;return 0; }
        if(scrollDrag_) { scrollDrag_=false;return 0; }
        if(pageScrollDrag_) { pageScrollDrag_=false;return 0; }
        break;
    case WM_CAPTURECHANGED:
        if(resizing_) CommitPanelResize();
        else if(strip&&stripPressed_) {
            // 抓屏（CaptureBackdrop 的 hide/show）会顺带丢掉 capture：这里把"点击被谁吃掉"
            // 记进事件环，门禁失败时可判定（真机/门禁 2026-10-21）。
            LogEvent(stripDragging_?"capture-lost-drag":"capture-lost-press");
            if(stripDragging_) CommitStripDrop();
            else { stripPressed_=false;stripDragging_=false; }
        }
        break;
    case WM_LBUTTONDOWN:
        if(strip) {
            POINT cursor{};
            GetCursorPos(&cursor);
            BeginStripPress(cursor);
            return 0;
        }
        if(log_) log_->Info("ui","mouse down at "+std::to_string(GET_X_LPARAM(lparam))+","
            +std::to_string(GET_Y_LPARAM(lparam)));
        {
            // A control under the cursor acts on press: the same handler the tray menu uses.
            RECT client{};
            GetClientRect(window,&client);
            const auto scale=static_cast<float>(WindowDpi())/96.0f;
            const auto view=CurrentView();
            const auto layout=ComputeLayout(view,static_cast<float>(client.right),
                static_cast<float>(client.bottom),scale);
            const auto x=static_cast<float>(GET_X_LPARAM(lparam));
            const auto y=static_cast<float>(GET_Y_LPARAM(lparam));
            // 底边条带：拖动改窗口高度（导航图标结束于底边上方 6 DIP，不会抢按钮）。
            if(CanResizeHeight()&&y>=static_cast<float>(client.bottom-ScaleDip(6,monitor_.geometry.dpi))) {
                POINT screen{static_cast<LONG>(x),static_cast<LONG>(y)};
                ClientToScreen(window,&screen);
                BeginPanelResize(screen);
                return 0;
            }
            // 状态中心打开时是模态浮层：点某一行执行该状态的动作；点其它地方只关闭。
            if(statusCenterOpen_) {
                for(std::size_t index=0;index<layout.statusRows.size()&&index<view.statuses.size();
                    ++index) {
                    const auto& row=layout.statusRows[index].row;
                    if(x>=row.left&&x<row.right&&y>=row.top&&y<row.bottom) {
                        statusCenterOpen_=false;
                        InvokeStatusAction(view.statuses[index].action);
                        InvalidateAll();
                        return 0;
                    }
                }
                statusCenterOpen_=false;
                InvalidateAll();
                return 0;
            }
            // 离线卡整块替换内容区（含它自己的「启动 WSL」按钮）。
            if(!hermesState_.error.empty()||hermesState_.wsl=="down") {
                if(const auto* start=ControlRectFor(layout,ControlId::OfflineStart);
                    start&&x>=start->left&&x<start->right&&y>=start->top&&y<start->bottom) {
                    StartWsl();
                    return 0;
                }
            }
            // 模态盖在整页上：点它交给当前页的处理函数（它自己先看模态），其余点击各页自理。
            if(pending_!=Pending::None) {
                // 模态事实行的横向滑条：拖动/点轨道先于模态按钮处理（轨道在正文盒底部）。
                if(layout.modalScrollVisible) {
                    const auto& track=layout.modalHScrollTrack;
                    if(x>=track.left&&x<track.right&&y>=track.top&&y<track.bottom) {
                        const auto& thumb=layout.modalHScrollThumb;
                        modalScrollDrag_=true;
                        modalScrollGrab_=(x>=thumb.left&&x<thumb.right)
                            ?x-thumb.left:(thumb.right-thumb.left)/2.0f;
                        ScrollModalFrom(x);
                        return 0;
                    }
                }
                // 模态是全局的：先给统一的模态处理，剩下的点击才轮到当前页（旧版点遮罩什么也不做）。
                if(HandleModalClick(x,y)) return 0;
                return 0;
            }
            // 页内滚动条（设置/添加/备份）：轨道在正文右缘之外，先于页面点击处理。
            if(layout.pageScrollVisible&&ControlAt(layout,x,y)==ControlId::None) {
                // v1.25：控件优先于滚动条。搜索框右端的 ✕/刷新同步 正好压在页内滚动条上，
                // 之前点它们会变成"拖滚动条"（真机反馈"按钮穿透/点不到"）。
                const auto& track=layout.pageScrollTrack;
                if(x>=track.left-2.0f&&x<track.right+2.0f&&y>=track.top&&y<track.bottom) {
                    const auto& thumb=layout.pageScrollThumb;
                    if(y>=thumb.top&&y<thumb.bottom) {
                        pageScrollDrag_=true;
                        pageScrollGrabOffset_=y-thumb.top;
                    } else {
                        const float viewport=layout.content.bottom-layout.content.top;
                        pageScroll_=std::clamp(pageScroll_+(y<(thumb.top+thumb.bottom)/2.0f
                            ?-viewport*0.9f:viewport*0.9f),0.0f,layout.pageMaxScroll);
                        InvalidateAll();
                    }
                    return 0;
                }
            }
            if(activePage_==1) { FallbackPageClick(x,y);return 0; }
            if(activePage_==2) { AddPageClick(x,y);return 0; }
            if(activePage_==3) { BackupPageClick(x,y);return 0; }
            if(activePage_==4) { SettingsPageClick(x,y);return 0; }
            const auto id=ControlAt(layout,x,y);
            if(log_) log_->Info("ui","click control="+std::to_string(static_cast<int>(id))
                +" at "+std::to_string(static_cast<int>(x))+","+std::to_string(static_cast<int>(y)));
            if(id==ControlId::SearchBox) { FocusField(&search_);return 0; }
            if(id==ControlId::SearchClear) { ClearSearch();FocusField(&search_);return 0; }
            if(id!=ControlId::None) { InvokeControl(id);BlurField();return 0; }
            // The models-page list: rows switch a model, stars favourite one, group heads collapse, and
            // the filters scope the list - all through the one hit test the renderer draws from.
            if(activePage_==0&&hermesState_.wsl!="down") {
                cursor_=-1;
                ModelsPageClick(x,y);
                BlurField();
                return 0;
            }
            BlurField();
        }
        break;
    case WM_MOUSELEAVE:
        if(!strip&&syntheticHover_) {
            // 合成悬停期间系统补发的离开消息不是用户动作：忽略一次（真实移动会重新 TrackHover）。
            syntheticHover_=false;
            return 0;
        }
        if(!strip) {
            PointerLeftPanel({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});
            pointerInside_=false;
            // 离开时必须清掉滑条悬停态，否则下次从外面移回槽里时 scrollHotChanged 为假、不重绘。
            scrollHot_=false;
            if(panel_) KillTimer(panel_,kTooltipTimer);
            tooltipVisible_=false;
            if(hoverControl_!=0) { hoverControl_=0;InvalidateRect(window,nullptr,FALSE); }
        } else {
            HideHoverCard();
        }
        return 0;
    case WM_GETDPISCALEDSIZE:
        if(strip||!lparam) break;
        {
            const auto tier=TierSize(model_,static_cast<unsigned>(wparam));
            auto* size=reinterpret_cast<SIZE*>(lparam);
            size->cx=tier.width;size->cy=tier.height;
        }
        return TRUE;
    case WM_DPICHANGED:
        {
            const auto* suggested=reinterpret_cast<const RECT*>(lparam);
            ApplyDpiChange(suggested?*suggested:RECT{});
        }
        return 0;
    case WM_DISPLAYCHANGE:
        RefreshTopology("display-change");
        return 0;
    case WM_SETTINGCHANGE:
        if(wparam==SPI_SETWORKAREA) { RefreshTopology("work-area");return 0; }
        // 「跟随系统」要跟着 Windows 的明暗切换走（旧版用 matchMedia）。
        if(lparam&&stored_.theme=="system") {
            const std::wstring setting(reinterpret_cast<const wchar_t*>(lparam));
            if(setting==L"ImmersiveColorSet") {
                // 系统明暗切换也走同一条交叉淡入（与设置页换主题一致）。
                StartThemeMotion(ResolveTheme("system"));
                InvalidateAll();
                if(strip_) InvalidateRect(strip_,nullptr,FALSE);
                if(log_) log_->Info("ui","system theme changed");
                return 0;
            }
        }
        break;
    case WM_COMMAND: {
        // 原生输入框的 EN_CHANGE：把控件文本实时写回镜像（既有逻辑都读 TextField::TextUtf8）。
        const int id=LOWORD(wparam);
        if(HIWORD(wparam)==EN_CHANGE&&id>=1) {
            HandleNativeFieldEvent(id,static_cast<int>(NativeField::Event::Change));
            return 0;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        // 原生输入框的背景与自绘框同色（主题切换后立刻生效）。
        auto dc=reinterpret_cast<HDC>(wparam);
        const auto toColour=[](const D2D1_COLOR_F& colour) {
            return RGB(static_cast<BYTE>(std::lround(colour.r*255.0f)),
                static_cast<BYTE>(std::lround(colour.g*255.0f)),
                static_cast<BYTE>(std::lround(colour.b*255.0f)));
        };
        SetTextColor(dc,toColour(palette_.text));
        SetBkColor(dc,toColour(palette_.raise));
        // 刷子按颜色缓存：每条消息都新建/删除会漏句柄（旧刷子往往还选在某个子控件 DC 里，
        // DeleteObject 失败）。只在颜色真的变化时换一次。
        static HBRUSH brush=nullptr;
        static COLORREF brushColour=CLR_INVALID;
        const auto wanted=toColour(palette_.raise);
        if(!brush||wanted!=brushColour) {
            if(brush) DeleteObject(brush);
            brush=CreateSolidBrush(wanted);
            brushColour=wanted;
        }
        return reinterpret_cast<LRESULT>(brush);
    }
    case WM_SETCURSOR:
        if(strip) {
            SetCursor(LoadCursorW(nullptr,stripPressed_?IDC_SIZEALL:IDC_HAND));
            return TRUE;
        }
        if(!strip&&LOWORD(lparam)==HTCLIENT&&CanResizeHeight()) {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window,&point);
            RECT client{};
            GetClientRect(window,&client);
            if(point.y>=client.bottom-ScaleDip(6,monitor_.geometry.dpi)) {
                SetCursor(LoadCursorW(nullptr,IDC_SIZENS));
                return TRUE;
            }
        }
        break;
    case WM_NCHITTEST:
        if(card) return HTTRANSPARENT;   // 信息卡点击穿透（WS_EX_TRANSPARENT 的双保险）
        if(strip) {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            ScreenToClient(window,&point);
            return HitStripShape(point)?HTCLIENT:HTTRANSPARENT;
        }
        return HitTest({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});
    case WM_NCLBUTTONDOWN:
        // 标题栏/贴边内条的按下就是拖拽起点：此刻记下窗口内偏移，WM_ENTERSIZEMOVE 可能迟到。
        if(!strip&&wparam==HTCAPTION) {
            POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
            RECT windowRect{};
            if(GetWindowRect(window,&windowRect)) {
                dragPressPx_={point.x-windowRect.left,point.y-windowRect.top};
                dragPressValid_=true;
            }
        }
        break;
    case WM_NCLBUTTONUP:
        dragPressValid_=false;
        break;
    case WM_ENTERSIZEMOVE:
        if(strip) break;
        BeginSystemDrag();
        return 0;
    case WM_EXITSIZEMOVE:
        if(strip) break;
        CommitDrop();
        return 0;
    case WM_CHAR:
    case WM_KEYDOWN:
        if(!strip&&wparam==VK_ESCAPE&&statusCenterOpen_) {
            statusCenterOpen_=false;
            InvalidateAll();
            return 0;
        }
        // 审计 F-010/I5：F5 / Ctrl+R = 重新读取并探活（纯键盘也能刷新）。
        // 必须判 message：WM_CHAR 的 't' 也是 0x74（VK_F5），否则输入字母 t 会触发刷新。
        if(!strip&&message==WM_KEYDOWN&&(wparam==VK_F5
            ||((wparam=='R')&&(GetKeyState(VK_CONTROL)<0)))) {
            RefreshData();
            return 0;
        }
        // 键盘焦点链：Tab/Shift+Tab 移动，Enter 激活，Esc 清除（焦点在表单字段时先走字段自己的逻辑）。
        if(!strip&&message==WM_KEYDOWN&&wparam==VK_TAB) {
            MoveFocus((GetKeyState(VK_SHIFT)<0)?-1:1);
            return 0;
        }
        if(!strip&&message==WM_KEYDOWN&&wparam==VK_RETURN&&focusControl_!=0&&!focusedField_) {
            ActivateFocus();
            return 0;
        }
        if(!strip&&message==WM_KEYDOWN&&wparam==VK_ESCAPE&&focusControl_!=0&&!focusedField_) {
            ClearFocus();
            return 0;
        }
        // 「/」聚焦搜索框（DESIGN §4.3 #10 与搜索框提示「/ 聚焦」承诺的行为）走的是 HandleFieldKey
        // 里的 **WM_CHAR '/'** 分支：那里聚焦的同时把这一击吞掉，查询框保持空。
        // 曾经这里也加过一条 WM_KEYDOWN VK_OEM_2 分支，结果是 keydown 先把焦点交给原生 EDIT，
        // 紧跟的 WM_CHAR 就落进 EDIT → 搜索框一聚焦就多一个斜杠（h-ime 门禁 3/3 复现）。
        if(!strip&&HandleFieldKey(message,wparam,lparam)) return 0;
        break;
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_REQUEST:
        if(!strip) { HandleFieldIme(window,message,wparam,lparam);return 0; }
        break;
    case WM_TIMER:
        if(wparam==kMotionTimer&&!strip) { MotionTick();return 0; }
        if(wparam==kTopmostGuardTimer&&!strip) { EnforceTopmost();return 0; }
        if(strip&&wparam==kStripCardTimer) { ShowHoverCard();return 0; }
        if(wparam==kTrayGraceTimer&&!strip) { GraceExpired();return 0; }
        if(wparam==kCaretTimer&&!strip) {
            // 原生输入框自己画光标：这里既不闪也不重绘（否则会带着整块面板一起重画）。
            if(NativeFor(focusedField_)) return 0;
            caretOn_=!caretOn_;panelContext_.surface.SetCaretOn(caretOn_);
            InvalidateRect(panel_,nullptr,FALSE);
            return 0;
        }
        if(wparam==kPollNowTimer&&!strip) { KillTimer(panel_,kPollNowTimer);StartRefresh();return 0; }
        if(wparam==kPollTimer&&!strip) { StartRefresh(true);return 0; }
        if(wparam==kLatencyTimer&&!strip) {
            // 只在面板可见且 WSL 绿灯时探测（文档约定）：看不见面板时不发无谓请求。
            if(IsWindowVisible(panel_)&&hermesState_.wsl=="up") StartLatencyProbe();
            return 0;
        }
        if(wparam==kTooltipTimer&&!strip) {
            KillTimer(panel_,kTooltipTimer);
            if(hoverControl_!=0) {
                tooltipHover_=hoverControl_;
                tooltipVisible_=true;
                StartTooltipMotion();
                if(log_) log_->Info("ui","tooltip shown for control "+std::to_string(hoverControl_));
                InvalidateRect(panel_,nullptr,FALSE);
            }
            return 0;
        }
        if(wparam==kToastTimer&&!strip) {
            toastText_.clear();
            KillTimer(panel_,kToastTimer);
            InvalidateRect(panel_,nullptr,FALSE);
            return 0;
        }
        if(wparam==kSpinTimer&&!strip) {
            // 刷新进行中：同步章图标绕中心旋转；顶部忙碌条的位置由 CurrentView 按时间算，
            // 这里只负责"多久重绘一次"。顺带做 UI 停顿探针：两拍之间隔得太久说明 UI 线程被
            // 别的东西挡住了（真机报"忙碌条偶尔卡一下"必须能从日志判断是渲染慢还是线程被占）。
            const auto now=GetTickCount64();
            if(lastSpinTick_!=0&&workerBusy_.load()) {
                const auto gap=now-lastSpinTick_;
                if(gap>200&&log_) log_->Info("ui","busy tick gap "+std::to_string(gap)+"ms");
            }
            lastSpinTick_=now;
            const bool busy=workerBusy_.load()||allProbeBusy_.load()||probeRunning_.load()
                ||syncAllBusy_.load();
            if(busy) {
                // 忙碌条在独立子窗口里重绘（单帧 0.05ms），每拍都刷即可。
                // 同步章的角度由 CurrentView 按时间算（这里不再累加，否则定时器变慢就"卡住"）。
                busyBar_.Invalidate();
                // 旋转图标在独立的 SpinGlyph 子窗口里转（单帧 <0.2ms，30fps 随便刷），
                // 主面板**不参与**——整窗重绘 10–170ms，拿它当动画时钟必然"不转、只在抖"。
                spinGlyph_.Invalidate();
                // 同步进度「n/N」是主面板上的文字：只有**同步**（有真实进度要显示）才重绘主面板，
                // 而且按 2 拍（≈66ms）限流，避免把 UI 线程占满。
                // 静默轮询（refreshQuiet_）完全不重绘（motion 门禁："an unchanged poll must not
                // repaint the whole panel"）。
                if(syncAllBusy_.load()&&!refreshQuiet_) {
                    if(++spinPanelTick_>=2) { spinPanelTick_=0;InvalidateRect(panel_,nullptr,FALSE); }
                }
            }
            else {
                // 所有工作都结束了：停表。对勾只为**手动刷新**显示 800ms——
                // 轮询（quiet）不显示，否则每 20s 都会为了一个勾重绘整窗（v1.28）。
                KillTimer(panel_,kSpinTimer);
                busyBarTimerArmed_=false;
                // 对勾只属于**同步章自己的手动刷新**：逐条探测/备份刷新结束不该在这里打勾
                // （用户真机反馈："根本不是一个功能"）。refreshOwnsBusy_ 在这一帧用掉后清掉。
                if(refreshQuiet_||!refreshOwnsBusy_) {
                    if(syncCheckShown_) { syncCheckShown_=false;InvalidateRect(panel_,nullptr,FALSE); }
                } else {
                    syncCheckShown_=true;
                    InvalidateRect(panel_,nullptr,FALSE);
                    SetTimer(panel_,kCheckTimer,800,nullptr);
                }
                refreshOwnsBusy_=false;
            }
            return 0;
        }
        if(wparam==kCheckTimer&&!strip) {
            KillTimer(panel_,kCheckTimer);
            syncCheckShown_=false;
            InvalidateRect(panel_,nullptr,FALSE);
            return 0;
        }
        break;
    case kDataMessage:
        if(!strip&&lparam) { ApplyDataResult(reinterpret_cast<DataResult*>(lparam));return 0; }
        break;
    case kLatencyMessage:
        if(!strip&&lparam) {
            HandleLatencyResults(reinterpret_cast<std::vector<LatencyResult>*>(lparam));
            return 0;
        }
        break;
    case kProfileMessage:
        if(!strip&&lparam) {
            HandleProfileResult(reinterpret_cast<ProfileApplyResult*>(lparam));
            return 0;
        }
        break;
    case kProbeProgressMessage:
        if(!strip&&panel_) InvalidateRect(panel_,nullptr,FALSE);
        return 0;
    case kProbeAllMessage:
        if(!strip) { ApplyProbeAll();if(lparam) {} return 0; }
        break;
    case kProbeRowMessage:
        if(!strip&&lparam) { HandleProbeRow(reinterpret_cast<ProbeRow*>(lparam));return 0; }
        break;
    case kSyncAllMessage:
        if(!strip) {
            // wparam=1 才是整批结束（要重读状态、交代网关重启）；0 只是"一个供应商同步完了"，
            // 重绘一帧让按钮上的 n/N 动起来。
            if(wparam) HandleSyncAllDone();
            else InvalidateAll();
            return 0;
        }
        break;
    case kNativeFieldMessage:
        if(!strip) { HandleNativeFieldEvent(static_cast<int>(wparam),static_cast<int>(lparam));return 0; }
        break;
    case WM_MOUSEWHEEL:
        // v1.21：添加页的模型列表有溢出时先滚它（用户要求"列表要能滑动"）。
        if(!strip&&activePage_==2&&addSub_==0&&addChips_.size()>5) {
            const int maxFirst=static_cast<int>(addChips_.size())-5;
            const int step=GET_WHEEL_DELTA_WPARAM(wparam)>0?-1:1;
            addModelScroll_=std::clamp(addModelScroll_+step,0,maxFirst);
            InvalidateAll();
            return 0;
        }
        // 展开中的下拉列表先吃滚轮（供应商 / 来源列表比可视行数长）：滚列表，不滚页面。
        if(!strip&&((activePage_==1&&fbSelectOpen_)||(activePage_==2&&addSelectOpen_))) {
            const auto laid=LayoutForInput();
            const int maxFirst=std::max(0,laid.selectTotal-laid.selectVisible);
            if(maxFirst>0) {
                const int step=GET_WHEEL_DELTA_WPARAM(wparam)>0?-1:1;
                selectScroll_=std::clamp(selectScroll_+step,0,maxFirst);
                InvalidateAll();
                return 0;
            }
        }
        if(!strip&&pending_!=Pending::None) {
            // 模态打开时滚轮 = 正文横向滚动（长事实行）。
            const auto laid=LayoutForInput();
            if(laid.modalScrollVisible) {
                const float step=(laid.modalBody.right-laid.modalBody.left)*0.6f
                    *static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam))/120.0f;
                modalScrollX_=std::clamp(modalScrollX_-step,0.0f,laid.modalMaxScroll);
                InvalidateAll();
                return 0;
            }
        }
        if(!strip&&activePage_==0) { ModelsPageWheel(GET_WHEEL_DELTA_WPARAM(wparam));return 0; }
        if(!strip&&activePage_==1) {
            // 回退链阶梯的内部滚动（条目多于视口时）。
            const auto laid=LayoutForInput();
            const float step=50.0f*3.0f*static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam))/120.0f;
            fallbackScroll_=std::clamp(fallbackScroll_-step,0.0f,laid.fbMaxScroll);
            InvalidateAll();
            return 0;
        }
        if(!strip&&(activePage_==2||activePage_==3||activePage_==4)) {
            // 正文比窗口高时整页滚动（旧 body.clamped .page overflow-y: auto）
            const auto laid=LayoutForInput();
            const float step=widgets::kRowHeight*3.0f*static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam))
                /120.0f;
            pageScroll_=std::clamp(pageScroll_-step,0.0f,laid.pageMaxScroll);
            InvalidateAll();
            return 0;
        }
        break;
    case WM_SIZE: surface.Resize();if(card) RenderHoverCard();else if(strip) RenderStrip();InvalidateRect(window,nullptr,FALSE);return 0;
    case WM_MOUSEACTIVATE: if(strip) return MA_NOACTIVATE;break;
    case WM_CLOSE: Hide();return 0;
    case WM_QUERYENDSESSION:
        // 安装器 RestartManager / 系统关机的关闭询问（DESIGN v1.6）。控制窗是 message-only
        // 窗口收不到这类广播，只有这里的顶层窗口被问到：保存设置并同意。若只同意而下面的
        // WM_ENDSESSION 不退出，RM 等待超时后会 TerminateProcess（真机 2026-10-19 的闪退）。
        if(window==panel_) PersistSettings(true);
        return TRUE;
    case WM_ENDSESSION:
        if(wparam&&window==panel_) {
            if(log_) log_->Info("app","session end requested, exiting cleanly");
            PostMessageW(control_,WM_CLOSE,0,0);   // → DestroyWindow → PostQuitMessage → 走正常退出与日志
        }
        return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
