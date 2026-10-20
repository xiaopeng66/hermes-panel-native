#include "panel.h"
#include "render/layout.h"
#include "render/widgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <stdexcept>

namespace hermes {
namespace {
std::wstring Widen(const std::string& value) {
    if(value.empty()) return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length);
    return result;
}
void Check(HRESULT result,const char* what="panel render") {
    if(FAILED(result)) throw std::runtime_error(std::string(what)+" failed with hresult 0x"
        +[result]{char buffer[16]{};std::snprintf(buffer,sizeof(buffer),"%08lX",static_cast<unsigned long>(result));return std::string(buffer);}());
}
D2D1_COLOR_F Fade(const D2D1_COLOR_F& colour,float alpha) {
    return D2D1::ColorF(colour.r,colour.g,colour.b,alpha);
}
// 两色混合（t=0 → b，t=1 → a）：挂件底色用当前供应商身份色轻轻染一层。
D2D1_COLOR_F Blend(const D2D1_COLOR_F& a,const D2D1_COLOR_F& b,float t) {
    return D2D1::ColorF(a.r*t+b.r*(1.0f-t),a.g*t+b.g*(1.0f-t),a.b*t+b.b*(1.0f-t),1.0f);
}
D2D1_COLOR_F LampColour(LampState state,const ThemePalette& palette) {
    switch(state) {
    case LampState::Ok: return palette.ok;
    case LampState::Warn: return palette.warn;
    case LampState::Err: return palette.err;
    default: return palette.faint;
    }
}
LampState WorstLamp(const std::vector<LampView>& lamps) {
    LampState worst=LampState::Idle;
    for(const auto& lamp:lamps) {
        if(lamp.state==LampState::Err) return LampState::Err;
        if(lamp.state==LampState::Warn) worst=LampState::Warn;
        else if(lamp.state==LampState::Ok&&worst!=LampState::Warn) worst=LampState::Ok;
    }
    return worst;
}
// 徽章中央的 1-2 个字符：ASCII 取字母数字并大写；CJK 取第一个码点。
std::string Initials(const std::string& model) {
    std::string out;
    for(std::size_t index=0;index<model.size()&&out.size()<2;) {
        const auto lead=static_cast<unsigned char>(model[index]);
        if((lead>='0'&&lead<='9')||(lead>='A'&&lead<='Z')||(lead>='a'&&lead<='z')) {
            out.push_back(static_cast<char>(lead>='a'&&lead<='z'?lead-32:lead));
            ++index;
            continue;
        }
        if(lead>=0x80) {
            std::size_t length=lead>=0xF0?4:(lead>=0xE0?3:(lead>=0xC0?2:1));
            if(out.empty()) out=model.substr(index,std::min(length,model.size()-index));
            break;
        }
        ++index;
    }
    return out.empty()?"H":out;
}
// The renderer works in DIP everywhere except the layout boxes, which come pre-scaled: this keeps the
// widget metrics (declared in DIP) usable next to them.
float Dip(float value,float scale) { return value*scale; }
// 模型短名（挂件竖排显示用）：跳过包装/档位词，取第一个有意义的词并大写。
// 例：workbuddy-global-deepseek-v4.1-flash → DEEP；glm-5.3 → GLM5；claude-sonnet-5 → CLAU。
std::string ModelTag(const std::string& model,std::size_t maxChars) {
    // 分词：分隔符（- _ . / : 空格）切开；全是 ASCII。
    std::vector<std::string> tokens;
    std::string current;
    for(const char character:model) {
        if(character=='-'||character=='_'||character=='.'||character=='/'||character==':'||character==' ') {
            if(!current.empty()) { tokens.push_back(current);current.clear(); }
            continue;
        }
        current.push_back(character);
    }
    if(!current.empty()) tokens.push_back(current);
    const auto lower=[](std::string value) {
        for(auto& character:value) if(character>='A'&&character<='Z') character=static_cast<char>(character+32);
        return value;
    };
    // 包装词/档位词：不表达"这是什么模型"。
    const auto wrapper=[&lower](const std::string& token) {
        const auto word=lower(token);
        return word=="workbuddy"||word=="custom"||word=="global"||word=="free"||word=="pro"
            ||word=="plus"||word=="api"||word=="v"||word=="chat";
    };
    const auto alnum=[](const std::string& token) {
        std::string out;
        for(const char character:token) {
            if((character>='0'&&character<='9')||(character>='A'&&character<='Z')
                ||(character>='a'&&character<='z')) out.push_back(character);
        }
        return out;
    };
    std::string chosen;
    for(const auto& token:tokens) {
        if(wrapper(token)) continue;
        const auto clean=alnum(token);
        if(clean.empty()) continue;                     // 纯 CJK 词留给下面的兜底
        if(chosen.empty()) { chosen=clean;continue; }
        // 第一个词太短（如 "v" 被过滤后剩 "4"）：再接下一个词，够 2 个字符就停。
        if(chosen.size()<2) chosen+=clean;
        else break;
    }
    if(!chosen.empty()) {
        for(auto& character:chosen)
            if(character>='a'&&character<='z') character=static_cast<char>(character-32);
        return chosen.substr(0,maxChars);
    }
    // 全 CJK / 没词可用：退回首字母规则（CJK 取首码点）。
    return Initials(model);
}
// 阶梯行的文案基线：.fb-row 的上内边距（8）让序号与文字从行顶下移。
constexpr float kLadderRowPadTrim=8.0f;
// .modal：62% 遮罩、卡片圆角 10（DESIGN §4.8 #33）
constexpr float kModalShadeAlpha=0.62f,kModalRadius=10.0f;
std::string Utf8(const std::wstring& value) {
    if(value.empty()) return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0,
        nullptr,nullptr);
    std::string narrow(static_cast<std::size_t>(std::max(length,0)),'\0');
    if(length>0) {
        WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),narrow.data(),length,
            nullptr,nullptr);
    }
    return narrow;
}
}
void PanelRenderer::EnsureResources(ID2D1RenderTarget* target,IDWriteFactory* text,float scale) {
    if(owner_!=target) { brush_.Reset();owner_=target;styleFactory_.Reset();arcStyle_.Reset(); }
    if(!brush_) {
        Check(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),brush_.GetAddressOf()),"CreateSolidColorBrush");
    }
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.GetAddressOf());
    if(!arcStyle_||styleFactory_.Get()!=factory.Get()) {
        styleFactory_=factory;
        arcStyle_.Reset();
        const auto properties=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,4.0f,D2D1_DASH_STYLE_SOLID,0.0f);
        Check(factory->CreateStrokeStyle(properties,nullptr,0,arcStyle_.GetAddressOf()),"CreateStrokeStyle");
    }
    if(!body_||std::abs(scale_-scale)>0.001f) {
        // Sizes and weights from the old stylesheet: .brand 12.5/500, .hero-p 10.5/500, .hero-m 14 mono,
        // .ttl 12.5/500, .busy 10.5, .chip 10.5/500, .sync 10.5 mono, .lamp 10, .tab 10, captions 11.
        const auto make=[&](const wchar_t* family,float size,DWRITE_FONT_WEIGHT weight,
            Microsoft::WRL::ComPtr<IDWriteTextFormat>& format,bool trim=false) {
            format.Reset();
            Check(text->CreateTextFormat(family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,size*scale,L"zh-CN",format.GetAddressOf()),"CreateTextFormat");
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            if(trim) {
                // SetTrimming 的第二个参数是**替换被裁字符的符号**：传 nullptr 只会把放不下的字符丢掉，
                // 一个省略号都不会画（真机上看到的就是"模型名被硬切、没有…"）。这里用工厂给的省略号符号。
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER,0,0};
                Microsoft::WRL::ComPtr<IDWriteInlineObject> sign;
                if(factoryText_&&SUCCEEDED(factoryText_->CreateEllipsisTrimmingSign(format.Get(),
                    sign.GetAddressOf())))
                    format->SetTrimming(&trimming,sign.Get());
                else
                    format->SetTrimming(&trimming,nullptr);
            }
        };
        // A dedicated lamp format with ellipsis trimming: on a narrow row the label shrinks instead of
        // pushing the chip onto the sync stamp (the old CSS did the same with text-overflow).
        make(L"Microsoft YaHei UI",widgets::kLampFont,DWRITE_FONT_WEIGHT_NORMAL,lamp_,true);
        const auto ui=L"Microsoft YaHei UI";
        const auto monoFamily=L"Cascadia Mono";
        // 旧 CSS 里几乎所有单行文本都带 overflow:hidden; text-overflow:ellipsis：装不下就截断加省略号，
        // 绝不让文字溢出容器（真机上报的"模型名超出边框"就是这个）。
        make(ui,12.5f,DWRITE_FONT_WEIGHT_MEDIUM,brand_,true);
        make(ui,10.5f,DWRITE_FONT_WEIGHT_MEDIUM,heroProvider_,true);
        make(monoFamily,widgets::kPlateFont,DWRITE_FONT_WEIGHT_MEDIUM,heroModel_,true);
        make(ui,12.5f,DWRITE_FONT_WEIGHT_MEDIUM,label_,true);
        make(ui,12.0f,DWRITE_FONT_WEIGHT_NORMAL,body_,true);
        make(ui,11.0f,DWRITE_FONT_WEIGHT_NORMAL,caption_,true);
        make(monoFamily,11.5f,DWRITE_FONT_WEIGHT_NORMAL,mono_,true);
        make(ui,10.0f,DWRITE_FONT_WEIGHT_NORMAL,tiny_);
        make(ui,widgets::kChipFont,DWRITE_FONT_WEIGHT_MEDIUM,chip_,true);
        make(ui,10.5f,DWRITE_FONT_WEIGHT_NORMAL,busy_,true);
        make(monoFamily,10.5f,DWRITE_FONT_WEIGHT_NORMAL,sync_);
        make(monoFamily,widgets::kSegCountFont,DWRITE_FONT_WEIGHT_NORMAL,count_);
        make(monoFamily,8.5f,DWRITE_FONT_WEIGHT_NORMAL,navBadge_);
        // ---- 模型页列表：.gname 12/500、.pill 9.5/500、.empty 11.5 换行居中 ----
        make(ui,widgets::kGroupNameFont,DWRITE_FONT_WEIGHT_MEDIUM,groupName_);
        make(ui,widgets::kPillFont,DWRITE_FONT_WEIGHT_MEDIUM,pillText_);
        make(ui,widgets::kEmptyFont,DWRITE_FONT_WEIGHT_NORMAL,emptyText_);
        emptyText_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        emptyText_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        scale_=scale;
    }
}
void PanelRenderer::PrepareFade(ID2D1RenderTarget* target,const ThemePalette& palette) {
    if(fade_&&fadeOwner_==target) return;
    fadeOwner_=target;
    fade_.Reset();
    // The old .list masked its last 16px to transparent; over our flat page background the same thing is
    // an ink-coloured veil, so rows fade out instead of being cut off by the nav divider.
    const auto ink=palette.ink;
    const D2D1_GRADIENT_STOP stops[2]{{0.0f,D2D1::ColorF(ink.r,ink.g,ink.b,0.0f)},
        {1.0f,D2D1::ColorF(ink.r,ink.g,ink.b,1.0f)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    Check(target->CreateGradientStopCollection(stops,2,collection.GetAddressOf()),"CreateGradientStopCollection");
    Check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),
        D2D1::Point2F(0,1)),collection.Get(),fade_.GetAddressOf()),"CreateLinearGradientBrush");
}
void PanelRenderer::PrepareStrip(ID2D1RenderTarget* target,const ThemePalette& palette) {
    if(strip_&&stripOwner_==target) return;
    stripOwner_=target;
    strip_.Reset();
    const D2D1_GRADIENT_STOP stops[2]{{0.0f,palette.stripTop},
        {1.0f,D2D1::ColorF(palette.stripBottom.r,palette.stripBottom.g,palette.stripBottom.b)}};
    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
    Check(target->CreateGradientStopCollection(stops,2,collection.GetAddressOf()),"CreateGradientStopCollection");
    Check(target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),
        D2D1::Point2F(0,1)),collection.Get(),strip_.GetAddressOf()),"CreateLinearGradientBrush");
}
// 挂件按“canonical”坐标绘制：x 从贴边侧(0)指向桌面(cw)，y 沿贴边方向(ch)。
// 上/下贴边只是把整块旋转 90°，所以三套样式各只有一份画法（旧实现每种样式写两遍）。
void PanelRenderer::Widget(ID2D1RenderTarget* target,D2D1_SIZE_F size,float scale,
    const PanelViewModel& view,const ThemePalette& palette) {
    const float width=size.width,height=size.height;
    if(width<=0.0f||height<=0.0f) return;
    const bool vertical=view.widgetVertical;
    const float cw=vertical?width:height;          // 厚度（10–16 DIP）
    const float ch=vertical?height:width;          // 长轴
    const auto style=WidgetStyleFromName(view.widgetStyle);
    const auto metrics=WidgetMetricsFor(style,static_cast<unsigned>(std::lround(scale*96.0f)));
    const float radius=static_cast<float>(metrics.radius);
    const float gap=static_cast<float>(metrics.gap);
    const float spine=static_cast<float>(metrics.spine);
    const float tail=static_cast<float>(metrics.tail);
    const float accent=static_cast<float>(metrics.accent);
    const float phase=view.widgetPhase;
    const auto centre=D2D1::Point2F(width/2.0f,height/2.0f);
    const auto lampAt=[&](std::size_t index) {
        return index<view.lamps.size()?LampColour(view.lamps[index].state,palette):palette.faint;
    };
    const auto worst=LampColour(WorstLamp(view.lamps),palette);
    const auto dot=[&](float x,float y,float dotRadius,const D2D1_COLOR_F& colour) {
        // 光晕在深色底上要"发光"（同色低透明），在浅色底上同色低透明会变成一圈灰盘 ——
        // 浅色主题把色相压向 text（浅色主题里 text 是深色）并收小环、压低透明度。
        const auto halo=palette.light?Blend(colour,palette.text,0.30f):colour;
        const float haloScale=palette.light?1.9f:2.4f;
        brush_->SetColor(Fade(halo,palette.light?0.22f:0.18f));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),dotRadius*haloScale,
            dotRadius*haloScale),brush_.Get());
        brush_->SetColor(colour);
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x,y),dotRadius,dotRadius),brush_.Get());
    };
    const auto fill=[&](const D2D1_RECT_F& rect,float r,const D2D1_COLOR_F& colour) {
        brush_->SetColor(colour);
        target->FillRoundedRectangle(D2D1::RoundedRect(rect,r,r),brush_.Get());
    };
    // 异常段的呼吸微光：这是挂件唯一的动画，含义 = 这条链路需要你处理。
    const auto glow=[&](const D2D1_RECT_F& rect,float r,const D2D1_COLOR_F& colour,float alpha) {
        brush_->SetColor(Fade(colour,alpha));
        const D2D1_RECT_F expanded{rect.left-accent,rect.top-accent,rect.right+accent,rect.bottom+accent};
        target->FillRoundedRectangle(D2D1::RoundedRect(expanded,r+accent,r+accent),brush_.Get());
    };
    // 形状整体旋转：左=不动，右=180°，上=+90°（+x 朝下），下=−90°（+x 朝上）。
    // 变换必须是「平移到画布中心 → 旋转 → 退回 canonical 原点」：直接绕窗口中心转，
    // 在宽高互换的横向挂件上会把整块形状转出窗口（文字走另一条路径，所以只剩字）。
    float angle=0.0f;
    switch(view.widgetEdge) {
    case 2: angle=180.0f;break;
    case 3: angle=90.0f;break;
    case 4: angle=-90.0f;break;
    default: break;
    }
    // D2D 矩阵是行向量约定：A*B 表示"先 A 后 B"。所以顺序必须是
    // 平移到原点 → 旋转 → 平移到窗口中心（写反会把整块形状转出窗口，只剩文字）。
    const auto stripTransform=angle!=0.0f
        ?D2D1::Matrix3x2F::Translation(-cw/2.0f,-ch/2.0f)
            *D2D1::Matrix3x2F::Rotation(angle)
            *D2D1::Matrix3x2F::Translation(centre.x,centre.y)
        :D2D1::Matrix3x2F::Identity();
    if(angle!=0.0f) target->SetTransform(stripTransform);
    // 像素对齐：形状边界落在半像素上会被抗锯齿糊成灰边（真机截图可见）。
    std::string tag;
    if(style==WidgetStyle::Beacon) {
        // 三段灯柱：每段 = 一条链路（WSL / 网关 / WebUI）；段间留缝，直接透出桌面。
        const float block=(ch-gap*2.0f)/3.0f;
        for(int index=0;index<3;++index) {
            const auto top=static_cast<float>(index)*(block+gap);
            const D2D1_RECT_F rect{0.0f,top,cw,top+block};
            const auto colour=lampAt(static_cast<std::size_t>(index));
            const auto state=index<static_cast<int>(view.lamps.size())
                ?view.lamps[static_cast<std::size_t>(index)].state:LampState::Idle;
            if(state==LampState::Err||state==LampState::Warn)
                glow(rect,radius,colour,0.12f+0.10f*(0.5f+0.5f*std::sin(phase)));
            fill(rect,radius,colour);
            // 桌面侧 1px 高光：薄条也有体积感。
            brush_->SetColor(Fade(palette.text,0.16f));
            target->FillRectangle(D2D1::RectF(cw-1.0f,top+radius,cw,top+block-radius),brush_.Get());
        }
    } else if(style==WidgetStyle::Tag) {
        // 铭牌：实心牌 + 贴边侧身份色轨 + 竖排模型名 + 底部三灯；顶部琥珀条 = 待重启。
        fill({0.0f,0.0f,cw,ch},radius,palette.surf);
        brush_->SetColor(Fade(palette.line2,0.9f));
        target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f,0.5f,cw-0.5f,ch-0.5f),
            radius,radius),brush_.Get(),1.0f);
        if(spine>0.0f)
            fill({0.0f,0.0f,spine,ch},std::min(radius,spine*0.5f),widgets::IdentityColour(view.heroHue));
        if(view.pendingGatewayRestart||!view.statuses.empty())
            fill({0.0f,0.0f,cw,accent},accent*0.5f,palette.warn);
        for(int index=0;index<3;++index)
            dot(cw/2.0f,ch-Dip(7.0f,scale)-static_cast<float>(index)*Dip(6.0f,scale),
                Dip(2.0f,scale),lampAt(static_cast<std::size_t>(index)));
        tag=ModelTag(view.currentModel,4);
    } else if(style==WidgetStyle::Capsule) {
        // 胶囊（真机主力样式）：贴边时贴边侧两角收小、桌面侧全椭圆；拖离贴边（浮起）
        // 过渡到**完整椭圆**（四角全半径）—— docked 0..1 插值驱动（贴边↔中间切换动画）。
        // 液态玻璃质感：纵向亮度渐变底 + 顶部高光带 + 内描边 + 贴边侧微投影（与桌面分层）。
        const float docked=std::clamp(view.motion.capsuleDocked,0.0f,1.0f);
        const float full=cw*0.5f;                             // 全椭圆半径 = 半个厚度
        // 贴边侧两角：docked=1 → 近乎直角（留 2 DIP 防尖刺），docked=0 → **也是全半径**，
        // 四角同半径才是真正的椭圆。以前浮起时贴边侧仍只收 3 DIP，看起来还是一头平一头圆。
        const float edgeR=docked<=0.001f?full
            :std::max(full*(1.0f-docked),Dip(2.0f,scale));
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if(factory) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(factory->CreatePathGeometry(path.GetAddressOf()))
                &&SUCCEEDED(path->Open(sink.GetAddressOf()))) {
                // canonical：x=厚向（贴边侧 x=0）、y=长向。贴边侧=x=0 的两个角。
                sink->BeginFigure(D2D1::Point2F(edgeR,0.0f),D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cw-full,0.0f));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cw,full),D2D1::SizeF(full,full),0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(cw,ch-full));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cw-full,ch),D2D1::SizeF(full,full),0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(edgeR,ch));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(0.0f,ch-edgeR),D2D1::SizeF(edgeR,edgeR),0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                sink->AddLine(D2D1::Point2F(0.0f,edgeR));
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(edgeR,0.0f),D2D1::SizeF(edgeR,edgeR),0.0f,
                    D2D1_SWEEP_DIRECTION_CLOCKWISE,D2D1_ARC_SIZE_SMALL));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                // ---- 液态玻璃（真背景 + 分层高光）----
                // 拖动中不抓快照（窗口每帧在动，抓取要隐藏窗口→会闪）：这时用**接近不透明**
                // 的主题底色撑住实体感，落手后重新抓取、玻璃再把桌面透出来。
                const bool hasBackdrop=view.backdrop.pixels&&view.backdrop.width>0
                    &&view.backdrop.height>0;
                // ① 桌面快照（Surface 已抓取并模糊）**放大 6% 画进胶囊**：磨砂玻璃的
                //    折射感来自"背后内容被透镜放大"，1:1 只是透明、放大才有玻璃味。
                if(hasBackdrop) {
                    if(!backdrop_||backdropGeneration_!=view.backdrop.generation) {
                        backdrop_.Reset();
                        const auto properties=D2D1::BitmapProperties(
                            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE));
                        if(SUCCEEDED(target->CreateBitmap(D2D1::SizeU(
                            static_cast<UINT32>(view.backdrop.width),
                            static_cast<UINT32>(view.backdrop.height)),view.backdrop.pixels,
                            static_cast<UINT32>(view.backdrop.width)*4u,properties,
                            backdrop_.GetAddressOf()))) {
                            backdropGeneration_=view.backdrop.generation;
                        }
                    }
                    Microsoft::WRL::ComPtr<ID2D1TransformedGeometry> screenPath;
                    if(backdrop_&&SUCCEEDED(factory->CreateTransformedGeometry(path.Get(),
                        stripTransform,screenPath.GetAddressOf()))) {
                        target->SetTransform(D2D1::Matrix3x2F::Identity());
                        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),
                            screenPath.Get(),D2D1_ANTIALIAS_MODE_PER_PRIMITIVE),nullptr);
                        // 以窗口中心为锚放大：快照 1.06× 绘制，边缘多出来的 3% 被胶囊裁掉。
                        const float zoom=1.06f;
                        const auto zoomed=D2D1::RectF(
                            width/2.0f*(1.0f-zoom),height/2.0f*(1.0f-zoom),
                            width/2.0f*(1.0f+zoom),height/2.0f*(1.0f+zoom));
                        target->DrawBitmap(backdrop_.Get(),zoomed,1.0f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                        target->PopLayer();
                        target->SetTransform(stripTransform);
                    }
                }
                // ② 玻璃底色：深色 ink 打底保对比度，再叠半透明主题渐变（受光面厚、背面薄）。
                // 浅色主题的"压暗"要轻得多：0.34 的深底会把 porcelain 压成灰片。
                // 没有快照（拖动中）时整层抬到接近不透明，避免变成一层能看见桌面的薄膜。
                const float inkAlpha=(palette.light?0.22f:0.34f)+(hasBackdrop?0.0f:0.30f);
                brush_->SetColor(Fade(palette.ink,inkAlpha));
                target->FillGeometry(path.Get(),brush_.Get());
                D2D1_GRADIENT_STOP glass[3];
                glass[0].position=0.0f; glass[0].color=Fade(palette.stripTop,hasBackdrop?0.55f:0.99f);
                glass[1].position=0.5f; glass[1].color=Fade(palette.stripTop,hasBackdrop?0.38f:0.96f);
                glass[2].position=1.0f; glass[2].color=Fade(palette.stripBottom,hasBackdrop?0.52f:0.99f);
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> glassStops;
                if(SUCCEEDED(target->CreateGradientStopCollection(glass,3,D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP,&glassStops))) {
                    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> glassBrush;
                    if(SUCCEEDED(target->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),D2D1::Point2F(cw,0)),
                        glassStops.Get(),&glassBrush))) {
                        target->FillGeometry(path.Get(),glassBrush.Get());
                    }
                }
                // ③ 贴边侧内阴影：贴边时给玻璃一道"厚度"，浮起时按 docked 淡出。
                if(docked>0.01f) {
                    D2D1_GRADIENT_STOP shade[2];
                    shade[0].position=0.0f;
                    shade[0].color=D2D1::ColorF(0.0f,0.0f,0.0f,0.44f*docked);
                    shade[1].position=1.0f; shade[1].color=D2D1::ColorF(0.0f,0.0f,0.0f,0.0f);
                    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> shadeStops;
                    if(SUCCEEDED(target->CreateGradientStopCollection(shade,2,D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP,&shadeStops))) {
                        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> shadeBrush;
                        if(SUCCEEDED(target->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),
                                D2D1::Point2F(cw*0.62f,0)),shadeStops.Get(),&shadeBrush))) {
                            target->FillGeometry(path.Get(),shadeBrush.Get());
                        }
                    }
                }
                // ④ 边缘折射线：**先深后亮**的双层描边 —— 外侧一圈深色把玻璃从任何
                //    背景下"切"出来（浅色主题压在浅色桌面上时，单靠亮边完全看不出轮廓），
                //    内侧再一道 1.1px 的亮折射线（桌面侧最亮、贴边侧衰减）。
                brush_->SetColor(D2D1::ColorF(0.0f,0.0f,0.0f,palette.light?0.34f:0.30f));
                target->DrawGeometry(path.Get(),brush_.Get(),2.2f);
                D2D1_GRADIENT_STOP rim[3];
                rim[0].position=0.0f; rim[0].color=Fade(palette.text,0.05f);
                rim[1].position=0.55f; rim[1].color=Fade(palette.text,0.15f);
                rim[2].position=1.0f; rim[2].color=Fade(palette.text,0.42f);
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> rimStops;
                if(SUCCEEDED(target->CreateGradientStopCollection(rim,3,D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP,&rimStops))) {
                    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> rimBrush;
                    if(SUCCEEDED(target->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),
                            D2D1::Point2F(cw,0)),rimStops.Get(),&rimBrush))) {
                        target->DrawGeometry(path.Get(),rimBrush.Get(),1.1f);
                    }
                }
                // ⑤ 镜面高光：厚度方向的一条窄亮带（玻璃的"液体"弧面），竖向再补一条
                //    长向顶部的柔光（受光面），两层叠出"液体"的体积。
                D2D1_GRADIENT_STOP specular[3];
                specular[0].position=0.0f; specular[0].color=Fade(palette.text,0.0f);
                specular[1].position=0.5f; specular[1].color=Fade(palette.text,0.18f);
                specular[2].position=1.0f; specular[2].color=Fade(palette.text,0.0f);
                Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> specularStops;
                if(SUCCEEDED(target->CreateGradientStopCollection(specular,3,D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP,&specularStops))) {
                    Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> specularBrush;
                    if(SUCCEEDED(target->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(D2D1::Point2F(cw*0.58f,0),
                            D2D1::Point2F(cw*0.94f,0)),specularStops.Get(),&specularBrush))) {
                        target->FillGeometry(path.Get(),specularBrush.Get());
                    }
                }
                // ⑥ 长向端头柔光：玻璃管两端的圆头会各自接住一点光（贴边时整体减淡，
                //    免得和内阴影打架）。只有一个端头亮会显得"半截塑料"，对称才像液体。
                {
                    const float endAlpha=0.13f*(1.0f-docked*0.55f);
                    D2D1_GRADIENT_STOP sweep[5];
                    sweep[0].position=0.0f; sweep[0].color=Fade(palette.text,endAlpha);
                    sweep[1].position=0.12f; sweep[1].color=Fade(palette.text,0.0f);
                    sweep[2].position=0.5f; sweep[2].color=Fade(palette.text,0.0f);
                    sweep[3].position=0.88f; sweep[3].color=Fade(palette.text,0.0f);
                    sweep[4].position=1.0f; sweep[4].color=Fade(palette.text,endAlpha);
                    Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> sweepStops;
                    if(SUCCEEDED(target->CreateGradientStopCollection(sweep,5,D2D1_GAMMA_2_2,
                        D2D1_EXTEND_MODE_CLAMP,&sweepStops))) {
                        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> sweepBrush;
                        if(SUCCEEDED(target->CreateLinearGradientBrush(
                            D2D1::LinearGradientBrushProperties(D2D1::Point2F(0,0),
                                D2D1::Point2F(0,ch)),sweepStops.Get(),&sweepBrush))) {
                            target->FillGeometry(path.Get(),sweepBrush.Get());
                        }
                    }
                }
            }
        }
        const float logo=Dip(14.0f,scale);
        const float cx=cw/2.0f;
        Icon(target,D2D1::RectF(cx-logo/2.0f,Dip(9.0f,scale),cx+logo/2.0f,Dip(9.0f,scale)+logo),
            IconId::Activity,palette.accentStrong,1.8f);
        for(int index=0;index<3;++index)
            dot(cx,ch*0.36f+ch*0.115f*static_cast<float>(index),Dip(3.4f,scale),
                lampAt(static_cast<std::size_t>(index)));
        tag=ModelTag(view.currentModel,4);
    } else {
        const auto field=Blend(worst,palette.surf,0.55f);
        fill({0.0f,0.0f,cw,ch-tail},radius,field);
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        if(factory) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if(SUCCEEDED(factory->CreatePathGeometry(path.GetAddressOf()))
                &&SUCCEEDED(path->Open(sink.GetAddressOf()))) {
                sink->BeginFigure(D2D1::Point2F(0.0f,ch-tail),D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cw,ch-tail));
                sink->AddLine(D2D1::Point2F(cw/2.0f,ch));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                brush_->SetColor(field);
                target->FillGeometry(path.Get(),brush_.Get());
            }
        }
        if(spine>0.0f) fill({0.0f,0.0f,spine,ch-tail},std::min(radius,spine*0.5f),worst);
        // 不画三灯：16 DIP 厚度里会和文字挤在一起。最差状态由色场 + 亮条表达，
        // 具体是哪条链路坏了下拉/展开后寄存器行和悬浮卡里都有。
        tag=Initials(view.currentModel);
    }
    target->SetTransform(D2D1::Matrix3x2F::Identity());
    if(!tag.empty()) {
        // 文字与贴条是**同一个刚体**：竖边把整串字放进一个横排盒，整体旋转 90° 后再套上
        // 贴条自身的旋转矩阵 —— 左边 = 横向 DEEP 左转 90°（从下往上读、字顶朝屏幕边），
        // 右边是它的 180° 镜像（从上往下读、字顶朝桌面）。横边仍横排（不把字倒过来）。
        // 截断按段长量：Measure 是横排宽度，旋转后长度不变。
        const float along=ch;
        // Tag 样式的三盏灯钉在 canonical 长向底部（ch-Dip7 起向上排）：文字段给灯让位。
        const float x0=style==WidgetStyle::Capsule?along*0.62f:Dip(3.0f,scale);
        const float x1=style==WidgetStyle::Capsule?along-Dip(4.0f,scale)
            :(style==WidgetStyle::Tag?along-Dip(22.0f,scale):along-Dip(3.0f,scale));
        std::string shown=tag;
        const float available=x1-x0;
        if(Measure(count_.Get(),shown).width>available) {
            const std::string ellipsis="\xE2\x80\xA6";
            std::size_t low=0,high=shown.size();
            const auto boundaryAt=[&](std::size_t index) {
                std::size_t end=std::min(index,shown.size());
                while(end>0&&end<shown.size()&&(static_cast<unsigned char>(shown[end])&0xC0)==0x80) --end;
                return end;
            };
            const auto fits=[&](std::size_t index) {
                return Measure(count_.Get(),shown.substr(0,boundaryAt(index))+ellipsis).width<=available;
            };
            while(low<high) {
                const std::size_t mid=low+(high-low+1)/2;
                if(fits(mid)) low=mid; else high=mid-1;
            }
            shown=shown.substr(0,boundaryAt(low))+ellipsis;
        }
        if(!shown.empty()&&vertical) {
            // 局部排版盒：宽=段长（阅读方向），高=厚度；旋转 −90°（视觉左转）后
            // (u,v)→(v,−u)，再平移 (0,x1) 落进 canonical 段 [x0,x1]。
            const float length=x1-x0;
            const auto local=D2D1::Matrix3x2F::Rotation(-90.0f)
                *D2D1::Matrix3x2F::Translation(0.0f,x1);
            target->SetTransform(local*stripTransform);
            brush_->SetColor(palette.text);
            count_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            count_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const auto converted=Widen(shown);
            if(!converted.empty()) {
                target->DrawTextW(converted.c_str(),static_cast<UINT32>(converted.size()),
                    count_.Get(),D2D1::RectF(0.0f,0.0f,length,cw),brush_.Get());
            }
            target->SetTransform(D2D1::Matrix3x2F::Identity());
        } else if(!shown.empty()) {
            // 横边：canonical 段矩形四角 → 屏幕 bbox，直接横排（上边段在左端、下边段在右端）。
            const auto tagCentre=D2D1::Point2F(width/2.0f,height/2.0f);
            const auto mapPoint=[&](float x,float y) {
                if(angle==0.0f) return D2D1::Point2F(x,y);
                const auto t=D2D1::Matrix3x2F::Translation(-cw/2.0f,-ch/2.0f)
                    *D2D1::Matrix3x2F::Rotation(angle)
                    *D2D1::Matrix3x2F::Translation(tagCentre.x,tagCentre.y);
                // 行向量约定：(x,y)·M；Matrix3x2F 的数据是 (m11,m12,m21,m22,dx,dy)。
                return D2D1::Point2F(x*t.m11+y*t.m21+t.dx,x*t.m12+y*t.m22+t.dy);
            };
            const D2D1_POINT_2F corners[4]={
                mapPoint(0.0f,x0),mapPoint(static_cast<float>(cw),x0),
                mapPoint(static_cast<float>(cw),x1),mapPoint(0.0f,x1)};
            float minX=corners[0].x,maxX=corners[0].x,minY=corners[0].y,maxY=corners[0].y;
            for(const auto& corner:corners) {
                minX=std::min(minX,corner.x);maxX=std::max(maxX,corner.x);
                minY=std::min(minY,corner.y);maxY=std::max(maxY,corner.y);
            }
            Text(target,shown,D2D1::RectF(minX,minY,maxX,maxY),count_.Get(),palette.text,
                DWRITE_TEXT_ALIGNMENT_CENTER,true);
        }
    }
}
std::string PanelRenderer::Ellipsize(IDWriteTextFormat* format,const std::string& value,float width) {    // 旧 CSS 的 text-overflow: ellipsis 等价物。DWrite 自己的 trimming 在真机上没画出省略号
    // （放不下的字符直接消失），所以这里按测量值自己截：二分找最长能放下的前缀 + 省略号。
    const std::string ellipsis="\xE2\x80\xA6";
    if(!format||Measure(format,ellipsis).width>width) return {};
    const auto boundary=[&](std::size_t count) {
        std::size_t end=std::min(count,value.size());
        while(end>0&&end<value.size()&&(static_cast<unsigned char>(value[end])&0xC0)==0x80) --end;
        return end;
    };
    const auto fits=[&](std::size_t count) {
        return Measure(format,value.substr(0,boundary(count))+ellipsis).width<=width;
    };
    if(!fits(0)) return {};
    std::size_t low=0,high=value.size();
    while(low<high) {
        const std::size_t mid=low+(high-low+1)/2;
        if(fits(mid)) low=mid; else high=mid-1;
    }
    return value.substr(0,boundary(low))+ellipsis;
}
void PanelRenderer::Text(ID2D1RenderTarget* target,const std::string& value,D2D1_RECT_F rect,
    IDWriteTextFormat* format,const D2D1_COLOR_F& colour,DWRITE_TEXT_ALIGNMENT alignment,bool verticalCentre) {
    if(value.empty()||!format) return;
    // 装不下就截断加省略号，绝不让文字冲出盒子（旧 CSS 里几乎所有单行文本都带 text-overflow）。
    std::string shown=value;
    const float available=rect.right-rect.left;
    if(available>1.0f&&Measure(format,shown).width>available) {
        shown=Ellipsize(format,shown,available);
        if(shown.empty()) return;
    }
    const auto converted=Widen(shown);
    if(converted.empty()) return;
    brush_->SetColor(colour);
    format->SetTextAlignment(alignment);
    format->SetParagraphAlignment(verticalCentre?DWRITE_PARAGRAPH_ALIGNMENT_CENTER:DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    target->DrawTextW(converted.c_str(),static_cast<UINT32>(converted.size()),format,rect,brush_.Get());
}
D2D1_SIZE_F PanelRenderer::Measure(IDWriteTextFormat* format,const std::string& value) {
    const auto converted=Widen(value);
    if(converted.empty()||!format||!factoryText_) return D2D1::SizeF(0,0);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if(FAILED(factoryText_->CreateTextLayout(converted.c_str(),static_cast<UINT32>(converted.size()),format,
        4096.0f,4096.0f,layout.GetAddressOf()))) return D2D1::SizeF(0,0);
    DWRITE_TEXT_METRICS metrics{};
    if(FAILED(layout->GetMetrics(&metrics))) return D2D1::SizeF(0,0);
    return D2D1::SizeF(metrics.width,metrics.height);
}
void PanelRenderer::TextWrapped(ID2D1RenderTarget* target,const std::string& value,D2D1_RECT_F rect,
    IDWriteTextFormat* format,const D2D1_COLOR_F& colour,DWRITE_TEXT_ALIGNMENT alignment) {
    // .empty is the only wrapping text in the panel: centred, two lines at most, fading into the page.
    if(value.empty()||!format) return;
    const auto converted=Widen(value);
    if(converted.empty()) return;
    // 单行格式都是 NO_WRAP（各自带省略号截断），所以这里按盒子尺寸现建一个布局并把换行改成
    // CHARACTER（旧 .meta / .empty 的 word-break: break-all）：路径这类无空格长串否则会直接
    // 冲出盒子右侧被裁掉——真机上报的"文字换行"问题。
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    if(!factoryText_||FAILED(factoryText_->CreateTextLayout(converted.c_str(),
        static_cast<UINT32>(converted.size()),format,std::max(1.0f,rect.right-rect.left),
        std::max(1.0f,rect.bottom-rect.top),layout.GetAddressOf()))) return;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_CHARACTER);
    layout->SetTextAlignment(alignment);
    layout->SetParagraphAlignment(alignment==DWRITE_TEXT_ALIGNMENT_LEADING?DWRITE_PARAGRAPH_ALIGNMENT_NEAR
        :DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush_->SetColor(colour);
    target->DrawTextLayout(D2D1::Point2F(rect.left,rect.top),layout.Get(),brush_.Get());
}
void PanelRenderer::Icon(ID2D1RenderTarget* target,const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,
    float thickness) {
    // One implementation for every icon: the shapes come from render/icons.cpp as data, so the widget
    // tests can assert the same definitions the renderer draws (DESIGN §4.9 #35).
    DrawIcon(target,brush_.Get(),arcStyle_.Get(),rect,id,colour,thickness,scale_);
}
void PanelRenderer::Lamp(ID2D1RenderTarget* target,const LampLayout& lamp,const LampView& view,
    const ThemePalette& palette) {
    // styles.css .lamp: a 6x6 rounded-square dot with a 2px soft ring; the label stays dim.
    widgets::DrawLampDot(target,brush_.Get(),lamp.dot,view.state,palette,scale_);
    // 旧 .lamp 只有「圆点 + 名称」：端口/在线与否写在自己的 title（气泡）里，这里没有气泡，
    // 所以不画 detail —— 少一个文本就不再"乱七八糟"。
    Text(target,view.label,lamp.label,lamp_.Get(),widgets::LampSkinOf(view.state,palette).label,
        DWRITE_TEXT_ALIGNMENT_LEADING,true);
}
void PanelRenderer::Chip(ID2D1RenderTarget* target,D2D1_RECT_F box,const ChipView& chip,bool hovered,
    const ThemePalette& palette) {
    if(!chip.present||box.right<=box.left) return;
    const auto kind=chip.kind==ChipKind::Warn?ChipKind::Warn:ChipKind::Err;
    const auto skin=widgets::ChipSkinOf(kind,hovered?widgets::Visual::Hovered:widgets::Visual::Normal,palette);
    widgets::DrawChipShape(target,brush_.Get(),box,skin,scale_);
    const auto iconBox=widgets::kChipIcon*scale_;
    Icon(target,D2D1::RectF(box.left+widgets::kChipPadX*scale_,
        box.top+(box.bottom-box.top-iconBox)/2.0f,
        box.left+widgets::kChipPadX*scale_+iconBox,
        box.top+(box.bottom-box.top+iconBox)/2.0f),
        chip.icon=="refresh-cw"?IconId::RefreshCw:IconId::WifiOff,skin.text,1.6f);
    Text(target,chip.text,D2D1::RectF(box.left+widgets::kChipPadX*scale_+iconBox
        +widgets::kChipGap*scale_,box.top,box.right-widgets::kChipPadX*scale_,box.bottom),
        chip_.Get(),skin.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
}
void PanelRenderer::Hero(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    // The current model is a full-width lit strip (old .hero): the provider identity tint over the hero
    // gradient, a divider at the bottom, and a glowing identity lamp (INV-H6: this is where colour
    // speaks). The lamp is always lit in the identity hue - it is not a status lamp.
    widgets::DrawHeroBackdrop(target,layout.hero,view.heroHue,palette,scale_);
    brush_->SetColor(palette.line);
    target->FillRectangle(layout.heroDivider,brush_.Get());
    const auto heroColour=widgets::IdentityColour(view.heroHue);
    const auto centre=D2D1::Point2F((layout.heroLamp.left+layout.heroLamp.right)/2.0f,
        (layout.heroLamp.top+layout.heroLamp.bottom)/2.0f);
    const auto radiusDot=(layout.heroLamp.right-layout.heroLamp.left)/2.0f;
    // 旧 .hlamp 的 box-shadow：`0 0 0 3px hsl(.../.17)` 是 3px 实心环（描边圆），
    // `0 0 13px hsl(.../.38)` 是 13px 模糊（径向渐变）。之前把它们画成两个实心圆盘——
    // 那正是真机上看到的"紫色大圆饼"。
    widgets::DrawSoftGlow(target,centre,radiusDot/scale_,widgets::kHeroLampGlow,heroColour,0.38f,scale_);
    brush_->SetColor(Fade(heroColour,0.17f));
    const auto ringRadius=radiusDot+widgets::kHeroLampRing*scale_*0.5f;
    target->DrawEllipse(D2D1::Ellipse(centre,ringRadius,ringRadius),brush_.Get(),
        widgets::kHeroLampRing*scale_);
    brush_->SetColor(heroColour);
    target->FillEllipse(D2D1::Ellipse(centre,radiusDot,radiusDot),brush_.Get());
    Text(target,view.currentProvider,layout.heroProvider,heroProvider_.Get(),palette.dim,
        DWRITE_TEXT_ALIGNMENT_LEADING,true);
    // The model id lives on a small gradient plate (.plate) that lifts from the strip; while a switch is
    // in flight it plays "→ new" in the accent colour and shrinks to 13px.
    // .plate 是 inline-block：宽度随模型名收缩（max-width 100%），文字过长则在盘内省略号截断。
    const float platePadX=widgets::kPlatePadX*scale_;
    const auto modelTextWidth=Measure(heroModel_.Get(),view.currentModel).width;
    const float plateWidth=std::min(layout.heroModel.right-layout.heroModel.left,
        modelTextWidth+platePadX*2.0f);
    const auto plate=D2D1::RectF(layout.heroModel.left,layout.heroModel.top,
        layout.heroModel.left+std::max(plateWidth,platePadX*2.0f+8.0f),layout.heroModel.bottom);
    widgets::DrawPlate(target,brush_.Get(),plate,view.currentSwap,palette,scale_);
    const float plateTextLeft=platePadX;
    auto plateText=D2D1::RectF(layout.heroModel.left+plateTextLeft,layout.heroModel.top,
        layout.heroModel.right-plateTextLeft,layout.heroModel.bottom);
    if(view.currentSwap) {
        // .plate .arrow: accent-strong, 2px left margin, 6px right margin.
        const auto arrowWidth=Measure(heroModel_.Get(),"\xE2\x86\x92").width;
        Text(target,"\xE2\x86\x92",plateText,heroModel_.Get(),palette.accentStrong,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        const float arrowRight=plateText.left+arrowWidth+6.0f*scale_;
        if(arrowRight<plateText.right) plateText.left=arrowRight;
    }
    plateText.right=plate.right-platePadX;
    Text(target,view.currentModel,plateText,heroModel_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    if(!view.currentMode.empty()) {
        const auto box=layout.heroModeTag;
        brush_->SetColor(palette.raise2);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,4.0f*scale_,4.0f*scale_),brush_.Get());
        Text(target,view.currentMode,box,count_.Get(),palette.dim,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    }
    if(view.hasLatency) {
        // F4 hero 徽章：最近一次 + 近 5 次可用率（没有历史时退回 currentBadge）。
        const auto text="~"+std::to_string(view.latencyMs)+"ms"
            +(view.latencyAvailability>=0
                ?" \xC2\xB7 "+std::to_string(view.latencyAvailability)+"%":std::string{});
        const auto colour=!view.latencyOk?palette.err
            :(view.latencyMs<1000?palette.ok:(view.latencyMs<3000?palette.warn:palette.err));
        Text(target,text,layout.heroBadge,tiny_.Get(),colour,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
    } else if(!view.currentBadge.empty()) {
        Text(target,view.currentBadge,layout.heroBadge,tiny_.Get(),palette.ok,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
    }
}
void PanelRenderer::Offline(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    // The old .offline is a bare centred column on the page background: an errored wifi glyph on a
    // soft plate (34, r10), the heading, the explanation, and the primary start button.
    brush_->SetColor(palette.errSoft);
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(layout.offlineIcon.left,layout.offlineIcon.top,
        layout.offlineIcon.right,layout.offlineIcon.bottom),10.0f*scale_,10.0f*scale_),brush_.Get());
    const auto iconBox=17.0f*scale_;
    Icon(target,D2D1::RectF((layout.offlineIcon.left+layout.offlineIcon.right)/2.0f-iconBox/2.0f,
        (layout.offlineIcon.top+layout.offlineIcon.bottom)/2.0f-iconBox/2.0f,
        (layout.offlineIcon.left+layout.offlineIcon.right)/2.0f+iconBox/2.0f,
        (layout.offlineIcon.top+layout.offlineIcon.bottom)/2.0f+iconBox/2.0f),
        IconId::WifiOff,palette.err,1.8f);
    Text(target,view.offlineReason.empty()?"WSL \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE":view.offlineReason,
        layout.offlineTitle,label_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    Text(target,"\xE9\x9D\xA2\xE6\x9D\xBF\xE6\xAF\x8F\xE4\xB8\xAA\xE5\x88\xB7\xE6\x96\xB0\xE5\x91\xA8\xE6\x9C\x9F\xE4\xBC\x9A\xE8\x87\xAA\xE5\x8A\xA8\xE9\x87\x8D\xE8\xAF\x95\xE5\x94\xA4\xE9\x86\x92\xE5\xAE\x83\xEF\xBC\x9B\xE4\xB9\x9F\xE5\x8F\xAF\xE4\xBB\xA5\xE7\x8E\xB0\xE5\x9C\xA8\xE6\x89\x8B\xE5\x8A\xA8\xE5\x90\xAF\xE5\x8A\xA8\xE3\x80\x82",
        layout.offlineBody,caption_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_CENTER,false);
    Button(target,layout.offlineButton,"\xE5\x90\xAF\xE5\x8A\xA8 WSL",IconId::Power,
        widgets::ButtonSkinOf(widgets::ButtonVariant::Primary,widgets::Visual::Normal,palette),false);
}
void PanelRenderer::Button(ID2D1RenderTarget* target,D2D1_RECT_F rect,const std::string& label,IconId icon,
    const widgets::ButtonSkin& skin,bool disabled) {
    // 文字必须**在按钮里居中**（真机反馈"按钮的文字也不居中"）：文字盒子的右边界要给到内容末端，
    // 不能给 rect.right —— 否则用 LEADING 排版时"可用宽度"比实际内容宽，文字看着偏左。
    const float alpha=disabled?widgets::kDisabledOpacity:1.0f;
    widgets::DrawButtonShape(target,brush_.Get(),rect,skin,scale_);
    const auto width=Measure(caption_.Get(),label).width;
    if(icon==IconId::None) {
        // 无图标：直接按按钮盒居中（Text 的 CENTER 对齐 + 垂直居中）。
        Text(target,label,rect,caption_.Get(),Fade(skin.text,alpha),DWRITE_TEXT_ALIGNMENT_CENTER,true);
        return;
    }
    const auto iconBox=13.0f*scale_;
    const auto total=iconBox+widgets::kButtonIconGap*scale_+width;
    const auto start=rect.left+(rect.right-rect.left-total)/2.0f;
    Icon(target,D2D1::RectF(start,rect.top+(rect.bottom-rect.top-iconBox)/2,start+iconBox,
        rect.top+(rect.bottom-rect.top+iconBox)/2),icon,Fade(skin.text,alpha),1.7f);
    // 有图标：文字盒正好是它自己的宽度（LEADING 从图标右侧开始），左右都不留空。
    const auto textLeft=start+iconBox+widgets::kButtonIconGap*scale_;
    Text(target,label,D2D1::RectF(textLeft,rect.top,textLeft+width+1.0f*scale_,rect.bottom),
        caption_.Get(),Fade(skin.text,alpha),DWRITE_TEXT_ALIGNMENT_LEADING,true);
}
void PanelRenderer::SearchRow(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette,const TextField* field) {
    // The models page search row (DESIGN #10): raise box + line border, accent outline + soft glow
    // while focused; the IME composition draws inline with an underline, the caret blinks 530ms.
    const D2D1_RECT_F box=layout.search;
    const bool focused=field!=nullptr&&field->focused;
    if(focused) {
        brush_->SetColor(palette.accentSoft);
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left-3.0f*scale_,box.top-3.0f*scale_,
            box.right+3.0f*scale_,box.bottom+3.0f*scale_),widgets::kRadius*scale_,widgets::kRadius*scale_),
            brush_.Get());
    }
    widgets::DrawButtonShape(target,brush_.Get(),box,
        widgets::ButtonSkinOf(widgets::ButtonVariant::Normal,widgets::Visual::Normal,palette),scale_);
    if(focused) {
        brush_->SetColor(palette.accentLine);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,widgets::kRadius*scale_,widgets::kRadius*scale_),
            brush_.Get(),1.0f*scale_);
    }
    Icon(target,layout.searchIcon,IconId::Search,palette.faint,1.8f);
    if(view.searchHasText) {
        // The clear button replaces the "/" kbd hint once there is something to clear (old markup kept
        // both and toggled .hidden).
        const auto* clear=ControlRectFor(layout,ControlId::SearchClear);
        if(clear) {
            if(hovered_(ControlId::SearchClear)) {
                brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(*clear,widgets::kIcbRadius*scale_,
                    widgets::kIcbRadius*scale_),brush_.Get());
            }
            const auto glyphBox=12.0f*scale_;
            Icon(target,D2D1::RectF((clear->left+clear->right)/2.0f-glyphBox/2.0f,
                (clear->top+clear->bottom)/2.0f-glyphBox/2.0f,
                (clear->left+clear->right)/2.0f+glyphBox/2.0f,
                (clear->top+clear->bottom)/2.0f+glyphBox/2.0f),
                IconId::X,hovered_(ControlId::SearchClear)?palette.text:palette.faint,1.8f);
        }
    } else {
        Text(target,"/",layout.searchHint,count_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_CENTER,true);
    }
    if(field==nullptr) {
        Text(target,"\xE6\x90\x9C\xE7\xB4\xA2\xE6\xA8\xA1\xE5\x9E\x8B\xE6\x88\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",
            layout.searchText,caption_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        return;
    }
    const auto shown=field->DisplayText();
    const auto before=field->caret>0?shown.substr(0,static_cast<std::size_t>(field->caret)):std::wstring{};
    const auto rest=static_cast<std::size_t>(field->caret)<shown.size()
        ?shown.substr(static_cast<std::size_t>(field->caret)):std::wstring{};
    float cursorX=layout.searchText.left;
    if(!before.empty()) {
        const auto width=Measure(body_.Get(),Utf8(before)).width;
        Text(target,Utf8(before),D2D1::RectF(cursorX,layout.searchText.top,layout.searchText.right,
            layout.searchText.bottom),body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        cursorX+=width;
    }
    if(field->Composing()) {
        const auto compWidth=Measure(body_.Get(),Utf8(field->composition)).width;
        brush_->SetColor(palette.accentLine);
        target->FillRectangle(D2D1::RectF(cursorX,layout.searchText.bottom-6.0f*scale_,
            cursorX+compWidth,layout.searchText.bottom-5.0f*scale_),brush_.Get());
        Text(target,Utf8(field->composition),D2D1::RectF(cursorX,layout.searchText.top,
            layout.searchText.right,layout.searchText.bottom),body_.Get(),palette.dim,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        cursorX+=compWidth;
    }
    if(!rest.empty()) {
        Text(target,Utf8(rest),D2D1::RectF(cursorX,layout.searchText.top,layout.searchText.right,
            layout.searchText.bottom),body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    if(focused&&caretOn_) {
        brush_->SetColor(palette.accent);
        target->FillRectangle(D2D1::RectF(cursorX,layout.searchText.top+7.0f*scale_,cursorX+1.0f*scale_,
            layout.searchText.bottom-7.0f*scale_),brush_.Get());
    }
    if(shown.empty()&&!field->Composing()) {
        Text(target,"\xE6\x90\x9C\xE7\xB4\xA2\xE6\xA8\xA1\xE5\x9E\x8B\xE6\x88\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86",
            layout.searchText,caption_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
}
void PanelRenderer::Filters(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    // .filters: 全部 / 收藏 / 最近 each with a mono count (.seg b), then the right-aligned .hint.
    static const ControlId ids[3]={ControlId::FilterAll,ControlId::FilterFav,ControlId::FilterRecent};
    for(std::size_t index=0;index<layout.segments.size()&&index<3;++index) {
        const auto& seg=layout.segments[index];
        const auto skin=widgets::SegSkinOf(view.filters[index].active,hovered_(ids[index]),palette);
        widgets::DrawSegShape(target,brush_.Get(),seg.rect,skin,scale_);
        const float padX=widgets::kSegPadX*scale_;
        Text(target,view.filters[index].label,D2D1::RectF(seg.rect.left+padX,seg.rect.top,
            seg.rect.right-padX,seg.rect.bottom),body_.Get(),skin.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        if(view.filters[index].showCount&&view.filters[index].count>0) {
            // 计数用 body_ 量标签（之前用了 count_ 的 9.5px 单宽字体，位置会偏）。
            const float labelWidth=Measure(body_.Get(),view.filters[index].label).width;
            Text(target,std::to_string(view.filters[index].count),
                D2D1::RectF(seg.rect.left+padX+labelWidth+widgets::kChipGap*scale_+2.0f*scale_,seg.rect.top,
                    seg.rect.right-padX,seg.rect.bottom),
                count_.Get(),skin.count,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        }
    }
    if(view.showHint&&!view.hintText.empty()) {
        Text(target,view.hintText,layout.filterHint,tiny_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
    }
}
void PanelRenderer::GroupHead(ID2D1RenderTarget* target,const ListItem& item,
    const PanelViewModel::ModelGroup& group,const ThemePalette& palette) {
    // .group-head: caret, 6px identity dot, name, mono count, codex tag, key warning, then the "…"
    // menu. The active head carries a 2px inset bar in the identity colour; a hairline closes it.
    const bool hovered=hoverItem_.inList&&hoverItem_.kind==ListItemKind::GroupHead
        &&hoverItem_.group==item.group;
    const auto hue=static_cast<float>(group.hue);
    const auto skin=widgets::GroupHeadSkinOf(group.active,hovered,hue,palette);
    brush_->SetColor(skin.fill);
    target->FillRectangle(item.rect,brush_.Get());
    if(skin.inset.a>0.0f) {
        brush_->SetColor(skin.inset);
        target->FillRectangle(D2D1::RectF(item.rect.left,item.rect.top,item.rect.left+2.0f*scale_,item.rect.bottom),
            brush_.Get());
    }
    float x=item.rect.left+widgets::kGroupHeadPadX*scale_;
    {
        const auto caret=Dip(widgets::kCaretBox,scale_);
        const auto box=D2D1::RectF(x,item.rect.top+(item.rect.bottom-item.rect.top-caret)/2.0f,x+caret,
            item.rect.top+(item.rect.bottom-item.rect.top+caret)/2.0f);
        target->SetTransform(D2D1::Matrix3x2F::Rotation(group.open?90.0f:0.0f,
            D2D1::Point2F((box.left+box.right)/2.0f,(box.top+box.bottom)/2.0f)));
        Icon(target,box,IconId::ChevronRight,palette.faint,2.0f);
        target->SetTransform(D2D1::Matrix3x2F::Identity());
        x=box.right+widgets::kGroupHeadGap*scale_;
    }
    {
        // .gdot: a 6px identity square with a soft glow.
        const auto size=Dip(widgets::kGroupDot,scale_);
        const auto box=D2D1::RectF(x,item.rect.top+(item.rect.bottom-item.rect.top-size)/2.0f,x+size,
            item.rect.top+(item.rect.bottom-item.rect.top+size)/2.0f);
        const auto colour=widgets::IdentityColour(hue);
        // .gdot { box-shadow: 0 0 7px hsl(.../.45) }：模糊光晕（半径 7 DIP）。
        widgets::DrawSoftGlow(target,D2D1::Point2F((box.left+box.right)/2.0f,(box.top+box.bottom)/2.0f),
            (box.right-box.left)/2.0f/scale_,7.0f,colour,0.45f,scale_);
        brush_->SetColor(colour);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kGroupDotRadius*scale_,
            widgets::kGroupDotRadius*scale_),brush_.Get());
        x=box.right+widgets::kGroupHeadGap*scale_;
    }
    const float textTop=item.rect.top,textBottom=item.rect.bottom;
    const float menuLeft=widgets::MenuButtonRect(item.rect,scale_).left-widgets::kGroupHeadGap*scale_;
    const auto pill=[&](const std::string& text,const D2D1_COLOR_F& fill,const D2D1_COLOR_F& colour,
        IDWriteTextFormat* format,float padX,float radius) {
        const auto width=Measure(format,text).width+padX*2;
        const float height=widgets::kCountPillHeight*scale_;
        const auto box=D2D1::RectF(x,item.rect.top+(item.rect.bottom-item.rect.top-height)/2.0f,x+width,
            item.rect.top+(item.rect.bottom-item.rect.top+height)/2.0f);
        brush_->SetColor(fill);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,radius*scale_,radius*scale_),brush_.Get());
        Text(target,text,box,format,colour,DWRITE_TEXT_ALIGNMENT_CENTER,true);
        x=box.right+widgets::kGroupHeadGap*scale_;
    };
    {
        const auto nameWidth=std::max(0.0f,menuLeft-x);
        Text(target,group.title,D2D1::RectF(x,textTop,x+nameWidth,textBottom),groupName_.Get(),skin.name,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        const auto measured=Measure(groupName_.Get(),group.title).width;
        x=std::min(x+measured+widgets::kGroupHeadGap*scale_,menuLeft);
    }
    if(!group.countText.empty())
        pill(group.countText,skin.countFill,skin.countText,count_.Get(),widgets::kCountPillPadX,
            widgets::kCountPillRadius);
    if(group.codex&&x<menuLeft)
        pill("codex",skin.tagFill,skin.tagText,count_.Get(),widgets::kCountPillPadX,
            widgets::kCountPillRadius);
    if(group.keyWarn&&x<menuLeft) {
        const auto size=12.0f*scale_;
        Icon(target,D2D1::RectF(x,item.rect.top+(item.rect.bottom-item.rect.top-size)/2.0f,x+size,
            item.rect.top+(item.rect.bottom-item.rect.top+size)/2.0f),IconId::Key,palette.err,1.7f);
        x+=size+widgets::kGroupHeadGap*scale_;
    }
    {
        // .gallbtn: faint at 45% until the head is hovered, then full - the provider menu itself
        // (probe/delete) arrives with the H5 forms.
        const auto box=widgets::MenuButtonRect(item.rect,scale_);
        const bool menuHovered=hovered&&hoverItem_.menu;
        const auto alpha=menuHovered?1.0f:widgets::kMenuIdleOpacity;
        if(menuHovered) {
            brush_->SetColor(palette.raise2);
            target->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kIcbRadius*scale_,
                widgets::kIcbRadius*scale_),brush_.Get());
        }
        const auto glyph=widgets::kMenuIcon*scale_;
        Icon(target,D2D1::RectF((box.left+box.right)/2.0f-glyph/2.0f,(box.top+box.bottom)/2.0f-glyph/2.0f,
            (box.left+box.right)/2.0f+glyph/2.0f,(box.top+box.bottom)/2.0f+glyph/2.0f),IconId::MoreHorizontal,
            Fade(menuHovered?palette.text:palette.faint,alpha),2.0f);
    }
    if(skin.hairline) widgets::DrawHairline(target,D2D1::RectF(item.rect.left,item.rect.bottom-1.0f*scale_,
        item.rect.right,item.rect.bottom),palette,scale_);
}
void PanelRenderer::ModelRow(ID2D1RenderTarget* target,const ListItem& item,
    const PanelViewModel::ModelRow& row,bool flatView,const ThemePalette& palette) {
    // .mrow: hover raise / active raise2, current rows carry the identity tint plus a 2px inset bar, and
    // the cursor row is outlined in the accent colour with a ↵ after the star.
    const bool hovered=hoverItem_.inList&&hoverItem_.kind==ListItemKind::Row&&hoverItem_.group==item.group
        &&hoverItem_.row==item.row;
    const bool cursor=row.index>=0&&row.index==cursorIndex_;
    const auto hue=static_cast<float>(row.hue);
    const auto skin=widgets::RowSkinOf(row.current,cursor,
        hovered?widgets::Visual::Hovered:widgets::Visual::Normal,hue,palette);
    if(skin.fill.a>0.0f) {
        brush_->SetColor(skin.fill);
        target->FillRectangle(item.rect,brush_.Get());
    }
    if(row.current) {
        brush_->SetColor(widgets::IdentityColour(hue));
        target->FillRectangle(D2D1::RectF(item.rect.left,item.rect.top,item.rect.left+2.0f*scale_,
            item.rect.bottom),brush_.Get());
    }
    if(cursor) {
        brush_->SetColor(skin.outline);
        target->DrawRectangle(D2D1::RectF(item.rect.left+0.75f*scale_,item.rect.top+0.75f*scale_,
            item.rect.right-0.75f*scale_,item.rect.bottom-0.75f*scale_),brush_.Get(),1.5f*scale_);
    }
    float x=item.rect.left+widgets::kRowPadLeft*scale_;
    const float starLeft=widgets::StarRect(item.rect,scale_).left;
    const float cursorMarkWidth=(cursor?12.0f*scale_:0.0f);
    const float textRight=std::max(x,starLeft-widgets::kRowGap*scale_-cursorMarkWidth);
    if(flatView) {
        const float colRight=std::min(x+widgets::kPcolWidth*scale_,textRight);
        Text(target,row.provider,D2D1::RectF(x,item.rect.top,colRight,item.rect.bottom),busy_.Get(),skin.pcol,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        x=colRight+widgets::kRowGap*scale_;
    }
    // F4 延迟徽章：行尾、等宽小字；<1s 绿 / <3s 黄 / ≥3s 或失败红。
    const std::string pillLabel="\xE5\xBD\x93\xE5\x89\x8D";
    const float pillWidth=row.current?Measure(pillText_.Get(),pillLabel).width
        +widgets::kPillPadX*2*scale_:0.0f;
    float contentRight=textRight-(row.current?pillWidth+widgets::kRowGap*scale_:0.0f);
    if(row.hasLatency) {
        const auto badgeText="~"+std::to_string(row.latencyMs)+"ms";
        const float badgeWidth=Measure(count_.Get(),badgeText).width+8.0f*scale_;
        const float badgeLeft=std::max(x,contentRight-badgeWidth);
        const auto colour=!row.latencyOk?palette.err
            :(row.latencyMs<1000?palette.ok:(row.latencyMs<3000?palette.warn:palette.err));
        Text(target,badgeText,D2D1::RectF(badgeLeft,item.rect.top,contentRight,item.rect.bottom),
            count_.Get(),colour,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
        contentRight=badgeLeft-widgets::kRowGap*scale_;
    }
    if(row.current) {
        // .pill 当前: 15px tall, radius 999, accent-soft fill with an accent-line border.
        const auto width=pillWidth;
        const auto box=D2D1::RectF(textRight-width,item.rect.top+(item.rect.bottom-item.rect.top
            -widgets::kPillHeight*scale_)/2.0f,textRight,item.rect.top+(item.rect.bottom-item.rect.top
            +widgets::kPillHeight*scale_)/2.0f);
        brush_->SetColor(palette.accentSoft);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kPillHeight*scale_/2.0f,
            widgets::kPillHeight*scale_/2.0f),brush_.Get());
        brush_->SetColor(palette.accentLine);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,widgets::kPillHeight*scale_/2.0f,
            widgets::kPillHeight*scale_/2.0f),brush_.Get(),1.0f*scale_);
        Text(target,pillLabel,box,pillText_.Get(),palette.accentStrong,DWRITE_TEXT_ALIGNMENT_CENTER,true);
        Text(target,row.model,D2D1::RectF(x,item.rect.top,
            std::min(box.left-widgets::kRowGap*scale_,contentRight),item.rect.bottom),mono_.Get(),
            skin.model,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    } else {
        Text(target,row.model,D2D1::RectF(x,item.rect.top,contentRight,item.rect.bottom),mono_.Get(),skin.model,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    if(cursor) {
        // .mrow.is-cursor::after: the ↵ sits after the star, in the accent colour.
        const auto star=widgets::StarRect(item.rect,scale_);
        const auto glyphWidth=12.0f*scale_;
        Icon(target,D2D1::RectF(star.right+widgets::kRowGap*scale_,item.rect.top+
            (item.rect.bottom-item.rect.top-glyphWidth)/2.0f,star.right+widgets::kRowGap*scale_+glyphWidth,
            item.rect.top+(item.rect.bottom-item.rect.top+glyphWidth)/2.0f),IconId::CornerDownLeft,
            palette.accentStrong,1.8f);
    }
    {
        // .star: invisible until the row is hovered (60%) or the model is a favourite (100%).
        const auto box=widgets::StarRect(item.rect,scale_);
        const float alpha=row.favorite?1.0f:(hovered?widgets::kStarHoverOpacity:widgets::kStarIdleOpacity);
        if(hovered&&!row.favorite) {
            // .star:hover { background: raise2 }
            if(hoverItem_.star) {
                brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kIcbRadius*scale_,
                    widgets::kIcbRadius*scale_),brush_.Get());
            }
        }
        if(alpha>0.0f) {
            const auto glyph=widgets::kStarGlyph*scale_;
            const auto rect=D2D1::RectF((box.left+box.right)/2.0f-glyph/2.0f,
                (box.top+box.bottom)/2.0f-glyph/2.0f,(box.left+box.right)/2.0f+glyph/2.0f,
                (box.top+box.bottom)/2.0f+glyph/2.0f);
            const auto colour=row.favorite?palette.accentStrong:palette.faint;
            DrawStarIcon(target,brush_.Get(),arcStyle_.Get(),rect,row.favorite,Fade(colour,alpha),2.0f,scale_);
        }
    }
    if(skin.underline) widgets::DrawHairline(target,D2D1::RectF(item.rect.left,item.rect.bottom-1.0f*scale_,
        item.rect.right,item.rect.bottom),palette,scale_);
}
void PanelRenderer::List(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    // .list: a hairline on top, the items (group heads / rows / section captions / empty states), the
    // scrollbar and the 16px bottom fade. Everything is clipped to the viewport.
    brush_->SetColor(palette.line2);
    target->FillRectangle(D2D1::RectF(layout.list.left,layout.list.top-1.0f*scale_,layout.list.right,
        layout.list.top),brush_.Get());
    target->PushAxisAlignedClip(D2D1::RectF(layout.list.left,layout.listViewport.top,layout.list.right,
        layout.listViewport.bottom),D2D1_ANTIALIAS_MODE_ALIASED);
    // 两趟画：先所有非粘性项，再粘性组头/分组标题 —— 旧 CSS 里 .group-head 是 sticky 且 z-index:2，
    // 行滚到它下面时被它盖住；一趟画会让后来的行压住组头（真机反馈的"供应商和其模型列表重叠"）。
    const auto drawItem=[&](const ListItem& item) {

        if(item.rect.bottom<layout.listViewport.top||item.rect.top>layout.listViewport.bottom) return;
        switch(item.kind) {
        case ListItemKind::GroupHead:
            if(item.group>=0&&item.group<static_cast<int>(view.groups.size()))
                GroupHead(target,item,view.groups[static_cast<std::size_t>(item.group)],palette);
            break;
        case ListItemKind::Row:
            if(item.group>=0&&item.group<static_cast<int>(view.groups.size())) {
                const auto& group=view.groups[static_cast<std::size_t>(item.group)];
                if(item.row>=0&&item.row<static_cast<int>(group.rows.size()))
                    ModelRow(target,item,group.rows[static_cast<std::size_t>(item.row)],view.flatView,palette);
            }
            break;
        case ListItemKind::Sect:
            Text(target,item.text,D2D1::RectF(item.rect.left+widgets::kGroupHeadPadX*scale_,item.rect.top,
                item.rect.right,item.rect.bottom),busy_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            break;
        case ListItemKind::Empty:
            TextWrapped(target,item.text,D2D1::RectF(item.rect.left+12.0f*scale_,item.rect.top,
                item.rect.right-12.0f*scale_,item.rect.bottom),emptyText_.Get(),palette.faint);
            break;
        }
    };
    // 十万级条目时不能全量绘制：只画视口内的行（旧的 DOM 会由浏览器裁剪，自绘要自己做）。
    for(const auto& item:layout.items) {
        if(item.sticky) continue;
        if(item.rect.bottom<layout.listViewport.top||item.rect.top>layout.listViewport.bottom)
            continue;
        drawItem(item);
    }
    for(const auto& item:layout.items) if(item.sticky) drawItem(item);
    target->PopAxisAlignedClip();
    if(layout.scrollVisible) {
        const bool hot=hoverItem_.inList&&view.pointerInside;
        auto thumb=layout.scrollThumb;
        if(hot) {
            const float grow=(widgets::kScrollBarHoverWidth-widgets::kScrollBarWidth)*scale_/2.0f;
            thumb=D2D1::RectF(thumb.left-grow,thumb.top,thumb.right+grow,thumb.bottom);
        }
        brush_->SetColor(hot?palette.line:palette.raise2);
        target->FillRoundedRectangle(D2D1::RoundedRect(thumb,widgets::kScrollBarWidth*scale_/2.0f,
            widgets::kScrollBarWidth*scale_/2.0f),brush_.Get());
    }
    PrepareFade(target,palette);
    // The gradient brush runs top->bottom in its own 0..1 space: place it over the last 16px of the list.
    fade_->SetStartPoint(D2D1::Point2F(layout.list.left,layout.listViewport.bottom-widgets::kListFade*scale_));
    fade_->SetEndPoint(D2D1::Point2F(layout.list.left,layout.listViewport.bottom));
    target->FillRectangle(D2D1::RectF(layout.list.left,layout.listViewport.bottom-widgets::kListFade*scale_,
        layout.list.right,layout.listViewport.bottom),fade_.Get());
}
void PanelRenderer::PageHead(ID2D1RenderTarget* target,const PanelLayout& layout,const std::string& title,
    const std::string& subtitle,const std::string& action,IconId actionIcon,bool actionHovered,
    const ThemePalette& palette) {
    // .ph: 12.5/500 title + 10.5 faint subtitle on the left, a ghost action button on the right.
    Text(target,title,layout.pageHeadTitle,label_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    if(!subtitle.empty())
        Text(target,subtitle,layout.pageHeadSub,busy_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    if(!action.empty()&&layout.pageHeadAction.right>layout.pageHeadAction.left) {
        Button(target,layout.pageHeadAction,action,actionIcon,
            widgets::ButtonSkinOf(widgets::ButtonVariant::Ghost,
                actionHovered?widgets::Visual::Hovered:widgets::Visual::Normal,palette),false);
    }
}
void PanelRenderer::Field(ID2D1RenderTarget* target,const PanelLayout::FieldBox& field,
    const std::string& label,const std::string& value,bool focused,bool masked,const char* hint,
    const ThemePalette& palette) {
    // .fld: an 11px dim label with a 4px gap, then the input box (.fld input: raise fill, line border,
    // radius 9, 6px 9px padding; focus = accent-line + accent-soft halo).
    Text(target,label,field.label,busy_.Get(),palette.dim,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    const auto box=field.box;
    widgets::DrawButtonShape(target,brush_.Get(),box,
        widgets::ButtonSkinOf(widgets::ButtonVariant::Normal,widgets::Visual::Normal,palette),scale_);
    if(focused) {
        brush_->SetColor(palette.accentLine);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,widgets::kRadius*scale_,widgets::kRadius*scale_),
            brush_.Get(),1.0f*scale_);
    }
    const auto inset=Dip(9.0f,scale_);
    const auto textRect=D2D1::RectF(box.left+inset,box.top,box.right-inset,box.bottom);
    if(field.kind==3) {
        // 下拉：右侧一个 chevron，文案是选中项
        Text(target,value,textRect,body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        const auto box2=Dip(14.0f,scale_);
        Icon(target,D2D1::RectF(box.right-inset-box2,(box.top+box.bottom)/2.0f-box2/2.0f,
            box.right-inset,(box.top+box.bottom)/2.0f+box2/2.0f),IconId::ChevronDown,palette.faint,1.8f);
        return;
    }
    if(value.empty()) {
        Text(target,hint?hint:"",textRect,caption_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        return;
    }
    if(masked) {
        // 掩码：每个字符一个 U+2022（旧 input[type=password] 的等宽点）
        std::string dots;
        for(std::size_t index=0;index<value.size();++index) dots+="\xE2\x80\xA2";
        Text(target,dots,textRect,body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        return;
    }
    Text(target,value,textRect,body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
}
void PanelRenderer::FallbackPage(ID2D1RenderTarget* target,const PanelLayout& layout,
    const PanelViewModel& view,const ThemePalette& palette) {
    const auto& page=view.fallback;
    PageHead(target,layout,page.title,page.subtitle,page.probeLabel,IconId::Activity,
        hovered_(ControlId::FbProbe),palette);
    // .ladder: one .fb-row per entry - a numbered badge (first row accent), the provider line, the mono
    // model line, the probe result and the three buttons.
    if(page.rows.empty()) {
        TextWrapped(target,page.emptyText,D2D1::RectF(layout.ladder.left+12.0f*scale_,layout.ladder.top,
            layout.ladder.right-12.0f*scale_,layout.ladder.top+Dip(69.0f,scale_)),emptyText_.Get(),palette.faint);
    }
    // 条目多时阶梯内部滚动：只在视口内绘制（否则会盖到「追加一条」和底部导航上）。
    target->PushAxisAlignedClip(layout.ladder,D2D1_ANTIALIAS_MODE_ALIASED);
    for(std::size_t index=0;index<page.rows.size()&&index<layout.ladderRows.size();++index) {
        const auto& row=page.rows[index];
        const auto box=layout.ladderRows[index].rect;
        if(box.top>=layout.ladder.bottom) break;
        const bool first=index==0;
        const auto hue=static_cast<float>((index*29)%8*12+186);
        // .fb-idx: 20x20 r5 badge; the first entry sits on the accent.
        const auto badge=Dip(20.0f,scale_);
        const auto badgeBox=D2D1::RectF(box.left+Dip(2.0f,scale_),box.top+Dip(kLadderRowPadTrim,scale_),
            box.left+Dip(2.0f,scale_)+badge,box.top+Dip(kLadderRowPadTrim,scale_)+badge);
        brush_->SetColor(first?palette.accentSoft:palette.raise2);
        target->FillRoundedRectangle(D2D1::RoundedRect(badgeBox,5.0f*scale_,5.0f*scale_),brush_.Get());
        Text(target,std::to_string(index+1),badgeBox,count_.Get(),first?palette.accentStrong:palette.dim,
            DWRITE_TEXT_ALIGNMENT_CENTER,true);
        if(index+1<page.rows.size()) {
            // .fb-idx::after: the ladder spine, 1px, 24px long, accent-tinted on the first row.
            brush_->SetColor(first?palette.accentLine:palette.line);
            target->FillRectangle(D2D1::RectF(badgeBox.left+badge/2.0f,badgeBox.bottom,
                badgeBox.left+badge/2.0f+1.0f*scale_,badgeBox.bottom+24.0f*scale_),brush_.Get());
        }
        const float textLeft=badgeBox.right+Dip(9.0f,scale_);
        const float buttonsLeft=LadderButtonBox(box,1,scale_).left-Dip(9.0f,scale_);
        Text(target,row.provider,D2D1::RectF(textLeft,box.top+Dip(kLadderRowPadTrim,scale_),
            buttonsLeft-Dip(60.0f,scale_),box.top+Dip(kLadderRowPadTrim+18.0f,scale_)),body_.Get(),
            palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        Text(target,row.model,D2D1::RectF(textLeft,box.top+Dip(kLadderRowPadTrim+18.0f,scale_),
            buttonsLeft-Dip(60.0f,scale_),box.top+Dip(kLadderRowPadTrim+36.0f,scale_)),mono_.Get(),
            palette.dim,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        if(row.keyWarn) {
            const auto size=Dip(11.0f,scale_);
            const auto warnX=textLeft+Measure(body_.Get(),row.provider).width+Dip(7.0f,scale_);
            Icon(target,D2D1::RectF(warnX,box.top+Dip(kLadderRowPadTrim+3.0f,scale_),warnX+size,
                box.top+Dip(kLadderRowPadTrim+3.0f,scale_)+size),IconId::Key,palette.err,1.6f);
        }
        if(row.probePending) {
            // 正在探测这一行：一个省略号占住结果位，配合页头的「检测中 n/N」。
            Text(target,"\xE2\x80\xA6",D2D1::RectF(buttonsLeft-Dip(56.0f,scale_),box.top+Dip(kLadderRowPadTrim,scale_),
                buttonsLeft-Dip(8.0f,scale_),box.top+Dip(kLadderRowPadTrim+18.0f,scale_)),count_.Get(),
                palette.dim,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
        } else if(row.hasProbe) {
            // .fb-pd: ●N when the probe answered and lists this model, ●N amber when it answered
            // without the model (the fallback is dead), ○ when unreachable.
            const auto text=row.probeOk?(row.probeCount>0
                ?"\xE2\x97\x8F"+std::to_string(row.probeCount):std::string("\xE2\x97\x8F"))
                :"\xE2\x97\x8B";
            const auto colour=!row.probeOk?palette.err:(row.probeHasModel?palette.ok:palette.warn);
            Text(target,text,D2D1::RectF(buttonsLeft-Dip(56.0f,scale_),box.top+Dip(kLadderRowPadTrim,scale_),
                buttonsLeft-Dip(8.0f,scale_),box.top+Dip(kLadderRowPadTrim+18.0f,scale_)),count_.Get(),
                colour,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
        }
        const auto actionIcon=[&](int which,IconId icon,const D2D1_COLOR_F& colour,float alpha) {
            const auto b=LadderButtonBox(box,which,scale_);
            const bool hot=hoverItem_.inList&&hoverItem_.kind==ListItemKind::Row&&hoverItem_.row==static_cast<int>(index)
                &&hoverItem_.star==false&&hoverKnob_==which;
            if(hot) {
                brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(b,6.0f*scale_,6.0f*scale_),brush_.Get());
            }
            const auto glyph=Dip(12.0f,scale_);
            Icon(target,D2D1::RectF((b.left+b.right)/2.0f-glyph/2.0f,(b.top+b.bottom)/2.0f-glyph/2.0f,
                (b.left+b.right)/2.0f+glyph/2.0f,(b.top+b.bottom)/2.0f+glyph/2.0f),icon,
                Fade(colour,alpha),1.8f);
        };
        actionIcon(1,IconId::ArrowUp,palette.faint,index==0?0.35f:1.0f);
        actionIcon(2,IconId::ArrowDown,palette.faint,index+1==page.rows.size()?0.35f:1.0f);
        actionIcon(3,IconId::X,palette.err,1.0f);
        widgets::DrawHairline(target,D2D1::RectF(box.left,box.bottom-1.0f*scale_,box.right,box.bottom),
            palette,scale_);
        (void)hue;
    }
    target->PopAxisAlignedClip();
    if(layout.fbMaxScroll>0.5f) {
        const float width=layout.fbScrollTrack.right-layout.fbScrollTrack.left;
        brush_->SetColor(palette.line);
        target->FillRoundedRectangle(D2D1::RoundedRect(layout.fbScrollTrack,width,width),brush_.Get());
        brush_->SetColor(palette.line2);
        target->FillRoundedRectangle(D2D1::RoundedRect(layout.fbScrollThumb,width,width),brush_.Get());
    }
    // .addline: the dashed "追加一条" line under the ladder.
    {
        const auto addBox=layout.addLine;
        brush_->SetColor(hovered_(ControlId::FbFoldToggle)?palette.accentLine:palette.line);
        const auto dashed=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT,D2D1_CAP_STYLE_FLAT,
            D2D1_CAP_STYLE_FLAT,D2D1_LINE_JOIN_MITER,10.0f,D2D1_DASH_STYLE_DASH,0.0f);
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        Microsoft::WRL::ComPtr<ID2D1StrokeStyle> style;
        if(SUCCEEDED(factory->CreateStrokeStyle(dashed,nullptr,0,style.GetAddressOf()))) {
            target->DrawRoundedRectangle(D2D1::RoundedRect(addBox,widgets::kRadius*scale_,
                widgets::kRadius*scale_),brush_.Get(),1.0f*scale_,style.Get());
        }
        const auto glyph=Dip(13.0f,scale_);
        const auto width=Measure(body_.Get(),page.addLabel).width;
        const auto start=(addBox.left+addBox.right)/2.0f-(glyph+Dip(6.0f,scale_)+width)/2.0f;
        Icon(target,D2D1::RectF(start,(addBox.top+addBox.bottom)/2.0f-glyph/2.0f,start+glyph,
            (addBox.top+addBox.bottom)/2.0f+glyph/2.0f),IconId::Plus,
            hovered_(ControlId::FbFoldToggle)?palette.text:palette.dim,1.8f);
        Text(target,page.addLabel,D2D1::RectF(start+glyph+Dip(6.0f,scale_),addBox.top,addBox.right,addBox.bottom),
            body_.Get(),hovered_(ControlId::FbFoldToggle)?palette.text:palette.dim,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    // .fold：追加表单（页内浮层卡片，不把页面撑高）
    if(page.formOpen) {
        const auto box=layout.fold;
        brush_->SetColor(Fade(D2D1::ColorF(0,0,0),0.45f));
        target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left,box.top+2.0f*scale_,
            box.right,box.bottom+2.0f*scale_),widgets::kRadius*scale_,widgets::kRadius*scale_),brush_.Get());
        brush_->SetColor(palette.surf);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kRadius*scale_,widgets::kRadius*scale_),
            brush_.Get());
        brush_->SetColor(palette.line);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,widgets::kRadius*scale_,widgets::kRadius*scale_),
            brush_.Get(),1.0f*scale_);
        {
            // .fold-x：右上角的收起按钮
            const auto close=layout.foldClose;
            if(hovered_(ControlId::FbFoldClose)) {
                brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(close,widgets::kIcbRadius*scale_,
                    widgets::kIcbRadius*scale_),brush_.Get());
            }
            const auto glyph=Dip(12.0f,scale_);
            Icon(target,D2D1::RectF((close.left+close.right)/2.0f-glyph/2.0f,
                (close.top+close.bottom)/2.0f-glyph/2.0f,(close.left+close.right)/2.0f+glyph/2.0f,
                (close.top+close.bottom)/2.0f+glyph/2.0f),IconId::X,palette.faint,1.8f);
        }
        if(layout.fields.size()>=2) {
            const auto source=page.sources.empty()?"":page.sources[static_cast<std::size_t>(
                std::clamp(page.source,0,static_cast<int>(page.sources.size())-1))];
            Field(target,layout.fields[0],"来源供应商",source,false,false,nullptr,palette);
            Field(target,layout.fields[1],"模型 id",page.modelValue,false,false,
                "deepseek-v4.1-flash",palette);
            if(layout.fields.size()>=6) {
                // 手填模式：provider/Base URL 与 Key 变量名/API Key 各占一行（见 layout 里的说明）。
                const char* labels[6]={nullptr,nullptr,"provider 名","Base URL",
                    "Key 变量名（可选）","API Key（可选，写入 .env）"};
                const char* hints[6]={nullptr,nullptr,"例如 myrelay","https://…/v1","MYRELAY_API_KEY",nullptr};
                const std::string values[6]={std::string{},std::string{},page.providerValue,page.urlValue,
                    page.keyEnvValue,page.keyValue};
                for(int index=2;index<=5;++index)
                    Field(target,layout.fields[static_cast<std::size_t>(index)],labels[index],values[index],
                        false,index==5,hints[index],palette);
            }
            Button(target,layout.primaryButton,page.buttonLabel,IconId::Plus,
                widgets::ButtonSkinOf(widgets::ButtonVariant::Primary,widgets::Visual::Normal,palette),false);
            if(!page.error.empty())
                Text(target,page.error,layout.errorLine,caption_.Get(),palette.err,
                    DWRITE_TEXT_ALIGNMENT_LEADING,true);
        }
        // 下拉列表在页面内容之后统一画（见 Draw 里的 SelectList 调用）：两个页面共用。
    }
}
void PanelRenderer::SegRow(ID2D1RenderTarget* target,const PanelLayout& layout,
        const std::vector<PanelViewModel::FilterSeg>& segs,std::size_t firstSegment,
        const ThemePalette& palette,bool bordered) {
    // .seg：选中态 accent-soft 底 + accent-line 描边 + accent-strong 文案；非选中透明底 + dim；
    // 计数用 mono（选中 70% accent-strong，否则 faint）。
    // bordered（主题选择器这类"选项"）：未选中也要有 surf 底 + line 描边——纯文字在浅色主题里
    // 散得像标签而不是可点的选项（真机反馈"主题的选项没有边框，很丑"）。
    for(std::size_t index=0;index<segs.size();++index) {
        const auto segmentIndex=firstSegment+index;
        if(segmentIndex>=layout.segments.size()) break;
        const auto& slot=layout.segments[segmentIndex];
        const auto& seg=segs[index];
        const bool hovered=hoverSegment_==static_cast<int>(segmentIndex);
        auto skin=widgets::SegSkinOf(seg.active,hovered,palette);
        if(bordered&&!seg.active) {
            skin.fill=hovered?palette.raise2:palette.surf;
            skin.border=palette.line;
        }
        widgets::DrawSegShape(target,brush_.Get(),slot.rect,skin,scale_);
        const float padX=Dip(widgets::kSegPadX,scale_);
        Text(target,seg.label,D2D1::RectF(slot.rect.left+padX,slot.rect.top,slot.rect.right-padX,
            slot.rect.bottom),body_.Get(),skin.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        // 计数显示与否由视图决定（旧备份页只有「全部 <b>N</b>」带计数）。
        if(seg.showCount&&seg.count>0) {
            const auto labelWidth=Measure(body_.Get(),seg.label).width;
            Text(target,std::to_string(seg.count),D2D1::RectF(slot.rect.left+padX+labelWidth+Dip(5,scale_),
                slot.rect.top,slot.rect.right-padX,slot.rect.bottom),count_.Get(),skin.count,
                DWRITE_TEXT_ALIGNMENT_LEADING,true);
        }
    }
}
void PanelRenderer::SwitchLine(ID2D1RenderTarget* target,D2D1_RECT_F row,const std::string& label,
    const std::string& note,bool on,bool hovered,const ThemePalette& palette) {
    // .switchrow：11.5px 文案 + 8px 间隔 + 28x16 开关（滑钮 10，选中 accent-soft/accent-strong）。
    const auto skin=widgets::SwitchSkinOf(on,false,palette);
    const float trackHeight=Dip(widgets::kSwitchHeight,scale_);
    const float trackWidth=Dip(widgets::kSwitchWidth,scale_);
    const auto track=D2D1::RectF(row.left,row.top+(row.bottom-row.top-trackHeight)/2.0f,
        row.left+trackWidth,row.top+(row.bottom-row.top+trackHeight)/2.0f);
    widgets::DrawSwitchShape(target,brush_.Get(),track,skin,on,scale_);
    const float textLeft=track.right+Dip(8,scale_);
    Text(target,label,D2D1::RectF(textLeft,row.top,row.right,row.bottom),body_.Get(),palette.text,
        DWRITE_TEXT_ALIGNMENT_LEADING,true);
    if(!note.empty()) {
        const auto labelWidth=Measure(body_.Get(),label).width;
        Text(target,note,D2D1::RectF(textLeft+labelWidth+Dip(6,scale_),row.top,row.right,row.bottom),
            caption_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    (void)hovered;
}
void PanelRenderer::AdminPages(ID2D1RenderTarget* target,const PanelLayout& layout,
    const PanelViewModel& view,const ThemePalette& palette) {
    const bool addPage=view.activePage==2,backupPage=view.activePage==3;
    if(addPage||backupPage) {          // 设置页没有页头（旧面板只有一个 .form）
        const auto title=addPage?view.add.title:view.backup.title;
        const auto subtitle=backupPage?view.backup.subtitle:std::string{};
        const auto action=backupPage?view.backup.refreshLabel:std::string{};
        PageHead(target,layout,title,subtitle,action,IconId::RefreshCw,hovered_(ControlId::BackupRefresh),
            palette);
    }
    // 正文裁剪到视口（窗口档位固定，正文比它高时整页滚动 —— 旧 body.clamped .page 的等价物）
    target->PushAxisAlignedClip(layout.content,D2D1_ANTIALIAS_MODE_ALIASED);
    for(const auto& note:layout.notes) {
        if(note.kind>0)
            Text(target,note.text,D2D1::RectF(note.rect.left+Dip(11,scale_),note.rect.top,note.rect.right,
                note.rect.bottom),busy_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        else if(!note.text.empty())
            // 这些是字段/分段的说明或标签（旧 .fld > span、.srow small 都左对齐），不居中。
            TextWrapped(target,note.text,note.rect,caption_.Get(),palette.faint,
                DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    for(const auto& label:layout.labels)
        Text(target,label.text,label.rect,body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    if(addPage) {
        const auto& page=view.add;
        if(page.danger&&layout.dangerCard.right>layout.dangerCard.left) {
            brush_->SetColor(palette.errSoft);
            target->FillRoundedRectangle(D2D1::RoundedRect(layout.dangerCard,widgets::kRadius*scale_,
                widgets::kRadius*scale_),brush_.Get());
            brush_->SetColor(Fade(palette.err,0.26f));
            target->DrawRoundedRectangle(D2D1::RoundedRect(layout.dangerCard,widgets::kRadius*scale_,
                widgets::kRadius*scale_),brush_.Get(),1.0f*scale_);
        }
        for(const auto& field:layout.fields) {
            if(field.index<0||field.index>=static_cast<int>(page.fields.size())) continue;
            const auto& source=page.fields[static_cast<std::size_t>(field.index)];
            const bool focused=view.focusedFormField==field.index;
            Field(target,field,source.label,source.value,focused,source.masked,source.hint.c_str(),palette);
            if(!source.note.empty()) {
                const auto labelWidth=Measure(busy_.Get(),source.label).width;
                const auto noteLeft=field.label.left+labelWidth+Dip(6,scale_);
                if(noteLeft+Measure(count_.Get(),source.note).width<=field.label.right)
                    Text(target,source.note,D2D1::RectF(noteLeft,field.label.top,field.label.right,
                        field.label.bottom),count_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            }
        }
        // 审计 F-012：API Key 的显隐按钮（眼睛）。位置由布局给出，与命中同源。
        if(page.sub==0) {
            for(const auto& control:layout.controls) {
                if(control.id!=ControlId::KeyReveal) continue;
                const auto colour=hovered_(ControlId::KeyReveal)?palette.accent:palette.faint;
                Icon(target,control.rect,page.keyRevealed?IconId::EyeOff:IconId::Eye,colour,1.7f);
                break;
            }
        }
        std::vector<PanelViewModel::FilterSeg> subTabs;
        for(std::size_t index=0;index<page.subs.size();++index)
            subTabs.push_back({page.subs[index],0,static_cast<int>(index)==page.sub,false}); // 模式切换无计数
        SegRow(target,layout,subTabs,0,palette);
        if(page.sub==0&&!page.modes.empty())
            SegRow(target,layout,page.modes,subTabs.size(),palette);
        if(!page.chips.empty()) {
            std::vector<PanelViewModel::FilterSeg> chips;
            for(const auto& chip:page.chips) chips.push_back({chip.model,0,false});
            SegRow(target,layout,chips,subTabs.size()+page.modes.size(),palette);
        }
        for(const auto& control:layout.controls) {
            if(control.id!=ControlId::SwitchRowControl||control.index<0
                ||control.index>=static_cast<int>(page.switches.size())) continue;
            const auto& row=page.switches[static_cast<std::size_t>(control.index)];
            SwitchLine(target,control.rect,row.label,std::string{},row.on,false,palette);
        }
        if(layout.primaryButton.right>layout.primaryButton.left) {
            // 删除页的主按钮是 danger 皮肤，且名称没匹配上时按禁用画（旧 disabled）。
            const auto variant=page.danger?widgets::ButtonVariant::Danger:widgets::ButtonVariant::Primary;
            const auto label=page.sub==1?"\xE8\xBF\xBD\xE5\x8A\xA0"
                :(page.danger?"\xE5\x88\xA0\xE9\x99\xA4\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86":"\xE4\xBF\x9D\xE5\xAD\x98");
            const auto visual=page.primaryDisabled?widgets::Visual::Disabled
                :widgets::Visual::Normal;
            Button(target,layout.primaryButton,label,page.danger?IconId::Trash2:IconId::None,
                page.primaryDisabled?widgets::ButtonSkinOf(variant,visual,palette)
                    :hoverSkin(ControlId::PrimaryButton,variant,palette),page.primaryDisabled);
        }
        if(layout.secondaryButton.right>layout.secondaryButton.left)
            Button(target,layout.secondaryButton,page.sub==1?
                "\xE4\xBB\x8E\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xE6\x8E\xA2\xE6\xB5\x8B\xE5\xA1\xAB\xE5\x85\x85"
                :"\xE6\x8E\xA2\xE6\xB5\x8B",IconId::Activity,
                hoverSkin(ControlId::ProbeButton,widgets::ButtonVariant::Normal,palette),
                false);
        if(!page.error.empty())
            Text(target,page.error,layout.errorLine,caption_.Get(),palette.err,
                DWRITE_TEXT_ALIGNMENT_LEADING,true);
        else if(!page.summary.empty())
            // 旧 .foot .note 是 flex:1 的 span：宽度不够时换行（不是省略号）。
            TextWrapped(target,page.summary,layout.errorLine,caption_.Get(),palette.faint,
                DWRITE_TEXT_ALIGNMENT_LEADING);
    } else if(backupPage) {
        const auto& page=view.backup;
        SegRow(target,layout,page.filters,0,palette);
        // 列表按自己的 384 高视口裁剪（旧 .list 是 overflow:auto），不会盖到底栏上。
        target->PushAxisAlignedClip(layout.listViewport,D2D1_ANTIALIAS_MODE_ALIASED);
        for(const auto& row:layout.rows) {
            if(row.row<0||row.row>=static_cast<int>(page.rows.size())) continue;
            const auto& entry=page.rows[static_cast<std::size_t>(row.row)];
            const float iconSize=Dip(24,scale_);
            const auto iconBox=D2D1::RectF(row.rect.left+Dip(4,scale_),
                row.rect.top+(row.rect.bottom-row.rect.top-iconSize)/2.0f,
                row.rect.left+Dip(4,scale_)+iconSize,row.rect.top+(row.rect.bottom-row.rect.top+iconSize)/2.0f);
            brush_->SetColor(entry.kind=="env"?palette.warnSoft:palette.raise2);
            target->FillRoundedRectangle(D2D1::RoundedRect(iconBox,6.0f*scale_,6.0f*scale_),brush_.Get());
            const auto glyph=Dip(13,scale_);
            Icon(target,D2D1::RectF((iconBox.left+iconBox.right)/2.0f-glyph/2.0f,
                (iconBox.top+iconBox.bottom)/2.0f-glyph/2.0f,(iconBox.left+iconBox.right)/2.0f+glyph/2.0f,
                (iconBox.top+iconBox.bottom)/2.0f+glyph/2.0f),
                entry.kind=="env"?IconId::Key:IconId::Archive,
                entry.kind=="env"?palette.warn:palette.dim,1.7f);
            const float textLeft=iconBox.right+Dip(9,scale_);
            const float buttonWidth=Dip(58,scale_);
            Text(target,entry.file,D2D1::RectF(textLeft,row.rect.top+Dip(7,scale_),
                row.rect.right-buttonWidth-Dip(9,scale_),row.rect.top+Dip(27,scale_)),mono_.Get(),
                palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            const auto line=entry.stamp+" \xC2\xB7 "+std::to_string(entry.sizeKb)+" KB \xC2\xB7 "+entry.note;
            Text(target,line,D2D1::RectF(textLeft,row.rect.top+Dip(24,scale_),
                row.rect.right-buttonWidth-Dip(9,scale_),row.rect.top+Dip(40,scale_)),caption_.Get(),
                palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            // RectF 的第三/四个参数是 right/bottom：行尾的「回滚」按钮用右边界与下边界算。
            Button(target,D2D1::RectF(row.rect.right-buttonWidth,row.rect.top+Dip(10.5,scale_),
                row.rect.right-Dip(2,scale_),row.rect.top+Dip(31.5,scale_)),
                "\xE5\x9B\x9E\xE6\xBB\x9A",IconId::None,
                widgets::ButtonSkinOf(widgets::ButtonVariant::Ghost,widgets::Visual::Normal,palette),false);
            widgets::DrawHairline(target,D2D1::RectF(row.rect.left,row.rect.bottom-1.0f*scale_,
                row.rect.right,row.rect.bottom),palette,scale_);
        }
        if(page.rows.empty()) {
            target->PopAxisAlignedClip();
            TextWrapped(target,page.busy?page.busyText:page.emptyText,
                D2D1::RectF(layout.cards.left+12.0f*scale_,layout.cards.top,
                    layout.cards.right-12.0f*scale_,layout.cards.top+Dip(69,scale_)),emptyText_.Get(),
                palette.faint);
        } else {
            // 脚注是列表里最后一段 .note（旧 <p class="note"> 是列表的最后一个孩子）：在同一个裁剪里画，
            // 否则它会盖在行上（真机反馈的"穿模"之一）。左对齐（TextWrapped 默认居中）。
            TextWrapped(target,page.footNote,layout.cardFoot,caption_.Get(),palette.faint,
                DWRITE_TEXT_ALIGNMENT_LEADING);
            target->PopAxisAlignedClip();
        }
    } else {
        // 设置页：小节标题在 notes 里，行 = 左标签（+ small 说明）+ 右侧控件
        const auto& page=view.settings;
        for(const auto& field:layout.fields) {
            if(field.index<0||field.index>=static_cast<int>(page.fields.size())) continue;
            const auto& source=page.fields[static_cast<std::size_t>(field.index)];
            Text(target,source.label,D2D1::RectF(field.label.left,field.label.top,field.label.right,
                field.label.bottom),body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            // 说明小字由 layout.notes 画在标签**下面**（旧 .srow small { display:block }）：
            // 这里不再画第二遍（之前画了两遍，一行压住输入框）。
            // 热键框在"按组合键"状态亮起来（真机反馈：点了要有可见效果）：accent 描边 + 柔底。
            // 约定：字段 4 = 热键（视图层的顺序固定，见 panel_view.cpp 的 fields 组装）。
            const bool capturing=field.index==4&&page.hotkeyCapturing;
            auto fieldSkin=widgets::ButtonSkinOf(widgets::ButtonVariant::Normal,widgets::Visual::Normal,palette);
            if(capturing) { fieldSkin.fill=palette.accentSoft;fieldSkin.border=palette.accent; }
            widgets::DrawButtonShape(target,brush_.Get(),field.box,fieldSkin,scale_);
            const bool dropdown=source.kind==3;
            Text(target,source.value,D2D1::RectF(field.box.left+Dip(7,scale_),field.box.top,
                field.box.right-Dip(dropdown?24.0f:7.0f,scale_),field.box.bottom),mono_.Get(),palette.text,
                DWRITE_TEXT_ALIGNMENT_TRAILING,true);
            if(dropdown) {
                const auto glyph=Dip(13,scale_);
                Icon(target,D2D1::RectF(field.box.right-Dip(22,scale_),
                    (field.box.top+field.box.bottom)/2.0f-glyph/2.0f,field.box.right-Dip(9,scale_),
                    (field.box.top+field.box.bottom)/2.0f+glyph/2.0f),IconId::ChevronDown,palette.faint,1.8f);
            }
        }
        for(const auto& control:layout.controls) {
            if(control.id!=ControlId::SwitchRowControl||control.index<0
                ||control.index>=static_cast<int>(page.switches.size())) continue;
            const auto& row=page.switches[static_cast<std::size_t>(control.index)];
            SwitchLine(target,control.rect,row.label,row.note,row.on,false,palette);
        }
        SegRow(target,layout,page.latencySegments,0,palette,true);
        SegRow(target,layout,page.widgetSegments,page.latencySegments.size(),palette,true);
        SegRow(target,layout,page.themes,
            page.latencySegments.size()+page.widgetSegments.size(),palette,true);
        if(layout.profilesButton.right>layout.profilesButton.left)
            Button(target,layout.profilesButton,"管理",IconId::None,
                hoverSkin(ControlId::SetProfiles,widgets::ButtonVariant::Ghost,palette),false);
        // 手动重启网关（真机反馈：自动重启关掉后找不到手动重启的入口）：ghost 按钮，与
        // 「其他」区的打开 WebUI 同一皮肤。
        if(layout.restartGatewayButton.right>layout.restartGatewayButton.left)
            Button(target,layout.restartGatewayButton,"\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3",
                IconId::RefreshCw,hoverSkin(ControlId::SetRestartGateway,
                    widgets::ButtonVariant::Ghost,palette),false);
        if(layout.diagnoseButton.right>layout.diagnoseButton.left)
            Button(target,layout.diagnoseButton,"\xE6\x9F\xA5\xE7\x9C\x8B",IconId::None,
                hoverSkin(ControlId::SetDiagnose,widgets::ButtonVariant::Ghost,palette),
                false);
        if(layout.usageButton.right>layout.usageButton.left)
            Button(target,layout.usageButton,"\xE6\x9F\xA5\xE7\x9C\x8B",IconId::None,
                hoverSkin(ControlId::SetUsage,widgets::ButtonVariant::Ghost,palette),
                false);
        if(layout.secondaryButton.right>layout.secondaryButton.left)
            Button(target,layout.secondaryButton,page.openWebUiLabel,IconId::None,
                hoverSkin(ControlId::OpenWebUiButton,widgets::ButtonVariant::Ghost,palette),
                false);
        // .meta：左对齐、可换行（旧 word-break: break-all），高度按内容给足。
        TextWrapped(target,page.meta,layout.cardFoot,caption_.Get(),palette.faint,
            DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    target->PopAxisAlignedClip();
    if(layout.pageScrollVisible) {
        // line2：浅色主题下 raise2 与内容底几乎同色（真机反馈"滑条看不见"）。
        brush_->SetColor(palette.line2);
        target->FillRoundedRectangle(D2D1::RoundedRect(layout.pageScrollThumb,
            widgets::kScrollBarWidth*scale_/2.0f,widgets::kScrollBarWidth*scale_/2.0f),brush_.Get());
    }
}
void PanelRenderer::Modal(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    if(view.modal.title.empty()) return;
    // .modal: a 62% ink scrim, a 304-wide card (radius 10, shadow), the title, the body and two buttons.
    brush_->SetColor(Fade(D2D1::ColorF(0.02f,0.03f,0.05f),kModalShadeAlpha));
    target->FillRectangle(layout.modalShade,brush_.Get());
    const auto box=layout.modalBox;
    brush_->SetColor(Fade(D2D1::ColorF(0,0,0),0.5f));
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left,box.top+4.0f*scale_,
        box.right,box.bottom+4.0f*scale_),kModalRadius*scale_,kModalRadius*scale_),brush_.Get());
    brush_->SetColor(palette.surf);
    target->FillRoundedRectangle(D2D1::RoundedRect(box,kModalRadius*scale_,kModalRadius*scale_),brush_.Get());
    brush_->SetColor(palette.line);
    target->DrawRoundedRectangle(D2D1::RoundedRect(box,kModalRadius*scale_,kModalRadius*scale_),brush_.Get(),
        1.0f*scale_);
    Text(target,view.modal.title,layout.modalTitle,label_.Get(),palette.text,
        DWRITE_TEXT_ALIGNMENT_LEADING,true);
    // 正文左对齐（旧 .modal-body 是普通段落）：TextWrapped 默认居中，长 URL 会看着像"溢出"。
    // 分点行（modal.facts）：每条一行、行高 17 DIP，从正文盒顶部依次往下（旧版信息卡就是多行 <div>）。
    if(!view.modal.facts.empty()) {
        // 长行可以左右滚动：文字按实测宽度绘制（不省略），超出正文盒的部分由裁剪裁掉。
        target->PushAxisAlignedClip(layout.modalBody,D2D1_ANTIALIAS_MODE_ALIASED);
        float lineTop=layout.modalBody.top;
        const float left=layout.modalBody.left-layout.modalScrollX;
        for(const auto& fact:view.modal.facts) {
            const float width=Measure(caption_.Get(),fact).width+Dip(8.0f,scale_);
            Text(target,fact,D2D1::RectF(left,lineTop,left+std::max(width,Dip(8.0f,scale_)),
                lineTop+Dip(17.0f,scale_)),caption_.Get(),palette.dim,
                DWRITE_TEXT_ALIGNMENT_LEADING,true);
            lineTop+=Dip(17.0f,scale_);
        }
        target->PopAxisAlignedClip();
        if(layout.modalScrollVisible) {
            brush_->SetColor(palette.line);
            target->FillRoundedRectangle(D2D1::RoundedRect(layout.modalHScrollTrack,
                layout.modalHScrollTrack.bottom-layout.modalHScrollTrack.top,
                layout.modalHScrollTrack.bottom-layout.modalHScrollTrack.top),brush_.Get());
            brush_->SetColor(palette.line2);
            target->FillRoundedRectangle(D2D1::RoundedRect(layout.modalHScrollThumb,
                layout.modalHScrollThumb.bottom-layout.modalHScrollThumb.top,
                layout.modalHScrollThumb.bottom-layout.modalHScrollThumb.top),brush_.Get());
        }
    } else {
        TextWrapped(target,view.modal.body,layout.modalBody,caption_.Get(),palette.dim,
            DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    // F5：配置档案行（名称 + 摘要，行尾「应用」「删除」）。
    for(std::size_t index=0;index<view.modal.profiles.size()
            &&index<layout.modalProfileRows.size();++index) {
        const auto& slot=layout.modalProfileRows[index];
        const auto& profile=view.modal.profiles[index];
        const float textRight=slot.apply.left-Dip(6.0f,scale_);
        Text(target,profile.name,D2D1::RectF(slot.row.left,slot.row.top+Dip(1.0f,scale_),
            textRight,slot.row.top+Dip(17.0f,scale_)),body_.Get(),palette.text,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        Text(target,profile.detail,D2D1::RectF(slot.row.left,slot.row.top+Dip(15.0f,scale_),
            textRight,slot.row.bottom),caption_.Get(),palette.faint,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
        Button(target,slot.apply,"应用",IconId::None,
            widgets::ButtonSkinOf(widgets::ButtonVariant::Ghost,widgets::Visual::Normal,palette),false);
        Button(target,slot.remove,"删除",IconId::None,
            widgets::ButtonSkinOf(widgets::ButtonVariant::Ghost,widgets::Visual::Normal,palette),false);
    }
    Button(target,layout.modalCancel,view.modal.cancelLabel,IconId::None,
        widgets::ButtonSkinOf(widgets::ButtonVariant::Ghost,
            widgets::Visual::Normal,palette),false);
    Button(target,layout.modalConfirm,view.modal.confirmLabel,IconId::None,
        widgets::ButtonSkinOf(view.modal.danger?widgets::ButtonVariant::Danger:widgets::ButtonVariant::Primary,
            widgets::Visual::Normal,palette),false);
}
void PanelRenderer::Toast(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    if(view.toast.text.empty()) return;
    // .toast: radius 9, 8px 10px padding, 11.5px text; ok/err tint the border and text.
    // 真机反馈：原样式（raise2 底）和下面的列表混在一起看不出来 —— 加阴影浮起、底色换 surf
    // （浅色主题白底/深色主题比页面亮一档）、边框 line2，任何主题下都与下层分开。
    const auto box=layout.toast;
    const auto radius=widgets::kRadius*scale_;
    brush_->SetColor(Fade(D2D1::ColorF(0,0,0),0.5f));
    target->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left,box.top+Dip(3.0f,scale_),
        box.right,box.bottom+Dip(3.0f,scale_)),radius,radius),brush_.Get());
    brush_->SetColor(palette.surf);
    target->FillRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush_.Get());
    const auto accent=view.toast.kind==1?palette.ok:(view.toast.kind==2?palette.err:palette.line2);
    brush_->SetColor(view.toast.kind==0?palette.line2:Fade(accent,view.toast.kind==2?0.65f:0.6f));
    target->DrawRoundedRectangle(D2D1::RoundedRect(box,radius,radius),brush_.Get(),1.0f*scale_);
    // 长文案两行完整显示（高度由布局按行数给足），不再省略号截断。
    TextWrapped(target,view.toast.text,D2D1::RectF(box.left+Dip(10.0f,scale_),
        box.top+Dip(8.0f,scale_),box.right-Dip(10.0f,scale_),box.bottom),body_.Get(),
        view.toast.kind==2?palette.err:palette.text,DWRITE_TEXT_ALIGNMENT_LEADING);
}
void PanelRenderer::Nav(ID2D1RenderTarget* target,const PanelLayout& layout,const PanelViewModel& view,
    const ThemePalette& palette) {
    static const IconId icons[5]={IconId::Layers,IconId::GitBranch,IconId::Plus,IconId::Archive,IconId::Settings2};
    const ControlId ids[5]={ControlId::TabModels,ControlId::TabFallback,ControlId::TabAdd,
        ControlId::TabBackup,ControlId::TabSettings};
    for(std::size_t index=0;index<layout.navItems.size()&&index<5;++index) {
        const auto& item=layout.navItems[index];
        const bool active=index<view.nav.size()&&view.nav[index].active;
        const bool hovered=hovered_(ids[index]);
        if(active) {
            // .tab.active::before: a 34x23 rounded pill behind the icon, not a full-width fill.
            const float pillWidth=widgets::NavPillWidth()*scale_;
            const float pillHeight=widgets::NavPillHeight()*scale_;
            const float centreX=(item.box.left+item.box.right)/2.0f;
            const auto pill=D2D1::RectF(centreX-pillWidth/2.0f,layout.nav.top+widgets::NavPillTop()*scale_,
                centreX+pillWidth/2.0f,layout.nav.top+widgets::NavPillTop()*scale_+pillHeight);
            brush_->SetColor(palette.accentSoft);
            target->FillRoundedRectangle(D2D1::RoundedRect(pill,widgets::NavPillRadius()*scale_,
                widgets::NavPillRadius()*scale_),brush_.Get());
        } else if(hovered) {
            brush_->SetColor(palette.raise);
            target->FillRectangle(item.box,brush_.Get());
        }
        const auto colour=active?palette.accentStrong:(hovered?palette.dim:palette.faint);
        Icon(target,item.icon,icons[index],colour,1.7f);
        if(index<view.nav.size()) {
            Text(target,view.nav[index].label,item.label,tiny_.Get(),colour,DWRITE_TEXT_ALIGNMENT_CENTER,false);
            if(view.nav[index].badge>0) {
                // .tab b: a small mono count pill right of the icon.
                const auto box=item.badge;
                brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(box,6.0f*scale_,6.0f*scale_),brush_.Get());
                Text(target,std::to_string(view.nav[index].badge),box,navBadge_.Get(),palette.dim,
                    DWRITE_TEXT_ALIGNMENT_CENTER,true);
            }
        }
    }
}
void PanelRenderer::Draw(ID2D1RenderTarget* target,IDWriteFactory* text,const PanelViewModel& view,
    D2D1_SIZE_F size,float scale,const ThemePalette& palette,bool strip,const TextField* field) {
    EnsureResources(target,text,scale);
    factoryText_=text;
    hoverControl_=view.hoverControl;
    hoverPrevious_=view.motion.hoverPrevious;
    hoverAmount_=view.motion.hoverAmount;
    if(strip) {
        // 收起瞬间贴条淡入：只包住挂件绘制；alpha=1 时不建层，无额外开销。
        const float stripAlpha=view.motion.stripAlpha;
        struct StripGuard {
            ID2D1RenderTarget* target;bool open;
            ~StripGuard() { if(open) target->PopLayer(); }
        } stripGuard{target,stripAlpha<0.999f};
        if(stripGuard.open) target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),stripAlpha),nullptr);
        Widget(target,size,scale,view,palette);
        return;
    }
    // 面板内容入场（移植自余额面板）：整体淡入 + 从下往上滑 8 DIP；alpha=1/offset=0 时无开销。
    const bool motioned=view.motion.panelAlpha<0.999f||std::abs(view.motion.panelOffsetDip)>0.01f;
    struct MotionGuard {
        ID2D1RenderTarget* target;bool open;
        ~MotionGuard() { if(open) { target->SetTransform(D2D1::Matrix3x2F::Identity());target->PopLayer(); } }
    } motion{target,false};
    if(motioned) {
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),view.motion.panelAlpha),nullptr);
        if(std::abs(view.motion.panelOffsetDip)>0.01f)
            target->SetTransform(D2D1::Matrix3x2F::Translation(0,Dip(view.motion.panelOffsetDip,scale)));
        motion.open=true;
    }
    brush_->SetColor(palette.ink);
    target->FillRectangle(D2D1::RectF(0,0,size.width,size.height),brush_.Get());
    const auto layout=ComputeLayout(view,size.width,size.height,scale);
    // ---- 标题栏 ----
    brush_->SetColor(palette.surf);
    target->FillRectangle(layout.titleBar,brush_.Get());
    brush_->SetColor(palette.line2);
    target->FillRectangle(layout.titleDivider,brush_.Get());
    {
        // .titlebar .mark: an 18x18 accent-soft rounded square holding the 12px activity glyph.
        brush_->SetColor(palette.accentSoft);
        target->FillRoundedRectangle(D2D1::RoundedRect(layout.brandMark,5.0f*scale_,5.0f*scale_),brush_.Get());
        const auto markBox=12.0f*scale_;
        Icon(target,D2D1::RectF((layout.brandMark.left+layout.brandMark.right)/2.0f-markBox/2.0f,
            (layout.brandMark.top+layout.brandMark.bottom)/2.0f-markBox/2.0f,
            (layout.brandMark.left+layout.brandMark.right)/2.0f+markBox/2.0f,
            (layout.brandMark.top+layout.brandMark.bottom)/2.0f+markBox/2.0f),
            IconId::Activity,palette.accentStrong,1.7f);
    }
    Text(target,view.title,layout.brandText,brand_.Get(),palette.dim,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    {
        // .icb title buttons: 24x24 hit boxes, the glyph at 14px, hover raise2; close hovers onto the
        // error colour, the pinned pin sits on the accent.
        const auto titleIcon=[&](ControlId id,IconId icon) {
            const auto* rect=ControlRectFor(layout,id);
            if(!rect) return;
            const bool hovered=hovered_(id);
            if(hovered) {
                if(id==ControlId::Close) brush_->SetColor(palette.err);
                else brush_->SetColor(palette.raise2);
                target->FillRoundedRectangle(D2D1::RoundedRect(*rect,widgets::kIcbRadius*scale_,
                    widgets::kIcbRadius*scale_),brush_.Get());
            }
            D2D1_COLOR_F colour=hovered?palette.text:palette.faint;
            if(id==ControlId::Pin&&view.pinned) colour=palette.accentStrong;
            if(id==ControlId::Close&&hovered) colour=D2D1::ColorF(1.0f,1.0f,1.0f,1.0f);
            const auto glyphBox=14.0f*scale_;
            Icon(target,D2D1::RectF((rect->left+rect->right)/2.0f-glyphBox/2.0f,
                (rect->top+rect->bottom)/2.0f-glyphBox/2.0f,
                (rect->left+rect->right)/2.0f+glyphBox/2.0f,
                (rect->top+rect->bottom)/2.0f+glyphBox/2.0f),icon,colour,1.8f);
        };
        titleIcon(ControlId::Close,IconId::X);
        titleIcon(ControlId::Minimize,IconId::Minus);
        titleIcon(ControlId::Snap,IconId::Minimize2);
        titleIcon(ControlId::Pin,IconId::Pin);
    }
    // ---- 状态寄存器 ----
    brush_->SetColor(palette.surf);
    target->FillRectangle(layout.lampRow,brush_.Get());
    brush_->SetColor(palette.line2);
    target->FillRectangle(layout.lampsDivider,brush_.Get());
    for(std::size_t index=0;index<layout.lamps.size()&&index<view.lamps.size();++index) {
        Lamp(target,layout.lamps[index],view.lamps[index],palette);
    }
    // 换模型后（自动重启关）的「重启网关」胶囊：网关灯后、待重启状态的可见动作入口——
    // 点一下重启网关，机器人立即用上新模型（点完消失）。
    if(view.pendingGatewayRestart&&layout.registerRestartButton.right>layout.registerRestartButton.left) {
        const auto pill=layout.registerRestartButton;
        const auto skin=hoverSkin(ControlId::RegisterRestartGateway,
            widgets::ButtonVariant::Normal,palette);
        widgets::DrawButtonShape(target,brush_.Get(),pill,skin,scale_);
        const float padX=Dip(7.0f,scale_);
        const auto iconBox=Dip(11.0f,scale_);
        Icon(target,D2D1::RectF(pill.left+padX,(pill.top+pill.bottom-iconBox)/2.0f,
            pill.left+padX+iconBox,(pill.top+pill.bottom+iconBox)/2.0f),
            IconId::RefreshCw,palette.accentStrong,1.6f);
        Text(target,"\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3",
            D2D1::RectF(pill.left+padX+iconBox+Dip(5,scale_),pill.top,
                pill.right-Dip(7.0f,scale_),pill.bottom),caption_.Get(),palette.accentStrong,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    if(view.busy) {
        // .busy: the 2px indeterminate sweep bar along the top of the page（旧 body.busy .app::before
        // 是流动动画；之前被画成静态半根，真机反馈"只有一根没有变化"）。现在复刻流动：
        // 一段 55% 宽的渐变块在轨道上循环滑动（sin 驱动，无缝循环），尾迹渐隐。
        const float trackLeft=layout.content.left;
        const float trackWidth=layout.content.right-layout.content.left;
        const float trackTop=layout.lampRow.top;
        const float trackBottom=trackTop+widgets::kBannerHeight*scale_;
        const float phase=view.busySweep*3.14159265f/180.0f;      // busySweep：0..720 循环（两拍）
        const float cycle=std::sin(phase);                         // -1..1 → 位置 0..1
        const float position=(cycle+1.0f)*0.5f;
        const float block=trackWidth*0.5f;
        const float head=trackLeft+position*(trackWidth-block);
        const float tail=head+block;
        // 三段渐变：透明 → accent → 透明（D2D 线性渐变笔刷按轨道坐标定位）。
        D2D1_GRADIENT_STOP stops[3];
        stops[0].position=0.0f; stops[0].color=Fade(palette.accent,0.0f);
        stops[1].position=0.5f; stops[1].color=palette.accent;
        stops[2].position=1.0f; stops[2].color=Fade(palette.accent,0.0f);
        Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
        Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> sweep;
        if(SUCCEEDED(target->CreateGradientStopCollection(stops,3,D2D1_GAMMA_2_2,
            D2D1_EXTEND_MODE_CLAMP,&collection))) {
            const D2D1_POINT_2F start{head,trackTop},finish{tail,trackTop};
            if(FAILED(target->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(start,finish),collection.Get(),&sweep))) {
                sweep.Reset();
            }
        }
        // 兜底：渐变笔刷创建失败时至少画实心段（不回到"永远半根"的旧观感）。
        if(sweep) {
            target->FillRectangle(D2D1::RectF(head,trackTop,tail,trackBottom),sweep.Get());
        } else {
            brush_->SetColor(palette.accent);
            target->FillRectangle(D2D1::RectF(head,trackTop,tail,trackBottom),brush_.Get());
        }
    }
    Chip(target,layout.chipHelper,view.chipHelper,hovered_(ControlId::ChipHelper),palette);
    Chip(target,layout.chip,view.chip,hovered_(ControlId::Chip),palette);
    Chip(target,layout.chipWarn,view.chipWarn,hovered_(ControlId::ChipWarn),palette);
    {
        const auto* sync=ControlRectFor(layout,ControlId::Refresh);
        if(sync) {
            if(hovered_(ControlId::Refresh)) {
                brush_->SetColor(palette.raise);
                target->FillRoundedRectangle(D2D1::RoundedRect(*sync,6.0f*scale_,6.0f*scale_),brush_.Get());
            }
            const auto width=Measure(sync_.Get(),view.syncTime).width;
            const auto iconBox=11.0f*scale_;
            const auto start=sync->right-4.0f*scale_-width-iconBox-5.0f*scale_;
            const D2D1_RECT_F iconRect(start,sync->top+(sync->bottom-sync->top-iconBox)/2,start+iconBox,
                sync->top+(sync->bottom-sync->top+iconBox)/2);
            if(view.busy) {
                // 刷新进行中：图标绕**自己的中心**旋转（真机反馈：不要「读取 WSL 配置…」的文字，
                // 改成图标转）。
                const auto centre=D2D1::Point2F((iconRect.left+iconRect.right)/2.0f,
                    (iconRect.top+iconRect.bottom)/2.0f);
                const auto radians=view.syncSpinAngle*3.14159265f/180.0f;
                target->SetTransform(D2D1::Matrix3x2F::Rotation(radians,centre));
                Icon(target,iconRect,IconId::RefreshCw,palette.faint,1.7f);
                target->SetTransform(D2D1::Matrix3x2F::Identity());
            } else if(view.syncCheck) {
                // 刷新成功：对勾短暂显示后恢复（绿色，与 ok 语义一致）。
                Icon(target,iconRect,IconId::Check,palette.ok,1.7f);
            } else {
                Icon(target,iconRect,IconId::RefreshCw,palette.faint,1.7f);
            }
            Text(target,view.syncTime,D2D1::RectF(start+iconBox+5.0f*scale_,sync->top,sync->right,sync->bottom),
                sync_.Get(),palette.faint,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        }
    }
    // ---- 当前模型 + 内容区 + 导航 ----
    if(view.activePage==0&&view.offlineReason.empty()) Hero(target,layout,view,palette);
    // The list hover comes from the same hit test a click uses, so the row the pointer is over is the
    // row that would react (and the star only lights up when it is actually the target).
    hoverItem_=view.pointerInside?ListHitTest(layout,view.pointerX,view.pointerY):ListHit{};
    hoverKnob_=0;
    hoverSegment_=-1;
    if(view.pointerInside) {
        for(std::size_t index=0;index<layout.segments.size();++index) {
            const auto& box=layout.segments[index].rect;
            if(view.pointerX>=box.left&&view.pointerX<box.right&&view.pointerY>=box.top
                &&view.pointerY<box.bottom) { hoverSegment_=static_cast<int>(index);break; }
        }
    }
    if(view.pointerInside&&view.activePage==1) {
        for(const auto& row:layout.ladderRows) {
            const auto box=row.rect;
            if(view.pointerY<box.top||view.pointerY>=box.bottom) continue;
            hoverItem_.inList=true;
            hoverItem_.kind=ListItemKind::Row;
            hoverItem_.row=row.row;
            hoverKnob_=LadderButtonAt(box,view.pointerX,scale_);
            break;
        }
    }
    cursorIndex_=view.cursor;
    if(!view.offlineReason.empty()) {
        Offline(target,layout,view,palette);
    } else if(view.activePage==0) {
        SearchRow(target,layout,view,palette,field);
        Filters(target,layout,view,palette);
        List(target,layout,view,palette);
    } else if(view.activePage==1) {
        FallbackPage(target,layout,view,palette);
    } else {
        // 添加 / 备份 / 设置三页：页头 + 可滚动正文（H5b/H5c）。
        AdminPages(target,layout,view,palette);
    }
    // ---- 多状态：合并胶囊 + 状态中心（画在正文之后，保证浮层在内容上方）----
    if(view.statuses.size()>=2&&layout.statusPill.right>layout.statusPill.left) {
        const auto pill=layout.statusPill;
        const auto skin=hoverSkin(ControlId::StatusPill,widgets::ButtonVariant::Normal,palette);
        widgets::DrawButtonShape(target,brush_.Get(),pill,skin,scale_);
        const float padX=Dip(7.0f,scale_);
        const auto iconBox=Dip(12.0f,scale_);
        Icon(target,D2D1::RectF(pill.left+padX,(pill.top+pill.bottom-iconBox)/2.0f,
            pill.left+padX+iconBox,(pill.top+pill.bottom+iconBox)/2.0f),
            IconId::AlertTriangle,palette.accentStrong,1.6f);
        const auto label=std::to_string(view.statuses.size())+" \xE9\xA1\xB9\xE7\x8A\xB6\xE6\x80\x81";
        Text(target,label,D2D1::RectF(pill.left+padX+iconBox+Dip(5,scale_),pill.top,
            pill.right-Dip(7.0f,scale_),pill.bottom),caption_.Get(),palette.accentStrong,
            DWRITE_TEXT_ALIGNMENT_LEADING,true);
    }
    if(layout.statusCard.right>layout.statusCard.left) {
        const auto card=layout.statusCard;
        const auto radius=Dip(9.0f,scale_);
        D2D1_ROUNDED_RECT shadow{D2D1::RectF(card.left,card.top+Dip(2,scale_),card.right,
            card.bottom+Dip(2,scale_)),radius,radius};
        D2D1_ROUNDED_RECT box{card,radius,radius};
        brush_->SetColor(D2D1::ColorF(0,0.32f));
        target->FillRoundedRectangle(shadow,brush_.Get());
        brush_->SetColor(palette.surf);
        target->FillRoundedRectangle(box,brush_.Get());
        brush_->SetColor(palette.line2);
        target->DrawRoundedRectangle(box,brush_.Get(),1.0f);
        for(std::size_t index=0;index<layout.statusRows.size()&&index<view.statuses.size();++index) {
            const auto& row=layout.statusRows[index];
            const auto& item=view.statuses[index];
            Text(target,item.label,D2D1::RectF(row.row.left+Dip(9,scale_),row.row.top,
                row.action.left-Dip(4,scale_),row.row.bottom),body_.Get(),palette.text,
                DWRITE_TEXT_ALIGNMENT_LEADING,true);
            const auto action=(item.action==StatusAction::RestartGateway
                ||item.action==StatusAction::RestartConfig)
                ?"\xE9\x87\x8D\xE5\x90\xAF":"\xE9\x87\x8D\xE8\xAF\x95";
            Text(target,action,D2D1::RectF(row.action.left,row.row.top,row.action.right-Dip(6,scale_),
                row.row.bottom),caption_.Get(),palette.accentStrong,
                DWRITE_TEXT_ALIGNMENT_TRAILING,true);
            if(index+1<view.statuses.size()) {
                brush_->SetColor(palette.line);
                target->FillRectangle(D2D1::RectF(row.row.left,row.row.bottom-Dip(1,scale_),
                    row.row.right,row.row.bottom),brush_.Get());
            }
        }
    }
    // 悬停气泡（旧版的 title）：挂在控件下方，越界翻到上方并夹进窗口；多行按行画。
    if(view.tooltip.visible&&!view.tooltip.text.empty()) {
        // 提示气泡淡入（120ms）：alpha<1 时整体套一层透明度，避免"啪"地出现。
        const bool tipMotion=view.motion.tooltipAlpha<0.999f;
        struct TipGuard {
            ID2D1RenderTarget* target;bool open;
            ~TipGuard() { if(open) target->PopLayer(); }
        } tip{target,false};
        if(tipMotion) {
            target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),
                view.motion.tooltipAlpha),nullptr);
            tip.open=true;
        }
        const auto anchorRect=D2D1::RectF(view.tooltip.left,view.tooltip.top,view.tooltip.right,
            view.tooltip.bottom);
        std::vector<std::string> lines;
        std::string current;
        for(const char character:view.tooltip.text) {
            if(character=='\n') { lines.push_back(current);current.clear();continue; }
            current.push_back(character);
        }
        lines.push_back(current);
        float width=0.0f;
        // Measure 返回的已经是**物理像素**（与布局同一坐标系），不能再乘 scale（乘了气泡会宽 25%，
        // 于是"贴着窗口边界被挤出去"）。
        for(const auto& line:lines) width=std::max(width,Measure(caption_.Get(),line).width+Dip(14,scale_));
        const float height=Dip(10,scale_)*static_cast<float>(lines.size())+Dip(11,scale_);
        // 指针右下 14/18（原生气泡的习惯）；右边/下边放不下就翻到指针的左上，最后夹进窗口。
        float left=anchorRect.left+Dip(14,scale_);
        if(left+width>layout.window.right-Dip(6,scale_)) left=anchorRect.left-Dip(14,scale_)-width;
        left=std::max(Dip(6,scale_),std::min(left,layout.window.right-width-Dip(6,scale_)));
        float top=anchorRect.top+Dip(18,scale_);
        if(top+height>layout.window.bottom-Dip(6,scale_)) top=anchorRect.top-Dip(10,scale_)-height;
        top=std::max(Dip(6,scale_),std::min(top,layout.window.bottom-height-Dip(6,scale_)));
        const auto box=D2D1::RectF(left,top,left+width,top+height);
        brush_->SetColor(palette.surf);
        target->FillRoundedRectangle(D2D1::RoundedRect(box,6.0f*scale_,6.0f*scale_),brush_.Get());
        brush_->SetColor(palette.line);
        target->DrawRoundedRectangle(D2D1::RoundedRect(box,6.0f*scale_,6.0f*scale_),brush_.Get(),1.0f*scale_);
        float lineY=box.top+Dip(5,scale_);
        for(const auto& line:lines) {
            Text(target,line,D2D1::RectF(box.left+Dip(7,scale_),lineY,box.right-Dip(7,scale_),
                lineY+Dip(10,scale_)*1.6f),caption_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
            lineY+=Dip(10,scale_);
        }
        if(tip.open) { target->PopLayer();tip.open=false; }
    }
    // 展开中的下拉（回退链页 / 添加页）：列表浮在页面内容之上，最后画。
    if(!layout.selectOptions.empty()&&!layout.selectLabels.empty()) {
        const auto listBox=layout.selectList;
        brush_->SetColor(palette.surf);
        target->FillRoundedRectangle(D2D1::RoundedRect(listBox,6.0f*scale_,6.0f*scale_),brush_.Get());
        brush_->SetColor(palette.line);
        target->DrawRoundedRectangle(D2D1::RoundedRect(listBox,6.0f*scale_,6.0f*scale_),brush_.Get(),
            1.0f*scale_);
        for(std::size_t index=0;index<layout.selectOptions.size()&&index<layout.selectLabels.size();++index) {
            const auto option=layout.selectOptions[index].rect;
            const bool active=layout.selectScroll+static_cast<int>(index)==layout.selectActive;
            if(active) {
                brush_->SetColor(palette.accentSoft);
                target->FillRectangle(option,brush_.Get());
            }
            Text(target,layout.selectLabels[index],D2D1::RectF(option.left+Dip(9.0f,scale_),option.top,
                option.right-Dip(9.0f,scale_),option.bottom),body_.Get(),
                active?palette.accentStrong:palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        }
        // 条目多于可视行数时的滑块（真机：18 个供应商，后 10 个选不到）。
        if(layout.selectTotal>layout.selectVisible&&layout.selectThumb.right>layout.selectThumb.left) {
            brush_->SetColor(Fade(palette.line2,0.55f));
            target->FillRoundedRectangle(D2D1::RoundedRect(layout.selectBar,
                Dip(1.5f,scale_),Dip(1.5f,scale_)),brush_.Get());
            brush_->SetColor(Fade(palette.text,0.55f));
            target->FillRoundedRectangle(D2D1::RoundedRect(layout.selectThumb,
                Dip(1.5f,scale_),Dip(1.5f,scale_)),brush_.Get());
        }
    }
    // 键盘焦点环（审计 F-010）：画在页面之上、模态之下；命中与绘制共用同一份控件矩形。
    if(view.focusControl!=0) {
        for(const auto& control:layout.controls) {
            if(static_cast<int>(control.id)!=view.focusControl) continue;
            if(view.focusIndex>=0&&control.index!=view.focusIndex) continue;
            brush_->SetColor(palette.accent);
            const auto inset=Dip(1.0f,scale_);
            target->DrawRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(control.rect.left-inset,control.rect.top-inset,
                    control.rect.right+inset,control.rect.bottom+inset),
                widgets::kRadius*scale_,widgets::kRadius*scale_),brush_.Get(),1.5f*scale_);
            break;
        }
    }
    Modal(target,layout,view,palette);
    Toast(target,layout,view,palette);
    brush_->SetColor(palette.surf);
    target->FillRectangle(layout.nav,brush_.Get());
    brush_->SetColor(palette.line2);
    target->FillRectangle(layout.navDivider,brush_.Get());
    Nav(target,layout,view,palette);
    // 窗口描边（1px line2）：无边框 WS_POPUP 全靠底色区分边界，浅色桌面上 porcelain 的左缘
    // 会和后面窗口的浅色输入框混在一起——真机反馈"左边的按钮被页边距切割了一点点"就是后面
    // 窗口的白色输入框贴着面板左缘造成的错觉。一圈细描边让面板边界在任何背景下都清晰。
    brush_->SetColor(palette.line2);
    target->DrawRectangle(D2D1::RectF(0.5f*scale_,0.5f*scale_,
        static_cast<float>(layout.window.right)-0.5f*scale_,
        static_cast<float>(layout.window.bottom)-0.5f*scale_),brush_.Get(),1.0f*scale_);
}
void PanelRenderer::DrawHoverCard(ID2D1RenderTarget* target,IDWriteFactory* text,
    const HoverCardView& card,D2D1_SIZE_F size,float scale,const ThemePalette& palette,
    float cardAlpha,float cardOffsetDip) {
    EnsureResources(target,text,scale);
    const auto dip=[&](float value) { return value*scale; };
    // 入场：内容从下方 4 DIP 滑上并淡入（只作用于绘制；窗口位置不动）。移植自 balance-panel-native。
    const bool cardMotion=cardAlpha<0.999f||std::abs(cardOffsetDip)>0.01f;
    struct CardGuard {
        ID2D1RenderTarget* target;bool open;
        ~CardGuard() { if(open) { target->SetTransform(D2D1::Matrix3x2F::Identity());target->PopLayer(); } }
    } cardGuard{target,false};
    if(cardMotion) {
        target->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::IdentityMatrix(),cardAlpha),nullptr);
        if(std::abs(cardOffsetDip)>0.01f)
            target->SetTransform(D2D1::Matrix3x2F::Translation(0,dip(cardOffsetDip)));
        cardGuard.open=true;
    }
    const auto cardRect=D2D1::RoundedRect(D2D1::RectF(0,0,size.width,size.height),dip(12),dip(12));
    // 独立窗口底下没有面板的渐变地面：先用 ink 打底，再叠 surf，避免透出桌面影响可读性。
    brush_->SetColor(palette.ink);
    target->FillRoundedRectangle(cardRect,brush_.Get());
    brush_->SetColor(palette.surf);
    target->FillRoundedRectangle(cardRect,brush_.Get());
    brush_->SetColor(palette.line);
    target->DrawRoundedRectangle(cardRect,brush_.Get(),1.0f*scale);
    Text(target,"Hermes 面板",D2D1::RectF(dip(14),dip(6),size.width-dip(74),dip(26)),
        label_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    Text(target,card.sync,D2D1::RectF(size.width-dip(74),dip(6),size.width-dip(14),dip(26)),
        caption_.Get(),palette.dim,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
    brush_->SetColor(palette.line2);
    target->FillRectangle(D2D1::RectF(dip(14),dip(32),size.width-dip(14),dip(33)),brush_.Get());
    Text(target,"当前："+card.model,D2D1::RectF(dip(14),dip(37),size.width-dip(14),dip(58)),
        body_.Get(),palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
    const auto dotColour=[&](int state) {
        switch(state) {
        case 0: return palette.ok;
        case 1: return palette.warn;
        case 2: return palette.err;
        default: return palette.faint;
        }
    };
    float y=dip(62);
    for(const auto& row:card.rows) {
        const auto colour=dotColour(row.state);
        const auto centre=D2D1::Point2F(dip(20),y+dip(9));
        brush_->SetColor(Fade(colour,0.18f));
        target->FillEllipse(D2D1::Ellipse(centre,dip(6.0f),dip(6.0f)),brush_.Get());
        brush_->SetColor(colour);
        target->FillEllipse(D2D1::Ellipse(centre,dip(3.2f),dip(3.2f)),brush_.Get());
        Text(target,row.label,D2D1::RectF(dip(31),y,dip(120),y+dip(18)),body_.Get(),
            palette.text,DWRITE_TEXT_ALIGNMENT_LEADING,true);
        Text(target,row.detail,D2D1::RectF(dip(120),y,size.width-dip(14),y+dip(18)),caption_.Get(),
            palette.dim,DWRITE_TEXT_ALIGNMENT_TRAILING,true);
        y+=dip(20);
    }
    // hint 已按真机反馈移除（"单击展开 · 拖动移动"对熟手是噪声）；note 存在时它顶替这个位置。
    if(!card.note.empty())
        Text(target,card.note,D2D1::RectF(dip(14),y+dip(2),size.width-dip(14),y+dip(20)),
            caption_.Get(),palette.warn,DWRITE_TEXT_ALIGNMENT_LEADING,true);
}
}
