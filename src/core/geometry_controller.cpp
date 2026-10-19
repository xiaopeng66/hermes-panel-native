#include "geometry_controller.h"
#include <stdexcept>

namespace hermes {
Rect GeometryController::Read(HWND window) {
    RECT rect{};
    if(!GetWindowRect(window,&rect)) throw std::runtime_error("GetWindowRect failed");
    return {rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top};
}
void GeometryController::Assert(bool condition,const char* invariant) {
    if(condition) return;
    ++violations_;
    OutputDebugStringA(invariant);
    if(log_) log_->Error("geometry-invariant",invariant);
    throw std::logic_error(invariant);
}
void GeometryController::Write(HWND window,const Rect& rect,bool strip,const char* reason) {
    const bool success=SetWindowPos(window,nullptr,rect.x,rect.y,rect.width,rect.height,SWP_NOACTIVATE|SWP_NOZORDER)!=0;
    const auto actual=Read(window);
    audit_.push_back({reason,strip,rect,actual});
    if(log_) log_->Geometry(std::string(strip?"strip ":"panel ")+reason+" target=("+std::to_string(rect.x)+","+
        std::to_string(rect.y)+","+std::to_string(rect.width)+","+std::to_string(rect.height)+") actual=("+
        std::to_string(actual.x)+","+std::to_string(actual.y)+","+std::to_string(actual.width)+","+std::to_string(actual.height)+")");
    Assert(success&&actual==rect,"INV1: absolute geometry write/read mismatch");
}
void GeometryController::Place(const GeometryModel& model,const Monitor& monitor,State state,const char* reason) {
    Assert(state!=State::Dragging,"INV2: geometry controller called during system drag");
    // 最小化窗口的 GetWindowRect 是任务栏小矩形：先恢复，否则任何 Place 都会报 INV1
    // （审计 F-020：随机点击命中最小化后，后续主题/挂件切换把进程带进 error 日志）。
    if(IsIconic(panel_)) ShowWindow(panel_,SW_RESTORE);
    const auto rects=ComputeRect(model,monitor);
    Write(panel_,rects.panel,false,reason);
    if(rects.strip) Write(strip_,*rects.strip,true,reason);
}
void GeometryController::Visibility(State state) {
    Assert(state!=State::Dragging,"INV2: visibility controller called during system drag");
    if(state==State::Hidden) { ShowWindow(panel_,SW_HIDE);ShowWindow(strip_,SW_HIDE); }
    else if(state==State::Collapsed) { ShowWindow(panel_,SW_HIDE);ShowWindow(strip_,SW_SHOWNOACTIVATE); }
    else {
        // 最小化的窗口用 SW_SHOWNOACTIVATE 不会恢复（GetWindowRect 仍是任务栏小矩形）；
        // 审计 F-020：先 SW_RESTORE 再摆可见性。
        if(IsIconic(panel_)) ShowWindow(panel_,SW_RESTORE);
        ShowWindow(strip_,SW_HIDE);ShowWindow(panel_,SW_SHOWNOACTIVATE);
    }
    Assert(!(IsWindowVisible(panel_)&&IsWindowVisible(strip_)),"INV5: both windows visible");
}
void GeometryController::Validate(const GeometryModel& model,const Monitor& monitor,State state) {
    Assert(state!=State::Dragging,"INV2: geometry assertion during system drag");
    const auto rects=ComputeRect(model,monitor);
    Assert(Read(panel_)==rects.panel,"INV1: stable panel does not match model");
    if(rects.strip) Assert(Read(strip_)==*rects.strip,"INV1/INV4: stable strip does not match model");
    Assert(!(IsWindowVisible(panel_)&&IsWindowVisible(strip_)),"INV5: both windows visible");
}
}
