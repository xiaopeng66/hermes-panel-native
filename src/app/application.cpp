#include "application.h"
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
const std::vector<MenuEntry>& MenuEntries() {
    static const std::vector<MenuEntry> entries{
        {static_cast<unsigned>(MenuCommand::ToggleVisibility),"hide",L"显示/隐藏面板"},
        {static_cast<unsigned>(MenuCommand::Refresh),"refresh",L"刷新状态"},
        {static_cast<unsigned>(MenuCommand::OpenWebUI),"open-webui",L"打开 WebUI"},
        {static_cast<unsigned>(MenuCommand::RestartGateway),"restart-gateway",L"重启网关"},
        {static_cast<unsigned>(MenuCommand::CompactView),"compact",L"紧凑视图"},
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
NativeMonitor ReadMonitor(HMONITOR monitor) {
    MONITORINFOEXW info{};info.cbSize=sizeof(info);
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
    instance_=instance;control_=control;settings_=settings;log_=log;
    // 表单字段的形态（旧 textarea 是多行；API Key 掩码由绘制层按 masked 处理）。
    addModels_.multiline=true;
    addModels_.placeholder="deepseek-v4-flash";
    addNewModels_.multiline=true;
    addNewModels_.placeholder="deepseek-v4-flash";
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
        // 浮起的胶囊：收起态没有贴边，位置单独持久化（与浮动面板的 floating 区分开）。
        model_.stripFloating=stored.stripFloat;
        model_.stripFloatingPos=stored.stripFloatPos;
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
    panel_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST,L"HermesModelPanelNative.Panel",L"Hermes \u6A21\u578B\u9762\u677F",
        WS_POPUP,rect.x,rect.y,rect.width,rect.height,nullptr,nullptr,instance,&panelContext_);
    strip_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED,L"HermesModelPanelNative.Strip",L"Hermes \u6A21\u578B\u9762\u677F",
        WS_POPUP,rect.x,rect.y,ScaleDip(8,monitor_.geometry.dpi),rect.height,nullptr,nullptr,instance,&stripContext_);
    hoverCard_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,
        L"HermesModelPanelNative.HoverCard",L"Hermes",WS_POPUP,0,0,8,8,nullptr,nullptr,instance,
        &hoverCardContext_);
    if(!panel_||!strip_||!hoverCard_) return false;
    ShowWindow(hoverCard_,SW_HIDE);
    const DWM_WINDOW_CORNER_PREFERENCE corners=DWMWCP_ROUND;
    DwmSetWindowAttribute(panel_,DWMWA_WINDOW_CORNER_PREFERENCE,&corners,sizeof(corners));
    geometry_.Attach(panel_,strip_,log_);
    // The persisted presentation decides what the user sees at startup: a docked panel comes back as
    // the collapsed strip, exactly like the tray "show" path (DESIGN §5.2 row 1, §6.5 step 5).
    // 浮起胶囊（edge=None + stripFloat）启动后仍是收起态；只有浮动面板才是 Floating。
    state_=model_.edge==Edge::None
        ?(model_.stripFloating?State::Collapsed:State::Floating)
        :State::Collapsed;
    geometry_.Place(model_,monitor_.geometry,state_,"startup");
    geometry_.Visibility(state_);
    geometry_.Validate(model_,monitor_.geometry,state_);
    if(state_==State::Collapsed) RenderStrip();
    SyncTopmostStyle();
    MigrateLegacySettings();
    RegisterHotkeys();
    if(!AddTray()) return false;
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
    // 结构逐条对齐旧 main.js buildTrayMenu()（用户报「分类混乱」就是这里：旧版是四组 + 分隔线，
    // 组内顺序固定；勾选项用 checkbox/radio 的语义）。所有来自 std::string（UTF-8）的文案都必须
    // 过 Widen()：以前用 std::wstring(begin,end) 逐字节加宽，中文在托盘菜单里就是乱码。
    HMENU menu=CreatePopupMenu();
    if(!menu) return nullptr;
    const bool visible=state_!=State::Hidden;
    const bool docked=model_.edge!=Edge::None;
    // 枚举 → 菜单 ID（旧版的 id 是固定数字：1001 显示/隐藏 … 1099 退出）。
    const auto label=[](MenuCommand command) { return static_cast<UINT_PTR>(command); };
    // 组 1：显示/隐藏 + 当前模型（信息行）+ 切换到 ▸
    {
        const auto text=visible?L"\u663E\u793A / \u9690\u85CF":L"\u663E\u793A";
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
    // 组 3：贴边（旧版是一个切换项「贴边收回 / 滑出」）、置顶、主题、关闭按钮、开机自启
    AppendMenuW(menu,MF_STRING|(docked?MF_CHECKED:0),label(MenuCommand::Dock),
        docked?L"\u6ED1\u51FA":L"\u8D34\u8FB9\u6536\u56DE");
    AppendMenuW(menu,MF_STRING|(stored_.topmost?MF_CHECKED:0),label(MenuCommand::AlwaysOnTop),
        L"\u59CB\u7EC8\u7F6E\u9876");
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
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    // 组 4：外壳自己的三项（旧版没有：紧致视图是原生档位、刷新间隔属于设置页、数据目录是新壳的入口）。
    // 单独成组放在最后，免得和旧版那三组混在一起（这就是用户看到的「分类混乱」）。
    AppendMenuW(menu,MF_STRING|(model_.tier==Tier::Compact?MF_CHECKED:0),label(MenuCommand::CompactView),
        L"\u7D27\u81F4\u89C6\u56FE");
    {
        HMENU poll=CreatePopupMenu();
        const auto addPoll=[&](MenuCommand command,int seconds) {
            AppendMenuW(poll,MF_STRING|(stored_.pollSeconds==seconds?MF_CHECKED:0),label(command),
                (std::to_wstring(seconds)+L" \u79D2").c_str());
        };
        addPoll(MenuCommand::Poll30,30);
        addPoll(MenuCommand::Poll60,60);
        addPoll(MenuCommand::Poll120,120);
        addPoll(MenuCommand::Poll300,300);
        AppendMenuW(menu,MF_STRING|MF_POPUP,reinterpret_cast<UINT_PTR>(poll),L"\u5237\u65B0\u95F4\u9694");
    }
    AppendMenuW(menu,MF_STRING,label(MenuCommand::OpenDataDir),L"\u6253\u5F00\u6570\u636E\u76EE\u5F55");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    // 组 5：关于 + 退出
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
    case MenuCommand::ToggleVisibility: if(state_==State::Hidden) Show(); else Hide(); return;
    // 旧版托盘只有一个「贴边收回 / 滑出」切换项（toggleSnap），所以同一个 ID 既是收起也是滑出。
    case MenuCommand::Dock: if(model_.edge!=Edge::None) Undock(); else DockNearest(); return;
    case MenuCommand::Undock: Undock(); return;
    case MenuCommand::Refresh: RefreshData();InvalidateAll();return;
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
        const std::wstring body=
            L"Hermes \u6A21\u578B\u9762\u677F\uFF08\u539F\u751F\u7248\uFF09\n"
            L"\u5F53\u524D\u6A21\u578B\uFF1A"+Widen((provider.empty()?std::string("\xE2\x80\x94"):provider)+" / "+model)+L"\n"
            L"\u547C\u51FA\u70ED\u952E\uFF1A"+Widen(stored_.hotkey)+L"\n"
            L"\u6570\u636E\u76EE\u5F55\uFF1A"+directory+L"\n"
            L"WSL \u91CC\u7684 ~/.hermes/config.yaml \u4E0E .env \u53EA\u7531\u52A9\u624B\u811A\u672C\u5199\u5165\u3002";
        MessageBoxW(panel_,body.c_str(),L"\u5173\u4E8E Hermes \u6A21\u578B\u9762\u677F",MB_OK|MB_ICONINFORMATION);
        return;
    }
    case MenuCommand::CompactView: SetTier(model_.tier==Tier::Compact?Tier::Full:Tier::Compact);return;
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
void Application::SetTier(Tier tier) {
    if(model_.tier==tier) return;
    const auto previous=state_;
    model_.tier=tier;
    // Whole-tier switch is one absolute write per window; floating keeps its top-left corner and a
    // docked panel keeps its anchor (DESIGN §5.4), both of which ComputeRect already guarantees.
    // 完整档回到用户固定高度，紧凑档固定 152——切档不再触发第二次 content-fit 写入。
    geometry_.Place(model_,monitor_.geometry,state_,"tier");
    ++tierWrites_;
    ApplyState("tier",previous);
    PersistSettings(true);
    InvalidateAll();
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
void Application::ApplyState(const char* reason,State previous) {
    if(log_&&previous!=state_) log_->Info("state",std::string(reason)+" "+std::to_string(static_cast<int>(previous))
        +"->"+std::to_string(static_cast<int>(state_)));
    if(VisibleFor(previous)!=VisibleFor(state_)) geometry_.Visibility(state_);
    // 动效触发（移植自余额面板）：贴条展开 → 面板内容淡入+上滑；收起 → 贴条淡入。
    // 面板按纪律立即隐藏（不做延迟隐藏），所以收起方向只有贴条做动画。
    if(previous==State::Collapsed&&state_==State::Expanded) StartPanelMotion(true);
    if(previous!=State::Collapsed&&state_==State::Collapsed) StartStripMotion();
    if(previous==State::Hidden&&state_!=State::Hidden&&state_!=State::Collapsed) StartPanelMotion(true);
    // 外部窗口状态（Aero Snap / Win+↑ 最大化 / 系统调整）可能让实际矩形偏离模型；状态迁移时
    // 先把模型落到窗口，再校验。否则托盘/热键路径会抛 INV1（无 catch → 进程直接退出）。
    const auto expected=ComputeRect(model_,monitor_.geometry);
    if(GeometryController::Read(panel_)!=expected.panel
        ||(expected.strip&&GeometryController::Read(strip_)!=*expected.strip))
        geometry_.Place(model_,monitor_.geometry,state_,"reconcile");
    geometry_.Validate(model_,monitor_.geometry,state_);
    UpdateWidgetTimer();
    // DESIGN v1.7：可见性变化 → 轮询节奏跟着变（收起 60s / 隐藏停）；重新可见立即刷新一次，
    // 保证从贴条/托盘回到面板时看到的就是当前状态。刷新走 30ms 一次性定时器**延后触发**：
    // StartRefresh 会在 UI 线程 join 上一轮 worker（可能正卡在 wsl.exe 上百毫秒），在点击
    // 处理里同步调它会让「点贴条展开」体感卡顿甚至超时（a-pullout/a-edge 门禁抓到）。
    if(VisibleFor(previous)!=VisibleFor(state_)) {
        UpdatePollTimer();
        if(state_!=State::Hidden) SetTimer(panel_,kPollTimer,30,nullptr);
    }
    if(state_==State::Collapsed) {
        // 贴条重新出现：玻璃背景可能已经变了（换了窗口/壁纸/显示器），重抓一次。
        if(VisibleFor(previous)!=VisibleFor(state_)) stripContext_.surface.InvalidateBackdrop();
        RenderStrip();
    }
    HideHoverCard();   // 状态/几何变了：卡片内容和位置都作废，下次悬停重新计算
    (void)reason;
}
void Application::UpdateWidgetTimer() {
    if(!panel_) return;
    KillTimer(panel_,kWidgetTimer);
    // 只有「信标 + 有链路不正常」才呼吸：这是挂件唯一的动画，含义是"需要你处理"。
    // 全部正常时挂件完全静止（不再每 120ms 重绘，审计的 CPU 空转检查也依赖这条）。
    const bool fault=hermesState_.wsl=="down"||!hermesState_.webui.reachable
        ||!hermesState_.gateway.reachable;
    const bool animate=state_==State::Collapsed&&model_.widget==WidgetStyle::Beacon
        &&hermesState_.wsl!=""&&fault;
    if(animate) SetTimer(panel_,kWidgetTimer,120,nullptr);
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
    case WidgetStyle::Beacon: {
        // 三段独立灯段：三段矩形取并集（缝里是真正的桌面，不该吃鼠标）。
        const int gap=metrics.gap;
        const int block=(ch-gap*2)/3;
        for(int index=0;index<3;++index) {
            const int top=index*(block+gap);
            HRGN part=polygon({{0,top+1},{cw,top+1},{cw,top+block-1},{0,top+block-1}});
            if(!region) region=part;
            else { CombineRgn(region,region,part,RGN_OR);DeleteObject(part); }
        }
        break;
    }
    case WidgetStyle::Tag:
        region=polygon({{1,1},{cw-1,1},{cw-1,ch-1},{1,ch-1}});
        break;
    case WidgetStyle::Flag: {
        const int tail=metrics.tail;
        region=polygon({{1,1},{cw-1,1},{cw-1,ch-tail},{cw/2,ch-1},{1,ch-tail}});
        break;
    }
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
        }
        stripContext_.surface.PaintStrip(strip_,view,palette_);
    } catch(const std::exception& error) {
        if(log_) log_->Error("paint",std::string("strip: ")+error.what());
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
    // 浮起胶囊（面板隐藏）按贴条自己的位置选边，否则会拿上一次的面板位置去贴边。
    const bool fromStrip=state_==State::Collapsed&&model_.stripFloating;
    const auto rect=GeometryController::Read(fromStrip?strip_:panel_);
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
    model_.stripFloating=false;
    geometry_.Place(model_,monitor_.geometry,state_,"dock");
    ApplyState("dock",previous);
    PersistSettings();
}
void Application::Undock() {
    // 浮起胶囊（edge=None + stripFloating）也响应「解除贴边」：展开成浮动面板。
    // 以前 edge==None 时直接 return，浮起胶囊的托盘「解除贴边」就成了死按钮。
    if(model_.edge==Edge::None&&!model_.stripFloating) return;
    const auto previous=state_;
    model_.edge=Edge::None;
    model_.stripFloating=false;
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
    if(state_!=State::Collapsed) return;
    const auto previous=state_;
    if(model_.edge==Edge::None) {
        // 浮起胶囊：面板在胶囊旁边展开（夹进工作区）。收起后胶囊回到浮起位置。
        RECT window{};
        if(GetWindowRect(strip_,&window)) {
            monitor_=ReadMonitor(MonitorFromWindow(strip_,MONITOR_DEFAULTTONEAREST));
            const auto size=TierSize(model_,monitor_.geometry.dpi);
            const auto dpi=monitor_.geometry.dpi;
            model_.floating=ClampFloating(
                {window.left+ScaleDip(24,dpi),window.top-ScaleDip(24,dpi)},size,monitor_.geometry.work);
        }
    }
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
void Application::StartPanelMotion(bool enter) {
    if(!AnimationsEnabled()) { panelMotionStart_=0;return; }
    panelMotionStart_=GetTickCount64();
    panelMotionEnter_=enter;
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
void Application::ApplyDpiChange(const RECT& suggested) {
    // 拖拽/拉伸期间窗口几何归交互循环；落点由 CommitDrop / CommitPanelResize 统一结算。
    if(dragging_||resizing_) return;
    const auto resolved=MonitorFromRect(&suggested,MONITOR_DEFAULTTONEAREST);
    monitor_=ReadMonitor(resolved);
    if(model_.edge==Edge::None) model_.floating={suggested.left,suggested.top};
    const auto expected=ComputeRect(model_,monitor_.geometry);
    // Both windows are notified for one DPI change; skip when the model outcome is already applied.
    if(GeometryController::Read(panel_)==expected.panel&&
        (!expected.strip||GeometryController::Read(strip_)==*expected.strip)) return;
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
        geometry_.Place(model_,monitor_.geometry,state_,reason);
        ApplyState(reason,state_);
        return;
    }
    if(model_.edge==Edge::None) return;
    monitor_=ReadMonitor(resolved);
    geometry_.Place(model_,monitor_.geometry,state_,reason);
    ApplyState(reason,state_);
}
void Application::BeginSystemDrag() {
    LogEvent("begin-drag");
    if(dragging_ || state_==State::Hidden) return;
    dragging_=true;
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
    // 只有浮动完整档可拉高：贴边/收起/隐藏的几何属于停靠状态，紧凑档是固定 152 的小视图。
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
    RECT window{};
    if(!GetWindowRect(strip_,&window)) return;
    const int width=window.right-window.left,height=window.bottom-window.top;
    // 拖动限制在目标显示器的工作区内（余额面板同款）：不会把挂件拖到屏幕外。
    const auto target=ReadMonitor(MonitorFromPoint(screenPoint,MONITOR_DEFAULTTONEAREST));
    const auto& work=target.geometry.work;
    const int maxX=std::max(work.x,work.x+work.width-width);
    const int maxY=std::max(work.y,work.y+work.height-height);
    const int x=std::clamp<int>(static_cast<int>(screenPoint.x)-stripDragOffset_.x,work.x,maxX);
    const int y=std::clamp<int>(static_cast<int>(screenPoint.y)-stripDragOffset_.y,work.y,maxY);
    SetWindowPos(strip_,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
    // 拖动中的胶囊形态：离最近边越远越圆（0=完整椭圆），贴边时收直。每帧重绘，
    // 松手后由 kCapsuleDockMs 的过渡动画接上最终状态（贴边直角 / 浮起椭圆）。
    int nearest=std::min(x-work.x,work.right()-(x+width));
    nearest=std::min(nearest,std::min(y-work.y,work.bottom()-(y+height)));
    const int band=ScaleDip(96,target.geometry.dpi);
    capsuleDockOverride_=1.0f-std::clamp(static_cast<float>(nearest)
        /static_cast<float>(std::max(1,band)),0.0f,1.0f);
    RenderStrip();
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
    const auto& work=monitor_.geometry.work;
    const auto dpi=monitor_.geometry.dpi;
    // 离四边都超过 48 DIP → 真正浮起（完整椭圆，停在原地）；否则照旧吸附到最近边。
    // 以前拖到屏幕中央也会被吸回边上，所以用户永远看不到椭圆形态。
    int nearest=std::min(static_cast<int>(window.left)-work.x,
        work.right()-static_cast<int>(window.right));
    nearest=std::min(nearest,std::min(static_cast<int>(window.top)-work.y,
        work.bottom()-static_cast<int>(window.bottom)));
    const bool floatNow=nearest>ScaleDip(48,dpi);
    if(floatNow) {
        model_.edge=Edge::None;
        model_.stripFloating=true;
        model_.stripFloatingPos={window.left,window.top};
    } else {
        // 挂件沿边移动 → 展开后的面板以挂件中心为锚点；夹进工作区。
        const auto snap=StripSnapDecision({centreX,centreY},TierSize(model_,monitor_.geometry.dpi),
            monitor_.geometry);
        model_.edge=snap.edge;
        model_.anchor=snap.anchor;
        model_.stripFloating=false;
    }
    capsuleDockOverride_=-1.0f;
    // 胶囊落点动画：贴上边（直角）↔ 浮起（完整椭圆）——按最终是否贴边切换。
    if(stored_.widgetStyle=="capsule") {
        const bool nowDocked=!floatNow;
        if(nowDocked!=capsuleDocked_) {
            capsuleDocked_=nowDocked;
            if(AnimationsEnabled()) { capsuleDockStart_=GetTickCount64();KeepMotionTimer(); }
        }
    }
    const auto previous=state_;
    state_=State::Collapsed;
    geometry_.Place(model_,monitor_.geometry,state_,"strip-drop");
    stripContext_.surface.InvalidateBackdrop();   // 位置变了：玻璃背景必须重抓
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
    const int width=ScaleDip(252,dpi);
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
    // 数据变化可能让 note 出现/消失：渲染前重新按当前内容定位/定尺（高度随行数动态算）。
    PositionHoverCard();
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
    if(state_!=State::Hidden) {
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
    settings.stripFloat=model_.stripFloating;
    settings.stripFloatPos=model_.stripFloatingPos;
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
    return status==ERROR_SUCCESS&&type==REG_SZ;
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
        const auto panel=progress(panelMotionStart_,kPanelMotionMs);
        input.motion.panelAlpha=panel;
        input.motion.panelOffsetDip=panelMotionEnter_?(1.0f-panel)*8.0f:0.0f;
        // 退场淡出优先：卡片逻辑上已隐藏（cardVisible_=false），窗口还会多留 80ms 把 alpha 画到 0。
        if(cardExitStart_) input.motion.cardAlpha=1.0f-progress(cardExitStart_,kCardExitMs);
        else input.motion.cardAlpha=progress(cardMotionStart_,kCardMotionMs);
        input.motion.cardOffsetDip=(1.0f-progress(cardMotionStart_,kCardMotionMs))*4.0f;
        input.motion.stripAlpha=progress(stripMotionStart_,kStripEnterMs);
        // 胶囊贴边度：目标态向 0/1 过渡（1=贴边直角、0=浮起椭圆）。动画中逐帧插值。
        {
            const float target=capsuleDocked_?1.0f:0.0f;
            const auto started=capsuleDockStart_;
            input.motion.capsuleDocked=capsuleDockOverride_>=0.0f?capsuleDockOverride_
                :started==0?target
                :capsuleDocked_?progress(started,kCapsuleDockMs)
                :1.0f-progress(started,kCapsuleDockMs);
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
    view.syncSpinAngle=syncSpinAngle_;
    view.busySweep=busySweep_;
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
        // 按钮），原生版用已有的模态承载：正文给信息，确定 = 第一项主操作（探测模型并追加），
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
            view.modal.confirmLabel="\xE6\x8E\xA2\xE6\xB5\x8B\xE6\xA8\xA1\xE5\x9E\x8B\xE5\xB9\xB6"
                "\xE8\xBF\xBD\xE5\x8A\xA0";
        }
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
        page.provider=fbProvider_.TextUtf8();
        page.baseUrl=fbUrl_.TextUtf8();
        page.keyEnv=fbKeyEnv_.TextUtf8();
        page.key=fbKey_.TextUtf8();
        page.error=fbError_;
        page.busy=workerBusy_.load();
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
        page.keyEnv=addKeyEnv_.TextUtf8();
        page.url=addUrl_.TextUtf8();
        page.key=addKey_.TextUtf8();
        page.modelsText=addModels_.TextUtf8();
        page.modelsNew=addNewModels_.TextUtf8();
        page.dangerName=addDanger_.TextUtf8();
        page.keyRevealed=addKeyRevealed_;
        page.probeBefore=addProbeBefore_;
        page.setMainAfter=addSetMainAfter_;
        page.purgeEnv=addPurgeEnv_;
        page.dropFallbacks=addDropFallbacks_;
        page.provider=addProvider_;
        page.providerScroll=selectScroll_;
        page.selectOpen=addSelectOpen_;
        page.selectField=addSelectField_;
        page.error=addError_;
        page.info=addInfo_;
        page.chips=addChips_;
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
    view.widgetEdge=static_cast<int>(model_.edge);
    // 浮起胶囊（edge=None）没有旋转，窗口本身就是竖条；横向边的贴条才会转置。
    view.widgetVertical=model_.edge==Edge::Left||model_.edge==Edge::Right
        ||model_.edge==Edge::None;
    view.widgetPhase=widgetPhase_;
    view.focusedFormField=FormFieldIndexOf(focusedField_);
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
        // The scroll offset is clamped against the real viewport, so the wheel/drag and the renderer
        // always agree on where the list is (the clamp needs the layout, hence the client rect here).
        RECT client{};
        if(panel_&&GetClientRect(panel_,&client)&&client.bottom>client.top) {
            const auto laid=ComputeLayout(view,static_cast<float>(client.right),
                static_cast<float>(client.bottom),static_cast<float>(monitor_.geometry.dpi)/96.0f);
            view.listScroll=laid.listScroll;
            if(laid.listScroll!=listScroll_) listScroll_=laid.listScroll;
        }
    }
    // 悬停气泡的文案查找放在最后：页面级提示（如回退链每行的探测说明）是在各页 attach 时
    // 才进 tooltips 表的，早于它查会漏掉这些条目。
    if(tooltipVisible_&&tooltipHover_!=0) {
        for(const auto& tip:view.tooltips)
            if(tip.control==tooltipHover_&&(tip.index<0||tip.index==tooltipHoverIndex_)) {
                view.tooltip.visible=true;view.tooltip.text=tip.text;break;
            }
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
    fbError_.clear();
    if(focusedField_==&fbModel_||focusedField_==&fbProvider_||focusedField_==&fbUrl_
        ||focusedField_==&fbKeyEnv_||focusedField_==&fbKey_) BlurField();
    InvalidateAll();
}
// ---- 模型页的输入：一次命中测试同时服务点击与悬停（列表行/星标/组头/过滤段/滚动条）----
// ---- H2c the self-drawn text fields: one focused field at a time; keyboard, IME and clipboard all
// enter through here. The models-page search box is the first host (DESIGN #10/#36).
void Application::InvalidateAll() {
    // 窗口高度是用户固定的，不再按页面内容 fit：切页只重排正文，底栏与窗口尺寸不动。
    if(panel_) InvalidateRect(panel_,nullptr,FALSE);
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
        // Explorer 重启后旧图标句柄失效：先删一次（不存在也无妨），再重新注册并刷新提示。
        ++trayReadds_;
        NOTIFYICONDATAW stale{};
        stale.cbSize=sizeof(stale);
        stale.hWnd=control_;
        stale.uID=kTrayId;
        Shell_NotifyIconW(NIM_DELETE,&stale);
        trayRegistered_=false;
        if(AddTray()) UpdateTrayStatus();
        return 0;
    }
    auto& surface=card?hoverCardContext_.surface:(strip?stripContext_.surface:panelContext_.surface);
    switch(message) {
    case WM_PAINT: {
        PAINTSTRUCT paint{};BeginPaint(window,&paint);
        // The search field is handed to the renderer even when it is not focused: an unfocused box must
        // still show its own text (the TextField's `focused` flag drives the caret and the glow).
        try {
            // 悬停气泡挂在控件下方：锚点由 LayoutForInput 从布局里取（它同时把 tooltipView_ 填好）。
            // 先跑一次布局，再把带锚点的气泡并进这一帧的视图 —— 直接读上一步缓存的 tooltipView_
            // 会拿到"气泡还没显示时"的空锚点（真机上就是 dump 说可见、画面上却没有）。
            auto view=CurrentView();
            if(card) {
                surface.PaintHoverCard(window,view,palette_);
            } else if(strip) {
                // 贴条走分层窗口：形状由 D2D 抗锯齿绘制；没有提示气泡，也不需要面板输入布局。
                surface.PaintStrip(window,view,palette_);
            } else {
                LayoutForInput();
                view.tooltip=tooltipView_;
                surface.Paint(window,false,std::move(view),palette_,&search_);
            }
        } catch(...) { EndPaint(window,&paint);throw; }
        EndPaint(window,&paint);return 0;
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
        TrackHover(window,static_cast<float>(GET_X_LPARAM(lparam)),static_cast<float>(GET_Y_LPARAM(lparam)));
        return 0;
    case WM_LBUTTONUP:
        if(strip) {
            // 拖动过 → 吸附到最近边缘（保持贴条，不展开）；没拖动 → 这一次就是单击展开。
            if(stripDragging_) { CommitStripDrop();return 0; }
            if(stripPressed_) {
                stripPressed_=false;
                if(GetCapture()==strip_) ReleaseCapture();
                LogEvent("strip-click");
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
            const auto scale=static_cast<float>(monitor_.geometry.dpi)/96.0f;
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
            if(layout.pageScrollVisible) {
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
        if(!strip) {
            PointerLeftPanel({GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)});
            pointerInside_=false;
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
        if(!strip&&HandleFieldKey(message,wparam,lparam)) return 0;
        break;
    case WM_IME_COMPOSITION:
    case WM_IME_ENDCOMPOSITION:
    case WM_IME_REQUEST:
        if(!strip) { HandleFieldIme(window,message,wparam,lparam);return 0; }
        break;
    case WM_TIMER:
        if(wparam==kMotionTimer&&!strip) { MotionTick();return 0; }
        if(strip&&wparam==kStripCardTimer) { ShowHoverCard();return 0; }
        if(wparam==kTrayGraceTimer&&!strip) { GraceExpired();return 0; }
        if(wparam==kCaretTimer&&!strip) { caretOn_=!caretOn_;panelContext_.surface.SetCaretOn(caretOn_);InvalidateRect(panel_,nullptr,FALSE);return 0; }
        if(wparam==kPollTimer&&!strip) { StartRefresh();return 0; }
        if(wparam==kWidgetTimer&&!strip) {
            // 声波/光环的呼吸：只重绘收起态的挂件窗口，不碰面板。
            widgetPhase_+=0.22f;
            if(strip_) RenderStrip();
            return 0;
        }
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
            // 刷新进行中：同步章图标绕中心旋转（真机反馈：不要「读取 WSL 配置…」的文字）；
            // 顶部忙碌条同步流动（busySweep 独立相位，快慢一致 50ms/拍）。
            busySweep_=std::fmod(busySweep_+12.0f,720.0f);
            if(workerBusy_.load()) { syncSpinAngle_+=30.0f;InvalidateRect(panel_,nullptr,FALSE); }
            else {
                // 刷新刚结束：对勾显示 800ms 再恢复静态刷新图标。
                KillTimer(panel_,kSpinTimer);
                syncCheckShown_=true;
                InvalidateRect(panel_,nullptr,FALSE);
                SetTimer(panel_,kCheckTimer,800,nullptr);
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
    case kProbeRowMessage:
        if(!strip&&lparam) { HandleProbeRow(reinterpret_cast<ProbeRow*>(lparam));return 0; }
        break;
    case WM_MOUSEWHEEL:
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
