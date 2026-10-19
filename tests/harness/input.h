#pragma once
#include <windows.h>
#include <cmath>
#include <stdexcept>

namespace harness {
// Acceptance-grade pointer driver: synthesizes real mouse input through SendInput so the
// system generates the genuine WM_MOUSEMOVE / WM_MOUSELEAVE / drag chain. Coordinates are
// physical pixels of the virtual desktop, normalised to SendInput's 0..65535 space here only.
class Pointer {
public:
    Pointer() {
        cursor_=Position();
        metrics_={GetSystemMetrics(SM_XVIRTUALSCREEN),GetSystemMetrics(SM_YVIRTUALSCREEN),
            GetSystemMetrics(SM_CXVIRTUALSCREEN),GetSystemMetrics(SM_CYVIRTUALSCREEN)};
    }
    ~Pointer() {
        // Always release the button: a test that aborts mid-drag would otherwise leave it pressed and
        // drag whatever window the next test creates into a modal move loop.
        try { Button(false); } catch(...) {}
        try { MoveTo(cursor_.x,cursor_.y); } catch(...) {}
    }
    void Release() { Button(false); }
    static POINT Position() { POINT point{};if(!GetCursorPos(&point)) throw std::runtime_error("GetCursorPos failed");return point; }
    void MoveTo(int x,int y) {
        // The machine may be in use: a human moving the mouse right after our SendInput would leave the
        // cursor somewhere else and the caller would blame the application. Verify the landing point and
        // retry a few times; only then is it an error, and the message says whose problem it is.
        INPUT input{};
        input.type=INPUT_MOUSE;
        input.mi.dwFlags=MOUSEEVENTF_MOVE|MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK;
        input.mi.dx=Normalise(x-metrics_.x,metrics_.width);
        input.mi.dy=Normalise(y-metrics_.y,metrics_.height);
        for(int attempt=0;attempt<4;++attempt) {
            if(SendInput(1,&input,sizeof(input))!=1) throw std::runtime_error("SendInput move failed");
            const auto deadline=GetTickCount64()+120;
            while(GetTickCount64()<deadline) {
                const auto position=Position();
                if(std::abs(position.x-x)<=1&&std::abs(position.y-y)<=1) return;
                Sleep(10);
            }
        }
        const auto position=Position();
        throw std::runtime_error("cursor interference: requested ("+std::to_string(x)+","+std::to_string(y)
            +") but the pointer sits at ("+std::to_string(position.x)+","+std::to_string(position.y)
            +"); the desktop is in use, re-run when idle");
    }
    void MoveTowards(int x,int y,int steps=12) {
        const auto start=Position();
        for(int step=1;step<=steps;++step) {
            const auto t=static_cast<double>(step)/steps;
            MoveTo(static_cast<int>(start.x+(x-start.x)*t),static_cast<int>(start.y+(y-start.y)*t));
            Sleep(8);
        }
    }
    // Two-modifier chord (for example Ctrl+Alt+B) as real keyboard input, so the system delivers
    // WM_HOTKEY to whichever process registered that hotkey.
    void Hotkey(WORD first,WORD second,WORD key) {
        INPUT inputs[6]{};
        const WORD keys[3]{first,second,key};
        for(int i=0;i<3;++i) { inputs[i].type=INPUT_KEYBOARD;inputs[i].ki.wVk=keys[i]; }
        for(int i=0;i<3;++i) {
            inputs[3+i].type=INPUT_KEYBOARD;inputs[3+i].ki.wVk=keys[2-i];inputs[3+i].ki.dwFlags=KEYEVENTF_KEYUP;
        }
        if(SendInput(6,inputs,sizeof(INPUT))!=6) throw std::runtime_error("SendInput hotkey failed");
    }
    void Button(bool down) {
        INPUT input{};
        input.type=INPUT_MOUSE;
        input.mi.dwFlags=down?MOUSEEVENTF_LEFTDOWN:MOUSEEVENTF_LEFTUP;
        if(SendInput(1,&input,sizeof(input))!=1) throw std::runtime_error("SendInput button failed");
    }
private:
    static LONG Normalise(int offset,int extent) {
        if(extent<=1) return 0;
        const auto scaled=(static_cast<long long>(offset)*65535+(extent-1)/2)/(extent-1);
        return static_cast<LONG>(scaled);
    }
    POINT cursor_{};
    struct Metrics { int x;int y;int width;int height; } metrics_{};
};
}
