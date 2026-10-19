#include "core/geometry.h"
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace hermes;
namespace {
int passed = 0;
int failed = 0;
void Check(bool condition, const std::string& name) {
    if (condition) ++passed;
    else { ++failed; std::cerr << "FAIL " << name << '\n'; }
}
}
int main() {
    const Monitor normal{{0, 0, 1920, 1080}, {0, 0, 1920, 1040}, 96};
    struct RectCase { const char* name; GeometryModel model; Monitor monitor; WindowRects expected; };
    const std::vector<RectCase> cases{
        {"R01 floating origin preserved", {Edge::None, Tier::Full, 0, {519, 514}}, normal, {{519,514,384,634}, std::nullopt}},
        {"R02 left dock", {Edge::Left, Tier::Full, 123}, normal, {{0,123,384,634}, Rect{0,402,10,76}}},
        {"R03 right dock", {Edge::Right, Tier::Full, 123}, normal, {{1536,123,384,634}, Rect{1910,402,10,76}}},
        {"R04 top dock", {Edge::Top, Tier::Full, 253}, normal, {{253,0,384,634}, Rect{407,0,76,10}}},
        {"R05 bottom dock", {Edge::Bottom, Tier::Full, 253}, normal, {{253,406,384,634}, Rect{407,1030,76,10}}},
        {"R06 left anchor clamps low", {Edge::Left, Tier::Full, -90}, normal, {{0,0,384,634}, Rect{0,279,10,76}}},
        {"R07 right anchor clamps high", {Edge::Right, Tier::Full, 1900}, normal, {{1536,406,384,634}, Rect{1910,685,10,76}}},
        {"R08 top anchor clamps low", {Edge::Top, Tier::Full, -10}, normal, {{0,0,384,634}, Rect{154,0,76,10}}},
        {"R09 bottom anchor clamps high", {Edge::Bottom, Tier::Full, 1919}, normal, {{1536,406,384,634}, Rect{1690,1030,76,10}}},
        {"R10 compact keeps floating origin", {Edge::None, Tier::Compact, 0, {519,514}}, normal, {{519,514,384,152}, std::nullopt}},
        {"R11 compact bottom remains exact", {Edge::Bottom, Tier::Compact, 253}, normal, {{253,888,384,152}, Rect{407,1030,76,10}}},
        {"R12 negative monitor left", {Edge::Left, Tier::Full, -720}, {{-1920,-800,1920,1080},{-1920,-800,1920,1040},96}, {{-1920,-720,384,634}, Rect{-1920,-441,10,76}}},
        {"R13 internal right edge stays on owner monitor", {Edge::Right, Tier::Full, 70}, {{-1920,0,1920,1080},{-1920,0,1920,1040},96}, {{-384,70,384,634}, Rect{-10,349,10,76}}},
        {"offset work area left taskbar", {Edge::Left, Tier::Full, 0}, {{0,0,1920,1080},{48,32,1872,1008},96}, {{48,32,384,634}, Rect{48,311,10,76}}},
        {"126 dpi full rounding", {Edge::Right, Tier::Full, 121}, {{0,0,2560,1440},{0,0,2560,1380},126}, {{2056,121,504,832}, Rect{2547,487,13,100}}},
        {"202 dpi compact rounding", {Edge::Bottom, Tier::Compact, 401}, {{2560,-500,2560,1440},{2560,-500,2560,1400},202}, {{2560,580,808,320}, Rect{2884,879,160,21}}},
        {"portrait bottom", {Edge::Bottom, Tier::Full, 700}, {{-1080,0,1080,1920},{-1080,0,1080,1880},96}, {{-384,1246,384,634}, Rect{-230,1870,76,10}}},
        {"work area smaller than panel clamps safely", {Edge::Left, Tier::Full, 400}, {{0,0,320,200},{0,20,320,180},96}, {{0,20,384,634}, Rect{0,124,10,76}}}
    };
    for (const auto& test : cases) Check(ComputeRect(test.model, test.monitor) == test.expected, test.name);

    // G11 挂件尺寸表（DIP → 像素）：竖边高、横边宽；三套窄型（真机要求"不占地方"）。
    Check(WidgetSize(WidgetStyle::Beacon,Edge::Left,96) == Size{10,76}, "beacon vertical size");
    Check(WidgetSize(WidgetStyle::Beacon,Edge::Top,96) == Size{76,10}, "beacon horizontal size");
    Check(WidgetSize(WidgetStyle::Tag,Edge::Right,96) == Size{14,74}, "tag size");
    Check(WidgetSize(WidgetStyle::Flag,Edge::Bottom,96) == Size{58,16}, "flag horizontal size");
    Check(WidgetSize(WidgetStyle::Capsule,Edge::Left,96) == Size{30,136}, "capsule vertical size");
    Check(WidgetSize(WidgetStyle::Capsule,Edge::Top,96) == Size{136,30}, "capsule horizontal size");
    Check(WidgetSize(WidgetStyle::Tag,Edge::Left,126) == Size{18,97}, "tag scales with dpi");
    Check(WidgetMetricsFor(WidgetStyle::Beacon,96).gap==3
        &&WidgetMetricsFor(WidgetStyle::Flag,96).tail==12
        &&WidgetMetricsFor(WidgetStyle::Tag,192).spine==6,"widget metrics scale with dpi");
    {
        // 每种样式都：贴边侧逐像素贴住工作区，整块落在工作区内。
        const GeometryModel base{Edge::Left,Tier::Full,123};
        const std::vector<WidgetStyle> styles{WidgetStyle::Beacon,WidgetStyle::Tag,WidgetStyle::Flag,
            WidgetStyle::Capsule};
        for(const auto style:styles) {
            for(const auto edge:{Edge::Left,Edge::Right,Edge::Top,Edge::Bottom}) {
                auto model=base;
                model.edge=edge;
                model.widget=style;
                const auto rects=ComputeRect(model,normal);
                const auto& strip=*rects.strip;
                Check(Contains(normal.work,{strip.x,strip.y})&&strip.right()<=normal.work.right()
                    &&strip.bottom()<=normal.work.bottom(),"widget stays inside the work area");
                if(edge==Edge::Left) Check(strip.x==normal.work.x,"left widget is flush");
                if(edge==Edge::Right) Check(strip.right()==normal.work.right(),"right widget is flush");
                if(edge==Edge::Top) Check(strip.y==normal.work.y,"top widget is flush");
                if(edge==Edge::Bottom) Check(strip.bottom()==normal.work.bottom(),"bottom widget is flush");
            }
        }
    }
    {
        bool overflowed=false;
        try { (void)ScaleDip(2147483000,288); } catch(const std::overflow_error&) { overflowed=true; }
        Check(overflowed,"ScaleDip reports overflow instead of wrapping");
        bool badDpi=false;
        try { (void)ScaleDip(10,0); } catch(const std::invalid_argument&) { badDpi=true; }
        Check(badDpi,"ScaleDip rejects a zero DPI");
    }
    Check(std::string(WidgetStyleName(WidgetStyle::Tag))=="tag"
        &&WidgetStyleFromName("flag")==WidgetStyle::Flag
        &&WidgetStyleFromName("halo")==WidgetStyle::Tag          // 旧值映射
        &&WidgetStyleFromName("capsule")==WidgetStyle::Capsule
        &&WidgetStyleFromName("pulse")==WidgetStyle::Beacon
        &&WidgetStyleFromName("nope")==WidgetStyle::Beacon,"widget style name round trip");

    // 贴边判定**只看窗口**（真机反馈）：final/start 两个矩形决定一切，鼠标位置不参与。
    // `start` 是这次拖拽开始时的窗口矩形 —— 越界那一档要求"拖拽前还在工作区内"，
    // 才不会把「面板比工作区高、一开始就露在外面」误判成贴边意图。
    struct SnapCase {
        const char* name; Rect rect; Rect start; Edge previous; Edge edge; int anchor;
    };
    const std::vector<SnapCase> snaps{
        {"snap left inclusive positive boundary", {16,121,360,470}, {40,121,360,470}, Edge::None, Edge::Left,121},
        {"snap left inclusive negative boundary", {-16,121,360,470}, {40,121,360,470}, Edge::None, Edge::Left,121},
        {"outside positive band floats", {17,121,360,470}, {40,121,360,470}, Edge::None, Edge::None,0},
        // 越过边缘：这次拖拽把窗口推过去的就算贴边，指针在哪里都一样（鼠标不可能再往前走，
        // 窗口浮在屏幕外一半也从来不是手势的意思）。
        {"overshoot left still docks", {-17,121,360,470}, {40,121,360,470}, Edge::None, Edge::Left,121},
        {"overshoot left far still docks", {-400,121,360,470}, {40,121,360,470}, Edge::None, Edge::Left,121},
        // 指针停在窗口中间、离边缘很远也一样贴边 —— 这正是真机上"窗口碰到边缘"的常规手势。
        {"overshoot left with the pointer far inside still docks",
            {-240,121,360,470}, {40,121,360,470}, Edge::None, Edge::Left,121},
        {"overshoot right still docks", {1700,121,360,470}, {1400,121,360,470}, Edge::None, Edge::Right,121},
        {"overshoot top still docks", {300,-60,360,470}, {300,40,360,470}, Edge::None, Edge::Top,300},
        {"overshoot bottom still docks", {300,700,360,470}, {300,300,360,470}, Edge::None, Edge::Bottom,300},
        {"crossed old edge does not pull back", {40,121,360,470}, {40,121,360,470}, Edge::Left, Edge::None,0},
        // 拖拽前就已经露在边缘外（比工作区高 / 起点在屏幕外）：不是这次拖拽的意图，保持浮动。
        {"a window already hanging past the bottom floats", {1340,766,450,713}, {1340,766,450,713},
            Edge::None, Edge::None,0},
        {"a too tall panel keeps floating", {300,700,450,713}, {300,700,450,713}, Edge::None, Edge::None,0},
        {"snap right boundary", {1544,121,360,470}, {1400,121,360,470}, Edge::None, Edge::Right,121},
        {"snap top boundary", {300,16,360,470}, {300,40,360,470}, Edge::None, Edge::Top,300},
        {"snap bottom boundary", {300,586,360,470}, {300,500,360,470}, Edge::None, Edge::Bottom,300},
        {"corner preserves current edge", {1,8,360,470}, {40,40,360,470}, Edge::Top, Edge::Top,1},
        {"corner picks nearest otherwise", {1,8,360,470}, {40,40,360,470}, Edge::None, Edge::Left,8},
        {"tie uses stable left right top bottom order", {8,8,360,470}, {40,40,360,470}, Edge::None, Edge::Left,8},
        {"left clamps vertical anchor", {0,600,360,470}, {40,500,360,470}, Edge::None, Edge::Left,570},
        {"bottom clamps horizontal anchor", {1800,570,360,470}, {1400,500,360,470}, Edge::None, Edge::Bottom,1560},
        {"distant floating drop preserved", {519,514,360,470}, {519,514,360,470}, Edge::Left, Edge::None,0},
        {"same edge free axis not quantized", {2,133,360,470}, {40,133,360,470}, Edge::Left, Edge::Left,133}
    };
    for (const auto& test : snaps) {
        const auto result = SnapDecision(test.rect, test.start, normal, test.previous);
        Check(result.edge == test.edge && result.anchor == test.anchor &&
            (test.edge != Edge::None || result.floating == Point{test.rect.x,test.rect.y}), test.name);
    }
    const Monitor high{{0,0,3840,2160},{0,0,3840,2100},202};
    Check(SnapDecision({34,200,758,989}, {200,200,758,989}, high, Edge::None).edge == Edge::Left, "high DPI snap band inclusive");
    Check(SnapDecision({35,200,758,989}, {200,200,758,989}, high, Edge::None).edge == Edge::None, "high DPI snap band exclusive");
    Check(ScaleDip(360,126) == 473 && ScaleDip(570,126) == 748 && ScaleDip(8,126) == 11, "126 dpi MulDiv rounding");
    Check(ScaleDip(360,202) == 758 && ScaleDip(570,202) == 1199 && ScaleDip(8,202) == 17, "202 dpi MulDiv rounding");
    Check(TierSize(Tier::Compact,202) == Size{808,320}, "compact uses same scale conversion");
    Check(Contains({-100,-50,20,30},{-100,-50}) && !Contains({-100,-50,20,30},{-80,-20}), "point bounds are half open");
    Check(!Intersects({-360,0,360,470},{0,0,1920,1080}) && Intersects({-359,0,360,470},{0,0,1920,1080}), "shared monitor edge has no intersection");
    Check(ClampFloating({-500,2000},{360,470},normal.work) == Point{0,570}, "off screen floating origin clamped");
    Check(NearestEdge({200,400,360,470},normal) == Edge::Bottom, "tray docking chooses nearest edge");
    GeometryModel stable{Edge::Bottom,Tier::Full,253};
    const auto initial = ComputeRect(stable,normal);
    for (int i=0;i<200;++i) Check(ComputeRect(stable,normal) == initial, "repeatability " + std::to_string(i));
    Check(stable.anchor == 253, "pure geometry never learns clamped readback");

    // ---- 属性模糊（第 6 轮迭代）：随机模型/显示器/DPI，挂件必须贴边、在工作区内、尺寸等于尺寸表 ----
    {
        std::mt19937 rng(20261018u);
        const auto randomInt=[&](int low,int high) {
            return std::uniform_int_distribution<int>(low,high)(rng);
        };
        const auto randomDpi=[&]() {
            static const int kDpis[]={96,120,125,144,150,168,192,216,240,288};
            return static_cast<unsigned>(kDpis[randomInt(0,9)]);
        };
        int iterations=0;
        for(int index=0;index<4000;++index) {
            Monitor monitor;
            monitor.dpi=randomDpi();
            const int workX=randomInt(-3000,3000),workY=randomInt(-3000,3000);
            const int workW=randomInt(120,4000),workH=randomInt(120,3000);
            monitor.work={workX,workY,workW,workH};
            monitor.bounds={workX,workY,workW,workH+randomInt(0,60)};
            GeometryModel model;
            model.edge=static_cast<Edge>(randomInt(0,4));
            model.tier=randomInt(0,1)==0?Tier::Full:Tier::Compact;
            model.anchor=randomInt(-6000,6000);
            model.floating={randomInt(-4000,4000),randomInt(-4000,4000)};
            model.widget=static_cast<WidgetStyle>(randomInt(0,3));
            const auto rects=ComputeRect(model,monitor);
            if(model.edge==Edge::None) {
                Check(!rects.strip.has_value(),"floating has no strip");
                continue;
            }
            ++iterations;
            if(!rects.strip) { Check(false,"a docked model always has a strip");continue; }
            const auto& strip=*rects.strip;
            const auto expected=WidgetSize(model.widget,model.edge,monitor.dpi);
            Check(strip.width==expected.width&&strip.height==expected.height,
                "the strip uses the documented widget size");
            // 挂件比工作区还大时（极端小屏）无法完全放进去：只要求贴边侧仍然贴住，
            // "整块在工作区内"只在尺寸放得下时断言。
            const bool fits=expected.width<=monitor.work.width&&expected.height<=monitor.work.height;
            if(fits)
                Check(Contains(monitor.work,{strip.x,strip.y})
                    &&strip.right()<=monitor.work.right()&&strip.bottom()<=monitor.work.bottom(),
                    "the strip stays inside the work area");
            if(model.edge==Edge::Left) Check(strip.x==monitor.work.x,"left strip is flush");
            if(model.edge==Edge::Right) Check(strip.right()==monitor.work.right(),"right strip is flush");
            if(model.edge==Edge::Top) Check(strip.y==monitor.work.y,"top strip is flush");
            if(model.edge==Edge::Bottom) Check(strip.bottom()==monitor.work.bottom(),"bottom strip is flush");
            Check(ComputeRect(model,monitor)==rects,"geometry is deterministic");
        }
        Check(iterations>2500,"the fuzz actually exercised docked models");
        // ScaleDip 在 1..400 DIP / 96..288 DPI 上单调不减且非负。
        for(unsigned dpi=96;dpi<=288;dpi+=24) {
            int previous=-1;
            for(int dip=0;dip<=400;++dip) {
                const int scaled=ScaleDip(dip,dpi);
                Check(scaled>=0&&scaled>=previous,"ScaleDip is monotonic and non-negative");
                previous=scaled;
            }
        }
        // 固定高度：静态范围 420..960，并且服从显示器工作区（留 16 DIP）。
        {
            const Rect work{0,0,1920,1080};
            Check(ClampPanelHeightDip(100,96,work)==kPanelMinHeightDip,"a tiny height clamps to the minimum");
            Check(ClampPanelHeightDip(5000,96,work)==kPanelMaxHeightDip,"a huge height clamps to the maximum");
            Check(ClampPanelHeightDip(700,96,work)==700,"an in-range height is kept");
            const Rect shortWork{0,0,1366,700};
            Check(ClampPanelHeightDip(960,96,shortWork)==684,"the work area caps the height (700-16)");
            Check(PanelHeightMaxDip(192,Rect{0,0,3840,2160})==960,"high-DPI monitors still allow 960 DIP");
            GeometryModel model;
            model.heightDip=700;
            Check(TierSize(model,120).height==ScaleDip(700,120),"TierSize uses the fixed height in DIP");
            model.tier=Tier::Compact;
            Check(TierSize(model,120).height==ScaleDip(152,120),"the compact tier stays 152 DIP");
        }
        // 贴条拖动落点：按挂件中心选最近边；拖到屏幕中央也只会吸附，不会变成浮动态。
        {
            const Monitor monitor{Rect{0,0,1920,1080},Rect{0,0,1920,1080},96};
            const Size panel{384,634};
            Check(StripSnapDecision({960,540},panel,monitor)==StripSnap{Edge::Top,768},
                "a centre drop snaps to the nearest edge (top) and centres the panel");
            Check(StripSnapDecision({1900,500},panel,monitor)==StripSnap{Edge::Right,183},
                "a right-edge drop snaps right with a clamped anchor");
            Check(StripSnapDecision({10,1070},panel,monitor)==StripSnap{Edge::Left,446},
                "an exact corner tie keeps the documented edge order and clamps the anchor");
            const Monitor tiny{Rect{0,0,300,300},Rect{0,0,300,300},96};
            Check(StripSnapDecision({150,150},panel,tiny)==StripSnap{Edge::Left,0},
                "a panel taller than the work area still gets a valid anchor");
        }
        // 悬浮信息卡的位置：放在贴条内侧，边界处夹进工作区。
        {
            const Rect work{0,0,1920,1080};
            Check(HoverCardRect(Rect{355,0,170,38},Edge::Top,work,315,183,10)==Rect{283,48,315,183},
                "the top-edge card sits below the strip and centres on it");
            Check(HoverCardRect(Rect{10,530,10,76},Edge::Left,work,252,146,8)==Rect{28,495,252,146},
                "the left-edge card sits to the right of the strip");
            Check(HoverCardRect(Rect{1890,530,10,76},Edge::Right,work,252,146,8)==Rect{1630,495,252,146},
                "the right-edge card sits to the left of the strip");
            Check(HoverCardRect(Rect{100,1000,170,38},Edge::Top,work,315,183,10).y==1080-183,
                "a card that would leave the work area is clamped");
        }
    }

    std::cout << "geometry: " << passed << " passed, " << failed
        << " failed; 18 rect fixtures + 15 snap fixtures + boundaries\n";
    return failed == 0 ? 0 : 1;
}
