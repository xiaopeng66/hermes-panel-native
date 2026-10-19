#pragma once
#include <string>

// Self-drawn text field (styles.css .inp / .fld input, PLAN §4 #5-#7): UTF-16 model with selection,
// inline IME composition and clipboard-grade editing ops. Pure: no window, no clock - Application
// routes WM_CHAR / WM_KEYDOWN / WM_IME_* into it and reads back for drawing. Indices are UTF-16 code
// unit offsets, same as DWrite metrics.
namespace hermes {
class TextField {
public:
    enum class Move { Left,Right,WordLeft,WordRight,Home,End,DocHome,DocEnd,Up,Down };

    bool focused=false,multiline=false,password=false,reveal=false;
    int caret=0,anchor=0;                 // selection = [min(anchor,caret), max(...))
    std::wstring composition;             // inline IME composition (drawn with an underline)
    int compositionCaret=0;
    std::string placeholder;              // UTF-8, drawn faint when empty+unfocused

    bool Empty() const;
    bool HasSelection() const;
    const std::wstring& Text() const;
    void SetText(const std::wstring& value);
    void SetTextUtf8(const std::string& value);
    std::string TextUtf8() const;
    // The characters that are actually drawn: masked bullets for a password, composition inline.
    std::wstring DisplayText() const;

    void Focus();   // selects nothing; caret to end (the old .inp behaves the same)
    void Blur();
    void SelectAll();
    std::wstring Selected() const;
    void Insert(const std::wstring& value);      // replaces the selection
    void Backspace();                            // deletes the selection or one unit left
    void DeleteForward();                        // deletes the selection or one unit right
    void MoveCaret(Move where,bool extend);
    // IME inline composition: update with GCS_COMPSTR, commit with GCS_RESULTSTR.
    void CompositionUpdate(const std::wstring& comp,int compCaret);
    void CompositionCommit(const std::wstring& result);
    bool Composing() const { return !composition.empty(); }

private:
    std::wstring text_;
    int targetColumn_=0;              // sticky column for multiline Up/Down
    void EraseSection(int first,int last);
};
}
