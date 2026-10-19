// 图标像素门禁（真机 bug 的回归网）：把每个图标单独渲染到一张位图上，断言"画出了可见像素"。
//
// 为什么需要：真机上出现过"鼠标悬浮到设置图标/供应商⋯上，图标就看不见了"。根因是 DrawIcon 里
// `brush->SetColor(colour)` 放在图形循环之后，而 Circle / RoundedRect 分支是"直接画"（吃当前笔刷色），
// 于是"纯圆构成"的图标（设置、⋯、搜索、钥匙、分支、归档、复制、眼睛 ×2）用了上一次残留的颜色。
// 这条门禁按"图标包围盒里必须有足够多、与背景明显不同的像素"判定，与形状无关 —— 九个受影响的
// 图标会一起被抓出来；它同时覆盖"以后有人再写一个纯圆的图标"。
#include "render/icons.h"
#include "render/theme.h"
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

namespace hermes {
using Microsoft::WRL::ComPtr;

// 每个图标画在一张 32x32 的位图中央；判定"和背景色的差异 ≥ 40（0..255 加权）的像素数 ≥ 24"。
// 参考：本 bug 发作时，纯圆图标在浅色主题下画出的可见像素是 0；形状正确时至少 60 像素。
constexpr int kTile = 32;
constexpr int kMinLitPixels = 24;
// 每个通道的差值 ≥ 24（0..255）算"看得见"：文字色与该主题 surf 的对比度远超这个门槛，
// 而"没画出来"（像素 == 背景）差值是 0。
constexpr int kMinChannelDelta = 24;

struct Sample { int lit=0; };

Sample SampleTile(const std::vector<std::uint32_t>& pixels,const D2D1_COLOR_F& background) {
    Sample sample;
    const auto red=static_cast<int>(background.r*255.0f+0.5f);
    const auto green=static_cast<int>(background.g*255.0f+0.5f);
    const auto blue=static_cast<int>(background.b*255.0f+0.5f);
    for(const auto pixel:pixels) {
        if((pixel>>24)<8) continue;                       // 透明像素不算
        const int delta=std::abs(static_cast<int>((pixel>>16)&0xFF)-red)
            +std::abs(static_cast<int>((pixel>>8)&0xFF)-green)
            +std::abs(static_cast<int>(pixel&0xFF)-blue);
        if(delta>=kMinChannelDelta*3) ++sample.lit;
    }
    return sample;
}
}   // namespace hermes

int wmain(int argc,wchar_t** argv) {
    using namespace hermes;
    (void)argc;   // 只做像素判定，不落文件（参数保留是为了将来要输出 PNG 时不用改签名）
    (void)argv;
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) { std::cerr<<"CoInitialize failed\n"; return 3; }
    ComPtr<ID2D1Factory> factory;
    if(FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()))) { std::cerr<<"D2D1CreateFactory failed\n"; return 3; }
    ComPtr<IWICImagingFactory> wic;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wic)))) { std::cerr<<"WIC factory failed\n"; return 3; }
    ComPtr<IDWriteFactory> text;
    if(FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(text.GetAddressOf())))) { std::cerr<<"DWrite factory failed\n"; return 3; }
    ComPtr<ID2D1StrokeStyle> style;
    {
        const auto properties=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,4.0f,D2D1_DASH_STYLE_SOLID,0.0f);
        factory->CreateStrokeStyle(properties,nullptr,0,style.GetAddressOf());
    }
    const auto dark=ResolveTheme("dark");
    const auto light=ResolveTheme("porcelain");
    int failures=0;
    for(const auto& shape:IconTable()) {
        for(const auto& variant:{std::pair<const char*,const ThemePalette*>("dark",&dark),
                                 std::pair<const char*,const ThemePalette*>("porcelain",&light)}) {
            // 背景 = 该主题的 surf（图标实际画在卡片/列表上）。
            const auto background=variant.second->surf;
            ComPtr<IWICBitmap> bitmap;
            if(FAILED(wic->CreateBitmap(kTile,kTile,GUID_WICPixelFormat32bppPBGRA,
                WICBitmapCacheOnDemand,bitmap.GetAddressOf()))) { std::cerr<<"WIC CreateBitmap failed\n"; return 3; }
            ComPtr<ID2D1RenderTarget> target;
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            if(FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(),properties,
                target.GetAddressOf()))) {
                std::cerr<<"CreateWicBitmapRenderTarget failed for "<<shape.name<<std::endl;
                return 3;
            }
            ComPtr<ID2D1SolidColorBrush> brush;
            target->CreateSolidColorBrush(background,brush.GetAddressOf());
            target->BeginDraw();
            // 先把笔刷设成**背景色**：这正是真机里"上一次用过的颜色"的等价物 —— 修复前，纯圆图标
            // 会继承这个颜色，于是画出来和背景一样（看不见）；修复后 DrawIcon 自己会 SetColor。
            brush->SetColor(background);
            target->FillRectangle(D2D1::RectF(0,0,kTile,kTile),brush.Get());
            DrawIcon(target.Get(),brush.Get(),style.Get(),
                D2D1::RectF(4,4,kTile-4,kTile-4),shape.id,variant.second->text,1.8f,1.0f);
            brush->SetColor(background);
            target->EndDraw();
            std::vector<std::uint32_t> pixels(static_cast<std::size_t>(kTile)*kTile);
            WICRect area{0,0,kTile,kTile};
            bitmap->CopyPixels(&area,kTile*4,kTile*kTile*4,
                reinterpret_cast<BYTE*>(pixels.data()));
            const auto sample=SampleTile(pixels,background);
            if(sample.lit<kMinLitPixels) {
                ++failures;
                std::cerr<<"FAIL icon "<<shape.name<<" ("<<variant.first<<") drew only "<<sample.lit
                    <<" visible pixels (need "<<kMinLitPixels<<") - invisible on screen"<<std::endl;
            } else {
                std::cout<<"PASS icon "<<shape.name<<" ("<<variant.first<<"): "<<sample.lit
                    <<" visible pixels"<<std::endl;
            }
        }
    }
    const int total=static_cast<int>(IconTable().size())*2;
    std::cout<<"icons: "<<(total-failures)<<"/"<<total<<" drawn in both themes\n";
    return failures==0?0:1;
}
