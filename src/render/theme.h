#pragma once
#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <string>

namespace hermes {
// The hermes panel's own visual language ("深色信号台", DESIGN §3): tokens taken verbatim from the old
// renderer's styles.css. The lamp/hero treatments are the only place colour is allowed to speak; the rest
// stays quiet. Field names follow the old CSS custom properties.
struct ThemePalette {
    // surfaces
    D2D1_COLOR_F ink;             // --hx-ink    window background
    D2D1_COLOR_F surf;            // --hx-surf   panels / cards
    D2D1_COLOR_F raise;           // --hx-raise  inputs, chips, list hover
    D2D1_COLOR_F raise2;          // --hx-raise2 pressed / nested
    D2D1_COLOR_F line;            // --hx-line   borders
    D2D1_COLOR_F line2;           // --hx-line2  hairlines and separators
    // text
    D2D1_COLOR_F text;            // --hx-text
    D2D1_COLOR_F dim;             // --hx-dim
    D2D1_COLOR_F faint;           // --hx-faint
    // accent + semantics
    D2D1_COLOR_F accent;          // --hx-accent
    D2D1_COLOR_F accentStrong;    // --hx-accent-strong
    D2D1_COLOR_F onAccent;        // --hx-on-accent
    D2D1_COLOR_F ok;              // --hx-ok
    D2D1_COLOR_F warn;            // --hx-warn
    D2D1_COLOR_F err;             // --hx-err
    D2D1_COLOR_F idle;            // --hx-idle
    // soft fills for chips / lamp halos
    D2D1_COLOR_F okSoft,warnSoft,errSoft,accentSoft,accentLine;
    // hero and plate gradients (top -> bottom), plus the top highlight
    D2D1_COLOR_F heroTop,heroBottom,plateTop,plateBottom;
    D2D1_COLOR_F highlight;       // --hx-hi
    D2D1_COLOR_F hair;            // --hx-hair mid-stop (the gradient fades to transparent at both ends)
    // edge sliver
    D2D1_COLOR_F stripTop,stripBottom,stripHoverTop,stripHoverBottom;
    bool light=false;
    // 刻意**没有**拟态（emboss/渐变材质）那一套令牌：用户在预览里挑的是四套平面风格，
    // 拟态整套已弃用（见 DESIGN 修订记录）。控件描边与阴影由 widgets 的 skin 表达。
    // Lamp colour for a state, so the renderer and the tests agree on one mapping.
    D2D1_COLOR_F LampColour(int state) const;   // 0 idle, 1 ok, 2 warn, 3 err
};
// hsl(a) -> rgba, used for the provider identity hues (.hy0..hy7, hue 186..264) of the hero lamp
// and the radial tint behind the current model (styles.css §"供应商身份色").
D2D1_COLOR_F HslColour(float hueDegrees,float saturation,float lightness,float alpha=1.0f);
// brightness(k): the old stylesheet's hover filter, mixed towards white. k<=0 returns the input.
D2D1_COLOR_F Lighten(const D2D1_COLOR_F& colour,float amount);
// Follows the system (registry AppsUseLightTheme) unless settings say dark/light/system.
ThemePalette ResolveTheme(const std::string& preference);
// 主题交叉淡入：按进度 t（0=旧、1=新）逐令牌混合两份调色板。移植自 balance-panel-native。
ThemePalette BlendPalette(const ThemePalette& from,const ThemePalette& to,float t);
}
