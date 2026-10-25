#include "ui/busy_bar.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace hermes {
namespace {
// 相位周期与主面板一致（1.6s 走完一整条轨道）。
constexpr double kPeriodMs=1600.0;
}

void RegisterBusyBarClass(HINSTANCE instance) {
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc=BusyBar::WndProc;
    type.hInstance=instance;
    type.lpszClassName=L"HermesModelPanelNative.BusyBar";
    type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    type.style=CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&type);
}

void BusyBar::Create(HWND parent,int id) {
    if(window_) return;
    window_=CreateWindowExW(0,L"HermesModelPanelNative.BusyBar",L"",WS_CHILD,0,0,10,4,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    if(window_) {
        SetWindowLongPtrW(window_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window_,SW_HIDE);
    }
}

void BusyBar::SetColours(COLORREF background,COLORREF accent) {
    background_=background;
    accent_=accent;
    if(window_) InvalidateRect(window_,nullptr,FALSE);
}

void BusyBar::Place(const RECT& rect,bool visible) {
    if(!window_) return;
    if(!visible) {
        if(visible_) { ShowWindow(window_,SW_HIDE);visible_=false; }
        return;
    }
    const int width=std::max(1L,rect.right-rect.left);
    const int height=std::max(1L,rect.bottom-rect.top);
    RECT current{};
    GetWindowRect(window_,&current);
    POINT origin{rect.left,rect.top};
    ClientToScreen(GetParent(window_),&origin);
    const bool moved=current.left!=origin.x||current.top!=origin.y
        ||current.right-current.left!=width||current.bottom-current.top!=height;
    if(moved)
        SetWindowPos(window_,HWND_TOP,rect.left,rect.top,width,height,
            SWP_NOACTIVATE|SWP_NOOWNERZORDER);
    if(!visible_) {
        phaseStart_=GetTickCount64();
        ShowWindow(window_,SW_SHOWNOACTIVATE);
        visible_=true;
    }
}

void BusyBar::Invalidate() {
    if(window_&&visible_) InvalidateRect(window_,nullptr,FALSE);
}

void BusyBar::Paint(HDC dc,const RECT& client) {
    LARGE_INTEGER frequency{},started{},finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    const int width=std::max(1L,client.right-client.left);
    const int height=std::max(1L,client.bottom-client.top);
    // 32bpp top-down DIB：每帧只填这一块（约 900x6 像素），再 BitBlt。
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width)*height);
    // 逐**通道**混合。以前把 0x00RRGGBB 打包值整体乘 alpha 再除 255：G 通道乘出来会进位到
    // R 字节、B 进到 G，结果每一列都是彩色噪点——真机看到的就是"忙碌条是一堆点点点"。
    const int backgroundRed=GetRValue(background_),backgroundGreen=GetGValue(background_),
        backgroundBlue=GetBValue(background_);
    const int accentRed=GetRValue(accent_),accentGreen=GetGValue(accent_),
        accentBlue=GetBValue(accent_);
    // 32bpp BI_RGB DIB 的内存字节序是 B,G,R,unused（不是 R,G,B）：打包时蓝在低位。
    const auto pack=[](int red,int green,int blue) {
        return static_cast<std::uint32_t>(blue&0xFF)|(static_cast<std::uint32_t>(green&0xFF)<<8)
            |(static_cast<std::uint32_t>(red&0xFF)<<16);
    };
    // 256 档混合表：每帧算一次，列循环里只查表。
    std::uint32_t ramp[256];
    for(int alpha=0;alpha<256;++alpha) {
        const auto mix=[alpha](int accent,int background) {
            return (accent*alpha+background*(255-alpha))/255;
        };
        ramp[alpha]=pack(mix(accentRed,backgroundRed),mix(accentGreen,backgroundGreen),
            mix(accentBlue,backgroundBlue));
    }
    // 相位按时间算：定时器偶尔晚了也只是步进变大，不会"停住再跳"。
    const auto elapsed=static_cast<double>(GetTickCount64()-phaseStart_);
    const double sweep=std::fmod(elapsed/kPeriodMs,1.0);
    // 扫光带：占轨道 44%，前 22% 快速起亮、后段长尾淡出（不定进度条的常见观感）；
    // 两端都按位置淡出，越过右端由左端接续时接缝不可见。
    const double bandWidth=0.44;
    const auto smooth=[](double value) {
        value=std::clamp(value,0.0,1.0);
        return value*value*(3.0-2.0*value);
    };
    // 轨道底色：accent 的 7% 淡淡铺一层，让这条细线看起来是"进度轨"而不是一条随机色带。
    constexpr int kTrackAlpha=18;
    for(int x=0;x<width;++x) {
        const double position=static_cast<double>(x)/static_cast<double>(width);
        double delta=position-sweep;
        if(delta<0.0) delta+=1.0;
        double level=0.0;
        if(delta<bandWidth) {
            const double t=delta/bandWidth;
            level=smooth(t/0.22)*(1.0-smooth((t-0.22)/0.78));
        }
        const int alpha=std::max(kTrackAlpha,static_cast<int>(std::lround(level*255.0)));
        const std::uint32_t colour=ramp[std::clamp(alpha,0,255)];
        for(int y=0;y<height;++y)
            pixels[static_cast<std::size_t>(y)*width+x]=colour;
    }
    StretchDIBits(dc,0,0,width,height,0,0,width,height,pixels.data(),&info,DIB_RGB_COLORS,SRCCOPY);
    QueryPerformanceCounter(&finished);
    ++paints_;
    if(frequency.QuadPart>0)
        lastPaintMs_=static_cast<double>(finished.QuadPart-started.QuadPart)*1000.0
            /static_cast<double>(frequency.QuadPart);
}

LRESULT CALLBACK BusyBar::WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* self=reinterpret_cast<BusyBar*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message) {
    case WM_ERASEBKGND: return 1;      // 自己整块填，不擦背景（少一次闪烁）
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc=BeginPaint(window,&paint);
        RECT client{};
        GetClientRect(window,&client);
        if(self) self->Paint(dc,client);
        EndPaint(window,&paint);
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
