#pragma once
#include "geometry.h"
#include "data/log.h"
#include <windows.h>
#include <string>
#include <vector>

namespace hermes {
struct GeometryWrite {
    std::string reason;
    bool strip=false;
    Rect target;
    Rect actual;
};
class GeometryController {
public:
    void Attach(HWND panel,HWND strip,Log* log=nullptr) { panel_=panel;strip_=strip;log_=log; }
    void Place(const GeometryModel& model,const Monitor& monitor,State state,const char* reason);
    void Visibility(State state);
    void Validate(const GeometryModel& model,const Monitor& monitor,State state);
    unsigned Writes() const { return static_cast<unsigned>(audit_.size()); }
    unsigned Violations() const { return violations_; }
    const std::vector<GeometryWrite>& Audit() const { return audit_; }
    static Rect Read(HWND window);
private:
    void Write(HWND window,const Rect& rect,bool strip,const char* reason);
    void Assert(bool condition,const char* invariant);
    Log* log_=nullptr;
    HWND panel_=nullptr;
    HWND strip_=nullptr;
    unsigned violations_=0;
    std::vector<GeometryWrite> audit_;
};
}
