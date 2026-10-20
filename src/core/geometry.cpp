#include "geometry.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace hermes {
namespace {
int Clamp(int value, int low, int high) { return std::clamp(value, low, std::max(low, high)); }
constexpr std::array<Edge,4> kEdges{Edge::Left,Edge::Right,Edge::Top,Edge::Bottom};
std::array<int,4> Gaps(const Rect& rect, const Monitor& monitor) {
    return {rect.x-monitor.work.x, monitor.work.right()-rect.right(),
        rect.y-monitor.work.y, monitor.work.bottom()-rect.bottom()};
}
int Anchor(Edge edge, const Rect& rect, const Rect& work) {
    return edge == Edge::Left || edge == Edge::Right
        ? Clamp(rect.y,work.y,work.bottom()-rect.height)
        : Clamp(rect.x,work.x,work.right()-rect.width);
}
}
int ScaleDip(int dip, unsigned dpi) {
    if (dpi == 0) throw std::invalid_argument("DPI must be positive");
    const auto product = static_cast<std::int64_t>(dip)*dpi;
    const auto scaled = (product + (product >= 0 ? 48 : -48))/96;
    if (scaled < std::numeric_limits<int>::min() || scaled > std::numeric_limits<int>::max()) {
        throw std::overflow_error("DIP conversion overflow");
    }
    return static_cast<int>(scaled);
}
int PanelHeightMaxDip(unsigned dpi, const Rect& work) {
    if (dpi == 0) throw std::invalid_argument("DPI must be positive");
    const int workDip = static_cast<int>(static_cast<std::int64_t>(work.height) * 96 / dpi);
    return std::max(200, std::min(kPanelMaxHeightDip, workDip - 16));
}
int ClampPanelHeightDip(int dip, unsigned dpi, const Rect& work) {
    const int maxDip = PanelHeightMaxDip(dpi, work);
    const int minDip = std::min(kPanelMinHeightDip, maxDip);
    return std::clamp(dip, minDip, maxDip);
}
Size TierSize(Tier tier, unsigned dpi) { return {ScaleDip(384,dpi),ScaleDip(tier==Tier::Full?634:152,dpi)}; }
Size TierSize(const GeometryModel& model, unsigned dpi) {
    // 宽度 384（旧 CANON_W："给模型名多留 24px"）；完整档用用户固定的高度（DIP），
    // 紧凑档固定 152（旧"紧凑视图"）。切页不再改窗口高度。
    const int height=model.tier==Tier::Compact?152:model.heightDip;
    return {ScaleDip(384,dpi),ScaleDip(height,dpi)};
}
Size WidgetSize(WidgetStyle style, Edge edge, unsigned dpi) {
    // DIP 尺寸表：竖边（左右）用 width×height，横边（上下）转置。
    int width=0,height=0;
    switch(style) {
    case WidgetStyle::Beacon: width=10;height=76;break;
    case WidgetStyle::Tag: width=14;height=74;break;
    case WidgetStyle::Flag: width=16;height=58;break;
    case WidgetStyle::Capsule: width=30;height=136;break;   // 旧胶囊原尺寸
    }
    if(edge==Edge::Top||edge==Edge::Bottom) std::swap(width,height);
    return {ScaleDip(width,dpi),ScaleDip(height,dpi)};
}
WidgetMetrics WidgetMetricsFor(WidgetStyle style,unsigned dpi) {
    // 绘制与命中（GDI 区域）共用同一份几何参数：两边各写一套必然漂移。
    WidgetMetrics metrics;
    switch(style) {
    case WidgetStyle::Beacon:
        metrics.gap=ScaleDip(3,dpi);          // 三段之间的缝
        metrics.radius=ScaleDip(2,dpi);       // 段端小圆角：读作"信号段"而不是小胶囊
        metrics.accent=ScaleDip(2,dpi);       // 异常段的呼吸微光外扩
        break;
    case WidgetStyle::Tag:
        metrics.radius=ScaleDip(4,dpi);
        metrics.spine=ScaleDip(3,dpi);        // 贴边侧身份色轨
        metrics.gap=ScaleDip(4,dpi);          // 底部三灯的间距
        metrics.accent=ScaleDip(2,dpi);       // 待重启时顶部琥珀条
        break;
    case WidgetStyle::Flag:
        metrics.radius=ScaleDip(4,dpi);
        metrics.tail=ScaleDip(12,dpi);        // 燕尾长度
        metrics.spine=ScaleDip(3,dpi);        // 最差状态高饱和亮条
        metrics.gap=ScaleDip(3,dpi);
        break;
    case WidgetStyle::Capsule:
        metrics.radius=ScaleDip(15,dpi);      // 半宽：桌面侧全圆角
        metrics.gap=ScaleDip(4,dpi);
        break;
    }
    return metrics;
}
const char* WidgetStyleName(WidgetStyle style) {
    switch(style) {
    case WidgetStyle::Beacon: return "beacon";
    case WidgetStyle::Tag: return "tag";
    case WidgetStyle::Flag: return "flag";
    case WidgetStyle::Capsule: return "capsule";
    }
    return "beacon";
}
WidgetStyle WidgetStyleFromName(const std::string& name) {
    // 旧值继续接受：capsule 仍是胶囊；pulse（声波）已删，落到最窄的信标；
    // halo/badge 落到铭牌，magnet 落到旗标。
    if(name=="capsule") return WidgetStyle::Capsule;
    if(name=="tag"||name=="halo"||name=="badge") return WidgetStyle::Tag;
    if(name=="flag"||name=="magnet") return WidgetStyle::Flag;
    return WidgetStyle::Beacon;   // beacon / pulse / 未知值
}
WindowRects ComputeRect(const GeometryModel& model, const Monitor& monitor) {
    const auto size = TierSize(model,monitor.dpi);
    const auto widget = WidgetSize(model.widget,model.edge,monitor.dpi);
    Rect panel{model.floating.x,model.floating.y,size.width,size.height};
    const auto& work = monitor.work;
    switch(model.edge) {
    case Edge::None: return {panel,std::nullopt};
    case Edge::Left:
    case Edge::Right: {
        panel.x = model.edge==Edge::Left ? work.x : work.right()-size.width;
        panel.y = Clamp(model.anchor,work.y,work.bottom()-size.height);
        // 挂件挂在面板停靠段的中线上（不再占满整条边），并夹进工作区。
        const int x = model.edge==Edge::Left ? work.x : work.right()-widget.width;
        const int y = Clamp(panel.y+panel.height/2-widget.height/2,work.y,work.bottom()-widget.height);
        return {panel,Rect{x,y,widget.width,widget.height}};
    }
    case Edge::Top:
    case Edge::Bottom: {
        panel.x = Clamp(model.anchor,work.x,work.right()-size.width);
        panel.y = model.edge==Edge::Top ? work.y : work.bottom()-size.height;
        const int x = Clamp(panel.x+panel.width/2-widget.width/2,work.x,work.right()-widget.width);
        const int y = model.edge==Edge::Top ? work.y : work.bottom()-widget.height;
        return {panel,Rect{x,y,widget.width,widget.height}};
    }
    }
    throw std::invalid_argument("Unknown edge");
}
SnapResult SnapDecision(const Rect& finalRect, const Rect& startRect, const Monitor& monitor,
    Edge currentEdge) {
    // **只看窗口**：贴不贴边由窗口矩形和屏幕（工作区）边缘的关系决定，与鼠标在哪里无关
    // （真机反馈：「现在根本不是窗口碰到边缘就贴边，而是鼠标」）。
    //
    // 两档意图：
    //   ① 窗口落在边缘带内（含正好贴住）—— gap 在 [0, band]；
    //   ② 这次拖拽把窗口推**越过**了边缘（gap < 0，而拖拽开始时还在工作区内）。
    // 第 ② 档要带「拖拽前是否已经越界」这一条：面板比工作区高、或起点本来就在屏幕外时，
    // 它一开始就"越界"，那不是这次拖拽的意图，不能因此贴边（旧版靠指针位置来区分，
    // 这里改成看起点矩形，仍然是窗口信息）。
    const auto gaps = Gaps(finalRect,monitor);
    const auto startGaps = Gaps(startRect,monitor);
    const int band = ScaleDip(16,monitor.dpi);
    Edge selected = Edge::None;
    int nearest = std::numeric_limits<int>::max();          // 边缘带内：取最小的正 gap
    int nearestBeyond = std::numeric_limits<int>::max();    // 越界：取最小的越界量
    Edge beyondEdge = Edge::None;
    for(std::size_t i=0;i<kEdges.size();++i) {
        const int gap=gaps[i];
        if(gap>=0) {
            if(gap>band) continue;
            if(kEdges[i]==currentEdge)
                return {currentEdge,Anchor(currentEdge,finalRect,monitor.work),{finalRect.x,finalRect.y}};
            if(gap<nearest) { nearest=gap; selected=kEdges[i]; }
            continue;
        }
        // 越界：只有"这条边是这次拖拽推过去的"才算意图。
        if(startGaps[i]<0) continue;
        if(kEdges[i]==currentEdge)
            return {currentEdge,Anchor(currentEdge,finalRect,monitor.work),{finalRect.x,finalRect.y}};
        if(-gap<nearestBeyond) { nearestBeyond=-gap; beyondEdge=kEdges[i]; }
    }
    if(selected==Edge::None) selected=beyondEdge;
    return {selected,selected==Edge::None?0:Anchor(selected,finalRect,monitor.work),{finalRect.x,finalRect.y}};
}
StripSnap StripSnapDecision(Point widgetCentre, Size panelSize, const Monitor& monitor) {
    const auto& work = monitor.work;
    const int left = widgetCentre.x - work.x;
    const int right = work.right() - widgetCentre.x;
    const int top = widgetCentre.y - work.y;
    const int bottom = work.bottom() - widgetCentre.y;
    Edge edge = Edge::Left;
    int nearest = left;
    if (right < nearest) { nearest = right; edge = Edge::Right; }
    if (top < nearest) { nearest = top; edge = Edge::Top; }
    if (bottom < nearest) { nearest = bottom; edge = Edge::Bottom; }
    const auto clamp = [](int value,int low,int high) {
        return std::clamp(value, low, std::max(low, high));
    };
    StripSnap result{edge, 0};
    if (edge == Edge::Left || edge == Edge::Right)
        result.anchor = clamp(widgetCentre.y - panelSize.height / 2, work.y, work.bottom() - panelSize.height);
    else
        result.anchor = clamp(widgetCentre.x - panelSize.width / 2, work.x, work.right() - panelSize.width);
    return result;
}
Rect HoverCardRect(const Rect& strip,Edge edge,const Rect& work,int width,int height,int gap) {
    // 卡片放在屏幕内侧：左贴边→卡片在贴条右边；右贴边→左边；上/下同理。最后夹进工作区。
    int x=strip.x+strip.width+gap;
    int y=strip.y+(strip.height-height)/2;
    switch(edge) {
    case Edge::Right: x=strip.x-gap-width;break;
    case Edge::Top: x=strip.x+(strip.width-width)/2;y=strip.y+strip.height+gap;break;
    case Edge::Bottom: x=strip.x+(strip.width-width)/2;y=strip.y-gap-height;break;
    default: break;
    }
    const auto maxX=std::max(work.x,work.x+work.width-width);
    const auto maxY=std::max(work.y,work.y+work.height-height);
    x=std::clamp(x,work.x,maxX);
    y=std::clamp(y,work.y,maxY);
    return {x,y,width,height};
}
Edge NearestEdge(const Rect& rect, const Monitor& monitor) {
    const auto gaps = Gaps(rect,monitor);
    std::size_t nearest = 0;
    for (std::size_t i=1;i<gaps.size();++i) if (std::abs(gaps[i])<std::abs(gaps[nearest])) nearest=i;
    return kEdges[nearest];
}
bool Contains(const Rect& rect, Point point) {
    return point.x>=rect.x && point.x<rect.right() && point.y>=rect.y && point.y<rect.bottom();
}
bool Intersects(const Rect& left, const Rect& right) {
    return left.x<right.right() && left.right()>right.x && left.y<right.bottom() && left.bottom()>right.y;
}
Point ClampFloating(Point point, Size size, const Rect& work) {
    return {Clamp(point.x,work.x,work.right()-size.width),Clamp(point.y,work.y,work.bottom()-size.height)};
}
}
