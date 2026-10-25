#include "app/application.h"
#include "app/application_internal.h"
#include "control_plane.h"
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdexcept>
#include "data/http.h"
#include "data/paths.h"
#include "data/probe.h"
#include "render/widgets.h"
#include <windowsx.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hermes {
void Application::FocusField(TextField* field) {
    if(focusedField_&&focusedField_!=field) focusedField_->Blur();
    // 换字段 = 结束热键捕获（否则旧的武装状态会留给下一个字段：按下的字母被当热键吞掉，
    // 带 Shift 的字母甚至会静默改写热键）。
    if(field!=&setHotkey_) setHotkeyCapturing_=false;
    focusedField_=field;
    if(field) {
        field->Focus();
        FocusNativeField(field);      // v1.23：原生控件拿焦点（光标/选区由系统管）
    }
    caretOn_=true;
    UpdateCaretTimer();
    InvalidateRect(panel_,nullptr,FALSE);
}
void Application::CommitFocusedField() {
    // 设置页的文本框在失焦时提交（旧面板的 input 也是这样写回去的）。
    // 先把控件里的文本同步回镜像再提交：EN_CHANGE 正常情况下已经同步过，但"点击别的字段"这条
    // 路径可能早于最后一次 EN_CHANGE 到达（控件是真相，镜像只是缓存）。
    if(!focusedField_) return;
    if(auto* native=NativeFor(focusedField_)) {
        const auto text=Narrow(native->Text());
        native->RefreshCache();
        if(focusedField_->TextUtf8()!=text) focusedField_->SetTextUtf8(text);
    }
    if(focusedField_==&setDistro_) SetSetting("distro",setDistro_.TextUtf8());
    else if(focusedField_==&setWebuiPort_) SetSetting("webuiPort",setWebuiPort_.TextUtf8());
    else if(focusedField_==&setPollSec_) SetSetting("pollSeconds",setPollSec_.TextUtf8());
}
void Application::BlurField() {
    // 失焦一律结束热键捕获：点输入框外/点别的控件/切页 = 取消这次编辑，显示还原为已保存的组合。
    setHotkeyCapturing_=false;
    if(!focusedField_) return;
    CommitFocusedField();
    focusedField_->Blur();
    focusedField_=nullptr;
    caretOn_=false;
    UpdateCaretTimer();
    InvalidateRect(panel_,nullptr,FALSE);
}
void Application::UpdateCaretTimer() {
    if(caretOn_) SetTimer(panel_,kCaretTimer,kCaretMilliseconds,nullptr);
    else KillTimer(panel_,kCaretTimer);
}
bool Application::HandleFieldKey(UINT message,WPARAM wparam,LPARAM lparam) {
    (void)lparam;
    // 模态是全局的：Enter 确定 / Esc 取消（DESIGN §4.8 #33）——焦点在哪个字段（或还没有字段）都一样。
    // 其余按键与字符一律吞掉：模态后面的字段不该被改到（之前只有 KEYDOWN 进了分支，WM_CHAR 会漏过去）。
    if(pending_!=Pending::None) {
        const bool modalKey=message==WM_KEYDOWN&&(wparam==VK_RETURN||wparam==VK_ESCAPE);
        if(!modalKey&&focusedField_) BlurField();
        if(!modalKey) return true;
        // 确定键与点模态按钮走**同一份**动作实现（RunPendingAction）：键盘与鼠标不可能分叉；
        // 之后与 HandleModalClick 一样收尾（关模态 + 清 pending）。
        if(wparam==VK_RETURN) RunPendingAction();
        pending_=Pending::None;
        pendingIndex_=-1;
        pendingProvider_.clear();
        pendingBackupFile_.clear();
        InvalidateAll();
        return true;
    }
    auto* field=focusedField_;
    if(!field) {
        if(message==WM_CHAR&&wparam==L'/') {
            // `/` is the old panel's search shortcut: show the models page and focus the box.
            if(activePage_!=0) { activePage_=0;PersistSettings(); }
            FocusField(&search_);
            ClearSearch();
            return true;
        }
        return false;
    }
    if(message==WM_KEYDOWN&&setHotkeyCapturing_) {
        // 热键捕获是字段自己的模式（旧 #set-hotkey keydown）：修饰键单独按下不算，等主键。
        CaptureHotkey(wparam);
        if(!setHotkeyCapturing_) BlurField();
        return true;
    }
    if(message==WM_KEYDOWN) {
        const bool shift=GetKeyState(VK_SHIFT)<0;
        // ↑↓ move the list cursor and ↵ switches the row under it while the search box keeps focus
        // (old app.js routed the search field's keydown into listKeydown).
        if(activePage_==0&&(wparam==VK_DOWN||wparam==VK_UP||wparam==VK_RETURN))
            return HandleListKey(wparam);
        if(wparam==VK_RETURN&&!field->multiline) {
            // 单行字段的 Enter = 离开字段（提交在 BlurField 里做）；多行字段的 Enter 由 WM_CHAR 插入换行。
            BlurField();
            return true;
        }
        switch(wparam) {
        case VK_LEFT: field->MoveCaret(TextField::Move::Left,shift);break;
        case VK_RIGHT: field->MoveCaret(TextField::Move::Right,shift);break;
        case VK_HOME: field->MoveCaret(TextField::Move::Home,shift);break;
        case VK_END: field->MoveCaret(TextField::Move::End,shift);break;
        case VK_DELETE: field->DeleteForward();break;
        case VK_ESCAPE: {
            if(fbSelectOpen_) { fbSelectOpen_=false;InvalidateAll();return true; }
            if(fbFormOpen_) { CloseForm();return true; }
            // Esc 清空（查询 + 游标）；本来就空的才把焦点还给面板。
            if(!field->Text().empty()||field->Composing()||cursor_>=0) ClearSearch();
            else BlurField();
            return true;
        }
        default: return false;
        }
        caretOn_=true;
        InvalidateRect(panel_,nullptr,FALSE);
        return true;
    }
    if(message!=WM_CHAR) return false;
    const wchar_t character=static_cast<wchar_t>(wparam);
    if(character==0x01) {
        field->SelectAll();                                              // Ctrl+A
    } else if(character==0x03||character==0x18) {                        // Ctrl+C / Ctrl+X
        const auto selected=field->Selected();
        if(!selected.empty()&&OpenClipboard(panel_)) {
            EmptyClipboard();
            const auto bytes=(selected.size()+1)*sizeof(wchar_t);
            const auto handle=GlobalAlloc(GMEM_MOVEABLE,bytes);
            if(handle) {
                auto* data=static_cast<wchar_t*>(GlobalLock(handle));
                if(data) { memcpy(data,selected.c_str(),bytes);GlobalUnlock(handle); }
                if(SetClipboardData(CF_UNICODETEXT,handle)==nullptr) GlobalFree(handle);
            }
            CloseClipboard();
        }
        if(character==0x18) field->Backspace();                          // cut removes the selection
    } else if(character==0x16) {                                         // Ctrl+V
        if(OpenClipboard(panel_)) {
            const auto* data=static_cast<const wchar_t*>(GetClipboardData(CF_UNICODETEXT));
            if(data) {
                // 剪贴板内容不可信：最多取 64k 字符，避免一次粘贴把界面卡死。
                std::wstring pasted;
                pasted.reserve(1024);
                for(std::size_t index=0;index<65536&&data[index];++index) pasted.push_back(data[index]);
                field->Insert(pasted);
            }
            CloseClipboard();
        }
    } else if(character==L'\b') {
        field->Backspace();
    } else if(character==L'\r'&&field->multiline) {
        field->Insert(L"\n");
    } else if(character==L'\r') {
        // Enter already selected the cursor row in the keydown pass; only an empty cursor blurs.
        if(cursor_<0) BlurField();
    } else if(character>=0x20) {
        field->Insert(std::wstring(1,character));
    } else {
        return false;
    }
    caretOn_=true;
    InvalidateRect(panel_,nullptr,FALSE);
    return true;
}
void Application::HandleFieldIme(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    // 模态期间不接受 IME 组字结果（否则会提交到模态后面的字段）。
    if(pending_!=Pending::None) return;
    auto* field=focusedField_;
    if(!field) return;
    if(message==WM_IME_COMPOSITION) {
        const HIMC context=ImmGetContext(window);
        if(!context) return;
        const auto read=[&](DWORD flag) {
            const auto size=ImmGetCompositionStringW(context,flag,nullptr,0);
            std::wstring value;
            if(size>=0) {
                value.resize(static_cast<std::size_t>(size)/sizeof(wchar_t));
                if(size>0) ImmGetCompositionStringW(context,flag,value.data(),size);
            }
            return value;
        };
        if(lparam&GCS_RESULTSTR) {
            field->CompositionCommit(read(GCS_RESULTSTR));
        } else if(lparam&GCS_COMPSTR) {
            const auto comp=read(GCS_COMPSTR);
            const auto position=ImmGetCompositionStringW(context,GCS_CURSORPOS,nullptr,0);
            field->CompositionUpdate(comp,static_cast<int>(position));
        }
        ImmReleaseContext(window,context);
        caretOn_=true;
        InvalidateRect(panel_,nullptr,FALSE);
        return;
    }
    if(message==WM_IME_ENDCOMPOSITION) {
        field->CompositionUpdate(L"",0);
        InvalidateRect(panel_,nullptr,FALSE);
        return;
    }
    if(message==WM_IME_REQUEST&&wparam==IMR_CANDIDATEWINDOW) {
        // Anchor the candidate window beside the field (PLAN §5): without this it floats to a corner.
        auto* form=reinterpret_cast<CANDIDATEFORM*>(lparam);
        RECT client{};
        GetClientRect(panel_,&client);
        const auto layout=ComputeLayout(CurrentView(),static_cast<float>(client.right),
            static_cast<float>(client.bottom),static_cast<float>(WindowDpi())/96.0f);
        POINT origin{0,0};
        ClientToScreen(panel_,&origin);
        const auto box=layout.search;
        form->dwIndex=0;
        form->dwStyle=CFS_CANDIDATEPOS;
        form->ptCurrentPos.x=static_cast<LONG>(origin.x+box.left);
        form->ptCurrentPos.y=static_cast<LONG>(origin.y+box.bottom);
        return;
    }
}

void Application::TrackHover(HWND window,float x,float y) {
    // Buttons light up on hover. This only ever invalidates: no geometry is touched, so the hover path
    // keeps its zero-write property (DESIGN §8 A1/A7).
    // v1.30：布局复用缓存（只算一次），指针移动不再重建视图。
    const auto& layout=HoverLayout();
    const auto id=static_cast<int>(ControlAt(layout,x,y));
    // 重复控件（分段/行）的序号：TooltipView.index 用它区分逐项说明（例如回退链每一行）。
    int hitIndex=-1;
    for(const auto& control:layout.controls) {
        if(control.id==ControlId::None) continue;
        if(x>=control.rect.left&&x<control.rect.right&&y>=control.rect.top&&y<control.rect.bottom) {
            hitIndex=control.index;
            break;
        }
    }
    // The list repaints on hover too: row / group-head / star states come from the same hit test as clicks.
    const auto previous=ListHitTest(layout,pointerX_,pointerY_);
    const auto current=ListHitTest(layout,x,y);
    const bool listMoved=previous.kind!=current.kind||previous.group!=current.group
        ||previous.row!=current.row||previous.menu!=current.menu||previous.star!=current.star;
    pointerX_=x;pointerY_=y;pointerInside_=true;
    // 注意：早先只有"命中控件变了"才重排提示定时器。于是**同一控件上 hover 两次**时，
    // 第一次已把 hoverControl_ 设成它、定时器却没走完（被 WM_MOUSELEAVE 或重绘挤掉），
    // 第二次进来 id==hoverControl_ → 不重启定时器 → 提示永远不弹（门禁偶发
    // "hovering lampWsl shows no tooltip"、hover=null 的根因）。
    if(id!=hoverControl_||listMoved||hitIndex!=tooltipHoverIndex_||
        (id!=0&&!tooltipVisible_&&panel_)) {
        // 悬停过渡（移植自余额面板）：记录上一个控件，90ms 内交叉淡出/淡入。
        if(id!=hoverControl_&&AnimationsEnabled()) {
            hoverPrevious_=hoverControl_;
            hoverMotionStart_=GetTickCount64();
            KeepMotionTimer();
        }
        hoverControl_=id;
        tooltipHoverIndex_=hitIndex;
        // 悬停提示：换了目标就重新计时（旧版是浏览器原生 title 的行为）。
        if(panel_) KillTimer(panel_,kTooltipTimer);
        tooltipVisible_=false;
        if(id!=0&&panel_) SetTimer(panel_,kTooltipTimer,kTooltipDelayMs,nullptr);
        if(log_) log_->Info("ui","hover control "+std::to_string(id));
        InvalidateRect(window,nullptr,FALSE);
    }
}
}

namespace hermes {
namespace {
// 焦点链只收“可操作”的控件：跳过装饰位（None/搜索清空在空的时候）与不可点项。
bool FocusableControl(ControlId id) {
    switch(id) {
    case ControlId::None: case ControlId::FbProbeDot: case ControlId::KeyReveal:
        return false;
    default: return true;
    }
}
}
void Application::MoveFocus(int delta) {
    // 审计 F-010：Tab/Shift+Tab 在当前页的控件表里移动（渲染与命中共用同一份矩形）。
    const auto laid=LayoutForInput();
    std::vector<std::pair<int,int>> order;   // (ControlId, index)
    for(const auto& control:laid.controls) {
        if(!FocusableControl(control.id)) continue;
        if(control.id==ControlId::SearchClear&&search_.Text().empty()) continue;
        order.emplace_back(static_cast<int>(control.id),control.index);
    }
    if(order.empty()) return;
    int current=-1;
    for(std::size_t index=0;index<order.size();++index)
        if(order[index].first==focusControl_&&order[index].second==focusControlIndex_) {
            current=static_cast<int>(index);
            break;
        }
    int next=current<0?(delta>0?0:static_cast<int>(order.size())-1)
        :(current+delta)%static_cast<int>(order.size());
    if(next<0) next+=static_cast<int>(order.size());
    focusControl_=order[static_cast<std::size_t>(next)].first;
    focusControlIndex_=order[static_cast<std::size_t>(next)].second;
    // 焦点落到表单字段上时把真实输入焦点也给它（键盘可以直接编辑）。
    if(focusControl_==static_cast<int>(ControlId::FormField)) {
        TextField* field=nullptr;
        if(activePage_==2) field=AddFieldAt(focusControlIndex_);
        else if(activePage_==1) field=FormFieldAt(focusControlIndex_);
        else if(activePage_==4) {
            switch(focusControlIndex_) {
            case 0: field=&setDistro_;break;
            case 1: field=&setWebuiPort_;break;
            case 2: field=&setPollSec_;break;
            case 4: field=&setHotkey_;break;
            default: break;
            }
        }
        if(field) FocusField(field);
        else BlurField();
    } else {
        BlurField();
    }
    HideTooltip();
    InvalidateAll();
}
void Application::ActivateFocus() {
    if(focusControl_==0) return;
    const auto laid=LayoutForInput();
    const auto id=static_cast<ControlId>(focusControl_);
    for(const auto& control:laid.controls) {
        if(control.id!=id) continue;
        if(focusControlIndex_>=0&&control.index!=focusControlIndex_) continue;
        const float x=(control.rect.left+control.rect.right)/2.0f;
        const float y=(control.rect.top+control.rect.bottom)/2.0f;
        // 与鼠标点击同一条分发链（分页处理函数里会先看模态/外壳）。
        if(activePage_==1) FallbackPageClick(x,y);
        else if(activePage_==2) AddPageClick(x,y);
        else if(activePage_==3) BackupPageClick(x,y);
        else if(activePage_==4) SettingsPageClick(x,y);
        else {
            if(id==ControlId::SearchBox) FocusField(&search_);
            else if(id==ControlId::SearchClear) { ClearSearch();FocusField(&search_); }
            else InvokeControl(id);
        }
        InvalidateAll();
        return;
    }
}
void Application::ClearFocus() {
    if(focusControl_==0) return;
    focusControl_=0;
    focusControlIndex_=-1;
    InvalidateAll();
}
}
