#include "core/state.h"
#include <iostream>
using namespace hermes;
int main() {
    struct Case { State from;Event event;Edge edge;State expected; };
    const Case cases[]{
        {State::Hidden,Event::Show,Edge::None,State::Floating},
        {State::Hidden,Event::Show,Edge::Left,State::Collapsed},
        {State::Floating,Event::Hide,Edge::None,State::Hidden},
        {State::Expanded,Event::Hide,Edge::Left,State::Hidden},
        {State::Collapsed,Event::Hide,Edge::Left,State::Hidden},
        {State::Floating,Event::BeginDrag,Edge::None,State::Dragging},
        {State::Expanded,Event::BeginDrag,Edge::Left,State::Dragging},
        {State::Dragging,Event::DropFloating,Edge::None,State::Floating},
        {State::Dragging,Event::DropDocked,Edge::Right,State::Collapsed},
        {State::Collapsed,Event::EnterStrip,Edge::Right,State::Expanded},
        {State::Expanded,Event::LeaveGrace,Edge::Right,State::Collapsed},
        {State::Collapsed,Event::Undock,Edge::None,State::Floating},
        {State::Expanded,Event::Undock,Edge::None,State::Floating},
        {State::Dragging,Event::Hide,Edge::Left,State::Dragging},
        {State::Dragging,Event::EnterStrip,Edge::Left,State::Dragging},
        {State::Dragging,Event::LeaveGrace,Edge::Left,State::Dragging},
        {State::Dragging,Event::Undock,Edge::Left,State::Dragging},
        {State::Floating,Event::LeaveGrace,Edge::None,State::Floating},
        {State::Hidden,Event::EnterStrip,Edge::Left,State::Hidden},
        {State::Collapsed,Event::Show,Edge::Left,State::Collapsed},
        {State::Floating,Event::DropDocked,Edge::Left,State::Collapsed},
        {State::Expanded,Event::DropDocked,Edge::Left,State::Collapsed},
        {State::Collapsed,Event::DropDocked,Edge::Left,State::Collapsed},
        {State::Hidden,Event::DropDocked,Edge::Left,State::Hidden},
        {State::Expanded,Event::DropDocked,Edge::None,State::Expanded},
        {State::Floating,Event::DropFloating,Edge::None,State::Floating},
        {State::Collapsed,Event::Undock,Edge::None,State::Floating}
    };
    int failed=0;
    for(std::size_t i=0;i<std::size(cases);++i) {
        const auto& test=cases[i];
        if(NextState(test.from,test.event,test.edge)!=test.expected) { ++failed;std::cerr<<"FAIL state case "<<i<<'\n'; }
    }
    const VisibleWindows expected[]{{false,false},{true,false},{true,false},{true,false},{false,true}};
    for(int i=0;i<5;++i) if(VisibleFor(static_cast<State>(i))!=expected[i]) { ++failed;std::cerr<<"FAIL visibility "<<i<<'\n'; }
    const unsigned total=static_cast<unsigned>(std::size(cases))+5;
    std::cout<<"state: "<<total-failed<<" passed, "<<failed<<" failed\n";
    return failed?1:0;
}
