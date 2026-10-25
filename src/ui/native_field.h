#pragma once
#include <windows.h>
#include <string>

// 原生单行输入框（用户要求「我要原生输入框」）。
//
// 面板主体是普通窗口（HwndRenderTarget），所以可以挂真实的 EDIT 子控件：光标定位、拖选、
// 双击选词、I 形指针、右键菜单、Ctrl+A/C/V/X、IME、横向滚动全部由系统提供，不再自绘。
// 自绘部分只保留字段外框/标签（圆角、聚焦描边、主题色），EDIT 内缩在框内、无边框，
// 背景色由父窗口的 WM_CTLCOLOREDIT 提供，视觉上与自绘框同色。
namespace hermes {
void RegisterNativeFieldClass(HINSTANCE instance);
class NativeField {
public:
    enum class Event { Enter=1,Escape=2,TabForward=3,TabBackward=4,Focus=5,Blur=6,Change=7,
        ArrowUp=8,ArrowDown=9,
        // 鼠标点进控件（WM_LBUTTONDOWN / WM_LBUTTONDBLCLK）。**不能只靠 Focus 事件**：控件已经
        // 持有键盘焦点时再点它不会产生 WM_SETFOCUS，应用侧就学不到"用户又开始编辑这个字段了"，
        // 随后的输入在失焦时被静默丢弃（真机复现：同一字段第二次编辑永远不提交）。
        Pressed=10 };

    // 创建（一次）：parent 是面板窗口，id 由调用方分配（用于事件回传）。
    void Create(HWND parent,int id,bool password,bool alignRight);
    void Destroy();
    HWND Handle() const { return window_; }
    bool Created() const { return window_!=nullptr; }

    // 位置/可见性：rect 为父窗口客户区坐标；visible=false 时隐藏。
    void Place(const RECT& rect,bool visible);
    void SetText(const std::wstring& text);
    // 只读缓存：记录"上次推下去/用户敲进去"的文本，SetText 用它判断是否需要真的动控件。
    // 没有它时每帧每个字段都要 GetWindowTextLength+GetWindowText 两次（7 个字段实测 ~4ms/帧）。
    const std::wstring& CachedText() const { return cachedText_; }
    // 用户编辑（EN_CHANGE）后由外壳调用：控件里的文本才是真的，缓存跟着更新。
    void RefreshCache();
    std::wstring Text() const;
    void SetCue(const std::wstring& cue);
    // 把 cue 真正推给控件（组词期间推空串：组词文字未上屏时控件文本还是空的，占位会与它重叠）。
    void ApplyCue();
    // 密码字段的「眼睛」：visible=true 时去掉掩码字符（EM_SETPASSWORDCHAR 0），false 恢复原掩码。
    void SetPasswordVisible(bool visible);
    void SetFontAndDpi(UINT dpi);
    void Focus();
    void Blur();
    bool Focused() const;

public:
    static LRESULT CALLBACK WndProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam);
private:
    HWND window_=nullptr;
    HWND parent_=nullptr;
    int id_=0;
    HFONT font_=nullptr;
    std::wstring cue_;              // 占位提示（首次显示时重新应用：隐藏状态下设置会失效）
    const std::wstring emptyCue_{};  // 组词期间推给控件的空 cue
    bool composing_=false;          // IME 组词中：占位要收起来，否则与组词文字重叠
    std::wstring cachedText_;       // 见 CachedText()：避免每帧回读控件文本
    int cachedTextValid_=0;         // 0 未初始化（首次必须回读一次）
    WNDPROC original_=nullptr;      // EDIT 自己的窗口过程（子类化必须转发给它）
    UINT dpi_=96;
    int lineHeight_=0;              // 字体行高（用来把控件竖向居中）
    bool password_=false;
    wchar_t passwordChar_=0;      // ES_PASSWORD 的默认掩码字符（EM_GETPASSWORDCHAR 取到后保存）
    bool passwordVisible_=false;  // 当前是否已去掉掩码（去重：避免每帧重设触发重绘）
    // 摆放缓存：位置没变就不调 SetWindowPos（否则子控件移动会触发父窗口重绘，形成自激）。
    RECT placed_{};
    int placedLineHeight_=0;        // 上次摆放时的字体行高（字号重建后必须重新竖向居中）
    bool placedVisible_=false;
};
}
