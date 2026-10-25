#pragma once
#include <windows.h>
#include <cstdint>

// 顶部忙碌条（独立子窗口，用户反馈"忙碌条还是很卡顿"）。
//
// 面板一帧 D2D 重绘实测 200-230ms（768x1326 / 125%），任何"每 16-40ms 重画整块面板"的方案都
// 只能降到 2.5fps。这里把不定进度条拆成自己的子窗口：每帧只填一块 900x6 的 32bpp 位图并
// BitBlt（<1ms），相位按时间算，面板本身在忙碌期间不再重绘。
namespace hermes {
void RegisterBusyBarClass(HINSTANCE instance);

class BusyBar {
public:
    void Create(HWND parent,int id);
    void Place(const RECT& rect,bool visible);
    void SetColours(COLORREF background,COLORREF accent);
    void Invalidate();
    HWND Handle() const { return window_; }
    unsigned long long Paints() const { return paints_; }
    double LastPaintMs() const { return lastPaintMs_; }
    bool Visible() const { return visible_; }

public:
    static LRESULT CALLBACK WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
private:
    void Paint(HDC dc,const RECT& client);

    HWND window_=nullptr;
    bool visible_=false;
    COLORREF background_=RGB(240,240,244);
    COLORREF accent_=RGB(64,120,255);
    unsigned long long phaseStart_=0;
    unsigned long long paints_=0;
    double lastPaintMs_=0.0;
};
}
