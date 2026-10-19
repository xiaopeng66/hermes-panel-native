#pragma once
#include "geometry.h"
namespace hermes {
enum class Event { Show,Hide,BeginDrag,DropFloating,DropDocked,EnterStrip,LeaveGrace,Undock };
State NextState(State current,Event event,Edge edge);
struct VisibleWindows { bool panel;bool strip;bool operator==(const VisibleWindows&) const=default; };
VisibleWindows VisibleFor(State state);
}
