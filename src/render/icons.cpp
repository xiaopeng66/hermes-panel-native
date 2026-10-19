#include "icons.h"
#include <wrl/client.h>
#include <cmath>

namespace hermes {
namespace {
constexpr float kPi=3.14159265358979f;
IconFigure Line(std::initializer_list<D2D1_POINT_2F> points) {
    IconFigure figure;
    figure.kind=IconFigure::Kind::Polyline;
    for(const auto& point:points) figure.points.push_back(point);
    return figure;
}
IconFigure Circle(float x,float y,float radius) {
    IconFigure figure;
    figure.kind=IconFigure::Kind::Circle;
    figure.centre=D2D1::Point2F(x,y);
    figure.radius=radius;
    return figure;
}
IconFigure Arc(float x,float y,float radius,float startDegrees,float sweepDegrees,bool clockwise=true) {
    IconFigure figure;
    figure.kind=IconFigure::Kind::Arc;
    figure.centre=D2D1::Point2F(x,y);
    figure.radius=radius;
    figure.startAngle=startDegrees*kPi/180.0f;
    figure.sweep=sweepDegrees*kPi/180.0f;
    figure.clockwise=clockwise;
    return figure;
}
IconFigure Box(float x,float y,float width,float height,float rounding) {
    IconFigure figure;
    figure.kind=IconFigure::Kind::RoundedRect;
    figure.points.push_back(D2D1::Point2F(x,y));
    figure.width=width;
    figure.height=height;
    figure.rounding=rounding;
    return figure;
}
// The table mirrors renderer/icons.js one entry per name: line work as polylines, curves as circles or
// arcs (a star or a keyhole does not need its exact bezier to read correctly at 12-16 px).
std::vector<IconShape> Build() {
    std::vector<IconShape> icons;
    const auto add=[&](IconId id,const char* name,std::initializer_list<IconFigure> figures) {
        IconShape shape;
        shape.id=id;
        shape.name=name;
        for(const auto& figure:figures) shape.figures.push_back(figure);
        icons.push_back(shape);
    };
    add(IconId::Search,"search",{Line({{21,21},{16.66f,16.66f}}),Circle(11,11,8)});
    add(IconId::X,"x",{Line({{6,6},{18,18}}),Line({{18,6},{6,18}})});
    add(IconId::Star,"star",{Line({{12,3},{14.5f,9},{21,9.7f},{16.3f,14.2f},{17.6f,20.5f},{12,17.4f},
        {6.4f,20.5f},{7.7f,14.2f},{3,9.7f},{9.5f,9},{12,3}})});
    // Lucide geometry checked per path: refresh-cw starts its arcs at (3,12)/(21,12) and runs 156
    // degrees clockwise to the arrow neck; git-branch's spine is the quarter arc (15,6) -> (6,15)
    // centred at (15,15) - convex to the upper left - then straight up to (6,3).
    add(IconId::RefreshCw,"refresh-cw",{Arc(12,12,9,180,156,true),Line({{21,3},{21,8},{16,8}}),
        Arc(12,12,9,0,156,true),Line({{8,16},{3,16},{3,21}})});
    add(IconId::ChevronRight,"chevron-right",{Line({{9,6},{15,12},{9,18}})});
    add(IconId::ChevronDown,"chevron-down",{Line({{6,9},{12,15},{18,9}})});
    add(IconId::ChevronUp,"chevron-up",{Line({{6,15},{12,9},{18,15}})});
    add(IconId::Plus,"plus",{Line({{5,12},{19,12}}),Line({{12,5},{12,19}})});
    add(IconId::Minus,"minus",{Line({{5,12},{19,12}})});
    add(IconId::Trash2,"trash-2",{Line({{10,11},{10,17}}),Line({{14,11},{14,17}}),
        Line({{19,6},{19,20},{5,20},{5,6}}),Line({{3,6},{21,6}}),
        Line({{8,6},{8,4},{16,4},{16,6}})});
    // 齿轮：外圈（8 个齿的折线）+ 中心孔。旧定义是"两个同心圆"，16px 下抗锯齿一冲就读不出形状
    // （用户报的"悬浮时设置图标看不见了"包含这一层；另一半是笔刷颜色顺序的 bug，见 DrawIcon）。
    add(IconId::Settings2,"settings",{Circle(12,12,3.2f),
        Line({{12,3.2f},{12,5.6f}}),Line({{12,18.4f},{12,20.8f}}),
        Line({{3.2f,12},{5.6f,12}}),Line({{18.4f,12},{20.8f,12}}),
        Line({{5.8f,5.8f},{7.5f,7.5f}}),Line({{16.5f,16.5f},{18.2f,18.2f}}),
        Line({{18.2f,5.8f},{16.5f,7.5f}}),Line({{7.5f,16.5f},{5.8f,18.2f}}),
        Arc(12,12,8.6f,0,360,true)});
    add(IconId::Pin,"pin",{Line({{12,17},{12,22}}),
        Line({{9,10.76f},{7.89f,12.55f},{6.11f,13.45f},{5,15.24f},{5,16},{6,17},{18,17},{19,16},
            {19,15.24f},{17.89f,13.45f},{16.11f,12.55f},{15,10.76f},{15,7},{16,6},{16,2},{8,2},
            {8,6},{9,7},{9,10.76f}})});
    add(IconId::Key,"key",{Line({{15.5f,7.5f},{17.8f,9.8f},{19.2f,9.8f},{21.3f,7.7f},{21.3f,6.3f},{19,4}}),
        Line({{21,2},{11.4f,11.6f}}),Circle(7.5f,15.5f,5.5f)});
    add(IconId::GitBranch,"git-branch",{Arc(15,15,9,270,90,false),Line({{6,15},{6,3}}),
        Circle(18,6,3),Circle(6,18,3)});
    add(IconId::Layers,"layers",{Line({{12,2.5f},{21.5f,6.5f},{12,10.5f},{2.5f,6.5f},{12,2.5f}}),
        Line({{2.5f,12},{12,16},{21.5f,12}}),Line({{2.5f,17},{12,21},{21.5f,17}})});
    add(IconId::Archive,"archive",{Box(2,3,20,5,1),Line({{4,8},{4,19},{6,21},{18,21},{20,19},{20,8}}),
        Line({{10,12},{14,12}})});
    add(IconId::ArrowUp,"arrow-up",{Line({{5,12},{12,5},{19,12}}),Line({{12,19},{12,5}})});
    add(IconId::ArrowDown,"arrow-down",{Line({{5,12},{12,19},{19,12}}),Line({{12,5},{12,19}})});
    add(IconId::Activity,"activity",{Line({{2,12},{4.5f,12},{6.9f,3.6f},{9.5f,20.4f},{12,12},{14.6f,12},
        {22,12}})});
    add(IconId::Copy,"copy",{Box(8,8,14,14,2),Line({{4,16},{2.5f,14.5f},{2.5f,4},{4,2.5f},{14,2.5f},{15.5f,4}})});
    add(IconId::Check,"check",{Line({{20,6},{9,17},{4,12}})});
    add(IconId::AlertTriangle,"alert-triangle",{Line({{12,3},{21.7f,20},{2.3f,20},{12,3}}),
        Line({{12,9},{12,13}}),Line({{12,16.5f},{12,17}})});
    add(IconId::Power,"power",{Line({{12,2},{12,12}}),Arc(12,13,6.5f,270,300,true)});
    add(IconId::WifiOff,"wifi-off",{Line({{12,20},{12,20.01f}}),Arc(12,19,3.4f,200,140,false),
        Line({{5,12.9f},{7.6f,11.4f}}),Line({{19,12.9f},{17.6f,12}}),Line({{2,8.8f},{5.6f,6.7f}}),
        Line({{22,8.8f},{18.6f,6.9f}}),Line({{2,2},{22,22}})});
    add(IconId::Minimize2,"minimize-2",{Line({{14,10},{21,3}}),Line({{21,3},{15,3},{21,9}}),
        Line({{3,21},{10,14}}),Line({{3,15},{9,21}})});
    // 三个点要"实心感"：旧半径 1.1 在 12px 下只有 0.55px，抗锯齿后是一团灰（用户报的"三个点看不见"）。
    add(IconId::MoreHorizontal,"more-horizontal",{Circle(12,12,1.8f),Circle(19,12,1.8f),Circle(5,12,1.8f)});
    add(IconId::RotateCcw,"rotate-ccw",{Arc(12,12,9,60,300,false),Line({{3,3},{3,8},{8,8}})});
    add(IconId::Eye,"eye",{Line({{2.2f,12},{5.4f,8.6f},{9,7},{12,6.6f},{15,7},{18.6f,8.6f},{21.8f,12},
        {18.6f,15.4f},{15,17},{12,17.4f},{9,17},{5.4f,15.4f},{2.2f,12}}),Circle(12,12,3)});
    add(IconId::EyeOff,"eye-off",{Line({{2,2},{22,22}}),Line({{9.7f,5.4f},{12,5.0f},{15,5.6f}}),
        Circle(12,12,3),Line({{5.4f,7.4f},{2.2f,12},{5.4f,16.6f},{9,18.2f},{15,18.2f},{18.6f,16.6f},{21.8f,12}})});
    add(IconId::CornerDownLeft,"corner-down-left",{Line({{20,4},{20,11},{16,15},{4,15}}),
        Line({{9,10},{4,15},{9,20}})});
    return icons;
}
}
const std::vector<IconShape>& IconTable() {
    static const std::vector<IconShape> table=Build();
    return table;
}
const IconShape* FindIcon(IconId id) {
    for(const auto& shape:IconTable()) if(shape.id==id) return &shape;
    return nullptr;
}
const IconShape* FindIcon(const std::string& name) {
    for(const auto& shape:IconTable()) if(name==shape.name) return &shape;
    return nullptr;
}
int IconCount() { return static_cast<int>(IconTable().size()); }

void DrawIcon(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,ID2D1StrokeStyle* style,
    const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,float strokeWidth,float scale) {
    const auto* shape=FindIcon(id);
    if(!shape||!target||!brush||!style) return;
    const auto box=std::min(rect.right-rect.left,rect.bottom-rect.top);
    if(box<=2.0f) return;
    const auto step=box/24.0f;
    const auto originX=rect.left+(rect.right-rect.left-box)/2.0f;
    const auto originY=rect.top+(rect.bottom-rect.top-box)/2.0f;
    const auto point=[&](const D2D1_POINT_2F& value) {
        return D2D1::Point2F(originX+value.x*step,originY+value.y*step);
    };
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if(FAILED(factory->CreatePathGeometry(path.GetAddressOf()))) return;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(path->Open(sink.GetAddressOf()))) return;
    // 先定笔刷颜色再画：Circle / RoundedRect 这些"直接画"的图形吃的是当前笔刷，
    // 以前 SetColor 放在循环之后，纯圆图标（设置、⋯、单选点）就用了上一次残留的颜色 → 看起来消失。
    brush->SetColor(colour);
    for(const auto& figure:shape->figures) {
        switch(figure.kind) {
        case IconFigure::Kind::Polyline: {
            if(figure.points.size()<2) break;
            sink->BeginFigure(point(figure.points.front()),D2D1_FIGURE_BEGIN_HOLLOW);
            for(std::size_t index=1;index<figure.points.size();++index) sink->AddLine(point(figure.points[index]));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            break;
        }
        case IconFigure::Kind::Circle: {
            const auto centre=point(figure.centre);
            const auto radius=figure.radius*step;
            target->DrawEllipse(D2D1::Ellipse(centre,radius,radius),brush,strokeWidth*scale,style);
            break;
        }
        case IconFigure::Kind::RoundedRect: {
            const auto topLeft=point(figure.points.front());
            const auto size=figure.rounding*step;
            target->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(topLeft.x,topLeft.y,topLeft.x+figure.width*step,topLeft.y+figure.height*step),
                size,size),brush,strokeWidth*scale,style);
            break;
        }
        case IconFigure::Kind::Arc: {
            const auto centre=point(figure.centre);
            const auto radius=figure.radius*step;
            const auto start=D2D1::Point2F(centre.x+radius*std::cos(figure.startAngle),
                centre.y+radius*std::sin(figure.startAngle));
            const auto endAngle=figure.startAngle+(figure.clockwise?figure.sweep:-figure.sweep);
            const auto end=D2D1::Point2F(centre.x+radius*std::cos(endAngle),centre.y+radius*std::sin(endAngle));
            sink->BeginFigure(start,D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(end,D2D1::SizeF(radius,radius),0.0f,
                figure.clockwise?D2D1_SWEEP_DIRECTION_CLOCKWISE:D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                std::abs(figure.sweep)>kPi?D2D1_ARC_SIZE_LARGE:D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            break;
        }
        }
    }
    sink->Close();
    brush->SetColor(colour);   // 路径与"直接画"的图形共用同一个颜色
    target->DrawGeometry(path.Get(),brush,strokeWidth*scale,style);
}

void DrawStarIcon(ID2D1RenderTarget* target,ID2D1SolidColorBrush* brush,ID2D1StrokeStyle* style,
    const D2D1_RECT_F& rect,bool filled,const D2D1_COLOR_F& colour,float strokeWidth,float scale) {
    const auto* shape=FindIcon(IconId::Star);
    if(!shape||!target||!brush||shape->figures.empty()) return;
    const auto box=std::min(rect.right-rect.left,rect.bottom-rect.top);
    if(box<=2.0f) return;
    const auto& points=shape->figures.front().points;
    if(points.size()<3) return;
    const auto step=box/24.0f;
    const auto originX=rect.left+(rect.right-rect.left-box)/2.0f;
    const auto originY=rect.top+(rect.bottom-rect.top-box)/2.0f;
    const auto centre=D2D1::Point2F(originX+12.0f*step,originY+12.0f*step);
    // The filled star shrinks by ~7% about its centre: the stroked silhouette and the fill then read the
    // same size (the polygon is drawn on the stroke's centre line, the fill has no stroke of its own).
    const auto shrink=filled?0.93f:1.0f;
    const auto point=[&](const D2D1_POINT_2F& value) {
        const auto x=originX+value.x*step,y=originY+value.y*step;
        return D2D1::Point2F(centre.x+(x-centre.x)*shrink,centre.y+(y-centre.y)*shrink);
    };
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if(FAILED(factory->CreatePathGeometry(path.GetAddressOf()))) return;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if(FAILED(path->Open(sink.GetAddressOf()))) return;
    sink->BeginFigure(point(points.front()),filled?D2D1_FIGURE_BEGIN_FILLED:D2D1_FIGURE_BEGIN_HOLLOW);
    for(std::size_t index=1;index<points.size();++index) sink->AddLine(point(points[index]));
    sink->EndFigure(filled?D2D1_FIGURE_END_CLOSED:D2D1_FIGURE_END_OPEN);
    sink->Close();
    brush->SetColor(colour);
    if(filled) target->FillGeometry(path.Get(),brush);
    else target->DrawGeometry(path.Get(),brush,strokeWidth*scale,style);
}
}
