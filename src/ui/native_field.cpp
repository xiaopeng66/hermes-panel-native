#include "ui/native_field.h"
#include <commctrl.h>
#include "app/application_internal.h"

namespace hermes {
namespace {
constexpr UINT kNotifyMessage=WM_APP+40;   // wParam=id, lParam=NativeField::Event

std::wstring ReadText(HWND window) {
    const int length=GetWindowTextLengthW(window);
    if(length<=0) return {};
    std::wstring text(static_cast<std::size_t>(length)+1,L'\0');
    const int copied=GetWindowTextW(window,text.data(),length+1);
    text.resize(static_cast<std::size_t>(std::max(copied,0)));
    return text;
}
}

void NativeField::Create(HWND parent,int id,bool password,bool alignRight) {
    if(window_) return;
    parent_=parent;
    id_=id;
    password_=password;
    DWORD style=WS_CHILD|ES_AUTOHSCROLL;
    if(password) style|=ES_PASSWORD;
    if(alignRight) style|=ES_RIGHT;
    // 必须是系统 EDIT 类：自定义类没有编辑行为（不会插入字符、没有选区/光标）。
    window_=CreateWindowExW(0,L"EDIT",L"",style,0,0,10,10,parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),nullptr,nullptr);
    if(!window_) return;
    SetWindowLongPtrW(window_,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
    // 密码字段：记下系统给的默认掩码字符，眼睛按钮恢复时要用（不能假定是某个具体字符）。
    if(password_) passwordChar_=static_cast<wchar_t>(SendMessageW(window_,EM_GETPASSWORDCHAR,0,0));
    // 子类化：只截获 Enter/Esc/Tab 与焦点通知，其余（编辑、选区、IME、右键菜单）交回 EDIT。
    original_=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window_,GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(NativeField::WndProc)));
    // 原生控件默认吃 Tab（会响一声）：改成回车式的前后导航由父窗口处理。
    SetFontAndDpi(GetDpiForWindow(parent));
    ShowWindow(window_,SW_HIDE);
}

void NativeField::Destroy() {
    if(font_) { DeleteObject(font_);font_=nullptr; }
    if(window_) { DestroyWindow(window_);window_=nullptr; }
}

void NativeField::SetFontAndDpi(UINT dpi) {
    dpi_=dpi?dpi:96;
    const int height=-MulDiv(9,static_cast<int>(dpi_),72);    // 9pt ≈ 12px body
    HFONT font=CreateFontW(height,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Microsoft YaHei UI");
    if(font) {
        if(font_) DeleteObject(font_);
        font_=font;
        SendMessageW(window_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        // 单行 EDIT 的文字是顶对齐的：量出字体行高，摆放时把控件收成"行高+2"再竖向居中，
        // 否则文字会贴着框顶（真机反馈"文字不在居中高度"）。
        if(window_) {
            if(HDC dc=GetDC(window_)) {
                const auto previous=SelectObject(dc,font_);
                TEXTMETRICW metrics{};
                if(GetTextMetricsW(dc,&metrics)) lineHeight_=metrics.tmHeight;
                SelectObject(dc,previous);
                ReleaseDC(window_,dc);
            }
        }
    }
}

void NativeField::Place(const RECT& rect,bool visible) {
    if(!window_) return;
    if(!visible) {
        if(placedVisible_) { ShowWindow(window_,SW_HIDE);placedVisible_=false; }
        return;
    }
    // 控件自己的 DPI 才是它所在显示器的 DPI：跨屏拖动时父窗口的 monitor_ 可能还是旧值，
    // 而系统移动循环会把窗口 DPI 先换掉——这里自愈一次，cue 就不会按旧字号画得比框还大。
    if(const UINT dpi=GetDpiForWindow(window_);dpi&&dpi!=dpi_) SetFontAndDpi(dpi);
    // 文字竖向居中：控件高度 = 行高 + 2，放在字段框的竖直中线。
    RECT target=rect;
    if(lineHeight_>0) {
        const LONG boxHeight=target.bottom-target.top;
        // 框比"行高+2"还矮时按框高收（宁可文字被裁，也不让控件/占位探出输入框）。
        const LONG wanted=std::min<LONG>(lineHeight_+2,boxHeight);
        if(boxHeight>wanted) {
            const LONG centre=(target.top+target.bottom)/2;
            target.top=centre-wanted/2;
            target.bottom=target.top+wanted;
        }
    }
    // 缓存键是**真正下发的**矩形 + 行高：字号重建后即使框没变也要重摆一次（否则竖向居中会偏）。
    if(placedVisible_&&target.left==placed_.left&&target.top==placed_.top
        &&target.right==placed_.right&&target.bottom==placed_.bottom
        &&lineHeight_==placedLineHeight_) return;
    SetWindowPos(window_,nullptr,target.left,target.top,std::max(1L,target.right-target.left),
        std::max(1L,target.bottom-target.top),SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOOWNERZORDER);
    placed_=target;
    placedLineHeight_=lineHeight_;
    if(!placedVisible_) {
        ShowWindow(window_,SW_SHOWNOACTIVATE);
        placedVisible_=true;
        // 隐藏期间推过的文本没有生效（见 SetText）：显示后必须允许下一次重新推。
        cachedTextValid_=0;
        // 控件隐藏时设置的 cue banner 不生效：第一次真正显示时再设一次（组词态由 ApplyCue 判断）。
        if(!cue_.empty()||composing_) ApplyCue();
    }
}

void NativeField::SetText(const std::wstring& text) {
    if(!window_) return;
    // 注意：控件**隐藏**时 SetWindowTextW 是无效的（和 cue banner 一样），文本并没有真的进去。
    // 所以缓存只在"控件可见且上次推送成功"时才可信；一旦控件不可见就作废，下次显示时重新推。
    if(!placedVisible_) cachedTextValid_=0;
    if(cachedTextValid_) {
        // 缓存命中：不再回读控件（GetWindowText 每次要走内核，7 个字段实测 ~4ms/帧）。
        if(cachedText_==text) return;
    } else {
        cachedText_=ReadText(window_);
        cachedTextValid_=1;
        if(cachedText_==text) return;
    }
    cachedText_=text;
    SetWindowTextW(window_,text.c_str());
}

void NativeField::RefreshCache() {
    if(!window_) return;
    cachedText_=ReadText(window_);
    cachedTextValid_=1;
}

std::wstring NativeField::Text() const { return window_?ReadText(window_):std::wstring{}; }

void NativeField::SetCue(const std::wstring& cue) {
    cue_=cue;
    ApplyCue();
}

void NativeField::ApplyCue() {
    if(!window_||!placedVisible_) return;
    // IME 组词期间不画占位：组词文字还没上屏，控件文本仍是空的，cue 会照常显示在组词文字底下
    // （真机实测：搜索框组词时显示成「glm模型或供应商」两串字叠在一起）。
    const auto& shown=composing_?emptyCue_:cue_;
    SendMessageW(window_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(shown.c_str()));
}

void NativeField::SetPasswordVisible(bool visible) {
    if(!window_||!password_||visible==passwordVisible_) return;
    passwordVisible_=visible;
    SendMessageW(window_,EM_SETPASSWORDCHAR,visible?0:static_cast<WPARAM>(passwordChar_),0);
    InvalidateRect(window_,nullptr,TRUE);
}

void NativeField::Focus() {
    if(window_) { SetFocus(window_);SendMessageW(window_,EM_SETSEL,0,-1); }
}

void NativeField::Blur() {
    if(window_) SetFocus(GetParent(window_));
}

bool NativeField::Focused() const { return window_&&GetFocus()==window_; }

LRESULT CALLBACK NativeField::WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* self=reinterpret_cast<NativeField*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    switch(message) {
    case WM_NCCREATE:
        return TRUE;
    case WM_GETDLGCODE:
        // 回车/转义/Tab 都由父窗口决定（保存、取消、字段导航），不是"输入一个换行"。
        return DLGC_WANTALLKEYS|DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if(self&&self->parent_) {
            int event=0;
            switch(wparam) {
            case VK_RETURN: event=static_cast<int>(Event::Enter);break;
            case VK_ESCAPE: event=static_cast<int>(Event::Escape);break;
            case VK_TAB: event=static_cast<int>(Event::TabForward);break;
            // 搜索框的 ↑↓ 要移动列表光标（自绘字段时代由 HandleFieldKey 转发给 HandleListKey）。
            case VK_UP: event=static_cast<int>(Event::ArrowUp);break;
            case VK_DOWN: event=static_cast<int>(Event::ArrowDown);break;
            default: break;
            }
            if(wparam==VK_TAB&&(GetKeyState(VK_SHIFT)<0)) event=static_cast<int>(Event::TabBackward);
            if(event) {
                PostMessageW(self->parent_,kNotifyMessage,static_cast<WPARAM>(self->id_),
                    static_cast<LPARAM>(event));
                return 0;
            }
        }
        break;
    case WM_CHAR:
        if(wparam==L'\r'||wparam==L'\t'||wparam==0x1B) return 0;   // 已由 KEYDOWN 处理
        break;
    case WM_SETFOCUS:
        if(self&&self->parent_)
            PostMessageW(self->parent_,kNotifyMessage,static_cast<WPARAM>(self->id_),
                static_cast<LPARAM>(Event::Focus));
        break;
    case WM_IME_STARTCOMPOSITION:
        // 组词开始：把 cue 收起来（见 ApplyCue 的注释），组词结束再放回去。
        if(self&&!self->composing_) { self->composing_=true;self->ApplyCue(); }
        break;
    case WM_IME_ENDCOMPOSITION:
        if(self&&self->composing_) { self->composing_=false;self->ApplyCue(); }
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        // 点进控件 = 明确的"又开始编辑这个字段"信号。控件已经持有键盘焦点时点击不会产生
        // WM_SETFOCUS（系统只在焦点真的变了才发），所以这里单独通知一次：外壳据此把
        // focusedField_ 指回来，后面的输入才会在失焦时被写回。转发给 EDIT 自己的过程照旧，
        // 所以光标定位/拖选/双击选词都不受影响。
        if(self&&self->parent_)
            PostMessageW(self->parent_,kNotifyMessage,static_cast<WPARAM>(self->id_),
                static_cast<LPARAM>(Event::Pressed));
        break;
    case WM_KILLFOCUS:
        if(self&&self->parent_)
            PostMessageW(self->parent_,kNotifyMessage,static_cast<WPARAM>(self->id_),
                static_cast<LPARAM>(Event::Blur));
        break;
    default: break;
    }
    // 转发给 EDIT 自己的过程：编辑、光标、选区、拖选、双击选词、IME、右键菜单都在那里。
    if(self&&self->original_)
        return CallWindowProcW(self->original_,window,message,wparam,lparam);
    return DefWindowProcW(window,message,wparam,lparam);
}

void RegisterNativeFieldClass(HINSTANCE instance) {
    (void)instance;   // 直接用系统 EDIT 类，无需注册
}
}
