#include "surface.h"
#include <wrl/client.h>
#include <dwmapi.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace hermes {
namespace {
void Check(HRESULT result,const char* what="render") {
    if(FAILED(result)) throw std::runtime_error(std::string(what)+" failed with hresult 0x"
        +[result]{char buffer[16]{};std::snprintf(buffer,sizeof(buffer),"%08lX",static_cast<unsigned long>(result));return std::string(buffer);}());
}
// 可分离盒式模糊（滑动窗口，边缘钳制）。两轮横+纵 ≈ 高斯，30×136 的贴条上不到 0.1ms。
void BoxBlurPass(const std::vector<unsigned int>& in,std::vector<unsigned int>& out,
    int width,int height,int radius,bool horizontal) {
    const int length=horizontal?width:height;
    const int lines=horizontal?height:width;
    const int window=radius*2+1;
    for(int line=0;line<lines;++line) {
        long long red=0,green=0,blue=0;
        for(int offset=-radius;offset<=radius;++offset) {
            const int index=std::clamp(offset,0,length-1);
            const auto pixel=horizontal?in[static_cast<std::size_t>(line)*width+index]
                :in[static_cast<std::size_t>(index)*width+line];
            red+=pixel&0xFF;green+=(pixel>>8)&0xFF;blue+=(pixel>>16)&0xFF;
        }
        for(int index=0;index<length;++index) {
            const auto value=(0xFFu<<24)
                |(static_cast<unsigned int>(blue/window)<<16)
                |(static_cast<unsigned int>(green/window)<<8)
                |static_cast<unsigned int>(red/window);
            if(horizontal) out[static_cast<std::size_t>(line)*width+index]=value;
            else out[static_cast<std::size_t>(index)*width+line]=value;
            const int addIndex=std::clamp(index+radius+1,0,length-1);
            const int removeIndex=std::clamp(index-radius,0,length-1);
            const auto add=horizontal?in[static_cast<std::size_t>(line)*width+addIndex]
                :in[static_cast<std::size_t>(addIndex)*width+line];
            const auto remove=horizontal?in[static_cast<std::size_t>(line)*width+removeIndex]
                :in[static_cast<std::size_t>(removeIndex)*width+line];
            red+=(add&0xFF)-(remove&0xFF);
            green+=((add>>8)&0xFF)-((remove>>8)&0xFF);
            blue+=((add>>16)&0xFF)-((remove>>16)&0xFF);
        }
    }
}
void BoxBlur(std::vector<unsigned int>& pixels,int width,int height,int radius) {
    if(radius<=0) return;
    std::vector<unsigned int> temp(pixels.size());
    BoxBlurPass(pixels,temp,width,height,radius,true);
    BoxBlurPass(temp,pixels,width,height,radius,false);
    BoxBlurPass(pixels,temp,width,height,radius,true);
    BoxBlurPass(temp,pixels,width,height,radius,false);
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
void Surface::InvalidateBackdrop() {
    backdrop_.clear();
    backdropWidth_=backdropHeight_=0;
    ++backdropGeneration_;
}
unsigned Surface::BackdropMean() const {
    if(backdrop_.empty()) return 0;
    std::size_t red=0,green=0,blue=0;
    for(const auto pixel:backdrop_) {
        red+=(pixel>>16)&0xFF;green+=(pixel>>8)&0xFF;blue+=pixel&0xFF;
    }
    const auto count=static_cast<unsigned>(backdrop_.size());
    return (static_cast<unsigned>(red/count)<<16)
        |(static_cast<unsigned>(green/count)<<8)
        |static_cast<unsigned>(blue/count);
}
void Surface::EnsureBackdrop(HWND window) {
    if(!backdrop_.empty()) return;
    CaptureBackdrop(window);
}
bool Surface::CaptureBackdrop(HWND window) {
    if(!window) return false;
    RECT rect{};
    if(!GetWindowRect(window,&rect)) return false;
    const int width=rect.right-rect.left,height=rect.bottom-rect.top;
    if(width<=0||height<=0||width>4096||height>4096) return false;
    // 分层窗口自己会挡住桌面：抓之前短暂隐藏（DwmFlush 保证合成器已经把它撤下），
    // 抓完立刻恢复；只在显示/落位/换屏时发生，轮询重绘不重复抓。
    const bool visible=IsWindowVisible(window)!=0;
    if(visible) { ShowWindow(window,SW_HIDE);DwmFlush(); }
    bool captured=false;
    HDC screen=GetDC(nullptr);
    if(screen) {
        HDC memory=CreateCompatibleDC(screen);
        if(memory) {
            BITMAPINFO info{};
            info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=width;
            info.bmiHeader.biHeight=-height;   // top-down：与 D2D 位图行序一致
            info.bmiHeader.biPlanes=1;
            info.bmiHeader.biBitCount=32;
            info.bmiHeader.biCompression=BI_RGB;
            void* bits=nullptr;
            HBITMAP bitmap=CreateDIBSection(memory,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(bitmap&&bits) {
                HGDIOBJ old=SelectObject(memory,bitmap);
                if(BitBlt(memory,0,0,width,height,screen,rect.left,rect.top,SRCCOPY|CAPTUREBLT)) {
                    auto* source=static_cast<unsigned int*>(bits);
                    backdrop_.assign(source,source+static_cast<std::size_t>(width)*height);
                    for(auto& pixel:backdrop_) pixel|=0xFF000000u;   // GDI 不写 alpha
                    captured=true;
                }
                SelectObject(memory,old);
            }
            if(bitmap) DeleteObject(bitmap);
            DeleteDC(memory);
        }
        ReleaseDC(nullptr,screen);
    }
    // BitBlt 兜底：HDR/驱动组合下屏幕 DC 可能整块纯黑（本机实测）。慢得多（~5µs/点，
    // 60×272≈80ms），但只在落位/换屏时跑一次且结果进缓存，轮询路径不付这个钱。
    if(captured) {
        std::size_t mean=0;
        for(const auto pixel:backdrop_) mean+=(pixel&0xFF)+((pixel>>8)&0xFF)+((pixel>>16)&0xFF);
        const bool uniformBlack=mean/3/backdrop_.size()<8;
        if(uniformBlack) {
            HDC fallback=GetDC(nullptr);
            if(fallback) {
                for(int y=0;y<height;++y)
                    for(int x=0;x<width;++x) {
                        const auto color=GetPixel(fallback,rect.left+x,rect.top+y);
                        backdrop_[static_cast<std::size_t>(y)*width+x]=0xFF000000u
                            |(static_cast<unsigned int>(GetRValue(color))<<16)
                            |(static_cast<unsigned int>(GetGValue(color))<<8)
                            |static_cast<unsigned int>(GetBValue(color));
                    }
                ReleaseDC(nullptr,fallback);
            }
        }
    }
    if(captured&&!backdrop_.empty()) {
        BoxBlur(backdrop_,width,height,std::max(4,std::min(width,height)/4));
        backdropWidth_=width;backdropHeight_=height;
        ++backdropGeneration_;
    } else {
        backdrop_.clear();
    }
    if(visible) ShowWindow(window,SW_SHOWNOACTIVATE);
    return captured;
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
