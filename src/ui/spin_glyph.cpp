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
    // WS_EX_LAYERED：配 UpdateLayeredWindow 做逐像素透明（圆环以外全透）。
    window_=CreateWindowExW(WS_EX_LAYERED|WS_EX_NOACTIVATE,L"HermesModelPanelNative.SpinGlyph",
        L"",WS_CHILD|WS_CLIPSIBLINGS,0,0,16,16,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    if(window_) {
        SetWindowLongPtrW(window_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        ShowWindow(window_,SW_HIDE);
    }
}

void SpinGlyph::Destroy() {
    if(window_) { DestroyWindow(window_);window_=nullptr; }
}

void SpinGlyph::SetColours(COLORREF stroke) {
    stroke_=stroke;
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

void SpinGlyph::Render() {
    if(!window_) return;
    RECT client{};
    if(!GetClientRect(window_,&client)) return;
    const int width=std::max<LONG>(1,client.right-client.left);
    const int height=std::max<LONG>(1,client.bottom-client.top);
    LARGE_INTEGER frequency{},started{},finished{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&started);
    // 32bpp 预乘 ARGB（内存序 B,G,R,A，top-down）。圆环以外 alpha=0 —— 于是它叠在任何底色上
    // 都是"透明的环"：以前整块填 surf 色，在备份页那种浅蓝按钮底上就是一个白方块（真机反馈）。
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width;
    info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    HDC screen=CreateCompatibleDC(nullptr);
    HBITMAP dib=screen?CreateDIBSection(screen,&info,DIB_RGB_COLORS,&bits,nullptr,0):nullptr;
    if(screen&&dib&&bits) {
        const float radius=std::min(width,height)*0.5f-1.5f;
        const float thickness=std::max(1.0f,width/8.0f);
        const float centreX=width*0.5f,centreY=height*0.5f;
        const float radians=Angle()*static_cast<float>(kPi/180.0);
        const float sweep=2.0f*static_cast<float>(kPi)*0.80f;    // 80% 的弧，留 ~72° 缺口
        const float twoPi=2.0f*static_cast<float>(kPi);
        const float red=static_cast<float>(GetRValue(stroke_));
        const float green=static_cast<float>(GetGValue(stroke_));
        const float blue=static_cast<float>(GetBValue(stroke_));
        auto* pixels=static_cast<std::uint32_t*>(bits);
        for(int y=0;y<height;++y) {
            const float offsetY=static_cast<float>(y)+0.5f-centreY;
            for(int x=0;x<width;++x) {
                const float offsetX=static_cast<float>(x)+0.5f-centreX;
                const float distance=std::sqrt(offsetX*offsetX+offsetY*offsetY);
                // 环带覆盖度：离环中线半厚度以内全实，再给 1px 过渡做抗锯齿。
                float cover=thickness*0.5f+0.5f-std::fabs(distance-radius);
                cover=std::clamp(cover,0.0f,1.0f);
                if(cover>0.0f) {
                    float angle=std::atan2(offsetY,offsetX)-radians;
                    while(angle<0.0f) angle+=twoPi;
                    while(angle>=twoPi) angle-=twoPi;
                    if(angle>sweep) cover=0.0f;      // 缺口
                }
                const auto alpha=static_cast<std::uint32_t>(std::lround(cover*255.0f));
                const auto premultiplied=[alpha](float channel) {
                    return static_cast<std::uint32_t>(
                        std::lround(channel*static_cast<float>(alpha)/255.0f));
                };
                pixels[y*width+x]=(alpha<<24)|(premultiplied(red)<<16)
                    |(premultiplied(green)<<8)|premultiplied(blue);
            }
        }
        HDC memory=CreateCompatibleDC(screen);
        const auto previous=memory?SelectObject(memory,dib):nullptr;
        BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
        POINT source{0,0};
        SIZE size{width,height};
        // pptDst 传 nullptr：位置/大小由 Place 的 SetWindowPos 决定，这里只换像素。
        UpdateLayeredWindow(window_,screen,nullptr,&size,memory,&source,0,&blend,ULW_ALPHA);
        if(memory) { SelectObject(memory,previous);DeleteDC(memory); }
    }
    if(dib) DeleteObject(dib);
    if(screen) DeleteDC(screen);
    QueryPerformanceCounter(&finished);
    ++paints_;
    if(frequency.QuadPart>0)
        lastPaintMs_=static_cast<double>(finished.QuadPart-started.QuadPart)*1000.0
            /static_cast<double>(frequency.QuadPart);
}

LRESULT CALLBACK SpinGlyph::WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* self=reinterpret_cast<SpinGlyph*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message) {
    case WM_PAINT: {
        // 内容由 UpdateLayeredWindow 提供；WM_PAINT 只是"该换一帧"的信号。
        PAINTSTRUCT paint{};
        BeginPaint(window,&paint);
        EndPaint(window,&paint);
        if(self) self->Render();
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
