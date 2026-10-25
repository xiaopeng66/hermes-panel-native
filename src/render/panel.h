#pragma once
#include <map>
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include "app/panel_view.h"
#include "render/icons.h"
#include "render/layout.h"
#include "render/theme.h"
#include "render/widgets.h"
#include "render/textfield.h"

namespace hermes {
// Draws the panel: title bar, lamp register, current-model hero, the active page and the bottom
// navigation. Shapes and colours come from render/widgets.cpp (the old stylesheet's values); H2a
// built the chrome, H2b the control primitives, and H4/H5 add the page bodies.
class PanelRenderer {
public:
    void Invalidate() {
        brush_.Reset();owner_=nullptr;arcStyle_.Reset();styleFactory_.Reset();
        strip_.Reset();stripOwner_=nullptr;fade_.Reset();fadeOwner_=nullptr;
        backdrop_.Reset();backdropGeneration_=0;
    }
    void Draw(ID2D1RenderTarget* target,IDWriteFactory* text,const PanelViewModel& view,
        D2D1_SIZE_F size,float scale,const ThemePalette& palette,bool strip,
        const TextField* field=nullptr);
    // 贴条悬浮信息卡（独立分层小窗）：圆角卡片 + 当前模型 + 三个服务状态 + 提示。
    void DrawHoverCard(ID2D1RenderTarget* target,IDWriteFactory* text,const HoverCardView& card,
        D2D1_SIZE_F size,float scale,const ThemePalette& palette,
        float cardAlpha=1.0f,float cardOffsetDip=0.0f);
private:
    void EnsureResources(ID2D1RenderTarget* target,IDWriteFactory* text,float scale);
    void PrepareStrip(ID2D1RenderTarget* target,const ThemePalette& palette);
    // G1：贴边收起态的两套小挂件（铭牌 tag / 胶囊 capsule），同一份液态玻璃与内容排布
    // （DESIGN v1.10：形体随贴边旋转、内容按屏幕空间排布）。
    void Widget(ID2D1RenderTarget* target,D2D1_SIZE_F size,float scale,
        const PanelViewModel& view,const ThemePalette& palette);
    // 挂件内容：竖直边 = logo 在上 → 三灯 网关/WebUI/WSL 从上到下 → 模型名在下（从下往上读）；
    // 水平边 = 名字在左 → 三灯 从左到右 → logo 在右。两样式只差尺寸档（logo/灯点/文字盒）。
    void WidgetContent(ID2D1RenderTarget* target,const PanelViewModel& view,const ThemePalette& palette,
        float scale,float width,float height,WidgetStyle style);
    // 挂件上的状态灯：连续衰减的光晕 + 实心点（与面板灯条同一套观感）。
    void LampDot(ID2D1RenderTarget* target,float x,float y,float radius,const D2D1_COLOR_F& colour,
        const ThemePalette& palette);
    void PrepareFade(ID2D1RenderTarget* target,const ThemePalette& palette);
    void Text(ID2D1RenderTarget* target,const std::string& value,D2D1_RECT_F rect,IDWriteTextFormat* format,
        const D2D1_COLOR_F& colour,DWRITE_TEXT_ALIGNMENT alignment,bool verticalCentre);
    std::string Ellipsize(IDWriteTextFormat* format,const std::string& value,float width);
    D2D1_SIZE_F Measure(IDWriteTextFormat* format,const std::string& value);
    // v1.29：测量缓存。同一帧里同一批标签会被反复量（行高、截断、居中都要宽度），
    // CreateTextLayout 是这里最贵的单项；按 (format, text) 记忆，条目过多时整体清空（有界）。
    std::map<std::pair<std::uintptr_t,std::string>,D2D1_SIZE_F> measureCache_;
public:
    // v1.29：测量缓存的命中情况（控制面暴露，门禁据此断言缓存在效）。
    unsigned long long MeasureHits() const { return measureHits_; }
    unsigned long long MeasureMisses() const { return measureMisses_; }
private:
    unsigned long long measureHits_=0,measureMisses_=0;
    void TextWrapped(ID2D1RenderTarget* target,const std::string& value,D2D1_RECT_F rect,
        IDWriteTextFormat* format,const D2D1_COLOR_F& colour,
        DWRITE_TEXT_ALIGNMENT alignment=DWRITE_TEXT_ALIGNMENT_CENTER);
    bool hovered_(ControlId id) const { return hoverControl_==static_cast<int>(id); }
    // 悬停过渡：当前控件按 hoverAmount_ 从 Normal 淡入 Hovered。
    // **离开不做淡出**（原来是 hot→normal 淡 90ms）：真机反馈"会闪过一个虚影然后消失"——
    // 那 90ms 里旧按钮还带着高亮底+描边，在快速划过时就是一次可见的闪烁。
    // 进入时淡入仍然保留（有"亮起来"的手感），离开立即恢复，没有残影。
    widgets::ButtonSkin hoverSkin(ControlId id,widgets::ButtonVariant variant,
        const ThemePalette& palette) const {
        if(hoverControl_!=static_cast<int>(id))
            return widgets::ButtonSkinOf(variant,widgets::Visual::Normal,palette);
        const auto normal=widgets::ButtonSkinOf(variant,widgets::Visual::Normal,palette);
        const auto hot=widgets::ButtonSkinOf(variant,widgets::Visual::Hovered,palette);
        return widgets::BlendButtonSkin(normal,hot,hoverAmount_);
    }
    // Thin wrapper around DrawIcon: the panel always strokes with its own round-cap style.
    void Icon(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,
        float thickness=2.0f);
    void Lamp(ID2D1RenderTarget* target,const LampLayout& lamp,const LampView& view,const ThemePalette& palette);
    void Chip(ID2D1RenderTarget* target,D2D1_RECT_F box,const ChipView& chip,bool hovered,
        const ThemePalette& palette);
    void Hero(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void Offline(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void Button(ID2D1RenderTarget* target,D2D1_RECT_F rect,const std::string& label,IconId icon,
        const widgets::ButtonSkin& skin,bool disabled);
    // ---- models page: search row, filter segments, the scrolling list (DESIGN §4.3 #9-#13) ----
    void SearchRow(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette,const TextField* field);
    void Filters(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void List(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void GroupHead(ID2D1RenderTarget* target,const ListItem& item,const PanelViewModel::ModelGroup& group,
        const ThemePalette& palette);
    void ModelRow(ID2D1RenderTarget* target,const ListItem& item,const PanelViewModel::ModelRow& row,
        bool flatView,const ThemePalette& palette);
    // ---- H5 其余四页 ----
    void FallbackPage(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void PageHead(ID2D1RenderTarget* target,const PanelLayout& layout,const std::string& title,
        const std::string& subtitle,const std::string& action,IconId actionIcon,bool actionHovered,
        const ThemePalette& palette);
    // A form field (label above a box): text / password / select / switch, shared by the H5 forms.
    void Field(ID2D1RenderTarget* target,const PanelLayout::FieldBox& box,const std::string& label,
        const std::string& value,bool focused,bool masked,const char* hint,const ThemePalette& palette);
    // v1.21：带插入符的表单字段（caretIndex<0 时行为与 Field 完全相同）。
    void FieldWithCaret(ID2D1RenderTarget* target,const PanelLayout::FieldBox& box,const std::string& label,
        const std::string& value,bool focused,bool masked,const char* hint,const ThemePalette& palette,
        int caretIndex,bool multiline);
    // 添加 / 备份 / 设置三页（H5b/H5c）：页内滚动 + 卡片流。
    void AdminPages(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void SegRow(ID2D1RenderTarget* target,const PanelLayout& layout,
        const std::vector<PanelViewModel::FilterSeg>& segs,std::size_t firstSegment,
        const ThemePalette& palette,bool bordered=false);
    void SwitchLine(ID2D1RenderTarget* target,D2D1_RECT_F row,const std::string& label,
        const std::string& note,bool on,bool hovered,const ThemePalette& palette);
    void Modal(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void Toast(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    void Nav(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
        const ThemePalette& palette);
    // Which control the pointer is over, set per frame by Draw's caller through the view model.
    int hoverControl_=0;
    int hoverIndex_=-1;      // 悬停控件的行号（ModelRow / 列表项这类重复控件）
    int hoverPrevious_=0;         // 历史字段（离开淡出已移除，见 hoverSkin；保留供控制面观测）
    float hoverAmount_=1.0f;      // 悬停过渡进度（0..1）
    ListHit hoverItem_;                 // the list item under the pointer (row/head/star hover states)
    int cursorIndex_=-1;                // view.cursor, cached for the row painter
    int hoverKnob_=0;                   // 阶梯行里指针压住的那个按钮（1 上移 / 2 下移 / 3 移除）
    int hoverSegment_=-1;               // 指针压住的分段控件（layout.segments 的下标）
    bool caretOn_=false;   // caret blink phase; the WM_TIMER tick flips it through SetCaretOn
public:
    void SetCaretOn(bool on) { caretOn_=on; }
private:

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> arcStyle_;
    Microsoft::WRL::ComPtr<ID2D1Factory> styleFactory_;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> strip_,fade_;
    // 液态玻璃的桌面快照（由 Surface 抓取+模糊，这里按 generation 缓存成 D2D 位图）。
    Microsoft::WRL::ComPtr<ID2D1Bitmap> backdrop_;
    unsigned backdropGeneration_=0;
    ID2D1RenderTarget* stripOwner_=nullptr;
    ID2D1RenderTarget* fadeOwner_=nullptr;
    IDWriteFactory* factoryText_=nullptr;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> brand_,heroProvider_,heroModel_,label_,body_,caption_,
        mono_,tiny_,lamp_,chip_,busy_,sync_,count_,navBadge_,groupName_,pillText_,emptyText_,
        // 贴条悬浮卡的模型名：**不截断**（真机要求"模型名称显示完全"）。卡片宽度按内容自适应，
        // 极端长的模型名仍然靠 cardWidth 上限兜底，这一行只负责别主动加省略号。
        cardBody_;
    float scale_=1.0f;
    ID2D1RenderTarget* owner_=nullptr;
};
}
