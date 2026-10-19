#pragma once
#include <d2d1.h>
#include "app/panel_view.h"
#include "render/theme.h"

// The basic control layer (PLAN §6 H2b): every number and colour pair here comes verbatim from the old
// stylesheet (renderer/styles.css), which is the visual contract (DESIGN §3/§4, INV-H3/INV-H6). The
// *Skin functions choose colours, the k* constants choose boxes, and both the renderer and the widget
// tests / widget gallery read this one file, so the sample sheet can never drift from the panel.
namespace hermes::widgets {
// --- lamp (.lamp): 6x6 square dot, corner radius 2, 2px soft halo ring, 5px gap, 10px label
constexpr float kLampDot=6.0f,kLampDotRadius=2.0f,kLampHalo=2.0f,kLampGap=5.0f,kLampFont=10.0f;
// --- chip (.chip): 10.5px text + 2*2.5px padding + 1px border = 22 tall, radius 6, 7px side padding
constexpr float kChipHeight=22.0f,kChipRadius=6.0f,kChipPadX=7.0f,kChipGap=5.0f,kChipFont=10.5f,
    kChipIcon=11.0f,kChipBorderOpacity=0.46f;
// --- button (.btn): 11.5px text + 2*6px padding + 1px border = 30 tall, radius --r 9, 11px side padding
constexpr float kButtonHeight=30.0f,kButtonRadius=9.0f,kButtonPadX=11.0f,kButtonIconGap=6.0f,
    kButtonFont=11.5f,kButtonTinyFont=10.5f,kButtonTinyHeight=21.0f,kButtonTinyPadX=8.0f,
    kButtonIconOnly=30.0f,kDisabledOpacity=0.45f,kHoverBrightness=0.08f;
// --- seg (.seg): 11.5px text + 2*2.5px padding + 1px border = 24 tall, radius 6, 8px side padding
constexpr float kSegHeight=24.0f,kSegRadius=6.0f,kSegPadX=8.0f,kSegFont=11.5f,kSegCountFont=9.5f;
// --- switch (.sw): 28x16 pill, a 10px knob inset 2px from the left, travelling 12px when checked
constexpr float kSwitchWidth=28.0f,kSwitchHeight=16.0f,kSwitchKnob=10.0f,kSwitchInset=3.0f,kSwitchTravel=12.0f,
    kRowFont=11.5f;
// --- icon button (.icb) in the title bar: 24x24, radius 6
constexpr float kIcbSize=24.0f,kIcbRadius=6.0f;
// --- plate (.plate): radius 8, 9px side padding, 14px model id (13 while swapping)
constexpr float kPlateRadius=8.0f,kPlatePadX=9.0f,kPlatePadTop=3.0f,kPlatePadBottom=4.0f,
    kPlateFont=14.0f,kPlateSwappingFont=13.0f;
// --- hero (.hero): full-width strip, identity tinted; its lamp is 8px with a 3px ring and a soft glow
constexpr float kHeroLamp=8.0f,kHeroLampRing=3.0f,kHeroLampGlow=13.0f,kDefaultHue=200.0f;
// --- list row and global radius (--row / --r), hairline stops (--hx-hair)
constexpr float kRowHeight=31.0f,kRadius=9.0f,kHairFadeStart=0.14f,kHairFadeEnd=0.86f;
// --- 列表：.group-head 30 / .mrow --row 31 / .sect / .empty；行内 .star 22、.pill 15、.pcol 86
constexpr float kGroupHeadHeight=30.0f,kGroupHeadPadX=11.0f,kGroupHeadGap=7.0f,kCaretBox=14.0f,
    kGroupDot=6.0f,kGroupDotRadius=2.0f,kMenuButtonSize=20.0f,kMenuIcon=12.0f,kMenuIdleOpacity=0.45f;
constexpr float kRowPadLeft=31.0f,kRowPadRight=9.0f,kRowGap=8.0f,kPcolWidth=86.0f,kPcolFont=10.5f,
    kRowModelFont=11.5f,kStarBox=22.0f,kStarGlyph=12.0f,kStarIdleOpacity=0.0f,kStarHoverOpacity=0.6f;
constexpr float kPillHeight=15.0f,kPillRadius=999.0f,kPillPadX=6.0f,kPillFont=9.5f;
constexpr float kCountPillHeight=15.0f,kCountPillRadius=4.0f,kCountPillPadX=4.5f;
constexpr float kGroupNameFont=12.0f,kGroupCountFont=9.5f,kSectFont=10.5f,kEmptyFont=11.5f;
constexpr float kScrollBarWidth=6.0f,kScrollBarHoverWidth=8.0f,kListFade=16.0f;
constexpr float kBannerHeight=2.0f;              // body.busy .app::before sweep bar

// Interaction states (PLAN §4). The stylesheet only styles :hover and [disabled]; a pressed button
// keeps its normal look (no scale-down) - the old panel had no :active rule either.
enum class Visual { Normal,Hovered,Disabled };
enum class ButtonVariant { Normal,Primary,Ghost,Danger };

struct LampSkin { D2D1_COLOR_F dot,halo,label; bool glow; };
struct ChipSkin { D2D1_COLOR_F fill,border,text; };
struct ButtonSkin { D2D1_COLOR_F fill,border,text; float alpha; };
struct SegSkin { D2D1_COLOR_F fill,border,text,count; };
struct SwitchSkin { D2D1_COLOR_F track,border,knob; float alpha; };
struct RowSkin { D2D1_COLOR_F fill,outline,pcol,model,star,arrow; bool underline; };
struct GroupHeadSkin { D2D1_COLOR_F fill,name,countFill,countText,tagFill,tagText,inset; bool hairline; };

LampSkin LampSkinOf(LampState state,const ThemePalette& palette);
ChipSkin ChipSkinOf(ChipKind kind,Visual visual,const ThemePalette& palette);
ButtonSkin ButtonSkinOf(ButtonVariant variant,Visual visual,const ThemePalette& palette);
// 悬停过渡（移植自 balance-panel-native）：在 Normal 与 Hovered 两套皮肤之间按进度混合，
// 用于"上一个控件淡出、当前控件淡入"的 90ms 交叉过渡。t=1 即完全 Hovered。
ButtonSkin BlendButtonSkin(const ButtonSkin& from,const ButtonSkin& to,float t);
SegSkin SegSkinOf(bool active,bool hovered,const ThemePalette& palette);
SwitchSkin SwitchSkinOf(bool on,bool disabled,const ThemePalette& palette);
// The model row (.mrow) and its group header (.group-head): both carry the provider identity hue, and
// both have a "current/active" shade plus a 2px inset bar in the identity colour.
RowSkin RowSkinOf(bool current,bool cursor,const Visual& visual,float hue,const ThemePalette& palette);
GroupHeadSkin GroupHeadSkinOf(bool active,bool hovered,float hue,const ThemePalette& palette);
// 供应商身份色：hsl(--ph 68% 62%)。灯、组头圆点、行内浅底都用它，主题不改变它（styles.css 只在
// .group-head.active .gname 上分深浅两套）。
D2D1_COLOR_F IdentityColour(float hueDegrees);
D2D1_COLOR_F IdentityTint(float hueDegrees,float alpha);
D2D1_COLOR_F GroupNameColour(float hueDegrees,bool light);

// Geometry contracts the tests assert and the renderer reuses.
D2D1_RECT_F HaloRect(const D2D1_RECT_F& dot);              // the lamp's ring around the dot
D2D1_RECT_F SwitchKnobRect(const D2D1_RECT_F& track,bool on,float scale=1.0f);
D2D1_RECT_F StarRect(const D2D1_RECT_F& row,float scale);   // .star 22px at the row's right end
D2D1_RECT_F MenuButtonRect(const D2D1_RECT_F& head,float scale);
float NavPillWidth(),NavPillHeight(),NavPillTop(),NavPillRadius();   // .tab.active::before

// Drawing primitives. Text and icons stay with the caller (panel.cpp owns the text formats); these
// draw only fills, borders, halos and gradients so the tests can assert the shapes without a device.
void DrawLampDot(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& dot,
    LampState state,const ThemePalette& palette,float scale);
void DrawHairline(ID2D1RenderTarget* target,const D2D1_RECT_F& row,const ThemePalette& palette,float scale);
void DrawChipShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const ChipSkin& skin,float scale);
void DrawButtonShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const ButtonSkin& skin,float scale);
void DrawSegShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const SegSkin& skin,float scale);
void DrawSwitchShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& track,
    const SwitchSkin& skin,bool on,float scale);
void DrawPlate(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,bool swapping,
    const ThemePalette& palette,float scale);
// 旧 CSS 的 `box-shadow: 0 0 Npx colour`：**模糊**光晕（不是实心圆盘）。用径向渐变画：
// 圆心 = 灯点边缘处 alpha，半径 R 处归零。
void DrawSoftGlow(ID2D1RenderTarget* target,const D2D1_POINT_2F& centre,float dotRadius,float blur,
    const D2D1_COLOR_F& colour,float alpha,float scale);
// 旧 CSS 的 `box-shadow: 0 0 Npx colour`：**模糊**光晕（不是实心圆盘）。用径向渐变画：
// 圆心 = 灯点边缘处 alpha，半径 R 处归零。
void DrawSoftGlow(ID2D1RenderTarget* target,const D2D1_POINT_2F& centre,float dotRadius,float blur,
    const D2D1_COLOR_F& colour,float alpha,float scale);
void DrawHeroBackdrop(ID2D1RenderTarget* target,const D2D1_RECT_F& box,float hueDegrees,
    const ThemePalette& palette,float scale);
}
