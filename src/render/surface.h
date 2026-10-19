#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include "app/panel_view.h"
#include "core/geometry.h"
#include "render/panel.h"
#include "render/textfield.h"
#include "render/theme.h"

namespace hermes {
class Surface {
public:
    ~Surface();
    void Paint(HWND window,bool strip,const PanelViewModel& view,const ThemePalette& palette,
        const TextField* field=nullptr);
    // 贴条是分层窗口（逐像素 alpha）：圆角/圆形边缘由 D2D 抗锯齿绘制，不再被 1-bit
    // 窗口区域切成锯齿。挂件内容画进 32bpp 预乘 DIB，再 UpdateLayeredWindow 呈现。
    void PaintStrip(HWND window,const PanelViewModel& view,const ThemePalette& palette);
    // 贴条悬浮信息卡：同一套分层窗口路径，画一张独立的小卡片。
    void PaintHoverCard(HWND window,const PanelViewModel& view,const ThemePalette& palette);
    void SetCaretOn(bool on) { renderer_.SetCaretOn(on); }
    void Resize();
private:
    bool EnsureLayered(UINT32 width,UINT32 height);
    void ReleaseLayered();
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<IDWriteFactory> textFactory_;
    Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> border_;
    ID2D1RenderTarget* borderOwner_=nullptr;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> dcTarget_;
    HDC layeredDc_=nullptr;
    HBITMAP layeredBitmap_=nullptr;
    HGDIOBJ layeredOldBitmap_=nullptr;
    UINT32 layeredWidth_=0,layeredHeight_=0;
    PanelRenderer renderer_;
};
}
