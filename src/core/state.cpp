#include "state.h"
namespace hermes {
State NextState(State current,Event event,Edge edge) {
    if(current==State::Dragging) {
        if(event==Event::DropFloating) return State::Floating;
        if(event==Event::DropDocked) return edge==Edge::None?State::Floating:State::Collapsed;
        return State::Dragging;
    }
    switch(event) {
    case Event::Show: return edge==Edge::None?State::Floating:State::Collapsed;
    case Event::Hide: return State::Hidden;
    case Event::BeginDrag: return current==State::Hidden?State::Hidden:State::Dragging;
    case Event::EnterStrip: return current==State::Collapsed&&edge!=Edge::None?State::Expanded:current;
    case Event::LeaveGrace: return current==State::Expanded?State::Collapsed:current;
    case Event::Undock: return current==State::Hidden?State::Hidden:State::Floating;
    case Event::DropFloating: return current==State::Hidden?State::Hidden:State::Floating;
    case Event::DropDocked:
        if(edge==Edge::None) return current;
        return current==State::Hidden?State::Hidden:State::Collapsed;
    }
    return current;
}
VisibleWindows VisibleFor(State state) {
    switch(state) {
    case State::Hidden: return {false,false};
    case State::Collapsed: return {false,true};
    case State::Floating:
    case State::Dragging:
    case State::Expanded: return {true,false};
    }
    return {false,false};
}
}
