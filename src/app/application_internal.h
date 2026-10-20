#pragma once
// application.cpp 拆分后的内部共享头：原匿名 namespace 的常量与辅助（每个 TU 私有副本，
// 内部链接，ODR 安全）。只在 src/app 内部使用。
#include "app/control_plane.h"   // HERMES_CONTROL_PLANE 宏（下面的 #if 求值前必须定义）
#include "core/geometry.h"
#include <json.hpp>
#include <string>
#include <windows.h>
#include <windowsx.h>
namespace hermes {
using Json=nlohmann::json;   // application.h 的别名在这里也生效（internal.h 单独被 include 时）
namespace {
constexpr UINT kTrayMessage=WM_APP+10;
constexpr UINT kDataMessage=WM_APP+13;
constexpr UINT kTrayId=1;
constexpr UINT_PTR kTrayGraceTimer=1;
constexpr UINT_PTR kCaretTimer=2;         // the 530ms caret blink - lives only while a field is focused
constexpr UINT_PTR kPollTimer=3;          // the refresh cadence (settings.pollSeconds)
// 可见性变化后的"立刻刷新一次"必须是**一次性**定时器：直接用 kPollTimer 设 30ms 会把它
// 永久改成 33Hz 轮询（真机 2026-10-20：任何一次展开/收起后 CPU 空转 25%，拖动卡顿）。
constexpr UINT_PTR kPollNowTimer=14;
constexpr UINT_PTR kToastTimer=4;         // one-shot: the toast clears itself after kToastMilliseconds
constexpr UINT_PTR kTooltipTimer=5;       // one-shot: 悬停 450ms 后显示提示（旧 title 的等价物）
constexpr UINT_PTR kSpinTimer=6;          // 周期：刷新进行中同步章图标的旋转动画
constexpr UINT_PTR kCheckTimer=7;         // one-shot: 刷新成功后对勾显示 800ms 再恢复刷新图标
constexpr UINT_PTR kLatencyTimer=8;       // 周期：延迟探测（settings.latencyProbe）
constexpr UINT_PTR kWidgetTimer=9;        // 周期：贴边挂件动画（声波/光环的呼吸）
constexpr UINT_PTR kStripCardTimer=11;    // one-shot：贴条悬浮信息卡的延迟（300ms 静止）
constexpr UINT kStripCardDelayMs=300;
// 动效（移植自 balance-panel-native）：统一 ease-out 进度 + 一个只在动画期间存在的 16ms 计时器。
constexpr UINT_PTR kMotionTimer=12;
constexpr unsigned kMotionTickMs=16;
constexpr unsigned kPanelMotionMs=140;    // 面板内容入场
constexpr unsigned kCardMotionMs=120;     // 悬浮卡入场
constexpr unsigned kCardExitMs=80;        // 悬浮卡退场淡出
constexpr unsigned kStripEnterMs=110;     // 收起瞬间贴条淡入
constexpr unsigned kCapsuleDockMs=280;    // 胶囊拖动椭圆 ↔ 贴边直角（smoothstep，两端更柔）
constexpr unsigned kTooltipMotionMs=120;  // 面板内提示气泡淡入
constexpr unsigned kHoverMotionMs=90;     // 控件悬停过渡
constexpr unsigned kThemeMotionMs=180;    // 主题交叉淡入
constexpr UINT kLatencyMessage=WM_APP+14; // 延迟探测结果回到 UI 线程（堆上的 vector）
constexpr UINT kProfileMessage=WM_APP+15; // 配置档案应用结果回到 UI 线程
constexpr UINT kProbeProgressMessage=WM_APP+16; // 逐条探测进度（每完成一条刷新一次按钮/行状态）
constexpr UINT kProbeRowMessage=WM_APP+17;      // 逐条探测的单行结果（堆上的 ProbeRow，测完一条就投）
constexpr UINT kTooltipDelayMs=450;
constexpr UINT kToastMilliseconds=2600;   // old .toast lifetime (app.js toast 2.6s)
constexpr unsigned long long kNotifySuppressionMs=10ull*60ull*1000ull;   // F6：同事件 10 分钟不重复
constexpr UINT kCaretMilliseconds=530;
constexpr UINT kGraceMilliseconds=400;
// The scroll position is kept as a float, so the wheel can move by less than a whole row.
constexpr int kFavoritesMax=200,kRecentsMax=12;   // main.js FAVORITES_MAX / RECENTS_MAX
Json RectJson(const Rect& rect) { return Json::array({rect.x,rect.y,rect.width,rect.height}); }
std::wstring LegacyDefaultDirectory() {
    // %APPDATA%\HermesModelPanel - the Electron panel's own directory (never written by us).
    wchar_t buffer[MAX_PATH*2]{};
    const auto size=GetEnvironmentVariableW(L"APPDATA",buffer,static_cast<DWORD>(std::size(buffer)));
    if(size==0||size>=std::size(buffer)) return {};
    return std::wstring(buffer)+L"\\HermesModelPanel";
}

// main.js providerLabel: the "custom:" prefix never reaches the user.
std::string ProviderLabelFor(const std::string& providerId) {
    return providerId.starts_with("custom:")?providerId.substr(7):providerId;
}
std::string Narrow(const std::wstring& value) {
    if(value.empty()) return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>(length),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length,nullptr,nullptr);
    return result;
}
POINT MessagePoint(HWND window,LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
    ClientToScreen(window,&point);
    return point;
}
std::wstring Widen(const std::string& value) {
    if(value.empty()) return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length);
    return result;
}
Point CenterOf(const Rect& rect) { return {rect.x+rect.width/2,rect.y+rect.height/2}; }
// 单行文本按字节截断，且不切断 UTF-8 码点（模态事实行 / 通知正文都用它）。
inline std::string TruncateUtf8(const std::string& text,std::size_t limit) {
    if(text.size()<=limit) return text;
    auto cut=limit;
    while(cut>0&&(static_cast<unsigned char>(text[cut])&0xC0)==0x80) --cut;
    return text.substr(0,cut)+"\xE2\x80\xA6";
}
// A helper job's result is posted back to the panel window as a heap pointer; the window message owns
// and deletes it. Only used by the H4 worker thread (PLAN §2.4: the UI thread never blocks on WSL).
LRESULT postMessage(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    return PostMessageW(window,message,wparam,lparam)?TRUE:FALSE;
}
#if HERMES_CONTROL_PLANE
// Test-only hook: the acceptance runs point the bridge at tools/fake_hermes.cpp through the child's
// environment. A release build has no control plane and never reads it.
std::wstring EnvironmentValue(const wchar_t* name) {
    wchar_t buffer[MAX_PATH*2]{};
    const auto size=GetEnvironmentVariableW(name,buffer,static_cast<DWORD>(std::size(buffer)));
    return size>0&&size<std::size(buffer)?std::wstring(buffer):std::wstring{};
}
#endif
}

namespace {
// 托盘图标栅格化：几何与 tools/make_icon.py / render/icons.cpp 的 activity 同源。
// 自己算覆盖度而不是用 GDI 画：GDI 在 32bpp DIB 上不写 alpha，图标会整体透明；而且自算的
// 圆头折线 + 柔光正好和 logo 的观感一致（每像素 4×4 子采样做抗锯齿）。
// 名字不叫 IconShape：render/icons.h 里已有同名类型（图标表条目），这里只是本文件的栅格化参数。
struct TrayMarkGeometry {
    double inset=0.04,radius=0.26,span=0.62,stroke=0.075,glow=0.09;
    static constexpr double grid[7][2]={{2,12},{4.5,12},{6.9,3.6},{9.5,20.4},{12,12},{14.6,12},{22,12}};
};
double DistanceToSegment(double px,double py,double ax,double ay,double bx,double by) {
    const double dx=bx-ax,dy=by-ay;
    const double lengthSquared=dx*dx+dy*dy;
    const double t=lengthSquared<=0.0?0.0:std::clamp(((px-ax)*dx+(py-ay)*dy)/lengthSquared,0.0,1.0);
    const double cx=ax+t*dx,cy=ay+t*dy;
    return std::hypot(px-cx,py-cy);
}
// 圆角方块内部判定（坐标已归一化到 0..1）。
bool InsideRoundedSquare(double x,double y,double inset,double radius) {
    const double low=inset,high=1.0-inset;
    if(x<low||x>high||y<low||y>high) return false;
    const double inner=radius;
    if(x>=low+inner&&x<=high-inner) return true;
    if(y>=low+inner&&y<=high-inner) return true;
    const double cx=x<low+inner?low+inner:high-inner;
    const double cy=y<low+inner?low+inner:high-inner;
    return std::hypot(x-cx,y-cy)<=inner;
}
}
}
