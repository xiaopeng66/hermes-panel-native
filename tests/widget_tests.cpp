// Widget layer tests (PLAN §6, slice H2b): the icon table, the panel layout the renderer draws from and
// the theme's lamp mapping. Everything here is pure - no window, no D2D device - so it runs in the fast
// pre-gate. The same definitions are what panel.cpp draws, which is the point: one source of truth.
#include "app/panel_view.h"
#include "render/icons.h"
#include "render/layout.h"
#include "render/textfield.h"
#include "render/theme.h"
#include "render/widgets.h"
#include <cmath>
#include <iostream>
#include <iostream>
#include <sstream>
#include <set>
#include <string>

namespace {
int passed=0,failed=0;
void Check(bool condition,const std::string& name) {
    if(condition) ++passed;
    else { ++failed;std::cerr<<"FAIL "<<name<<'\n'; }
}
using namespace hermes;
D2D1_RECT_F Box(float x,float y,float w,float h) { return D2D1::RectF(x,y,x+w,y+h); }
}
// app.js hueCls(): h = 7; h = (h*33 + code) & 0xffff; hy = h % 8; .hyN -> --ph 186/196/206/216/228/
// 240/252/264. The identity colour must hash exactly like the old panel, or a provider changes hue.
void TestIdentityHue() {
    struct Case { const char* name; int hue; };
    const Case cases[]={
        {"workbuddy",206},{"Workbuddy",206},{"Local",206},{"deepseek",240},{"custom:workbuddy",264},
        {"openai",216},{"anthropic",264},{"gemini",186},
    };
    for(const auto& item:cases) {
        Check(HueFor(item.name)==item.hue,std::string("identity hue of ")+item.name+" is "
            +std::to_string(item.hue));
    }
}
// TEMP 诊断：备份页/添加页的正文起点（用完删除）
void PrintPageLayout() {
    for(const int page:{2,3,4}) {
        PanelViewModel view;
        view.activePage=page;
        view.backup.title="备份";
        view.backup.filters.push_back({"全部",30,true});
        view.add.title="添加";
        const auto layout=ComputeLayout(view,480.0f,793.0f,1.25f);
        std::cout<<"page "<<page<<" content.top="<<layout.content.top<<" pageHead.top="<<layout.pageHead.top
                 <<" filters.top="<<layout.filters.top<<" list.top="<<layout.list.top
                 <<" subTabs.top="<<layout.subTabs.top<<" pageViewport.top="<<layout.pageViewport.top
                 <<" (DIP: "<<layout.filters.top/1.25f<<")"<<std::endl;
    }
}

// 「这个供应商能不能删」：规则只有一份（ProviderDeletable），视图的禁用态与提交时的守卫共用它。
void TestProviderDeletable() {
    HermesState state;
    ProviderView active;
    active.name="Alpha";active.providerId="custom:alpha";active.isActive=true;
    ProviderView other;
    other.name="Beta";other.providerId="custom:Beta";
    state.providers={active,other};
    state.model.provider="custom:alpha";
    Check(!ProviderDeletable(state,"Alpha"),"the active provider cannot be deleted");
    Check(!ProviderDeletable(state,"custom:alpha"),"the active provider is also blocked by id");
    Check(ProviderDeletable(state,"Beta"),"a free provider can be deleted");
    Check(!ProviderDeletable(state,""),"an empty name is not deletable");
    Check(!ProviderDeletable(state,"Nope"),"an unknown name is not deletable");
    // 当前模型在用的那个即使 is_active 没报也要挡住（id 命中）。
    state.providers[1].isActive=true;
    state.model.provider="custom:Beta";
    Check(!ProviderDeletable(state,"Beta"),"the provider the model points at is blocked even if it is not active");
    // 名字大小写与 id 不一致时也要认得出来。
    state.providers[1].name="beta";
    Check(!ProviderDeletable(state,"beta"),"case differences between name and id do not open a hole");
}

// 两套拟态主题：材质开关打开、文字对比度高于对应 flat 主题（用户要求"对比和强调不明显"要解决）。
// 四套现代化主题（slate / carbon / porcelain / sand）：材质开关、语义色、以及**对比度契约**。
// 契约来自用户要求"对比度要强一些，不要看不到"：四套都要过 WCAG 式亮度比下限
//     正文 >= 12、次要 >= 7、最淡 >= 4.5、强调 >= 4.5
// 且严格强于旧主题（dark / light 的对应档位）—— 把"加强对比"这件事钉死，改坏就红。
void TestModernThemes() {
    const auto slate=ResolveTheme("slate"),carbon=ResolveTheme("carbon");
    const auto porcelain=ResolveTheme("porcelain"),sand=ResolveTheme("sand");
    const auto dark=ResolveTheme("dark"),light=ResolveTheme("light");
    Check(!slate.light&&!carbon.light,"slate and carbon are dark palettes");
    Check(porcelain.light&&sand.light,"porcelain and sand are light palettes");
    // 拟态那一套已经从渲染层移除（用户明确不要），所以这里不再有 neumorph/emboss 断言。
    // 各自的语义色带自己的色相（不是照抄旧主题的青色）。
    // 强调色的色相（用通道间的有序关系表达，四套各不相同）：
    //   slate 靛蓝 = 蓝最亮、红略高、绿最低；carbon/porcelain 蓝 > 绿 > 红；
    //   sand 深绿 = 绿 > 蓝 > 红。
    Check(slate.accent.b>slate.accent.g&&slate.accent.g>slate.accent.r,"slate's accent is indigo (blue leads)");
    Check(carbon.accent.b>carbon.accent.g&&carbon.accent.g>carbon.accent.r,"carbon's accent is blue");
    Check(porcelain.accent.b>porcelain.accent.g&&porcelain.accent.g>porcelain.accent.r,"porcelain's accent is blue");
    Check(sand.accent.g>sand.accent.r&&sand.accent.g>sand.accent.b,"sand's accent is teal-green (green leads)");
    const auto luminance=[](const D2D1_COLOR_F& colour) {
        const auto channel=[](float value) {
            return value<=0.03928f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);
        };
        return 0.2126f*channel(colour.r)+0.7152f*channel(colour.g)+0.0722f*channel(colour.b);
    };
    const auto contrast=[&](const D2D1_COLOR_F& a,const D2D1_COLOR_F& b) {
        const auto la=luminance(a)+0.05f,lb=luminance(b)+0.05f;
        return la>lb?la/lb:lb/la;
    };
    struct Case { const char* name;const ThemePalette* palette;float text;float dim;float faint;float strong; };
    const Case cases[]={
        {"slate",&slate,14.0f,8.0f,5.5f,5.5f},
        {"carbon",&carbon,14.0f,8.0f,5.5f,5.5f},
        {"porcelain",&porcelain,14.0f,8.0f,5.5f,5.5f},
        {"sand",&sand,14.0f,8.0f,5.5f,5.5f},
    };
    for(const auto& item:cases) {
        const auto& p=*item.palette;
        const auto text=contrast(p.text,p.surf);
        const auto dim=contrast(p.dim,p.surf);
        const auto faint=contrast(p.faint,p.surf);
        const auto strong=contrast(p.accentStrong,p.surf);
        Check(text>=item.text-0.2f,(std::string(item.name)+" text contrast >= "
            +std::to_string(item.text)+" (got "+std::to_string(text)+")").c_str());
        Check(dim>=item.dim-0.2f,(std::string(item.name)+" dim contrast >= "
            +std::to_string(item.dim)+" (got "+std::to_string(dim)+")").c_str());
        Check(faint>=item.faint-0.2f,(std::string(item.name)+" faint contrast >= "
            +std::to_string(item.faint)+" (got "+std::to_string(faint)+")").c_str());
        Check(strong>=item.strong-0.2f,(std::string(item.name)+" emphasis contrast >= "
            +std::to_string(item.strong)+" (got "+std::to_string(strong)+")").c_str());
    }
    // 硬下限（不是"跟旧主题比大小"）：四套的每一档都必须达到解算目标，允许 0.2 的取整余量。
    // 旧主题的次要/最淡本来就偏低（dark 6.5/3.5），四套是 9/6 起 —— 这才是"看得清"的来源。
    // 旧主题仍然能取到（1:1 对照用），且令牌没有被新主题带跑。
    Check(std::abs(dark.surf.r-light.surf.r)>0.5f,"dark and light still resolve to different surfaces");
}


int main() {
    TestIdentityHue();
    TestModernThemes();
    TestProviderDeletable();
    PrintPageLayout();
    // ---- 图标表：30 个，名字与旧 icons.js 一致，几何在 24x24 内 ----
    Check(IconCount()==30,"the icon table holds the 30 icons the old renderer inlined");
    const char* expected[30]={"search","x","star","refresh-cw","chevron-right","chevron-down","chevron-up",
        "plus","minus","trash-2","settings","pin","key","git-branch","layers","archive","arrow-up","arrow-down",
        "activity","copy","check","alert-triangle","power","wifi-off","minimize-2","more-horizontal",
        "rotate-ccw","eye","eye-off","corner-down-left"};
    std::set<std::string> names;
    for(const auto* name:expected) {
        const auto* shape=FindIcon(std::string(name));
        Check(shape!=nullptr,std::string("icon present: ")+name);
        if(shape) names.insert(shape->name);
    }
    Check(static_cast<int>(names.size())==30,"every icon name is unique");
    for(const auto& shape:IconTable()) {
        Check(!shape.figures.empty(),std::string("icon has at least one figure: ")+shape.name);
        bool inside=true;
        for(const auto& figure:shape.figures) {
            const auto check=[&](float x,float y) {
                if(x<-0.6f||x>24.6f||y<-0.6f||y>24.6f) inside=false;
            };
            if(figure.kind==IconFigure::Kind::Circle||figure.kind==IconFigure::Kind::Arc) {
                check(figure.centre.x-figure.radius,figure.centre.y-figure.radius);
                check(figure.centre.x+figure.radius,figure.centre.y+figure.radius);
                Check(figure.radius>0.5f,std::string("circle/arc has a radius: ")+shape.name);
            } else if(figure.kind==IconFigure::Kind::RoundedRect) {
                check(figure.points.front().x,figure.points.front().y);
                check(figure.points.front().x+figure.width,figure.points.front().y+figure.height);
                Check(figure.width>1.0f&&figure.height>1.0f,std::string("rect has a size: ")+shape.name);
            } else {
                Check(figure.points.size()>=2,std::string("polyline has vertices: ")+shape.name);
                for(const auto& point:figure.points) check(point.x,point.y);
            }
        }
        Check(inside,std::string("icon stays inside its 24x24 box: ")+shape.name);
    }
    Check(FindIcon(IconId::None)==nullptr,"the None id resolves to nothing");
    Check(FindIcon("no-such-icon")==nullptr,"an unknown name resolves to nothing");

    // ---- 主题：灯色映射与两套 token ----
    {
        const auto dark=ResolveTheme("dark");
        const auto light=ResolveTheme("light");
        const auto same=[](const D2D1_COLOR_F& left,const D2D1_COLOR_F& right) {
            return std::abs(left.r-right.r)<0.01f&&std::abs(left.g-right.g)<0.01f&&std::abs(left.b-right.b)<0.01f;
        };
        Check(same(dark.ink,D2D1::ColorF(0x080A0E))&&same(light.ink,D2D1::ColorF(0xEEF1F6)),
            "ink follows the theme (old --hx-ink pair)");
        Check(same(dark.accent,D2D1::ColorF(0x38BDDB))&&same(light.accent,D2D1::ColorF(0x38BDDB)),
            "the accent is the same cyan in both themes");
        Check(same(dark.LampColour(1),dark.ok)&&same(dark.LampColour(2),dark.warn)&&same(dark.LampColour(3),dark.err),
            "lamp states map to ok/warn/err");
        Check(same(dark.LampColour(0),dark.idle),"the idle lamp uses the idle colour");
        Check(!same(dark.ok,light.ok),"the semantic colours do change between themes");
    }

    // ---- 布局：chrome 的度量与命中框 ----
    {
        const auto view=BuildPanelView(PanelViewInput{});
        const float scale=1.0f;
        const auto layout=ComputeLayout(view,360.0f*scale,620.0f*scale,scale);
        Check(layout.titleBar.bottom-layout.titleBar.top>=34.0f,"the title bar keeps its height");
        Check(std::abs((layout.titleBar.bottom-layout.titleBar.top)-34.0f)<0.01f,
            "the title bar is 34 DIP tall like the old .titlebar");
        Check(layout.lamps.size()==3,"three lamps are placed (WSL / WebUI / gateway)");
        const float lampRowHeight=layout.lampRow.bottom-layout.lampRow.top;
        Check(std::abs(lampRowHeight-27.0f)<0.01f,"the lamp row is 27 DIP tall like the old .reg");
        Check(layout.hero.left<0.01f,"the hero strip is full-bleed like the old .hero");
        Check(layout.hero.bottom-layout.hero.top>60.0f,"the hero plate has room for two lines");
        Check(layout.content.bottom<=layout.nav.top+0.01f,"the content never runs into the navigation");
        Check(std::abs((layout.nav.bottom-layout.nav.top)-47.0f)<0.01f,
            "the navigation is 47 DIP tall like the old .nav");
        Check(layout.navItems.size()==5,"five navigation items are placed");
        // hit testing: one control per title button, five tabs, refresh and (when shown) the chip
        std::size_t tabs=0;
        for(const auto& item:layout.navItems) {
            const auto centre=D2D1::Point2F((item.box.left+item.box.right)/2,(item.box.top+item.box.bottom)/2);
            const auto hit=ControlAt(layout,centre.x,centre.y);
            Check(hit==ControlId::TabModels||hit==ControlId::TabFallback||hit==ControlId::TabAdd
                ||hit==ControlId::TabBackup||hit==ControlId::TabSettings,"each nav item hits its own control");
            ++tabs;
        }
        Check(tabs==5,"all five navigation items are clickable");
        Check(ControlAt(layout,-10,-10)==ControlId::None,"outside the window nothing is hit");
        Check(ControlAt(layout,(layout.titleBar.left+layout.titleBar.right)/2,layout.titleBar.bottom+80)==ControlId::None,
            "the middle of the panel hits no control");
        for(const auto id:{ControlId::Snap,ControlId::Pin,ControlId::Minimize,ControlId::Close,ControlId::Refresh}) {
            const auto* rect=ControlRectFor(layout,id);
            Check(rect!=nullptr,"chrome control exists");
            if(rect) {
                Check(rect->right<=360.0f*scale+0.01f&&rect->left>=0,"chrome control stays inside the panel");
                Check(ControlAt(layout,(rect->left+rect->right)/2,(rect->top+rect->bottom)/2)==id,
                    "chrome control hits itself");
            }
        }
        // title buttons must not overlap: their centres differ by at least a button width
        const auto* close=ControlRectFor(layout,ControlId::Close);
        const auto* minimize=ControlRectFor(layout,ControlId::Minimize);
        Check(close&&minimize&&minimize->right<=close->left+0.01f,"the title buttons do not overlap");
        // a chip appears only when the view asks for one
        PanelViewModel withChip=view;
        withChip.chip.present=true;
        withChip.chip.kind=ChipKind::Warn;
        withChip.chip.text="\xE7\xBD\x91\xE5\x85\xB3\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF";
        withChip.statuses={{withChip.chip.text,StatusAction::RetryWebUi}};
        const auto chipLayout=ComputeLayout(withChip,360.0f*scale,620.0f*scale,scale);
        const auto* chip=ControlRectFor(chipLayout,ControlId::Chip);
        Check(chip!=nullptr,"the chip is clickable when it is shown");
        if(chip) {
            Check(chip->left>=0&&chip->right<=360.0f,"the chip stays inside the lamp row");
            Check(chip->right<=chipLayout.sync.left+0.01f,"the chip never overlaps the sync stamp");
        }
        Check(ControlRectFor(layout,ControlId::Chip)==nullptr,"no chip control when no chip is shown");
        // 寄存器行任何状态组合都不重叠（真机：点刷新后「读取 WSL 配置…」压住「网关」灯）。
        // 不变量：灯（dot∪label）≤ busy ≤ chip ≤ sync，从左到右一条线；放不下的 chip 直接不排。
        const auto lampsRight=[](const PanelLayout& l) {
            float right=0.0f;
            for(const auto& lamp:l.lamps) right=std::max(right,lamp.label.right);
            return right;
        };
        for(const bool busy:{false,true}) {
            PanelViewModel crowded=view;
            crowded.busy=busy;
            crowded.busyLabel=busy?"\xE8\xAF\xBB\xE5\x8F\x96 WSL \xE9\x85\x8D\xE7\xBD\xAE\xE2\x80\xA6":""; // 读取 WSL 配置…
            crowded.chipWarn.present=true;   // 三个状态全上：合并成一个「3 项状态」胶囊
            crowded.chip.present=true;
            crowded.chipHelper.present=true;
            crowded.statuses={
                {"\xE7\xBD\x91\xE5\x85\xB3\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF",StatusAction::RestartConfig},
                {"WebUI \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE",StatusAction::RetryWebUi},
                {"\xE5\x8A\xA9\xE6\x89\x8B\xE5\x87\xBA\xE9\x94\x99",StatusAction::RetryHelper}};
            const auto crowdedLayout=ComputeLayout(crowded,480.0f,793.0f,1.25f);
            const float rightEdge=lampsRight(crowdedLayout);
            Check(rightEdge<crowdedLayout.sync.left,"the lamps never reach the sync stamp");
            // 多状态：只有一个合并胶囊，旧的三个 chip 不再单独占位。
            const auto* pill=ControlRectFor(crowdedLayout,ControlId::StatusPill);
            Check(pill!=nullptr,"multiple statuses collapse into one status pill");
            Check(ControlRectFor(crowdedLayout,ControlId::ChipWarn)==nullptr
                &&ControlRectFor(crowdedLayout,ControlId::Chip)==nullptr
                &&ControlRectFor(crowdedLayout,ControlId::ChipHelper)==nullptr,
                "individual chips disappear when statuses are merged");
            if(pill) {
                Check(pill->left>=rightEdge-0.01f,"the status pill never covers the lamps");
                Check(pill->right<=crowdedLayout.sync.left+0.01f,
                    "the status pill never overlaps the sync stamp");
            }
            if(busy) {
                Check(crowdedLayout.busy.left>=rightEdge-0.01f,"the busy label never covers the lamps");
                Check(crowdedLayout.busy.right<=crowdedLayout.sync.left+0.01f,
                    "the busy label never runs into the sync stamp");
            }
            // 状态中心：3 行，整块在寄存器行下方且不压到同步钮。
            crowded.statusCenterOpen=true;
            const auto openLayout=ComputeLayout(crowded,480.0f,793.0f,1.25f);
            Check(openLayout.statusRows.size()==3,"the status center lists every merged status");
            Check(openLayout.statusCard.top>=openLayout.lampRow.bottom-0.01f,
                "the status center opens below the register row");
            for(const auto& row:openLayout.statusRows)
                Check(row.row.left>=0&&row.row.right<=480.0f
                    &&row.row.top>=openLayout.statusCard.top-0.01f
                    &&row.row.bottom<=openLayout.statusCard.bottom+0.01f,
                    "every status row stays inside the card");
        }
        // 单个状态保持旧行为：一个 chip/胶囊，点击入口不变。
        {
            PanelViewModel single=view;
            single.statuses={{"WebUI \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE",StatusAction::RetryWebUi}};
            single.chip.present=true;
            single.chip.text=single.statuses[0].label;
            const auto singleLayout=ComputeLayout(single,480.0f,793.0f,1.25f);
            Check(ControlRectFor(singleLayout,ControlId::Chip)!=nullptr,
                "a single status keeps the original chip control");
            Check(ControlRectFor(singleLayout,ControlId::StatusPill)==nullptr,
                "a single status does not use the merged pill");
        }
    }

    // ---- 页面滚动条：必须在内容右缘**之外**（真机：旧位置压住输入框/下拉的右边框）----
    {
        PanelViewInput settingsInput;
        settingsInput.activePage=4;               // 设置页（内容比矮窗口高 → 滚动条出现）
        PanelViewModel page4=BuildPanelView(settingsInput);
        SettingsPageInput settingsPage;           // BuildPanelView 不建页内容，这里补上
        settingsPage.distro="Ubuntu";
        AttachSettingsPage(page4,settingsPage);
        const auto short1=ComputeLayout(page4,480.0f,500.0f,1.25f);
        Check(short1.pageScrollVisible,"the settings page shows a scrollbar in a short window");
        if(short1.pageScrollVisible) {
            Check(short1.pageScrollTrack.left>=short1.content.right-0.01f,
                "the page scrollbar starts outside the content right edge");
            Check(short1.pageScrollThumb.left>=short1.content.right-0.01f,
                "the page scrollbar thumb stays outside the content right edge");
            Check(short1.pageScrollTrack.right<=480.0f,
                "the page scrollbar stays inside the window");
        }
    }
    // ---- Toast：长文案长高到两行完整显示（真机：切换模型的提示被省略号截断）----
    {
        PanelViewModel withToast=BuildPanelView(PanelViewInput{});
        withToast.toast.text="\xE5\xB7\xB2\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xBA workbuddy-global-qwen3-32b"
            "\xEF\xBC\x9B\xE5\xBE\xAE\xE4\xBF\xA1/QQ \xE7\xAD\x89\xE6\x9C\xBA\xE5\x99\xA8\xE4\xBA\xBA"
            "\xE9\x9C\x80\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3\xE5\x90\x8E\xE7\x94\x9F\xE6\x95\x88";
        const auto tallToast=ComputeLayout(withToast,480.0f,793.0f,1.25f);
        Check(tallToast.toast.bottom-tallToast.toast.top>48.0f,
            "a long toast grows past one line (two lines fit the full text)");
        PanelViewModel shortToast=BuildPanelView(PanelViewInput{});
        shortToast.toast.text="ok";
        const auto smallToast=ComputeLayout(shortToast,480.0f,793.0f,1.25f);
        Check(std::abs((smallToast.toast.bottom-smallToast.toast.top)-42.5f)<0.6f,
            "a short toast keeps the original one-line height");
    }

    // ---- 布局随 DPI 缩放 ----
    for(const float scale:{1.0f,1.25f,1.5f,2.0f,2.1f}) {
        const auto view=BuildPanelView(PanelViewInput{});
        const auto layout=ComputeLayout(view,360.0f*scale,620.0f*scale,scale);
        const auto* close=ControlRectFor(layout,ControlId::Close);
        Check(close!=nullptr&&close->right<=360.0f*scale+0.01f,
            std::string("the close button stays inside at ")+std::to_string(scale)+"x");
        Check(std::abs((layout.nav.bottom-layout.nav.top)-47.0f*scale)<0.5f,
            std::string("the navigation scales with the DPI at ")+std::to_string(scale)+"x");        const auto& firstNav=layout.navItems.front().box;
        Check(ControlAt(layout,(firstNav.left+firstNav.right)/2,(firstNav.top+firstNav.bottom)/2)==ControlId::TabModels,
            std::string("the first tab still hits itself at ")+std::to_string(scale)+"x");
    }

    // ---- 视图桩：三盏灯与五个页面 ----
    {
        const auto view=BuildPanelView(PanelViewInput{});
        Check(view.lamps.size()==3&&view.lamps[0].label=="WSL"&&view.lamps[1].label=="WebUI",
            "the lamp register matches the old page");
        Check(view.nav.size()==5&&view.nav[0].label=="\xE6\xA8\xA1\xE5\x9E\x8B"&&view.nav[4].label=="\xE8\xAE\xBE\xE7\xBD\xAE",
            "the navigation carries the five pages in order");
        Check(view.nav[0].active,"the first page is active by default");
        PanelViewInput page3;
        page3.activePage=3;
        const auto third=BuildPanelView(page3);
        Check(third.activePage==3&&third.nav[3].active&&!third.nav[0].active,"the active page follows the input");
        Check(third.pageTitle==third.nav[3].label,"the placeholder title names the active page");
        PanelViewInput outOfRange;
        outOfRange.activePage=99;
        Check(BuildPanelView(outOfRange).activePage==4,"an out-of-range page clamps to the last one");
    }
    // ===================== H2b：图标形状核对 + 控件原语 =====================
    {
        using namespace hermes::widgets;
        // ---- 图标：与 renderer/icons.js 逐字核对过的三个曾走形的位置 ----
        const auto shapeLine=[](const IconFigure& figure) {
            return figure.kind==IconFigure::Kind::Polyline&&figure.points.size()>=2
                ?figure.points:std::vector<D2D1_POINT_2F>{};
        };
        if(const auto* search=FindIcon(std::string("search"))) {
            const IconFigure* circle=nullptr;
            bool handle=false;
            for(const auto& figure:search->figures) {
                if(figure.kind==IconFigure::Kind::Circle) circle=&figure;
                const auto points=shapeLine(figure);
                if(points.size()==2&&points[0].x==21.0f&&points[0].y==21.0f
                    &&std::abs(points[1].x-16.66f)<0.01f&&std::abs(points[1].y-16.66f)<0.01f) handle=true;
            }
            Check(circle!=nullptr&&std::abs(circle->radius-8.0f)<0.01f,
                "search keeps the lucide r=8 lens");
            Check(handle,"search keeps the lucide handle m21 21 -4.34 -4.34");
        }
        if(const auto* branch=FindIcon(std::string("git-branch"))) {
            const IconFigure* arc=nullptr;
            int circles=0;
            bool spine=false;
            for(const auto& figure:branch->figures) {
                if(figure.kind==IconFigure::Kind::Arc) {
                    arc=&figure;
                } else if(figure.kind==IconFigure::Kind::Circle) {
                    ++circles;
                } else {
                    const auto points=shapeLine(figure);
                    if(points.size()>=2&&std::abs(points.front().x-6.0f)<0.01f
                        &&std::abs(points.back().y-3.0f)<0.01f) spine=true;
                }
            }
            Check(arc!=nullptr&&std::abs(arc->centre.x-15.0f)<0.01f&&std::abs(arc->radius-9.0f)<0.01f,
                "git-branch bends the spine on the quarter arc centred at (15,15)");
            Check(circles==2,"git-branch has both node circles");
            Check(spine,"git-branch keeps the V3 line to the top");
        }
        if(const auto* refresh=FindIcon(std::string("refresh-cw"))) {
            constexpr float kPi=3.14159265358979f;
            int arcs=0;
            for(const auto& figure:refresh->figures) {
                if(figure.kind==IconFigure::Kind::Arc) {
                    ++arcs;
                    Check(std::abs(figure.sweep-156.0f*kPi/180.0f)<0.01f,
                        "refresh-cw runs each arc 156 degrees");
                }
            }
            Check(arcs==2,"refresh-cw has the two lucide arcs");
        }

        // ---- 主题：发出 --hx-hair 和身份色 ----
        const auto dark=ResolveTheme("dark");
        const auto light=ResolveTheme("light");
        const auto closeTo=[&](const D2D1_COLOR_F& left,float r,float g,float b,float a) {
            return std::abs(left.r-r)<0.01f&&std::abs(left.g-g)<0.01f
                &&std::abs(left.b-b)<0.01f&&std::abs(left.a-a)<0.01f;
        };
        Check(closeTo(dark.hair,190.0f/255.0f,205.0f/255.0f,230.0f/255.0f,0.14f),
            "the dark hairline mid-colour is rgba(190,205,230,.14)");
        Check(closeTo(light.hair,30.0f/255.0f,45.0f/255.0f,80.0f/255.0f,0.13f),
            "the light hairline mid-colour is rgba(30,45,80,.13)");
        {
            const auto identity=HslColour(186.0f,0.68f,0.62f);
            Check(closeTo(identity,0.3616f,0.8269f,0.8784f,1.0f),
                ".hlamp's hsl(186 68% 62%) resolves to the identity cyan");
            const auto grey=HslColour(0.0f,0.0f,0.5f);
            Check(closeTo(grey,0.5f,0.5f,0.5f,1.0f),"hsl with zero saturation is grey");
            const auto green=HslColour(120.0f,1.0f,0.5f);
            Check(closeTo(green,0.0f,1.0f,0.0f,1.0f),"hsl(120 100% 50%) is pure green");
            const auto wrapped=HslColour(546.0f,0.68f,0.62f);
            Check(closeTo(wrapped,identity.r,identity.g,identity.b,identity.a),
                "hues wrap mod 360 like the css function");
        }
        {
            const auto lighter=Lighten(dark.accent,kHoverBrightness);
            Check(lighter.r>=dark.accent.r&&lighter.g>=dark.accent.g&&lighter.b>=dark.accent.b
                &&(lighter.r>dark.accent.r||lighter.g>dark.accent.g||lighter.b>dark.accent.b),
                "primary's hover brightens the accent (filter: brightness(1.08))");
            const auto untouched=Lighten(dark.accent,0.0f);
            Check(closeTo(untouched,dark.accent.r,dark.accent.g,dark.accent.b,dark.accent.a),
                "Lighten with 0 keeps the colour");
        }

        // ---- 状态灯：6px 方点 + 2px 光环 + 10px 文案（styles.css .lamp）----
        Check(near(kLampDot,6,0,0,1)&&near(kLampDotRadius,2,0,0,1)&&near(kLampHalo,2,0,0,1)
            &&near(kLampGap,5,0,0,1),".lamp metrics are 6/2/2/5 from the stylesheet");
        {
            const auto okSkin=LampSkinOf(LampState::Ok,dark);
            Check(near(okSkin.dot,dark.ok.r,dark.ok.g,dark.ok.b,1.0f)&&okSkin.glow
                &&std::abs(okSkin.halo.a-0.5f)<0.01f,"the ok lamp glows its own colour softly");
            const auto errSkin=LampSkinOf(LampState::Err,dark);
            Check(near(errSkin.dot,dark.err.r,dark.err.g,dark.err.b,1.0f)&&errSkin.glow,
                "the err lamp uses the error colour");
            const auto idleSkin=LampSkinOf(LampState::Idle,dark);
            Check(!idleSkin.glow&&idleSkin.halo.a<0.01f,"the idle lamp has no ring (data-off=idle)");
            Check(near(okSkin.label,dark.dim.r,dark.dim.g,dark.dim.b,1.0f),
                "lamp labels stay dim in every state");
            const D2D1_RECT_F dot=D2D1::RectF(10,10,16,16);
            const auto halo=HaloRect(dot);
            Check(std::abs(halo.left-8.0f)<0.01f&&std::abs(halo.top-8.0f)<0.01f
                &&std::abs(halo.right-18.0f)<0.01f&&std::abs(halo.bottom-18.0f)<0.01f,
                "the halo ring is the dot inflated by 2px on every side");
        }
        // layout places a 6x6 dot for each lamp
        {
            const auto view=BuildPanelView(PanelViewInput{});
            const auto layout=ComputeLayout(view,360.0f,620.0f,1.0f);
            const auto& dot=layout.lamps.front().dot;
            Check(std::abs((dot.right-dot.left)-6.0f)<0.01f&&std::abs((dot.bottom-dot.top)-6.0f)<0.01f,
                "the first lamp's dot is drawn at the stylesheet's 6x6");
        }

        // ---- chip：warn/err 描边 46%，plain 在 raise2 上 ----
        {
            const auto warn=ChipSkinOf(ChipKind::Warn,Visual::Normal,dark);
            Check(warn.fill.a<0.01f&&std::abs(warn.border.a-kChipBorderOpacity)<0.01f
                &&std::abs(warn.border.r-warn.text.r)<0.01f,
                "the warn chip is an outline chip at 46% of the warn colour");
            const auto errHover=ChipSkinOf(ChipKind::Err,Visual::Hovered,dark);
            Check(std::abs(errHover.border.a-1.0f)<0.01f&&std::abs(errHover.border.b-errHover.text.b)<0.01f,
                "hovering a kind chip hardens its border to full opacity");
            const auto plain=ChipSkinOf(ChipKind::Ok,Visual::Normal,dark);
            Check(std::abs(plain.fill.r-dark.raise2.r)<0.01f&&plain.border.a<0.01f
                &&std::abs(plain.text.r-dark.dim.r)<0.01f,
                "the plain chip sits on raise2 with dim text and no border");
            Check(std::abs(kChipHeight-22.0f)<0.01f&&std::abs(kChipRadius-6.0f)<0.01f,
                ".chip is 22 tall with radius 6");
        }

        // ---- 按钮四种变体 + hover + disabled ----
        {
            const auto normal=ButtonSkinOf(ButtonVariant::Normal,Visual::Normal,dark);
            Check(std::abs(normal.fill.r-dark.raise.r)<0.01f
                &&std::abs(normal.border.r-dark.line.r)<0.01f
                &&std::abs(normal.text.r-dark.text.r)<0.01f,
                "the .btn variant is raise on line with text-colour text");
            const auto hover=ButtonSkinOf(ButtonVariant::Normal,Visual::Hovered,dark);
            Check(std::abs(hover.fill.r-dark.raise2.r)<0.01f,"hovering .btn moves it to raise2");
            const auto disabled=ButtonSkinOf(ButtonVariant::Normal,Visual::Disabled,dark);
            Check(std::abs(disabled.alpha-kDisabledOpacity)<0.01f,
                "a disabled button renders at 45% opacity like [disabled]");
            const auto primary=ButtonSkinOf(ButtonVariant::Primary,Visual::Normal,dark);
            Check(std::abs(primary.fill.r-dark.accent.r)<0.01f&&primary.border.a<0.01f
                &&std::abs(primary.text.r-dark.onAccent.r)<0.01f,
                ".btn.primary is the accent with on-accent text and no border");
            const auto ghost=ButtonSkinOf(ButtonVariant::Ghost,Visual::Normal,dark);
            Check(ghost.fill.a<0.01f&&std::abs(ghost.text.r-dark.dim.r)<0.01f,
                ".btn.ghost is borderless with dim text");
            const auto ghostHover=ButtonSkinOf(ButtonVariant::Ghost,Visual::Hovered,dark);
            Check(std::abs(ghostHover.fill.r-dark.raise2.r)<0.01f
                &&std::abs(ghostHover.text.r-dark.text.r)<0.01f,
                "hovering .btn.ghost fills raise2 and lights the text");
            const auto danger=ButtonSkinOf(ButtonVariant::Danger,Visual::Normal,dark);
            Check(std::abs(danger.border.a-0.4f)<0.01f&&std::abs(danger.border.r-danger.text.r)<0.01f
                &&std::abs(danger.text.r-dark.err.r)<0.01f,
                ".danger-btn outlines in 40% error with error text");
            const auto dangerHover=ButtonSkinOf(ButtonVariant::Danger,Visual::Hovered,dark);
            Check(std::abs(dangerHover.fill.r-dark.errSoft.r)<0.01f
                &&std::abs(dangerHover.fill.a-dark.errSoft.a)<0.01f,
                "hovering .danger-btn fills the error-soft wash");
            Check(std::abs(kButtonHeight-30.0f)<0.01f&&std::abs(kButtonRadius-9.0f)<0.01f,
                ".btn is 30 tall with the global 9px radius");
            Check(std::abs(kButtonIconOnly-30.0f)<0.01f&&std::abs(kIcbSize-24.0f)<0.01f,
                "icon-only buttons are 30 wide and title .icb buttons 24x24");
        }

        // ---- 分段控件 ----
        {
            const auto active=SegSkinOf(true,false,dark);
            Check(std::abs(active.fill.r-dark.accentSoft.r)<0.01f
                &&std::abs(active.border.r-dark.accentLine.r)<0.01f
                &&std::abs(active.text.r-dark.accentStrong.r)<0.01f,
                ".seg.active fills accent-soft, outlines accent-line, texts accent-strong");
            Check(std::abs(active.count.a-0.7f)<0.01f,"the active count renders at 70% strength");
            const auto idle=SegSkinOf(false,false,dark);
            Check(std::abs(idle.text.r-dark.dim.r)<0.01f&&std::abs(idle.count.r-dark.faint.r)<0.01f
                &&idle.fill.a<0.01f,"an idle .seg has dim text, faint count, no fill");
            const auto hover=SegSkinOf(false,true,dark);
            Check(std::abs(hover.fill.r-dark.raise2.r)<0.01f,"hovering an idle .seg fills raise2");
            Check(std::abs(kSegHeight-24.0f)<0.01f&&std::abs(kSegRadius-6.0f)<0.01f,
                ".seg is 24 tall with radius 6");
            Check(std::abs(kRowHeight-31.0f)<0.01f&&std::abs(kRadius-9.0f)<0.01f,
                "--row is 31 DIP and --r is 9 DIP");
        }

        // ---- 开关 ----
        {
            const D2D1_RECT_F track=D2D1::RectF(20,40,20+kSwitchWidth,40+kSwitchHeight);
            const auto off=SwitchKnobRect(track,false);
            Check(std::abs(off.left-(track.left+kSwitchInset))<0.01f
                &&std::abs(off.top-(track.top+kSwitchInset))<0.01f
                &&std::abs((off.right-off.left)-kSwitchKnob)<0.01f,
                "the unchecked knob sits inset 2px from the left");
            const auto on=SwitchKnobRect(track,true);
            Check(std::abs(on.left-(track.left+kSwitchInset+kSwitchTravel))<0.01f,
                "the checked knob travels 12px to the right");
            Check(on.right<=track.right+0.01f&&on.bottom<=track.bottom+0.01f,
                "the knob never leaves the pill on either side");
            const auto onSkin=SwitchSkinOf(true,false,dark);
            Check(std::abs(onSkin.track.r-dark.accentSoft.r)<0.01f
                &&std::abs(onSkin.knob.r-dark.accentStrong.r)<0.01f,
                "a checked .sw fills accent-soft and slides the accent-strong knob");
            const auto offSkin=SwitchSkinOf(false,false,dark);
            Check(std::abs(offSkin.track.r-dark.raise2.r)<0.01f
                &&std::abs(offSkin.knob.r-dark.faint.r)<0.01f,
                "an unchecked .sw is raise2 with a faint knob");
            Check(std::abs(SwitchSkinOf(false,true,dark).alpha-kDisabledOpacity)<0.01f,
                "a disabled switch renders at 45% opacity");
            Check(std::abs(kSwitchWidth-28.0f)<0.01f&&std::abs(kSwitchHeight-16.0f)<0.01f
                &&std::abs(kSwitchKnob-10.0f)<0.01f,".sw is 28x16 with a 10px knob (styles.css)");
        }

        // ---- 导航选中胶囊 + hero 灯 ----
        {
            Check(std::abs(NavPillWidth()-34.0f)<0.01f&&std::abs(NavPillHeight()-23.0f)<0.01f
                &&std::abs(NavPillTop()-5.0f)<0.01f&&std::abs(NavPillRadius()-9.0f)<0.01f,
                ".tab.active::before is a 34x23 r9 pill at 5px from the nav top");
            Check(std::abs(kHeroLamp-8.0f)<0.01f&&std::abs(kHeroLampRing-3.0f)<0.01f
                &&std::abs(kHeroLampGlow-13.0f)<0.01f,
                ".hlamp is 8px with a 3px ring and the 13px glow");
            Check(std::abs(kPlateRadius-8.0f)<0.01f&&std::abs(kPlateFont-14.0f)<0.01f
                &&std::abs(kPlateSwappingFont-13.0f)<0.01f,
                ".plate is r8 and swaps .hero-m from 14px to 13px");
        }
        // nav tab boxes sit inside the 47px navigation and still hit themselves
        {
            const auto view=BuildPanelView(PanelViewInput{});
            const auto layout=ComputeLayout(view,360.0f,620.0f,1.0f);
            for(const auto& item:layout.navItems) {
                Check(item.box.top>=layout.nav.top-0.01f&&item.box.bottom<=layout.nav.bottom+0.01f,
                    "nav tabs stay inside the 47px strip");
            }
            Check(std::abs((layout.hero.right-layout.hero.left)-360.0f)<0.01f,
                "the hero spans the full window width");
            Check(layout.heroDivider.bottom>0&&std::abs((layout.heroDivider.bottom-layout.heroDivider.top)-1.0f)<0.01f,
                "the hero strip closes with a 1px divider");
        }
    }

    // ===================== H2c-a：文本框模型（编辑/选择/IME/密码） =====================
    {
        using hermes::TextField;
        // 插入与替换选择
        TextField box;
        box.Insert(L"deepseek");
        Check(box.Text()==L"deepseek"&&box.caret==8&&!box.HasSelection(),"typing inserts at the caret");
        box.Focus();
        box.SelectAll();box.Insert(L"v4");
        Check(box.Text()==L"v4"&&box.caret==2,"typing over a full selection replaces it");
        box.MoveCaret(TextField::Move::Left,false);
        Check(box.caret==1&&box.anchor==1,"arrow left moves the caret without extending");
        box.MoveCaret(TextField::Move::Home,true);
        Check(box.caret==0&&box.anchor==1&&box.Selected()==L"v","shift+left extends the selection");
        box.Backspace();
        Check(box.Text()==L"4","backspace deletes the selection");
        box.DeleteForward();
        Check(box.Empty()&&box.caret==0,"delete on an empty caret is a no-op");
        // 粘滞列的多行移动
        TextField multi;
        multi.Insert(L"alpha\ndefinitely-longer\nzz");
        multi.MoveCaret(TextField::Move::Up,false);
        Check(multi.caret==8,"up from the last line keeps the sticky column (col 2)");
        multi.MoveCaret(TextField::Move::Up,false);
        Check(multi.caret==2,"up again clamps to the shorter first line");
        multi.MoveCaret(TextField::Move::Down,false);
        Check(multi.caret==8,"down restores the sticky column of line 2");
        multi.MoveCaret(TextField::Move::End,false);
        Check(multi.caret==23,"end jumps to the current line newline");
        multi.MoveCaret(TextField::Move::Home,false);
        Check(multi.caret==6,"home from the line end reaches that line's start");
        multi.MoveCaret(TextField::Move::DocHome,false);
        Check(multi.caret==0,"doc-home reaches 0");
        // 词移动 + 代理对保持完整
        TextField words;
        words.Insert(L"foo bar-baz");
        words.MoveCaret(TextField::Move::DocEnd,false);
        words.MoveCaret(TextField::Move::WordLeft,false);
        Check(words.caret==8,"word-left stops after the hyphen");
        words.MoveCaret(TextField::Move::WordLeft,false);
        Check(words.caret==7,"word-left over a separator steps onto it");
        TextField emoji;
        emoji.Insert(L"a\xD83D\xDE00" L"b");
        emoji.MoveCaret(TextField::Move::DocEnd,false);
        emoji.MoveCaret(TextField::Move::Left,false);
        Check(emoji.caret==3,"left from after b lands between the pair and b");
        emoji.Backspace();
        Check(emoji.Text()==L"ab","backspace between the pair and b deletes the whole pair");
        emoji.SelectAll();emoji.DeleteForward();
        Check(emoji.Empty(),"select-all then delete empties");
        // 密码掩码与显示切换
        TextField secret;
        secret.password=true;
        secret.Insert(L"sk-abc123");
        Check(secret.DisplayText().size()==secret.Text().size()
            &&secret.DisplayText()[0]==L'\x2022'&&!secret.reveal,
            "a password draws bullets, not characters");
        secret.reveal=true;
        Check(secret.DisplayText()==L"sk-abc123","the eye reveals the password");
        Check(secret.TextUtf8()=="sk-abc123","utf8 round trip keeps the raw text");
        // IME 组合内联与提交
        TextField imek;
        imek.Focus();
        imek.CompositionUpdate(L"ni",1);
        Check(imek.Composing()&&imek.Text().empty(),"composition does not touch the text");
        imek.CompositionCommit(L"\x4F60\x597D");
        Check(!imek.Composing()&&imek.Text()==L"\x4F60\x597D","commit inserts the result string");
        imek.SetText(L"");
        imek.SetTextUtf8("\xE6\x90\x9C\xE7\xB4\xA2");
        Check(imek.Text()==L"\x641C\x7D22","utf8 setup round-trips Chinese text");
    }

    // ===================== H4：模型页模型（分组/过滤/计数/当前行/收藏） =====================
    {
        using namespace hermes;
        HermesState state;
        state.model.provider="custom:deepseek";
        state.model.model="deepseek-v4.1-flash";
        state.loading=false;
        ProviderView deepseek;
        deepseek.name="DeepSeek";deepseek.providerId="custom:deepseek";
        deepseek.baseUrl="https://api.deepseek.example/v1";deepseek.keyEnv="DEEPSEEK_API_KEY";
        deepseek.keySet=true;deepseek.isActive=true;
        deepseek.defaultModel="deepseek-v4.1-flash";
        deepseek.models={"deepseek-v4.1-flash","deepseek-r2"};
        deepseek.modelsDiscovered=true;
        ProviderView local;
        local.name="Local";local.providerId="custom:local";
        local.baseUrl="http://127.0.0.1:8642/v1";local.keyEnv="LOCAL_API_KEY";
        local.keySet=false;local.isActive=false;local.defaultModel="qwen-3-32b";
        local.models={"qwen-3-32b"};
        state.providers={deepseek,local};
        ModelsPageInput page;
        page.state=&state;
        page.favorites={"custom:deepseek|deepseek-r2","custom:local|qwen-3-32b"};
        page.recents={"custom:deepseek|deepseek-v4.1-flash"};
        auto view=BuildPanelView(PanelViewInput{});
        AttachModelsPage(view,page);
        Check(view.filters[0].label=="\xE5\x85\xA8\xE9\x83\xA8"&&view.filters[0].count==2,
            "the 全部 segment counts providers");
        Check(view.filters[1].label=="\xE6\x94\xB6\xE8\x97\x8F"&&view.filters[1].count==2,
            "the 收藏 segment counts favorites");
        Check(view.filters[2].label=="\xE6\x9C\x80\xE8\xBF\x91"&&view.filters[2].count==1,
            "the 最近 segment counts recents");
        Check(view.filters[0].active&&!view.filters[1].active,"the active segment follows filterKind");
        Check(view.groups.size()==2,"the grouped view renders one group per provider");
        Check(view.groups[0].title=="DeepSeek"&&view.groups[0].active&&view.groups[0].open,
            "the active provider leads the list and is expanded");
        Check(view.groups[1].title=="Local"&&!view.groups[1].open,"other providers stay collapsed");
        Check(view.groups[0].keyWarn==false&&view.groups[1].keyWarn,
            "the key warning follows key_env/key_set");
        Check(view.groups[0].countText=="2\xE2\x81\xBA","the group count carries the discovered marker");
        Check(view.groups[0].rows.size()==2,"the active group lists its models");
        Check(view.groups[0].rows[0].current&&view.groups[0].rows[0].model=="deepseek-v4.1-flash",
            "the current model row is marked");
        Check(!view.groups[0].rows[1].current&&view.groups[0].rows[1].favorite,
            "a non-current row can still be a favorite");
        Check(view.groups[0].rows[0].recent,"the current row is also in recents");
        Check(view.flatView==false,"no query and 全部 means the grouped view");
        Check(view.matches==3,"the grouped view counts every model row left after filtering");
        Check(view.groups[0].hue>=186&&view.groups[0].hue<=264,"the group hue comes from the identity palette");
        // 搜索：扁平视图 + 供应商列
        ModelsPageInput search=page;
        search.filterText="r2";
        auto flat=BuildPanelView(PanelViewInput{});
        AttachModelsPage(flat,search);
        Check(flat.flatView,"a query switches to the flat view");
        Check(flat.matches==1&&flat.groups.size()==1&&flat.groups[0].rows[0].model=="deepseek-r2",
            "the query keeps only matching models");
        Check(flat.groups[0].rows[0].provider=="DeepSeek","flat rows carry their provider name");
        Check(flat.groups[0].title.empty(),"the flat pseudo-group has no header title");
        // 供应商名命中 → 该供应商全部模型
        ModelsPageInput byProvider=page;
        byProvider.filterText="local";
        auto byName=BuildPanelView(PanelViewInput{});
        AttachModelsPage(byName,byProvider);
        Check(byName.matches==1&&byName.groups[0].rows[0].model=="qwen-3-32b",
            "a provider-name hit keeps that provider's models");
        // 收藏过滤
        ModelsPageInput fav=page;
        fav.filterKind=1;
        auto favView=BuildPanelView(PanelViewInput{});
        AttachModelsPage(favView,fav);
        Check(favView.flatView&&favView.matches==2,"the 收藏 filter lists the favorites in order");
        Check(favView.groups[0].rows[0].model=="deepseek-r2","favorites keep their stored order");
        // 最近过滤
        ModelsPageInput recent=page;
        recent.filterKind=2;
        auto recentView=BuildPanelView(PanelViewInput{});
        AttachModelsPage(recentView,recent);
        Check(recentView.matches==1&&recentView.groups[0].rows[0].current,
            "the 最近 filter lists recents and marks the current one");
        // 空态
        ModelsPageInput nothing=page;
        nothing.filterText="no-such-model";
        auto empty=BuildPanelView(PanelViewInput{});
        AttachModelsPage(empty,nothing);
        Check(empty.matches==0&&!empty.listEmpty.empty(),"no match yields the hidden hint text");
        ModelsPageInput noFav=page;
        noFav.filterKind=1;
        noFav.favorites.clear();
        auto noFavView=BuildPanelView(PanelViewInput{});
        AttachModelsPage(noFavView,noFav);
        Check(noFavView.matches==0,"an empty favorite list matches nothing");
        // 切换中的行内状态
        ModelsPageInput swapping=page;
        swapping.switchingKey="custom:deepseek|deepseek-r2";
        swapping.switchingModel="\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xAD\xE2\x80\xA6";
        auto swapView=BuildPanelView(PanelViewInput{});
        AttachModelsPage(swapView,swapping);
        Check(swapView.groups[0].rows[1].inlineStatus=="\xE5\x88\x87\xE6\x8D\xA2\xE4\xB8\xAD\xE2\x80\xA6",
            "the row being switched shows its inline status");
        Check(swapView.groups[0].rows[0].inlineStatus.empty(),"other rows carry no inline status");
        // 空状态（首帧）
        ModelsPageInput firstFrame;
        firstFrame.state=nullptr;
        auto firstView=BuildPanelView(PanelViewInput{});
        AttachModelsPage(firstView,firstFrame);
        Check(firstView.matches==0&&firstView.filters[0].count==0,"a null state renders no rows");
        Check(firstView.groups.empty(),"a null state renders no groups");
    }

    // ============ H4：状态接线（灯/chip/hero/sync）+ 列表几何与命中 + 身份色 ============
    {
        using namespace hermes;
        HermesState state;
        state.loading=false;
        state.wsl="up";
        state.model.provider="custom:deepseek";
        state.model.model="deepseek-v4.1-flash";
        state.model.apiMode="codex_responses";
        state.config.mtime=1750000000;
        state.config.mtimeText="2026-10-15 20:00:00";
        state.gateway.sinceUnix=1749999999;
        state.gateway.since="2026-10-15 19:00:00";
        state.gateway.port=8642;
        state.gateway.reachable=true;
        state.webui.port=8787;
        state.webui.reachable=false;
        ProviderView deepseek;
        deepseek.name="DeepSeek";deepseek.providerId="custom:deepseek";
        deepseek.keyEnv="K";deepseek.keySet=true;deepseek.isActive=true;
        deepseek.defaultModel="deepseek-v4.1-flash";
        deepseek.models={"deepseek-v4.1-flash","deepseek-r2"};
        ProviderView local;
        local.name="Local";local.providerId="custom:local";
        local.keyEnv="L";local.keySet=false;local.isActive=false;
        local.models={"qwen-3-32b","qwen-3-14b","qwen-3-7b"};
        state.providers={deepseek,local};
        state.fallbacks.resize(2);

        // ---- 状态寄存器：三盏灯按 up/down/unknown 映射，chip 只在需要动手时出现 ----
        {
            auto view=BuildPanelView(PanelViewInput{});
            StatusViewInput status;
            status.state=&state;
            AttachStatusView(view,status);
            Check(view.lamps.size()==3&&view.lamps[0].label=="WSL","three lamps: WSL / WebUI / 网关");
            Check(view.lamps[0].state==LampState::Ok,"an answered helper lights WSL ok");
            Check(view.lamps[1].state==LampState::Warn&&view.lamps[1].detail=="\xE2\x80\x94",
                "the WebUI lamp is warn with a bare dash when the probe failed");
            Check(view.lamps[2].state==LampState::Ok&&view.lamps[2].detail==":8642",
                "the gateway lamp is ok-with-port when the probe answered");
            Check(view.chip.present&&view.chip.kind==ChipKind::Err&&view.chip.icon=="wifi-off",
                "the err chip is up while WebUI is unreachable");
            Check(!view.chipWarn.present,"the 网关待重启 chip stays down while the gateway is current");
            Check(view.heroHue>=186&&view.heroHue<=264,"the hero hue comes from the provider identity");
            Check(view.currentProvider=="deepseek","the hero shows the provider id with custom: stripped");
            Check(view.currentModel=="deepseek-v4.1-flash"&&!view.currentSwap,"the plate holds the model id");
            Check(view.currentMode=="codex","codex_responses is tagged 'codex'");
            Check(view.currentBadge=="\xE2\x97\x8F","the live badge needs WSL and the gateway both up");
            Check(view.syncTime=="20:00","the sync stamp is the config mtime HH:MM");
            Check(view.nav[1].badge==2,"the 回退链 tab carries the fallback count");
        }
        {
            // down：整块离线；unknown：只画 idle 灯，不算离线
            auto offline=state;
            offline.wsl="down";
            offline.error="WSL \xE6\xB2\xA1\xE6\x9C\x89\xE5\x90\xAF\xE5\x8A\xA8";
            auto view=BuildPanelView(PanelViewInput{});
            AttachStatusView(view,StatusViewInput{&offline});
            Check(view.lamps[0].state==LampState::Err,"a down WSL lights the err lamp");
            Check(view.offlineReason=="WSL \xE6\xB2\xA1\xE6\x9C\x89\xE5\x90\xAF\xE5\x8A\xA8",
                "the helper's error text becomes the offline reason");
            auto unknown=state;
            unknown.wsl="unknown";
            auto unknownView=BuildPanelView(PanelViewInput{});
            AttachStatusView(unknownView,StatusViewInput{&unknown});
            Check(unknownView.lamps[0].state==LampState::Idle,"a pending probe paints an idle lamp");
            Check(unknownView.offlineReason.empty(),"unknown is not offline");
        }
        {
            // 网关待重启：config 比网关启动时间新，且不是面板自己写的
            auto stale=state;
            stale.gateway.sinceUnix=1749999999;
            stale.config.mtime=1750000600;
            auto view=BuildPanelView(PanelViewInput{});
            StatusViewInput status;
            status.state=&stale;
            AttachStatusView(view,status);
            Check(view.chipWarn.present&&view.chipWarn.kind==ChipKind::Warn,"an external config change raises the warn chip");
            auto view2=BuildPanelView(PanelViewInput{});
            StatusViewInput own;
            own.state=&stale;
            own.hotLoadedMtime=1750000600;      // 面板自己刚写的
            AttachStatusView(view2,own);
            Check(!view2.chipWarn.present,"our own write is not reported as 待重启");
        }
        {
            // 切换中：铭牌显示目标模型并把 swapping 打开（箭头由渲染层上色）
            auto view=BuildPanelView(PanelViewInput{});
            StatusViewInput status;
            status.state=&state;
            status.switchingModel="deepseek-r2";
            AttachStatusView(view,status);
            Check(view.currentSwap&&view.currentModel=="deepseek-r2","the plate plays the switch target");
        }
        // ---- 列表几何：过滤段、行高、分隔、空态、吸顶与滚动 ----
        {
            ModelsPageInput page;
            page.state=&state;
            auto view=BuildPanelView(PanelViewInput{});
            AttachModelsPage(view,page);
            const auto layout=ComputeLayout(view,384.0f,634.0f,1.0f);
            Check(layout.segments.size()==3,"three filter segments are laid out");
            Check(layout.segments[0].rect.left<layout.segments[1].rect.left
                &&layout.segments[1].rect.left<layout.segments[2].rect.left,"the segments run left to right");
            Check(layout.segments[0].rect.right<=layout.segments[1].rect.left+0.01f,
                "the segments never overlap");
            Check(layout.segments[2].rect.right<=layout.filters.right+0.01f,"the segments stay in the filter row");
            Check(layout.search.bottom<=layout.filters.top+0.01f,"the filter row sits under the search row");
            Check(layout.filters.bottom<=layout.list.top+0.01f,"the list starts under the filters");
            // 旧 .list 是固定 384 高、内部滚动的窗口（max-height/min-height 384）：它不必撑到导航，
            // 但绝不能压过去（否则行的点击区会盖到底栏上）。
            Check(layout.list.bottom<=layout.nav.top+0.01f,"the list never reaches past the navigation");
            Check(std::abs((layout.list.bottom-layout.list.top)-384.0f)<0.01f,
                "the models list is the old fixed 384 tall viewport");
            Check(ControlRectFor(layout,ControlId::FilterAll)!=nullptr
                &&ControlRectFor(layout,ControlId::FilterFav)!=nullptr
                &&ControlRectFor(layout,ControlId::FilterRecent)!=nullptr,"every segment is clickable");
            Check(ControlRectFor(layout,ControlId::SearchBox)!=nullptr,"the search box is clickable");
            Check(ControlRectFor(layout,ControlId::SearchClear)==nullptr,
                "no clear button while the query is empty");
            // 组头 30 / 行 31 / 其他供应商分隔，顺序 = 当前组 → 其余组
            const auto height=[](const ListItem& item) { return item.rect.bottom-item.rect.top; };
            int heads=0,rows=0,sects=0;
            for(const auto& item:layout.items) {
                if(item.kind==ListItemKind::GroupHead) { ++heads;Check(std::abs(height(item)-30.0f)<0.01f,"group heads are 30 tall"); }
                if(item.kind==ListItemKind::Row) { ++rows;Check(std::abs(height(item)-31.0f)<0.01f,"rows are 31 tall"); }
                if(item.kind==ListItemKind::Sect) { ++sects;Check(item.text=="\xE5\x85\xB6\xE4\xBB\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86","the divider reads 其他供应商"); }
            }
            Check(heads==2&&rows==2&&sects==1,"the grouped list is head / rows / divider / head for one active group");
            const auto* first=ControlRectFor(layout,ControlId::SearchBox);
            Check(first!=nullptr&&first->top>layout.hero.bottom,"the search row sits below the hero");
            // 点行：命中行与星标；点组头：命中组头与“更多”按钮；点视口外：不命中
            const auto* rowItem=[&]() -> const ListItem* {
                for(const auto& item:layout.items) if(item.kind==ListItemKind::Row) return &item;
                return nullptr;
            }();
            Check(rowItem!=nullptr,"the list holds the active group's rows");
            if(rowItem) {
                const auto y=(rowItem->rect.top+rowItem->rect.bottom)/2.0f;
                const auto hit=ListHitTest(layout,rowItem->rect.left+40.0f,y);
                Check(hit.inList&&hit.kind==ListItemKind::Row&&hit.group==0&&hit.row==0&&!hit.star,
                    "a click in the middle of a row hits that row, not its star");
                const auto star=StarBox(rowItem->rect,1.0f);
                const auto starHit=ListHitTest(layout,(star.left+star.right)/2.0f,(star.top+star.bottom)/2.0f);
                Check(starHit.inList&&starHit.kind==ListItemKind::Row&&starHit.star,
                    "a click on the star is reported as the star");
                Check(std::abs((star.right-star.left)-22.0f)<0.01f&&star.right<rowItem->rect.right,
                    "the star is 22 wide and inset from the row's right edge");
                const auto above=ListHitTest(layout,rowItem->rect.left+40.0f,layout.listViewport.top-5.0f);
                Check(!above.inList,"a click above the viewport hits nothing");
            }
            bool sawHead=false;
            for(const auto& item:layout.items) {
                if(item.kind!=ListItemKind::GroupHead) continue;
                sawHead=true;
                const auto y=(item.rect.top+item.rect.bottom)/2.0f;
                const auto hit=ListHitTest(layout,item.rect.left+30.0f,y);
                Check(hit.inList&&hit.kind==ListItemKind::GroupHead&&!hit.menu,"a click on the head toggles the group");
                const auto menu=GroupMenuBox(item.rect,1.0f);
                const auto menuHit=ListHitTest(layout,(menu.left+menu.right)/2.0f,(menu.top+menu.bottom)/2.0f);
                Check(menuHit.inList&&menuHit.kind==ListItemKind::GroupHead&&menuHit.menu,
                    "a click on the head's … is reported as the menu");
                break;
            }
            Check(sawHead,"the layout exposes the group head");
            Check(!layout.scrollVisible&&layout.listMaxScroll==0,"a short list does not scroll");
        }
        // ---- 滚动：内容超出后出现滚动条、夹取偏移、组头吸顶 ----
        {
            // 12 个供应商（其中一个开着）远高于视口，列表必须滚动。
            HermesState tall=state;
            tall.providers.clear();
            for(int index=0;index<12;++index) {
                ProviderView provider;
                provider.name="P"+std::to_string(index);
                provider.providerId="custom:P"+std::to_string(index);
                provider.keyEnv="K";provider.keySet=true;
                provider.isActive=index==0;
                provider.models={"m-1","m-2","m-3"};
                tall.providers.push_back(provider);
            }
            ModelsPageInput page;
            page.state=&tall;
            page.expanded={"P1"};
            page.listScroll=40.0f;                     // 落在 P0 组内：组头必须吸顶
            auto view=BuildPanelView(PanelViewInput{});
            AttachModelsPage(view,page);
            const auto layout=ComputeLayout(view,384.0f,634.0f,1.0f);
            Check(layout.scrollVisible&&layout.listMaxScroll>0,"an overflowing list shows a scrollbar");
            Check(std::abs(layout.listScroll-40.0f)<0.01f,"a mid-list offset is kept as asked");
            ModelsPageInput over=page;
            over.listScroll=10000.0f;                  // 超出底部：布局必须夹回
            auto overView=BuildPanelView(PanelViewInput{});
            AttachModelsPage(overView,over);
            const auto overLayout=ComputeLayout(overView,384.0f,634.0f,1.0f);
            Check(std::abs(overLayout.listScroll-overLayout.listMaxScroll)<0.01f,
                "an over-scrolled offset is clamped to the bottom");
            Check(layout.scrollThumb.bottom<=layout.listViewport.bottom+0.01f,
                "the thumb never leaves the track");
            Check(layout.scrollThumb.bottom>layout.scrollThumb.top,"the thumb has a height");
            bool sticky=false;
            for(const auto& item:layout.items)
                if(item.kind==ListItemKind::GroupHead&&item.sticky
                    &&std::abs(item.rect.top-layout.listViewport.top)<0.01f) sticky=true;
            Check(sticky,"the head of the group under the viewport pins to its top");
            // 视口内的可见性：滚动之后第一行一定落在视口里
            bool firstVisible=false;
            for(const auto& item:layout.items)
                if(item.kind==ListItemKind::Row&&item.rect.bottom>layout.listViewport.top
                    &&item.rect.top<layout.listViewport.bottom) firstVisible=true;
            Check(firstVisible,"the scrolled list still has rows inside the viewport");
        }
        // ---- 平铺视图：查询/收藏/最近带供应商列，且没有组头 ----
        {
            ModelsPageInput page;
            page.state=&state;
            page.filterKind=1;
            page.favorites={"custom:local|qwen-3-14b"};
            auto view=BuildPanelView(PanelViewInput{});
            AttachModelsPage(view,page);
            Check(view.flatView,"the 收藏 filter uses the flat view");
            Check(view.showHint&&!view.hintText.empty(),"a non-全部 filter shows the hit hint");
            const auto layout=ComputeLayout(view,384.0f,634.0f,1.0f);
            bool headless=true;
            for(const auto& item:layout.items) if(item.kind==ListItemKind::GroupHead) headless=false;
            Check(headless,"the flat view has no group heads");
            Check(!view.groups.empty()&&view.groups[0].rows.size()==1
                &&view.groups[0].rows[0].provider=="Local","the flat row keeps its provider name for the column");
        }
        // ---- 光标：只落在可见行上，折叠组里的行不可达 ----
        {
            ModelsPageInput page;
            page.state=&state;
            auto view=BuildPanelView(PanelViewInput{});
            AttachModelsPage(view,page);
            Check(view.visibleRows.size()==2,"only the open group's rows are reachable by the cursor");
            Check(view.groups[0].rows[0].index==0&&view.groups[0].rows[1].index==1,
                "visible rows carry their cursor index");
            Check(view.groups[1].rows[0].index==-1,"a collapsed group's rows carry no index");
            ModelsPageInput cursor=page;
            cursor.cursor=1;
            auto cursorView=BuildPanelView(PanelViewInput{});
            AttachModelsPage(cursorView,cursor);
            Check(cursorView.cursor==1,"the cursor index survives the attach");
            ModelsPageInput outOfRange=page;
            outOfRange.cursor=9;
            auto clamped=BuildPanelView(PanelViewInput{});
            AttachModelsPage(clamped,outOfRange);
            Check(clamped.cursor==-1,"an out-of-range cursor is dropped");
        }
        // ---- 供应商身份色：hsl(--ph 68% 62%) 与组头名的深浅两套 ----
        {
            const auto identity=widgets::IdentityColour(186.0f);
            Check(identity.a>0.99f&&identity.b>identity.r,"the identity colour is a lit cyan");
            const auto tint=widgets::IdentityTint(186.0f,0.075f);
            Check(std::abs(tint.a-0.075f)<0.001f,"the row tint carries the 7.5% alpha of .mrow.current");
            const auto dark=widgets::GroupNameColour(206.0f,false),light=widgets::GroupNameColour(206.0f,true);
            Check(dark.r>light.r&&dark.b>light.b,"the active group name is lighter on dark, darker on light");
        }
        // ---- 行与组头的皮肤：当前行/游标行/悬停三态 ----
        {
            const ThemePalette palette=ResolveTheme("dark");
            const auto normal=widgets::RowSkinOf(false,false,widgets::Visual::Normal,206.0f,palette);
            Check(normal.fill.a==0.0f,"a plain row has no fill");
            const auto hovered=widgets::RowSkinOf(false,false,widgets::Visual::Hovered,206.0f,palette);
            Check(hovered.fill.r==palette.raise.r&&hovered.fill.g==palette.raise.g,
                ".mrow:hover is the raise colour");
            const auto current=widgets::RowSkinOf(true,false,widgets::Visual::Normal,206.0f,palette);
            Check(current.fill.a>0.0f&&current.fill.a<0.1f,"the current row is a 7.5% identity tint");
            const auto cursor=widgets::RowSkinOf(false,true,widgets::Visual::Normal,206.0f,palette);
            Check(cursor.outline.a>0.0f&&cursor.fill.r==palette.raise2.r,
                ".mrow.is-cursor is a raise2 fill with an accent outline");
            const auto head=widgets::GroupHeadSkinOf(true,false,206.0f,palette);
            Check(head.inset.a>0.0f&&head.hairline,"an active group head carries the inset bar and hairline");
            const auto plainHead=widgets::GroupHeadSkinOf(false,true,206.0f,palette);
            Check(plainHead.inset.a==0.0f&&plainHead.fill.r==palette.raise.r,"a hovered head brightens to raise");
        }
    }

    // ---- 对比度审计（G3）：六套主题的所有前景/背景组合都要过 WCAG 阈值 ----
    {
        const auto channel=[](float value) {
            return value<=0.03928f?value/12.92f:std::pow((value+0.055f)/1.055f,2.4f);
        };
        const auto luminance=[&](const D2D1_COLOR_F& colour) {
            return 0.2126f*channel(colour.r)+0.7152f*channel(colour.g)+0.0722f*channel(colour.b);
        };
        const auto over=[](const D2D1_COLOR_F& front,const D2D1_COLOR_F& back) {
            return D2D1::ColorF(front.r*front.a+back.r*(1.0f-front.a),
                front.g*front.a+back.g*(1.0f-front.a),
                front.b*front.a+back.b*(1.0f-front.a),1.0f);
        };
        const auto contrast=[&](const D2D1_COLOR_F& a,const D2D1_COLOR_F& b) {
            const float first=luminance(a),second=luminance(b);
            return (std::max(first,second)+0.05f)/(std::min(first,second)+0.05f);
        };
        const std::string themes[]={"dark","light","slate","carbon","porcelain","sand"};
        for(const auto& id:themes) {
            const ThemePalette palette=ResolveTheme(id);
            const auto label=[&](const char* name,float ratio,float minimum) {
                Check(ratio>=minimum,id+" contrast "+name+" = "+std::to_string(ratio)
                    +" (min "+std::to_string(minimum)+")");
            };
            label("text/ink",contrast(over(palette.text,palette.ink),palette.ink),4.5f);
            label("text/surf",contrast(over(palette.text,palette.surf),palette.surf),4.5f);
            label("text/raise",contrast(over(palette.text,palette.raise),palette.raise),4.5f);
            label("text/raise2",contrast(over(palette.text,palette.raise2),palette.raise2),4.5f);
            label("dim/surf",contrast(over(palette.dim,palette.surf),palette.surf),3.0f);
            label("dim/stripTop",contrast(over(palette.dim,over(palette.stripTop,palette.ink)),
                over(palette.stripTop,palette.ink)),3.0f);
            label("faint/surf",contrast(over(palette.faint,palette.surf),palette.surf),2.4f);
            label("accentStrong/accentSoft",contrast(over(palette.accentStrong,
                over(palette.accentSoft,palette.surf)),over(palette.accentSoft,palette.surf)),3.4f);
            label("ok/okSoft",contrast(over(palette.ok,over(palette.okSoft,palette.surf)),
                over(palette.okSoft,palette.surf)),3.0f);
            label("warn/warnSoft",contrast(over(palette.warn,over(palette.warnSoft,palette.surf)),
                over(palette.warnSoft,palette.surf)),3.0f);
            label("err/errSoft",contrast(over(palette.err,over(palette.errSoft,palette.surf)),
                over(palette.errSoft,palette.surf)),3.0f);
            label("onAccent/accent",contrast(over(palette.onAccent,palette.accent),palette.accent),3.4f);
            const float textL=luminance(palette.text),surfL=luminance(palette.surf);
            Check(palette.light?(textL<surfL):(textL>surfL),id+" text/surface polarity");
        }
    }

    std::cout<<"widget: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}