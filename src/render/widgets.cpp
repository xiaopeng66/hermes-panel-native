#include "render/widgets.h"
#include <algorithm>
#include <wrl/client.h>

namespace hermes::widgets {
namespace {
D2D1_COLOR_F Transparent() { return D2D1::ColorF(0.0f,0.0f,0.0f,0.0f); }
D2D1_COLOR_F OpaqueBlack() { return D2D1::ColorF(0.0f,0.0f,0.0f,1.0f); }
D2D1_COLOR_F Fade(const D2D1_COLOR_F& colour,float alpha) {
    return D2D1::ColorF(colour.r,colour.g,colour.b,alpha);
}
}

LampSkin LampSkinOf(LampState state,const ThemePalette& palette) {
    // styles.css: .lamp i takes the semantic colour plus a 2px soft ring; data-off="idle" drops the
    // ring. The label stays dim in every state - only the dot is the signal (INV-H6).
    LampSkin skin;
    skin.glow=state!=LampState::Idle;
    skin.dot=palette.LampColour(static_cast<int>(state));
    skin.halo=skin.glow?Fade(skin.dot,0.5f):Transparent();
    skin.label=palette.dim;
    return skin;
}

ChipSkin ChipSkinOf(ChipKind kind,Visual visual,const ThemePalette& palette) {
    // data-kind="warn"/"err" are outline chips (transparent fill, 46% border of the same hue); the
    // plain chip sits on raise2 with dim text. :hover { filter: brightness(1.12) } is reproduced by
    // hardening the border to full opacity (and for the plain chip a raise2 -> line fill step).
    ChipSkin skin;
    skin.fill=kind==ChipKind::Ok?palette.raise2:Transparent();
    skin.border=Transparent();
    // 注意：**不能**把 ChipKind 强转成 int 再喂 LampColour —— 两个枚举的序号不一样
    // （ChipKind: Ok=0/Warn=1/Err=2；ThemePalette::LampColour: 0=idle/1=ok/2=warn/3=err）。
    // 错位的后果是真机上「WebUI 不可达」（err）画成琥珀、「网关待重启」（warn）画成绿色。
    const auto chipColour=[&] {
        switch(kind) {
        case ChipKind::Ok: return palette.ok;
        case ChipKind::Warn: return palette.warn;
        default: return palette.err;
        }
    }();
    skin.text=kind==ChipKind::Ok?palette.dim:chipColour;
    if(kind!=ChipKind::Ok) skin.border=Fade(skin.text,kChipBorderOpacity);
    if(visual==Visual::Hovered) {
        if(kind==ChipKind::Ok) skin.fill=palette.line;
        else skin.border=Fade(skin.text,1.0f);
    }
    return skin;
}

ButtonSkin ButtonSkinOf(ButtonVariant variant,Visual visual,const ThemePalette& palette) {
    ButtonSkin skin;
    skin.alpha=1.0f;
    skin.border=Transparent();
    switch(variant) {
    case ButtonVariant::Primary:
        // .btn.primary: accent fill, no border, on-accent text; hover = brightness(1.08).
        skin.fill=visual==Visual::Hovered?Lighten(palette.accent,kHoverBrightness):palette.accent;
        skin.text=palette.onAccent;
        break;
    case ButtonVariant::Ghost:
        // 悬停底 + 描边：以前只用 raise2 当填充，而浅色主题（porcelain/sand/light）的
        // raise2 与页面底色 ink 差值只有 0–9（实测 porcelain 差 (0,1,3)）——悬停几乎不可见，
        // 真机看到的就是"闪过一个虚影/点了没反应"。改成 raise2 底 + accentLine 描边 + text 色：
        // 描边是实色（accent 55%），在白底/浅灰底上都能看出边界，六套主题一致。
        skin.fill=visual==Visual::Hovered?palette.raise2:Transparent();
        skin.border=visual==Visual::Hovered?palette.accentLine:Transparent();
        skin.text=visual==Visual::Hovered?palette.text:palette.dim;
        break;
    case ButtonVariant::Danger:
        skin.fill=visual==Visual::Hovered?palette.errSoft:Transparent();
        skin.border=Fade(palette.err,0.4f);
        skin.text=palette.err;
        break;
    case ButtonVariant::Normal:
        skin.fill=visual==Visual::Hovered?palette.raise2:palette.raise;
        skin.border=palette.line;
        skin.text=palette.text;
        break;
    }
    if(visual==Visual::Disabled) skin.alpha=kDisabledOpacity;
    return skin;
}
ButtonSkin BlendButtonSkin(const ButtonSkin& from,const ButtonSkin& to,float t) {
    // 逐分量线性混合（含 alpha）：0 = from，1 = to。
    t=t<0.0f?0.0f:(t>1.0f?1.0f:t);
    const auto mix=[t](const D2D1_COLOR_F& a,const D2D1_COLOR_F& b) {
        return D2D1::ColorF(a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t);
    };
    ButtonSkin skin;
    skin.fill=mix(from.fill,to.fill);
    skin.border=mix(from.border,to.border);
    skin.text=mix(from.text,to.text);
    skin.alpha=from.alpha+(to.alpha-from.alpha)*t;
    return skin;
}

SegSkin SegSkinOf(bool active,bool hovered,const ThemePalette& palette) {
    // .seg.active: accent-soft fill, accent-line border, accent-strong text; the mono count inside is
    // faint when idle and accent-strong at 70% when active.
    SegSkin skin;
    if(active) {
        skin.fill=palette.accentSoft;
        skin.border=palette.accentLine;
        skin.text=palette.accentStrong;
        skin.count=Fade(palette.accentStrong,0.7f);
            return skin;
    }
    skin.fill=hovered?palette.raise2:Transparent();
    skin.border=Transparent();
    skin.text=hovered?palette.text:palette.dim;
    skin.count=palette.faint;
    return skin;
}

SwitchSkin SwitchSkinOf(bool on,bool disabled,const ThemePalette& palette) {
    SwitchSkin skin;
    if(on) {
        skin.track=palette.accentSoft;
        skin.border=palette.accentLine;
        skin.knob=palette.accentStrong;
    } else {
        skin.track=palette.raise2;
        skin.border=palette.line;
        skin.knob=palette.faint;
    }
    skin.alpha=disabled?kDisabledOpacity:1.0f;
    return skin;
}

D2D1_RECT_F HaloRect(const D2D1_RECT_F& dot) {
    return D2D1::RectF(dot.left-kLampHalo,dot.top-kLampHalo,dot.right+kLampHalo,dot.bottom+kLampHalo);
}

D2D1_RECT_F SwitchKnobRect(const D2D1_RECT_F& track,bool on,float scale) {
    // 度量是 DIP：轨道在物理像素里，滑钮必须跟着 scale 走，否则在 125% 屏上会偏出胶囊。
    const float inset=kSwitchInset*scale;
    const float travel=kSwitchTravel*scale;
    const float knob=kSwitchKnob*scale;
    const float x=track.left+inset+(on?travel:0.0f);
    // 旧 CSS .sw i { border:1px } + .sw i::after { top:2px; left:2px; width/height 10 }：
    // ::after 定位的参照是**padding box**（边框内侧），所以外框到圆点是 1+2 = 3px，上下左右一致
    // （真机反馈"小胶囊里的圆还是有点偏上"就是少算了这 1px 边框）。
    const float y=track.top+inset;
    return D2D1::RectF(x,y,x+knob,y+knob);
}

D2D1_RECT_F StarRect(const D2D1_RECT_F& row,float scale) {
    const float size=kStarBox*scale;
    return D2D1::RectF(row.right-kRowPadRight*scale-size,row.top+(row.bottom-row.top-size)/2.0f,
        row.right-kRowPadRight*scale,row.top+(row.bottom-row.top+size)/2.0f);
}

D2D1_RECT_F MenuButtonRect(const D2D1_RECT_F& head,float scale) {
    const float size=kMenuButtonSize*scale;
    return D2D1::RectF(head.right-kGroupHeadPadX*scale-size,head.top+(head.bottom-head.top-size)/2.0f,
        head.right-kGroupHeadPadX*scale,head.top+(head.bottom-head.top+size)/2.0f);
}

D2D1_COLOR_F IdentityColour(float hueDegrees) { return HslColour(hueDegrees,0.68f,0.62f); }
D2D1_COLOR_F IdentityTint(float hueDegrees,float alpha) { return HslColour(hueDegrees,0.62f,0.62f,alpha); }
D2D1_COLOR_F GroupNameColour(float hueDegrees,bool light) {
    // .group-head.active .gname: hsl(--ph 72% 75%) on dark, hsl(--ph 62% 40%) on light.
    return light?HslColour(hueDegrees,0.62f,0.40f):HslColour(hueDegrees,0.72f,0.75f);
}

RowSkin RowSkinOf(bool current,bool cursor,const Visual& visual,float hue,const ThemePalette& palette) {
    // .mrow: hover raise, active raise2, current = identity tint at 7.5% + a 2px inset bar; the cursor
    // (.is-cursor) is a 1.5px accent outline inside the row on a raise2 fill. The provider column is
    // dim, the model id is the row's text, the star is faint until it is on or hovered.
    RowSkin skin;
    skin.fill=visual==Visual::Hovered?palette.raise:(current?IdentityTint(hue,0.075f):Transparent());
    skin.outline=Transparent();
    skin.pcol=palette.dim;
    skin.model=palette.text;
    skin.star=palette.faint;
    skin.arrow=palette.accentStrong;
    skin.underline=true;
    if(cursor) {
        skin.fill=palette.raise2;
        skin.outline=palette.accent;
    }
    return skin;
}

GroupHeadSkin GroupHeadSkinOf(bool active,bool hovered,float hue,const ThemePalette& palette) {
    // .group-head: surf, hover raise, a hairline underneath; the active head carries a 2px identity
    // inset bar and tints its name.
    GroupHeadSkin skin;
    skin.fill=hovered?palette.raise:palette.surf;
    skin.name=active?GroupNameColour(hue,palette.light):palette.dim;
    skin.countFill=palette.raise2;
    skin.countText=palette.faint;
    skin.tagFill=palette.raise2;
    skin.tagText=palette.faint;
    skin.inset=active?IdentityColour(hue):Transparent();
    skin.hairline=true;
    return skin;
}

float NavPillWidth() { return 34.0f; }
float NavPillHeight() { return 23.0f; }
float NavPillTop() { return 5.0f; }
float NavPillRadius() { return 9.0f; }

void DrawLampDot(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& dot,
    LampState state,const ThemePalette& palette,float scale) {
    const auto skin=LampSkinOf(state,palette);
    const auto radius=kLampDotRadius*scale;
    if(skin.glow) {
        brush->SetColor(skin.halo);
        target->FillRoundedRectangle(D2D1::RoundedRect(HaloRect(dot),radius+kLampHalo*scale,
            radius+kLampHalo*scale),brush);
    }
    brush->SetColor(skin.dot);
    target->FillRoundedRectangle(D2D1::RoundedRect(dot,radius,radius),brush);
}

void DrawHairline(ID2D1RenderTarget* target,const D2D1_RECT_F& row,const ThemePalette& palette,float scale) {
    // --hx-hair: 1px, the mid colour only between 14% and 86%, transparent at both ends.
    const auto hair=Fade(palette.hair,palette.hair.a);
    const D2D1_GRADIENT_STOP stops[4]{{0.0f,Fade(hair,0.0f)},{kHairFadeStart,hair},
        {kHairFadeEnd,hair},{1.0f,Fade(hair,0.0f)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if(FAILED(target->CreateGradientStopCollection(stops,4,collection.GetAddressOf()))) return;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> brush;
    if(FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(row.left,row.top),D2D1::Point2F(row.right,row.top)),collection.Get(),brush.GetAddressOf()))) return;
    target->FillRectangle(D2D1::RectF(row.left,row.top,row.right,row.top+std::max(1.0f*scale,1.0f)),brush.Get());
}

void DrawChipShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const ChipSkin& skin,float scale) {
    const auto radius=kChipRadius*scale;
    if(skin.fill.a>0.0f) {
        brush->SetColor(skin.fill);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush);
    }
    if(skin.border.a>0.0f) {
        brush->SetColor(skin.border);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush,1.0f*scale);
    }
}

void DrawButtonShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const ButtonSkin& skin,float scale) {
    const auto radius=kButtonRadius*scale;
    if(skin.fill.a>0.0f) {
        brush->SetColor(Fade(skin.fill,skin.fill.a*skin.alpha));
        target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush);
    }
    if(skin.border.a>0.0f) {
        brush->SetColor(Fade(skin.border,skin.border.a*skin.alpha));
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush,1.0f*scale);
    }
}

void DrawSegShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,
    const SegSkin& skin,float scale) {
    const auto radius=kSegRadius*scale;
    if(skin.fill.a>0.0f) {
        brush->SetColor(skin.fill);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush);
    }
    if(skin.border.a>0.0f) {
        brush->SetColor(skin.border);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush,1.0f*scale);
    }
}

void DrawSwitchShape(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& track,
    const SwitchSkin& skin,bool on,float scale) {
    const auto radius=track.bottom-track.top>=1.0f?(track.bottom-track.top)/2.0f:1.0f;
    brush->SetColor(Fade(skin.track,skin.track.a*skin.alpha));
    target->FillRoundedRectangle(D2D1::RoundedRect(track,radius,radius),brush);
    if(skin.border.a>0.0f) {
        brush->SetColor(Fade(skin.border,skin.border.a*skin.alpha));
        target->DrawRoundedRectangle(D2D1::RoundedRect(track,radius,radius),brush,1.0f*scale);
    }
    const auto knob=SwitchKnobRect(track,on,scale);
    const auto centre=D2D1::Point2F((knob.left+knob.right)/2.0f,(knob.top+knob.bottom)/2.0f);
    brush->SetColor(Fade(skin.knob,skin.knob.a*skin.alpha));
    target->FillEllipse(D2D1::Ellipse(centre,(knob.right-knob.left)/2.0f,(knob.bottom-knob.top)/2.0f),brush);
}

void DrawSoftGlow(ID2D1RenderTarget* target,const D2D1_POINT_2F& centre,float dotRadius,float blur,
    const D2D1_COLOR_F& colour,float alpha,float scale) {
    if(blur<=0.0f||alpha<=0.0f) return;
    const float inner=dotRadius/scale;                     // gradient 空间按外框归一化
    const float outer=blur;
    const D2D1_GRADIENT_STOP stops[3]{{0.0f,Fade(colour,alpha)},
        {std::max(0.01f,inner/(inner+outer)),Fade(colour,alpha*0.55f)},
        {1.0f,Fade(colour,0.0f)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if(FAILED(target->CreateGradientStopCollection(stops,3,collection.GetAddressOf()))) return;
    Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> gradient;
    const float radius=(dotRadius+blur*scale);
    if(FAILED(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(centre,
        D2D1::Point2F(0,0),radius,radius),collection.Get(),gradient.GetAddressOf()))) return;
    target->FillEllipse(D2D1::Ellipse(centre,radius,radius),gradient.Get());
}

void DrawPlate(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,const D2D1_RECT_F& box,bool swapping,
    const ThemePalette& palette,float scale) {
    // .plate: a 1px drop shadow (0 1px 2px rgba(0,0,0,.45)) approximated by an offset under-fill,
    // the plate gradient, the inset top highlight, and - while swapping - an accent-line outline.
    const auto radius=kPlateRadius*scale;
    brush->SetColor(Fade(OpaqueBlack(),0.45f*0.6f));
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left,box.top+1.0f*scale,
        box.right,box.bottom+1.0f*scale),radius,radius),brush);
    const D2D1_GRADIENT_STOP stops[2]{{0.0f,palette.plateTop},{1.0f,palette.plateBottom}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if(FAILED(target->CreateGradientStopCollection(stops,2,collection.GetAddressOf()))) return;
    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient;
    if(FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
        D2D1::Point2F(0,box.top),D2D1::Point2F(0,box.bottom)),collection.Get(),gradient.GetAddressOf()))) return;
    target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),gradient.Get());
    brush->SetColor(palette.highlight);
    target->DrawLine(D2D1::Point2F(box.left+radius,box.top+1.0f*scale),
        D2D1::Point2F(box.right-radius,box.top+1.0f*scale),brush,1.0f*scale);
    if(swapping) {
        brush->SetColor(palette.accentLine);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush,1.0f*scale);
    }
}

void DrawHeroBackdrop(ID2D1RenderTarget* target,const D2D1_RECT_F& box,float hueDegrees,
    const ThemePalette& palette,float scale) {
    // .hero: linear(hero-a -> hero-b 78%) is the base, the provider identity tint
    // radial(52% x 150% at left) sits on top, and a 1px line closes the strip at the bottom.
    const D2D1_GRADIENT_STOP stops[2]{{0.0f,palette.heroTop},{0.78f,palette.heroBottom}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    if(FAILED(target->CreateGradientStopCollection(stops,2,collection.GetAddressOf()))) return;
    {
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> gradient;
        if(FAILED(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
            D2D1::Point2F(0,box.top),D2D1::Point2F(0,box.bottom)),collection.Get(),gradient.GetAddressOf()))) return;
        target->FillRectangle(box,gradient.Get());
    }
    {
        const D2D1_GRADIENT_STOP tint[2]{{0.0f,HslColour(hueDegrees,0.70f,0.62f,0.12f)},
            {0.72f,HslColour(hueDegrees,0.70f,0.62f,0.0f)}};
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> tintStops;
        if(FAILED(target->CreateGradientStopCollection(tint,2,tintStops.GetAddressOf()))) return;
        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> radial;
        if(FAILED(target->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(
            D2D1::Point2F(box.left,(box.top+box.bottom)/2.0f),
            D2D1::Point2F(0,0),0.52f*(box.right-box.left),1.5f*(box.bottom-box.top)),tintStops.Get(),
            radial.GetAddressOf()))) return;
        target->FillRectangle(box,radial.Get());
    }
    (void)scale;
}

void DrawScrollBar(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,
    const D2D1_RECT_F& track,const D2D1_RECT_F& thumb,bool horizontal,bool hot,
    const ThemePalette& palette,float scale) {
    // 轨道是"凹槽"不是"条"：整条铺一遍很淡的底，告诉用户这里可滚；拇指才是可抓的东西。
    // （弹窗横向滑条以前反过来：轨道 line 实色、拇指 line2 只有 5–9% 墨 —— 白卡片上拇指消失，
    //   整条看着像正文下的一根分割线，用户直接报告"看不见"。）
    if(track.right>track.left&&track.bottom>track.top) {
        const float trackRadius=horizontal?(track.bottom-track.top)/2.0f:(track.right-track.left)/2.0f;
        brush->SetColor(palette.scrollTrack);
        target->FillRoundedRectangle(D2D1::RoundedRect(track,trackRadius,trackRadius),brush);
    }
    auto handle=thumb;
    if(hot) {
        // 悬停：沿"粗"的方向各胀 1 DIP（竖条是左右、横条是上下），总增量 2 DIP。
        if(horizontal) { handle.top-=1.0f*scale;handle.bottom+=1.0f*scale; }
        else { handle.left-=1.0f*scale;handle.right+=1.0f*scale; }
    }
    if(handle.right<=handle.left||handle.bottom<=handle.top) return;
    const float radius=horizontal?(handle.bottom-handle.top)/2.0f:(handle.right-handle.left)/2.0f;
    brush->SetColor(hot?palette.scrollThumbHot:palette.scrollThumb);
    target->FillRoundedRectangle(D2D1::RoundedRect(handle,radius,radius),brush);
}
}
