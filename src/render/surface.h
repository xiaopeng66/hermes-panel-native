#pragma once
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <vector>
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
    // 液态玻璃：抓取贴条窗口背后的桌面像素并做盒式模糊。窗口在抓取期间短暂隐藏，
    // 所以只在显示/落位/换显示器时调用；轮询重绘直接复用缓存。
    void EnsureBackdrop(HWND window);
    void InvalidateBackdrop();
    const std::vector<unsigned int>& BackdropPixels() const { return backdrop_; }
    int BackdropWidth() const { return backdropWidth_; }
    int BackdropHeight() const { return backdropHeight_; }
    unsigned BackdropGeneration() const { return backdropGeneration_; }
    // 门禁诊断：缓存快照的平均色（0x00RRGGBB）。黑玻璃/白玻璃对照实验用。
    unsigned BackdropMean() const;
    // 诊断：模糊后的快照还剩多少高频（相邻像素平均差，0..255）。玻璃"像噪点"时用它判断
    // 是快照没糊开还是绘制阶段出了问题。
    float BackdropNoise() const;
    // 诊断：模糊之前的原始噪声（同上）。两者一起看能区分"源数据是垃圾"与"模糊没生效"。
    float BackdropNoiseBefore() const { return backdropNoiseBefore_; }
private:
    bool EnsureLayered(UINT32 width,UINT32 height);
    bool CaptureBackdrop(HWND window);
    std::vector<unsigned int> backdrop_;
    int backdropWidth_=0,backdropHeight_=0;
    float backdropNoiseBefore_=0.0f;
    unsigned backdropGeneration_=0;
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
