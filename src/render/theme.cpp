#include "theme.h"
#include <windows.h>
#include <cmath>

namespace hermes {
namespace {
D2D1_COLOR_F Colour(unsigned rgb,float alpha=1.0f) {
    return D2D1::ColorF(((rgb>>16)&0xFF)/255.0f,((rgb>>8)&0xFF)/255.0f,(rgb&0xFF)/255.0f,alpha);
}
bool SystemPrefersLight() {
    DWORD value=1;
    DWORD size=sizeof(value);
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        0,KEY_QUERY_VALUE,&key)!=ERROR_SUCCESS) return false;
    DWORD type=0;
    const auto status=RegQueryValueExW(key,L"AppsUseLightTheme",nullptr,&type,reinterpret_cast<BYTE*>(&value),&size);
    RegCloseKey(key);
    return status==ERROR_SUCCESS&&type==REG_DWORD&&value!=0;
}
float Clamp01(float value) { return value<0.0f?0.0f:(value>1.0f?1.0f:value); }
}

D2D1_COLOR_F HslColour(float hueDegrees,float saturation,float lightness,float alpha) {
    // Standard CSS hsl(): C and X per hue sextant, then (r,g,b) rotated to the right sixth.
    saturation=Clamp01(saturation);
    lightness=Clamp01(lightness);
    const float chroma=(1.0f-std::abs(2.0f*lightness-1.0f))*saturation;
    const float scaledHue=std::fmod(std::fmod(hueDegrees,360.0f)+360.0f,360.0f)/60.0f;
    const float secondary=chroma*(1.0f-std::abs(std::fmod(scaledHue,2.0f)-1.0f));
    float red=0,green=0,blue=0;
    if(scaledHue<1.0f) { red=chroma;green=secondary; }
    else if(scaledHue<2.0f) { red=secondary;green=chroma; }
    else if(scaledHue<3.0f) { green=chroma;blue=secondary; }
    else if(scaledHue<4.0f) { green=secondary;blue=chroma; }
    else if(scaledHue<5.0f) { red=secondary;blue=chroma; }
    else { red=chroma;blue=secondary; }
    const float offset=lightness-chroma/2.0f;
    return D2D1::ColorF(Clamp01(red+offset),Clamp01(green+offset),Clamp01(blue+offset),Clamp01(alpha));
}
D2D1_COLOR_F Lighten(const D2D1_COLOR_F& colour,float amount) {
    if(amount<=0.0f) return colour;
    const auto mix=[&](float channel) { return Clamp01(channel+(1.0f-channel)*amount); };
    return D2D1::ColorF(mix(colour.r),mix(colour.g),mix(colour.b),colour.a);
}
D2D1_COLOR_F ThemePalette::LampColour(int state) const {
    switch(state) {
    case 1: return ok;
    case 2: return warn;
    case 3: return err;
    default: return idle;
    }
}
ThemePalette ResolveTheme(const std::string& preference) {
    // 六套主题：dark / light（旧面板原样，1:1 对照用）+ 四套现代化候选（用户在预览页里挑的那四套）：
    //   slate     冷近黑 + 靛蓝   · 细边框、圆角 6/10  · 像 Linear/Vercel 那种克制感
    //   carbon    中性灰 + 企业蓝 · 4px 直角、边框说话  · 像 IBM Carbon 类工程软件
    //   porcelain 冷白灰 + 蓝     · 1px 清晰描边、6/10   · 像现代 IDE 的浅色皮肤
    //   sand      暖米色 + 深绿   · 圆角 8/12、柔和阴影  · 像 Notion / Things 那种温和
    // 四套的 text/dim/faint/accentStrong 都按 WCAG 式亮度比解算（>=14 / >=8 / >=5.5 / >=5.5），
    // 比旧主题明显更"看得清"；dark/light 的令牌一个字节都没动，保证 1:1 对照仍然成立。
    const auto themeId=[&] {
        if(preference=="slate"||preference=="carbon"||preference=="porcelain"||preference=="sand")
            return preference;
        if(preference=="light") return std::string("light");
        if(preference=="dark") return std::string("dark");
        return SystemPrefersLight()?std::string("light"):std::string("dark");
    }();
    ThemePalette palette;
    palette.light=(themeId=="light"||themeId=="porcelain"||themeId=="sand");
    struct Semantic { unsigned accent,accentStrong,onAccent,ok,warn,err;float soft,line,okSoft,warnSoft,errSoft; };
    const auto semantic=[&]() -> Semantic {
        if(themeId=="slate")   return {0x6366F1,0x9A95EE,0xFFFFFF,0x34D399,0xF5B84B,0xF87171,0.18f,0.55f,0.16f,0.16f,0.16f};
        if(themeId=="carbon")  return {0x3A78EE,0x72A8F8,0xFFFFFF,0x42BE65,0xF1C21B,0xFA4D56,0.20f,0.60f,0.18f,0.18f,0.18f};
        if(themeId=="porcelain") return {0x2563EB,0x134CC9,0xFFFFFF,0x0A7C55,0xB45309,0xD42C3C,0.14f,0.50f,0.14f,0.14f,0.13f};
        if(themeId=="sand")    return {0x0F766E,0x105B54,0xFFFFFF,0x15803D,0xA15C07,0xBE3B34,0.14f,0.48f,0.13f,0.13f,0.13f};
        if(palette.light)      return {0x38BDDB,0x084457,0x04212B,0x0B7A50,0xA86C05,0xCF3A47,0.15f,0.42f,0.16f,0.14f,0.14f};
        return {0x38BDDB,0x7DD8EE,0x04212B,0x3ED598,0xF2B13C,0xF4626D,0.15f,0.42f,0.16f,0.14f,0.14f};
    }();
    palette.accent=Colour(semantic.accent);
    palette.accentStrong=Colour(semantic.accentStrong);
    palette.onAccent=Colour(semantic.onAccent);
    palette.ok=Colour(semantic.ok);
    palette.warn=Colour(semantic.warn);
    palette.err=Colour(semantic.err);
    palette.accentSoft=Colour(semantic.accent,semantic.soft);
    palette.accentLine=Colour(semantic.accent,semantic.line);
    palette.okSoft=Colour(semantic.ok,semantic.okSoft);
    palette.warnSoft=Colour(semantic.warn,semantic.warnSoft);
    palette.errSoft=Colour(semantic.err,semantic.errSoft);
    struct Neutrals {
        unsigned ink,surf,raise,raise2,line,text,dim,faint,idle;
        unsigned heroTop,heroBottom,plateTop,plateBottom;
        unsigned stripTop,stripBottom,stripHoverTop,stripHoverBottom;
        float line2,highlight,hair;
    };
    const auto n=[&]() -> Neutrals {
        if(themeId=="slate") return {0x07080C,0x0E1015,0x15181F,0x1D212A,0x2A303C,0xF2F2F2,0xB2B2B4,0x909193,0x4A5262,
            0x161A22,0x0C0F14,0x1F2430,0x151920,0x1A1F28,0x0E1116,0x242A36,0x141920,0.06f,0.07f,0.12f};
        if(themeId=="carbon") return {0x101010,0x1A1A1A,0x232323,0x2E2E2E,0x3D3D3D,0xFCFCFC,0xBABABA,0x989898,0x525252,
            0x262626,0x171717,0x2B2B2B,0x212121,0x262626,0x181818,0x303030,0x202020,0.08f,0.06f,0.11f};
        if(themeId=="porcelain") return {0xE7EAF0,0xFFFFFF,0xF6F8FC,0xE7EBF3,0xCDD4E0,0x1C1C1C,0x494949,0x636363,0xB4BCCB,
            0xFFFFFF,0xEFF3F9,0xFFFFFF,0xE9EEF7,0xD3DAE6,0xBFC8D8,0xBEC8DC,0xA9B5CC,0.08f,0.90f,0.14f};
        if(themeId=="sand") return {0xEDEAE2,0xFFFDF8,0xF8F5EE,0xEAE5D9,0xD5CFC0,0x1B1B1A,0x494847,0x636260,0xBDB6A8,
            0xFFFDF8,0xF2EDE2,0xFFFFFF,0xEFEAE0,0xE2DCCE,0xCEC7B6,0xD2CBBB,0xBCB4A1,0.09f,0.85f,0.14f};
        if(palette.light) return {0xEEF1F6,0xFFFFFF,0xF6F8FB,0xE9EDF4,0xDBE1EC,0x141821,0x272D3B,0x43495A,0xB7BFD0,
            0xFFFFFF,0xF4F7FB,0xF2F5FA,0xE9EDF4,0xD7DEEA,0xC4CCDD,0xC2CBE0,0xAFB9D2,0.06f,0.55f,0.13f};
        return {0x080A0E,0x0F1218,0x151A22,0x1D2330,0x242B38,0xE9ECF3,0x8F99AD,0x616A7D,0x3B4257,
            0x151A23,0x0E1218,0x1D232E,0x151A22,0x232A38,0x161B24,0x2E374A,0x1D2330,0.05f,0.07f,0.14f};
    }();
    palette.ink=Colour(n.ink);
    palette.surf=Colour(n.surf);
    palette.raise=Colour(n.raise);
    palette.raise2=Colour(n.raise2);
    palette.line=Colour(n.line);
    palette.line2=Colour(0xFFFFFF,n.line2*(palette.light?1.4f:1.0f));
    if(palette.light&&themeId!="sand") palette.line2=Colour(0x141A2C,n.line2);
    if(themeId=="sand") palette.line2=Colour(0x3C321E,n.line2);
    palette.text=Colour(n.text);
    palette.dim=Colour(n.dim);
    palette.faint=Colour(n.faint);
    palette.idle=Colour(n.idle);
    palette.heroTop=Colour(n.heroTop);
    palette.heroBottom=Colour(n.heroBottom);
    palette.plateTop=Colour(n.plateTop);
    palette.plateBottom=Colour(n.plateBottom);
    palette.highlight=Colour(0xFFFFFF,n.highlight);
    palette.hair=Colour(palette.light?0x1E2D50:0xBECDE6,n.hair);
    palette.stripTop=Colour(n.stripTop);
    palette.stripBottom=Colour(n.stripBottom);
    palette.stripHoverTop=Colour(n.stripHoverTop);
    palette.stripHoverBottom=Colour(n.stripHoverBottom);
    return palette;
}
ThemePalette BlendPalette(const ThemePalette& from,const ThemePalette& to,float t) {
    // 逐令牌线性混合：颜色是普通 RGBA，按分量插值即可（绘制层只认一份 palette）。
    t=t<0.0f?0.0f:(t>1.0f?1.0f:t);
    const auto mix=[t](const D2D1_COLOR_F& a,const D2D1_COLOR_F& b) {
        return D2D1::ColorF(a.r+(b.r-a.r)*t,a.g+(b.g-a.g)*t,a.b+(b.b-a.b)*t,a.a+(b.a-a.a)*t);
    };
    ThemePalette result=to;
    result.ink=mix(from.ink,to.ink);
    result.surf=mix(from.surf,to.surf);
    result.raise=mix(from.raise,to.raise);
    result.raise2=mix(from.raise2,to.raise2);
    result.line=mix(from.line,to.line);
    result.line2=mix(from.line2,to.line2);
    result.text=mix(from.text,to.text);
    result.dim=mix(from.dim,to.dim);
    result.faint=mix(from.faint,to.faint);
    result.accent=mix(from.accent,to.accent);
    result.accentStrong=mix(from.accentStrong,to.accentStrong);
    result.onAccent=mix(from.onAccent,to.onAccent);
    result.ok=mix(from.ok,to.ok);
    result.warn=mix(from.warn,to.warn);
    result.err=mix(from.err,to.err);
    result.idle=mix(from.idle,to.idle);
    result.okSoft=mix(from.okSoft,to.okSoft);
    result.warnSoft=mix(from.warnSoft,to.warnSoft);
    result.errSoft=mix(from.errSoft,to.errSoft);
    result.accentSoft=mix(from.accentSoft,to.accentSoft);
    result.accentLine=mix(from.accentLine,to.accentLine);
    result.heroTop=mix(from.heroTop,to.heroTop);
    result.heroBottom=mix(from.heroBottom,to.heroBottom);
    result.plateTop=mix(from.plateTop,to.plateTop);
    result.plateBottom=mix(from.plateBottom,to.plateBottom);
    result.highlight=mix(from.highlight,to.highlight);
    result.hair=mix(from.hair,to.hair);
    result.stripTop=mix(from.stripTop,to.stripTop);
    result.stripBottom=mix(from.stripBottom,to.stripBottom);
    result.stripHoverTop=mix(from.stripHoverTop,to.stripHoverTop);
    result.stripHoverBottom=mix(from.stripHoverBottom,to.stripHoverBottom);
    result.light=t>=0.5f?to.light:from.light;
    return result;
}
}
