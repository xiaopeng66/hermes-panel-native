#include "ui/spin_glyph.h"
#include <algorithm>
#include <cmath>

namespace hermes {
namespace {
// 一圈 1200ms（0.83 圈/秒）——比之前"按帧跳"的观感稳，也不会快到看不清。
constexpr double kTurnMs=1200.0;
constexpr double kPi=3.14159265358979323846;
}

void RegisterSpinGlyphClass(HINSTANCE instance) {
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc=SpinGlyph::WndProc;
    type.hInstance=instance;
    type.lpszClassName=L"HermesModelPanelNative.SpinGlyph";
    type.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    type.style=CS_HREDRAW|CS_VREDRAW;
    RegisterClassExW(&type);
}

void SpinGlyph::Create(HWND parent,int id) {
    if(window_) return;
    // WS_CLIPSIBLINGS：与原生 EDIT 同级时不互相刷掉。
    window_=CreateWindowExW(0,L"HermesModelPanelNative.SpinGlyph",L"",
        WS_CHILD|WS_CLIPSIBLINGS,0,0,16,16,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    if(window_) {
        SetWindowLongPtrW(window_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window_,SW_HIDE);
    }
}

void SpinGlyph::Destroy() {
    if(window_) { DestroyWindow(window_);window_=nullptr; }
}

void SpinGlyph::SetColours(COLORREF stroke,COLORREF background) {
    stroke_=stroke;
    background_=background;
    if(window_) InvalidateRect(window_,nullptr,FALSE);
}

void SpinGlyph::Restart() {
    startTick_=GetTickCount64();
    if(window_) InvalidateRect(window_,nullptr,FALSE);
}

void SpinGlyph::Invalidate() {
    if(window_&&visible_) InvalidateRect(window_,nullptr,FALSE);
}

float SpinGlyph::Angle() const {
    if(!startTick_) return 0.0f;
    const auto elapsed=static_cast<double>(GetTickCount64()-startTick_);
    return static_cast<float>(std::fmod(elapsed/kTurnMs,1.0)*360.0);
}

void SpinGlyph::Place(const RECT& rect,bool visible) {
    if(!window_) return;
    if(!visible) {
        if(placedVisible_) { ShowWindow(window_,SW_HIDE);placedVisible_=false;visible_=false; }
        return;
    }
    const bool moved=!placedVisible_||rect.left!=placed_.left||rect.top!=placed_.top
        ||rect.right!=placed_.right||rect.bottom!=placed_.bottom;
    if(moved) {
        SetWindowPos(window_,HWND_TOP,rect.left,rect.top,
            std::max<LONG>(1,rect.right-rect.left),std::max<LONG>(1,rect.bottom-rect.top),
            SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        placed_=rect;
    }
    if(!placedVisible_) {
        if(!startTick_) startTick_=GetTickCount64();
        ShowWindow(window_,SW_SHOWNOACTIVATE);
        placedVisible_=true;
        visible_=true;
        InvalidateRect(window_,nullptr,FALSE);
    }
}

void SpinGlyph::Paint(HDC dc,const RECT& client) {
    LARGE_INTEGER frequency{},started{},finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    const int width=std::max<LONG>(1,client.right-client.left);
    const int height=std::max<LONG>(1,client.bottom-client.top);
    // 双缓冲：先画到内存 DC，再一次性 BitBlt（避免逐笔闪烁）。
    HDC memory=CreateCompatibleDC(dc);
    HBITMAP bitmap=CreateCompatibleBitmap(dc,width,height);
    const auto previous=memory?SelectObject(memory,bitmap):nullptr;
    if(memory&&bitmap) {
        RECT all{0,0,width,height};
        HBRUSH background=CreateSolidBrush(background_);
        FillRect(memory,&all,background);
        DeleteObject(background);
        // 画一个"缺口圆环"：360° 的环里留 ~70° 缺口，旋转起来就能看出方向。
        const float radius=std::min(width,height)*0.5f-1.5f;
        const float centreX=width*0.5f,centreY=height*0.5f;
        const auto radians=Angle()*static_cast<float>(kPi/180.0);
        HPEN pen=CreatePen(PS_SOLID,std::max(1,static_cast<int>(std::lround(width/8.0))),stroke_);
        const auto oldPen=SelectObject(memory,pen);
        SetBkMode(memory,TRANSPARENT);
        const int steps=24;
        const float sweep=2.0f*static_cast<float>(kPi)*0.80f;   // 80% 的弧
        POINT points[steps+1];
        for(int index=0;index<=steps;++index) {
            const float t=static_cast<float>(index)/static_cast<float>(steps);
            const float angle=radians+sweep*t;
            points[index]={static_cast<LONG>(std::lround(centreX+std::cos(angle)*radius)),
                static_cast<LONG>(std::lround(centreY+std::sin(angle)*radius))};
        }
        Polyline(memory,points,steps+1);
        SelectObject(memory,oldPen);
        DeleteObject(pen);
        BitBlt(dc,0,0,width,height,memory,0,0,SRCCOPY);
        SelectObject(memory,previous);
    }
    if(bitmap) DeleteObject(bitmap);
    if(memory) DeleteDC(memory);
    QueryPerformanceCounter(&finished);
    ++paints_;
    if(frequency.QuadPart>0)
        lastPaintMs_=static_cast<double>(finished.QuadPart-started.QuadPart)*1000.0
            /static_cast<double>(frequency.QuadPart);
}

LRESULT CALLBACK SpinGlyph::WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* self=reinterpret_cast<SpinGlyph*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message) {
    case WM_ERASEBKGND: return 1;      // 自己整块填
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
