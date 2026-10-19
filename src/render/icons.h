#pragma once
#include <d2d1.h>
#include <string>
#include <vector>

namespace hermes {
// The 30 icons the panel uses, taken from Lucide 1.17.0 (ISC) exactly as the old renderer inlined them:
// same 24x24 coordinate space, stroke width 2, round caps and joins. The shapes live as data here so the
// renderer and the widget tests read the same definitions (DESIGN §4.9 #35).
enum class IconId {
    None,Search,X,Star,RefreshCw,ChevronRight,ChevronDown,ChevronUp,Plus,Minus,Trash2,Settings2,Pin,Key,
    GitBranch,Layers,Archive,ArrowUp,ArrowDown,Activity,Copy,Check,AlertTriangle,Power,WifiOff,Minimize2,
    MoreHorizontal,RotateCcw,Eye,EyeOff,CornerDownLeft
};
// One figure of an icon. Everything is stroked, nothing is filled, so a figure is a polyline, a circle or
// a rounded rectangle - the three primitives the old SVG data used.
struct IconFigure {
    enum class Kind { Polyline,Circle,RoundedRect,Arc } kind=Kind::Polyline;
    std::vector<D2D1_POINT_2F> points;   // polyline vertices, or the arc's start/end
    D2D1_POINT_2F centre{0,0};           // circle / arc centre
    float radius=0,width=0,height=0,rounding=0,startAngle=0,sweep=0;
    bool clockwise=true;
};
struct IconShape { IconId id=IconId::None;const char* name="";std::vector<IconFigure> figures; };
const std::vector<IconShape>& IconTable();
const IconShape* FindIcon(IconId id);
const IconShape* FindIcon(const std::string& name);
int IconCount();
// Draws one icon into rect, stroked with the panel's round-cap style. 24x24 coordinates are scaled to fit.
void DrawIcon(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,ID2D1StrokeStyle* style,
    const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,float strokeWidth,float scale);
// The star is the one icon the panel also draws filled while it is on (.star.on svg { fill: currentColor }).
// Filled builds the same points as a closed polygon and fills it; otherwise it strokes like DrawIcon.
void DrawStarIcon(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,ID2D1StrokeStyle* style,
    const D2D1_RECT_F& rect,bool filled,const D2D1_COLOR_F& colour,float strokeWidth,float scale);
}
