#pragma once
#include <windows.h>

// 同步章（刷新）图标的旋转指示器：独立的分层子窗口，GDI 算像素，单帧 <0.2ms。
//
// 为什么不做在主面板里：主面板是 D2D HwndRenderTarget，整窗重绘实测 10–170ms（视构建与
// 场景而定）。把"每 33ms 一次的角度变化"挂在主面板重绘上，`InvalidateRect` 会堆在队列里，
// WM_PAINT 实际只有 ~11fps 才处理一次 —— 每帧跳 49°，真机看到的就是"不转、只在抖"。
// 这里照搬忙碌条的做法：自己一块小位图，按时间算角度，与主面板重绘完全解耦。
//
// 为什么是**分层**窗口（UpdateLayeredWindow + 32bpp 预乘 alpha）：以前它是普通子窗口，
// 整块客户区填背景色，于是必须知道"自己盖住的那块底色"——模型页那块刚好是 surf 白，备份页
// 的按钮底色偏蓝，白方块立刻露馅（真机反馈"底色是白色，不是透明的"）。现在圆环以外逐像素
// 透明，叠在任何底色/主题上都对。
namespace hermes {
void RegisterSpinGlyphClass(HINSTANCE instance);

class SpinGlyph {
public:
    // parent 是面板窗口；rect 由 Place 给出（父客户区坐标）。
    void Create(HWND parent,int id);
    void Destroy();
    void Place(const RECT& rect,bool visible);
    // 只有一个线条色：背景靠逐像素 alpha 透出去（见类注释）。
    void SetColours(COLORREF stroke);
    // 起始时刻重置（每次刷新开始时调用，让图标从 0° 起转）。
    void Restart();
    void Invalidate();
    HWND Handle() const { return window_; }
    bool Visible() const { return visible_; }
    unsigned long long Paints() const { return paints_; }
    double LastPaintMs() const { return lastPaintMs_; }
    float Angle() const;

public:
    static LRESULT CALLBACK WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
private:
    void Render();

    HWND window_=nullptr;
    bool visible_=false;
    bool placedVisible_=false;
    COLORREF stroke_=RGB(140,140,150);
    unsigned long long startTick_=0;
    unsigned long long paints_=0;
    double lastPaintMs_=0.0;
    RECT placed_{};
};
}
