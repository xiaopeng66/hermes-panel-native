#include "render/textfield.h"
#include <windows.h>
#include <algorithm>
#include <vector>

namespace hermes {
namespace {
bool IsWordChar(wchar_t c) { return (c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'z')||(c>=L'A'&&c<=L'Z')||c>=0x80; }
int SurrogateStep(const std::wstring& text,int caret,int direction) {
    // Walk whole surrogate pairs and CRLF as one unit so the caret never splits either.
    if(direction>0&&caret<static_cast<int>(text.size())) {
        if(text[static_cast<std::size_t>(caret)]==L'\r'&&caret+1<static_cast<int>(text.size())
            &&text[static_cast<std::size_t>(caret+1)]==L'\n') return 2;
        if(IS_HIGH_SURROGATE(text[static_cast<std::size_t>(caret)])&&caret+1<static_cast<int>(text.size())
            &&IS_LOW_SURROGATE(text[static_cast<std::size_t>(caret+1)])) return 2;
        return 1;
    }
    if(direction<0&&caret>0) {
        if(caret>=2&&text[static_cast<std::size_t>(caret-2)]==L'\r'
            &&text[static_cast<std::size_t>(caret-1)]==L'\n') return 2;
        if(caret>=2&&IS_LOW_SURROGATE(text[static_cast<std::size_t>(caret-1)])
            &&IS_HIGH_SURROGATE(text[static_cast<std::size_t>(caret-2)])) return 2;
        return 1;
    }
    return 0;
}
int LineStartBefore(const std::wstring& text,int caret) {
    for(int index=caret-1;index>=0;--index)
        if(text[static_cast<std::size_t>(index)]==L'\n') return index+1;
    return 0;
}
}

bool TextField::Empty() const { return text_.empty(); }
bool TextField::HasSelection() const { return anchor!=caret; }
const std::wstring& TextField::Text() const { return text_; }

void TextField::SetText(const std::wstring& value) {
    text_=value;
    caret=anchor=static_cast<int>(value.size());
    targetColumn_=0;
    composition.clear();
    compositionCaret=0;
}
void TextField::SetTextUtf8(const std::string& value) {
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring wide(static_cast<std::size_t>(std::max(length,0)),L'\0');
    if(length>0) MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),wide.data(),length);
    SetText(wide);
}
std::string TextField::TextUtf8() const {
    const int length=WideCharToMultiByte(CP_UTF8,0,text_.data(),static_cast<int>(text_.size()),
        nullptr,0,nullptr,nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(length,0)),'\0');
    if(length>0) {
        WideCharToMultiByte(CP_UTF8,0,text_.data(),static_cast<int>(text_.size()),narrow.data(),length,
            nullptr,nullptr);
    }
    return narrow;
}
std::wstring TextField::DisplayText() const {
    if(!password||reveal) return text_;
    // U+2022 per code unit, keeping newlines so a multiline password keeps its shape.
    std::wstring masked;
    masked.reserve(text_.size());
    for(const wchar_t c:text_) masked.push_back(c==L'\n'?L'\n':L'\x2022');
    return masked;
}

void TextField::Focus() {
    focused=true;
    caret=anchor=static_cast<int>(text_.size());
    targetColumn_=0;
}
void TextField::Blur() { focused=false;composition.clear();compositionCaret=0; }
void TextField::SelectAll() { caret=static_cast<int>(text_.size());anchor=0; }
std::wstring TextField::Selected() const {
    const auto first=std::min(anchor,caret),last=std::max(anchor,caret);
    if(first>=last) return {};
    return text_.substr(static_cast<std::size_t>(first),static_cast<std::size_t>(last-first));
}
void TextField::EraseSection(int first,int last) {
    if(first>=last) return;
    text_.erase(static_cast<std::size_t>(first),static_cast<std::size_t>(last-first));
    caret=anchor=first;
}
void TextField::Insert(const std::wstring& value) {
    const auto first=std::min(anchor,caret),last=std::max(anchor,caret);
    EraseSection(first,last);
    text_.insert(static_cast<std::size_t>(caret),value);
    caret=anchor=caret+static_cast<int>(value.size());
    targetColumn_=caret-LineStartBefore(text_,caret);
}
void TextField::Backspace() {
    if(HasSelection()) { EraseSection(std::min(anchor,caret),std::max(anchor,caret));anchor=caret;return; }
    const auto step=SurrogateStep(text_,caret,-1);
    if(step==0) return;
    EraseSection(caret-step,caret);
    targetColumn_=caret-LineStartBefore(text_,caret);
}
void TextField::DeleteForward() {
    if(HasSelection()) { EraseSection(std::min(anchor,caret),std::max(anchor,caret));anchor=caret;return; }
    const auto step=SurrogateStep(text_,caret,1);
    if(step==0) return;
    EraseSection(caret,caret+step);
    targetColumn_=caret-LineStartBefore(text_,caret);
}
void TextField::MoveCaret(Move where,bool extend) {
    const auto set=[&](int position) {
        caret=std::clamp(position,0,static_cast<int>(text_.size()));
        if(!extend) anchor=caret;
    };
    switch(where) {
    case Move::Left: {
        const auto step=SurrogateStep(text_,caret,-1);
        if(step!=0) set(caret-step);
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::Right: {
        const auto step=SurrogateStep(text_,caret,1);
        if(step!=0) set(caret+step);
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::WordLeft: {
        int position=caret;
        while(position>0&&IsWordChar(text_[static_cast<std::size_t>(position-1)])) --position;
        if(position==caret&&position>0) --position;
        set(position);
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::WordRight: {
        int position=caret;
        while(position<static_cast<int>(text_.size())&&IsWordChar(text_[static_cast<std::size_t>(position)])) ++position;
        if(position==caret&&position<static_cast<int>(text_.size())) ++position;
        set(position);
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::Home: {
        set(LineStartBefore(text_,caret));
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::End: {
        const auto lineEnd=text_.find(L'\n',static_cast<std::size_t>(caret));
        set(lineEnd==std::wstring::npos?static_cast<int>(text_.size()):static_cast<int>(lineEnd));
        targetColumn_=caret-LineStartBefore(text_,caret);
        break;
    }
    case Move::DocHome: set(0);break;
    case Move::DocEnd: set(static_cast<int>(text_.size()));break;
    case Move::Up: case Move::Down: {
        // Multiline navigation on hard newlines, keeping the sticky column across consecutive moves.
        std::vector<int> starts{0};
        for(std::size_t index=0;index<text_.size();++index)
            if(text_[static_cast<std::size_t>(index)]==L'\n') starts.push_back(static_cast<int>(index)+1);
        int line=0;
        for(std::size_t index=0;index<starts.size();++index) {
            const auto start=starts[index];
            const auto finish=index+1<starts.size()?starts[index+1]-1:static_cast<int>(text_.size());
            if(caret>=start&&caret<=finish) { line=static_cast<int>(index);break; }
        }
        const auto target=where==Move::Up?line-1:line+1;
        if(target<0||target>=static_cast<int>(starts.size())) return;
        const auto start=starts[static_cast<std::size_t>(target)];
        const auto finish=target+1<static_cast<int>(starts.size())
            ?starts[static_cast<std::size_t>(target+1)]-1:static_cast<int>(text_.size());
        set(start+std::min(targetColumn_,finish-start));
        break;
    }
    }
}
void TextField::CompositionUpdate(const std::wstring& comp,int compCaret) {
    composition=comp;
    compositionCaret=compCaret;
}
void TextField::CompositionCommit(const std::wstring& result) {
    Insert(result);
    composition.clear();
    compositionCaret=0;
}
}
