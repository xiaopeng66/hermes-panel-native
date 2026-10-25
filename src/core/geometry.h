#pragma once

#include <array>
#include <optional>
#include <string>

namespace hermes {
enum class Edge { None, Left, Right, Top, Bottom };
enum class Tier { Full, Compact };
// 贴边收起时的挂件样式（G11）：只剩两套（用户要求删掉信标/旗标）。
// Tag=铭牌（竖排模型名 + 身份色轨 + 三灯）；Capsule=胶囊（logo + 三灯 + 竖排模型名）。
enum class WidgetStyle { Tag=0, Capsule=1 };
enum class State { Hidden, Floating, Dragging, Expanded, Collapsed };

struct Point {
    int x = 0;
    int y = 0;
    bool operator==(const Point&) const = default;
};
struct Size {
    int width = 0;
    int height = 0;
    bool operator==(const Size&) const = default;
};
struct Rect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int right() const { return x + width; }
    int bottom() const { return y + height; }
    bool operator==(const Rect&) const = default;
};
struct Monitor {
    Rect bounds;
    Rect work;
    unsigned dpi = 96;
};
struct GeometryModel {
    Edge edge = Edge::None;
    Tier tier = Tier::Full;
    int anchor = 0;
    Point floating{200, 120};
    // 固定窗口高度（DIP，用户可拖动底边改变并持久化）。不再按页面内容自适应——底栏因此
    // 始终贴在窗口底边，切页不跳动；正文超高由页内滚动条承担。
    int heightDip = 634;
    WidgetStyle widget = WidgetStyle::Capsule;   // 贴边收起时的挂件样式
};
// 手动拉伸的合理范围（DIP）：420 保住标题栏+寄存器+导航和一屏列表；960 在 1080p/125%
// 的工作区里仍能整窗放下。实际运行时还会再夹到显示器工作区高度。
constexpr int kPanelMinHeightDip = 420;
constexpr int kPanelMaxHeightDip = 960;
constexpr int kPanelDefaultHeightDip = 634;
struct WindowRects {
    Rect panel;
    std::optional<Rect> strip;
    bool operator==(const WindowRects&) const = default;
};
struct SnapResult {
    Edge edge = Edge::None;
    int anchor = 0;
    Point floating;
    bool operator==(const SnapResult&) const = default;
};
// 贴条拖动松手：按挂件中心到工作区四边的距离选最近的边，等价于把展开面板的锚点
// 摆到挂件中心（再夹进工作区）。纯函数，便于单测"拖到屏幕中央也只会吸附"。
struct StripSnap { Edge edge = Edge::None; int anchor = 0; bool operator==(const StripSnap&) const = default; };
StripSnap StripSnapDecision(Point widgetCentre, Size panelSize, const Monitor& monitor);
// 贴条悬浮信息卡的窗口矩形：放在贴条的屏幕内侧（左贴边→右侧），并夹进工作区。
Rect HoverCardRect(const Rect& strip, Edge edge, const Rect& work, int width, int height, int gap);
int ScaleDip(int dip, unsigned dpi);
// 高度上限还要服从当前显示器工作区（留 16 DIP 余量）；工作区极小时 min 也会跟着降，
// 保证 min<=max 不会出现不可满足的约束。
int PanelHeightMaxDip(unsigned dpi, const Rect& work);
int ClampPanelHeightDip(int dip, unsigned dpi, const Rect& work);
Size TierSize(Tier tier, unsigned dpi);
// 面板窗口尺寸：宽 384 DIP（旧 CANON_W），高 = 用户设定的固定高度（紧凑档 152）。
Size TierSize(const GeometryModel& model, unsigned dpi);
// 挂件尺寸（DIP → 像素）：竖边高、横边宽；贴边那一侧不额外留厚度。
Size WidgetSize(WidgetStyle style, Edge edge, unsigned dpi);
// 挂件形状参数（像素）：绘制（D2D 几何掩码）与命中（GDI 区域）共用，避免两套常量漂移。
struct WidgetMetrics {
    int radius=0;   // 圆角
    int spine=0;    // 贴边侧色轨宽度
    int accent=0;   // 强调条/微光厚度
};
WidgetMetrics WidgetMetricsFor(WidgetStyle style,unsigned dpi);
const char* WidgetStyleName(WidgetStyle style);
WidgetStyle WidgetStyleFromName(const std::string& name);
WindowRects ComputeRect(const GeometryModel& model, const Monitor& monitor);
// 贴边判定**只看窗口**（真机反馈：要由窗口碰到边缘触发，而不是鼠标靠近）：窗口落在边缘带内、
// 或这次拖拽把窗口推过了边缘（startRect 还在工作区内）都算意图；起点就已越界（面板比工作区高）不算。
SnapResult SnapDecision(const Rect& finalRect, const Rect& startRect, const Monitor& monitor,
    Edge currentEdge);
Edge NearestEdge(const Rect& rect, const Monitor& monitor);
bool Contains(const Rect& rect, Point point);
bool Intersects(const Rect& left, const Rect& right);
Point ClampFloating(Point point, Size size, const Rect& work);
}
