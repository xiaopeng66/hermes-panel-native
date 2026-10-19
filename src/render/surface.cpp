#include "surface.h"
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace hermes {
namespace {
void Check(HRESULT result,const char* what="render") {
    if(FAILED(result)) throw std::runtime_error(std::string(what)+" failed with hresult 0x"
        +[result]{char buffer[16]{};std::snprintf(buffer,sizeof(buffer),"%08lX",static_cast<unsigned long>(result));return std::string(buffer);}());
}
}
void Surface::Resize() { target_.Reset();border_.Reset();borderOwner_=nullptr;renderer_.Invalidate(); }
Surface::~Surface() { ReleaseLayered(); }
void Surface::ReleaseLayered() {
    dcTarget_.Reset();
    if(layeredDc_) {
        if(layeredOldBitmap_) { SelectObject(layeredDc_,layeredOldBitmap_);layeredOldBitmap_=nullptr; }
        DeleteDC(layeredDc_);
        layeredDc_=nullptr;
    }
    if(layeredBitmap_) { DeleteObject(layeredBitmap_);layeredBitmap_=nullptr; }
    layeredWidth_=layeredHeight_=0;
}
bool Surface::EnsureLayered(UINT32 width,UINT32 height) {
    if(dcTarget_&&width==layeredWidth_&&height==layeredHeight_) return true;
    ReleaseLayered();
    HDC screen=GetDC(nullptr);
    layeredDc_=CreateCompatibleDC(screen);
    if(screen) ReleaseDC(nullptr,screen);
    if(!layeredDc_) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=static_cast<LONG>(width);
    info.bmiHeader.biHeight=-static_cast<LONG>(height);   // top-down: D2D 的行序
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;
    layeredBitmap_=CreateDIBSection(layeredDc_,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    if(!layeredBitmap_||!bits) { ReleaseLayered();return false; }
    layeredOldBitmap_=SelectObject(layeredDc_,layeredBitmap_);
    if(!dcTarget_) {
        const auto properties=D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        Check(factory_->CreateDCRenderTarget(&properties,dcTarget_.GetAddressOf()),"CreateDCRenderTarget");
    }
    RECT bounds{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    Check(dcTarget_->BindDC(layeredDc_,&bounds),"BindDC");
    layeredWidth_=width;layeredHeight_=height;
    return true;
}
void Surface::PaintStrip(HWND window,const PanelViewModel& view,const ThemePalette& palette) {
    if(!factory_) Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));
    if(!textFactory_) Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(textFactory_.GetAddressOf())));
    RECT client{};
    GetClientRect(window,&client);
    const UINT32 width=static_cast<UINT32>(std::max<LONG>(0,client.right));
    const UINT32 height=static_cast<UINT32>(std::max<LONG>(0,client.bottom));
    if(width==0||height==0) return;
    if(!EnsureLayered(width,height)) return;
    const auto dpi=GetDpiForWindow(window);
    const float scale=static_cast<float>(dpi)/96.0f;
    dcTarget_->BeginDraw();
    dcTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    dcTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dcTarget_->Clear(D2D1::ColorF(0,0.0f));          // 透明：形状外是真正的桌面
    renderer_.Draw(dcTarget_.Get(),textFactory_.Get(),view,
        D2D1::SizeF(static_cast<float>(width),static_cast<float>(height)),scale,palette,true,nullptr);
    const auto result=dcTarget_->EndDraw();
    if(result==D2DERR_RECREATE_TARGET) { ReleaseLayered();return; }
    Check(result);
    RECT windowRect{};
    GetWindowRect(window,&windowRect);
    POINT destination{windowRect.left,windowRect.top};
    POINT source{0,0};
    SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(window,nullptr,&destination,&size,layeredDc_,&source,0,&blend,ULW_ALPHA);
}
void Surface::PaintHoverCard(HWND window,const PanelViewModel& view,const ThemePalette& palette) {
    if(!factory_) Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));
    if(!textFactory_) Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(textFactory_.GetAddressOf())));
    RECT client{};
    GetClientRect(window,&client);
    const UINT32 width=static_cast<UINT32>(std::max<LONG>(0,client.right));
    const UINT32 height=static_cast<UINT32>(std::max<LONG>(0,client.bottom));
    if(width==0||height==0) return;
    if(!EnsureLayered(width,height)) return;
    const auto dpi=GetDpiForWindow(window);
    const float scale=static_cast<float>(dpi)/96.0f;
    dcTarget_->BeginDraw();
    dcTarget_->SetTransform(D2D1::Matrix3x2F::Identity());
    dcTarget_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dcTarget_->Clear(D2D1::ColorF(0,0.0f));
    renderer_.DrawHoverCard(dcTarget_.Get(),textFactory_.Get(),view.hoverCard,
        D2D1::SizeF(static_cast<float>(width),static_cast<float>(height)),scale,palette,
        view.motion.cardAlpha,view.motion.cardOffsetDip);
    const auto result=dcTarget_->EndDraw();
    if(result==D2DERR_RECREATE_TARGET) { ReleaseLayered();return; }
    Check(result);
    RECT windowRect{};
    GetWindowRect(window,&windowRect);
    POINT destination{windowRect.left,windowRect.top};
    POINT source{0,0};
    SIZE size{static_cast<LONG>(width),static_cast<LONG>(height)};
    BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    UpdateLayeredWindow(window,nullptr,&destination,&size,layeredDc_,&source,0,&blend,ULW_ALPHA);
}
void Surface::Paint(HWND window,bool strip,const PanelViewModel& view,const ThemePalette& palette,
    const TextField* field) {
    if(!factory_) Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory_.GetAddressOf()));
    if(!textFactory_) Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(textFactory_.GetAddressOf())));
    RECT client{};GetClientRect(window,&client);
    const auto dpi=GetDpiForWindow(window);
    const float scale=static_cast<float>(dpi)/96.0f;
    if(!target_) {
        Check(factory_->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(),96,96),D2D1::HwndRenderTargetProperties(window,D2D1::SizeU(
            static_cast<UINT32>(client.right),static_cast<UINT32>(client.bottom))),target_.GetAddressOf()));
    }
    target_->BeginDraw();
    // 窗口底色始终不透明：入场动画只淡"内容"（Draw 里内容是 alpha 层），不会闪出桌面/上一帧。
    if(!strip) target_->Clear(palette.ink);
    renderer_.Draw(target_.Get(),textFactory_.Get(),view,
        D2D1::SizeF(static_cast<float>(client.right),static_cast<float>(client.bottom)),scale,palette,strip,
        field);
    if(!strip) {
        // Window edge: DWM rounds the frame, this hairline keeps the popup readable on light and dark
        // desktops alike (DESIGN §9-4). It uses the design's line colour, the same one the card border uses.
        if(borderOwner_!=target_.Get()) { border_.Reset();borderOwner_=target_.Get(); }
        if(!border_) Check(target_->CreateSolidColorBrush(palette.line,border_.GetAddressOf()),"CreateBorderBrush");
        else border_->SetColor(palette.line);
        target_->DrawRectangle(D2D1::RectF(0.5f,0.5f,static_cast<float>(client.right)-0.5f,
            static_cast<float>(client.bottom)-0.5f),border_.Get(),1.0f);
    }
    auto result=target_->EndDraw();
    if(result==D2DERR_RECREATE_TARGET) { Resize(); }
    else if(result==D2DERR_WRONG_RESOURCE_DOMAIN) {
        // Stale target-bound resources: rebuild them for the current target and repaint once.
        Resize();
        InvalidateRect(window,nullptr,FALSE);
    } else Check(result);
}
}
