#include "render/layout.h"

#include "render/widgets.h"

#include <algorithm>

#include <cmath>



namespace hermes {

namespace {

// Metrics from the old stylesheet, in DIP at 96 DPI: .titlebar 34 (padding 0 6 0 9, gap 7),

// .reg 27 (padding 0 9, gap 11), .lamp 6px dot + 5px gap, .hero 64 (padding 10 11, gap 10),

// .nav 47, .tab.active::before 34x23 at top 5. Page gutters are 11 (.searchrow/.filters/.list).

constexpr float kTitleBar=34.0f,kTitlePadL=9.0f,kTitlePadR=6.0f,kTitleGap=7.0f;

constexpr float kIcb=24.0f;

constexpr float kLampRow=27.0f,kRegPadX=9.0f,kLampDot=6.0f,kLampGap=5.0f,kRegGap=11.0f;

// 同步章（刷新按钮）宽度 = 图标 11 + 间距 5 + "HH:MM" ≈ 30 + 右侧内边距 4 + 少量留白。
// 以前 92 DIP：内容右对齐，按钮左侧空出约 50 DIP 的空白点击区（真机 2026-10-21 报
// 「logo 左边一大块空白控件」）。宽度只影响命中/悬停底，内容仍右对齐。
constexpr float kSyncWidth=60.0f;

constexpr float kChipHeight=22.0f,kChipWidth=110.0f;

constexpr float kHeroHeight=64.0f,kHeroPadX=11.0f,kHeroGap=10.0f,kHeroLampMargin=2.0f;

constexpr float kPagePadX=11.0f;

// 描边内缩量（见 content 的注释）：控件贴窗口边时不让半像素被裁。

constexpr float kStrokeInset=0.5f;

constexpr float kNavHeight=47.0f;

// .searchrow padding 9 11 6 around a 30px .inp; .filters padding 0 11 8 around 24px .seg

constexpr float kSearchPadTop=9.0f,kSearchPadBottom=6.0f,kSearchHeight=30.0f;

constexpr float kSegHeight=24.0f,kSegGap=6.0f,kSegPadX=8.0f,kSegCountGap=5.0f,kFiltersPadBottom=8.0f;

constexpr float kSegFont=11.5f,kSegCountFont=9.5f,kButtonFont=11.5f;

// 分段宽度是"估算 + 余量"：估算按 CJK 1em / ASCII 0.55em，给 4 DIP 余量避免真机上被截断。

constexpr float kSegSlack=4.0f;

// List rows: .group-head 30, .mrow --row 31, .sect (7+15.2+3), .empty (26+17+26)

constexpr float kGroupHead=30.0f,kSect=25.0f,kEmpty=69.0f,kRow=widgets::kRowHeight;

constexpr float kGroupHeadPadX=11.0f,kGroupHeadGap=7.0f,kCaret=14.0f,kGdot=6.0f,kGcountGap=7.0f,

    kMenuButton=20.0f;

constexpr float kRowRightPad=9.0f,kRowGap=8.0f,kPcol=86.0f,kStar=22.0f;

// ---- H5 页面的度量（styles.css：.ph 9/11/7、.fb-row 8px 竖距 + 20px 序号、.addline 8/11、

//      .fold top 46 padding 11 gap 9、.form gap 9、.bkp-row 42 高、.srow 5px 竖距 ----

// 旧 stylesheet 的页面最小高度：旧窗口高度就是照这些量出来的（渲染层 app:fit）。

constexpr float kListMinHeight=384.0f,kFallbackMinHeight=400.0f,kAddMinHeight=452.0f,

    kOfflineMinHeight=532.0f,kSheetMetaMinHeight=72.0f;

constexpr float kPageHeadTop=9.0f,kPageHeadGap=8.0f,kPageHeadHeight=34.0f,kPageTitleFont=12.5f;

constexpr float kLadderPadTop=2.0f,kLadderRowPad=8.0f,kLadderIndex=20.0f;

constexpr float kAddLineMargin=8.0f,kAddLineBottom=11.0f,kAddLineHeight=34.0f;

constexpr float kFoldTop=46.0f,kFoldPad=11.0f,kFoldGap=9.0f,kFoldX=7.0f;

constexpr float kFieldLabel=18.0f,kFieldInput=30.0f,kFieldGap=4.0f,kFieldRowGap=9.0f;   // 15 会裁掉小字的降部（真机看到小字像被切了底）

constexpr float kModalShadeAlpha=0.62f,kModalPad=12.0f,kModalWidth=304.0f,kModalRadius=10.0f;

// 模态正文的字号（旧 .modal-body 11px）；布局按它估行数，渲染层用 caption_（11px）画。

constexpr float kModalBodyFont=11.0f;

constexpr float kToastBottom=55.0f,kToastPadX=10.0f,kToastPadY=8.0f,kToastGap=8.0f;


// ---- H5b/H5c 三页的度量（styles.css：.form gap 9 / .srow 5px 竖距 / .sect 7/11/3 / .bkp-row 42）----

constexpr float kSubRow=24.0f,kSubRowBottom=8.0f,kFormPadTop=2.0f,kFormRowGap=7.0f;

constexpr float kInputHeight=28.0f,kSectHeight=25.0f,kSwitchHeight=16.0f,kBackupRow=42.0f;

constexpr float kFootHeight=46.0f,kDangerPad=9.0f,kChipPillHeight=21.0f;

constexpr float kScrollBar=6.0f,kScrollBarMinThumb=24.0f;

// A device-free text width estimate: CJK glyphs are ~1em, ASCII ~0.55em. The filter segments are the

// only intrinsic-width control in the chrome and they sit on a left-aligned row, so an estimate keeps

// the layout pure (no DWrite in ComputeLayout) without any visible drift; the renderer still measures

// for real when it centres the count.

float EstimateWidth(const std::string& text,float size) {

    float width=0;

    for(std::size_t index=0;index<text.size();++index) {

        const auto byte=static_cast<unsigned char>(text[index]);

        if(byte<0x80) { width+=size*0.55f;continue; }

        // count one glyph per UTF-8 lead byte

        if(byte>=0xC0) width+=size;

        // skip the continuation bytes of that glyph

        while(index+1<text.size()&&(static_cast<unsigned char>(text[index+1])&0xC0)==0x80) ++index;

    }

    return width;

}

D2D1_RECT_F Box(float x,float y,float width,float height) {

    return D2D1::RectF(x,y,x+width,y+height);

}

// 下拉列表的落点：默认贴在字段下沿，下方装不下就整体翻到字段**上方**。
// 设置页「关闭按钮」就在视口最后一行，向下展开时第二项（退出程序）正好落进底栏，
// 渲染出来被整块盖住、点也点不到（真机实测：下拉只露出第一项）。
float SelectListTop(float fieldBottom,float fieldTop,float listHeight,float limitBottom) {
    if(fieldBottom+listHeight<=limitBottom) return fieldBottom;
    const float flipped=fieldTop-listHeight;
    return flipped>=0.0f?flipped:std::max(0.0f,limitBottom-listHeight);
}

}

PanelLayout ComputeLayout(const PanelViewModel& view,float width,float height,float scale) {

    PanelLayout layout;

    layout.scale=scale;

    const auto dip=[&](float value) { return value*scale; };

    layout.window=Box(0,0,width,height);



    // ---- 标题栏：品牌标记(18, r5) + Hermes + 右侧四个 24px 图标按钮（置顶/贴边/最小化/隐藏）----

    layout.titleBar=Box(0,0,width,dip(kTitleBar));

    layout.titleDivider=Box(0,dip(kTitleBar)-dip(1),width,dip(1));

    layout.brandMark=Box(dip(kTitlePadL),dip((kTitleBar-18)/2),dip(18),dip(18));

    layout.brandText=Box(dip(kTitlePadL+18+kTitleGap),0,width*0.5f,dip(kTitleBar));

    {

        // Buttons sit left->right pin/snap/min/close; pitch is size + the 7px titlebar gap.

        float right=width-dip(kTitlePadR);

        const auto place=[&](ControlId id) {

            layout.controls.push_back({id,Box(right-dip(kIcb),

                dip((kTitleBar-kIcb)/2),dip(kIcb),dip(kIcb))});

            right-=dip(kIcb+kTitleGap);

        };

        place(ControlId::Close);

        place(ControlId::Minimize);

        place(ControlId::Snap);

        place(ControlId::Pin);

    }



    // ---- 状态寄存器：三盏灯 + 忙碌 + chip + 同步时间 ----

    layout.lampRow=Box(0,dip(kTitleBar),width,dip(kLampRow));

    layout.lampsDivider=Box(0,layout.lampRow.bottom-dip(1),width,dip(1));

    {

        // 旧 .reg 是弹性布局：灯按内容宽靠左，中间 .sp 吃掉剩余，忙碌文案/chip/同步钮靠右。

        // 之前这里把剩余宽度平分给灯的标签框（每个拉到 90+ DIP），而忙碌文案右对齐、起排又用了

        // 灯的起点 ——「读取 WSL 配置…」正好压住第三盏灯的文字（真机：点刷新即现）。

        // 共享的 EstimateWidth 是 0.55×字号的混排平均：对全大写偏窄（"WSL"/"WebUI" 先后省略成

        // "W…"/"We…"），对小写偏宽（会把灯行撑到挤掉 chip）—— 这行全是短标签，改用下面的

        // 按字符分类估宽：大写 0.78 / 小写 0.54 / 数字 0.56 / 空格与符号 0.34 / CJK 1.0。

        const auto textWidth=[&](const std::string& text,float size,float safety) {

            float w=0;

            for(std::size_t index=0;index<text.size();++index) {

                const auto byte=static_cast<unsigned char>(text[index]);

                if(byte>=0xC0) {

                    w+=size*safety;

                    while(index+1<text.size()&&(static_cast<unsigned char>(text[index+1])&0xC0)==0x80) ++index;

                } else if(byte>='A'&&byte<='Z') w+=size*0.78f*safety;

                else if(byte>='a'&&byte<='z') w+=size*0.54f*safety;

                else if(byte>='0'&&byte<='9') w+=size*0.56f*safety;

                else w+=size*0.34f*safety;

            }

            return w;

        };

        const float syncLeft=width-dip(kRegPadX+kSyncWidth);

        layout.sync=Box(syncLeft,layout.lampRow.top,dip(kSyncWidth),dip(kLampRow));

        layout.controls.push_back({ControlId::Refresh,layout.sync});

        // 灯按 view.lamps 的实际数量排（离线/空视图可能一个都没有；不要按 max(1,…) 读越界）。

        const ControlId lampIds[3]={ControlId::LampWsl,ControlId::LampWebui,ControlId::LampGateway};

        float x=dip(kRegPadX);

        for(std::size_t index=0;index<view.lamps.size()&&index<3;++index) {

            LampLayout lamp;

            const auto dotSize=dip(kLampDot);

            const auto label=view.lamps[index].label;

            const float labelWidth=std::max(dip(16.0f),textWidth(label,widgets::kLampFont,1.12f)*scale);

            lamp.dot=Box(x,layout.lampRow.top+(dip(kLampRow)-dotSize)/2.0f,dotSize,dotSize);

            lamp.label=Box(x+dotSize+dip(kLampGap),layout.lampRow.top,labelWidth,dip(kLampRow));

            lamp.detail=lamp.label;

            layout.lamps.push_back(lamp);

            // 灯本身没有点击动作，但要能悬停（旧 .lamp 有 title）：登记成控件（dot ∪ label）。

            if(index<3) {

                // 悬停/命中盒覆盖**整行高度**：以前只到 dot.bottom（8px 高的一条），
                // 鼠标落在灯的文字上不弹提示，取矩形中点也会落到灯下面那一行。
                const auto box=D2D1::RectF(lamp.dot.left,layout.lampRow.top,lamp.label.right,
                    layout.lampRow.bottom);

                layout.controls.push_back({lampIds[index],box,static_cast<int>(index)});

            }

            x=lamp.label.right+dip(kRegGap);

        }

        const float lampEnd=x-dip(kRegGap);

        // 状态集中：0 个不占位；1 个保持原来的单胶囊（重启网关紧邻网关灯，单个 chip 右对齐）；
        // >=2 个合并成「N 项状态」胶囊，点开状态中心——这行任何状态下都不重叠。
        const float pillTop=layout.lampRow.top+dip((kLampRow-kChipHeight)/2.0f);
        const float pillGap=dip(kRegGap);
        const bool multiple=view.statuses.size()>=2;
        layout.statusPill=D2D1::RectF();
        layout.statusCard=D2D1::RectF();
        layout.statusRows.clear();
        layout.chipWarn=D2D1::RectF();
        layout.chip=D2D1::RectF();
        layout.chipHelper=D2D1::RectF();
        layout.registerRestartButton=D2D1::RectF();
        if(!view.statuses.empty()) {
            std::string label=view.statuses.front().label;
            if(multiple)
                label=std::to_string(view.statuses.size())+" \xE9\xA1\xB9\xE7\x8A\xB6\xE6\x80\x81";
            const float minLeft=lampEnd+pillGap;
            float pillWidth=std::max(dip(60.0f),
                dip(widgets::kChipPadX*2+widgets::kChipIcon+widgets::kChipGap)
                +textWidth(label,widgets::kChipFont,1.08f)*scale);
            float pillLeft=syncLeft-dip(kRegGap)-pillWidth;
            if(!multiple&&view.statuses.front().action==StatusAction::RestartGateway)
                pillLeft=std::min(minLeft,pillLeft);
            pillLeft=std::max(pillLeft,minLeft);
            pillWidth=std::min(pillWidth,syncLeft-dip(kRegGap)-pillLeft);
            if(pillWidth>dip(40.0f)) {
                layout.statusPill=Box(pillLeft,pillTop,pillWidth,dip(kChipHeight));
                if(multiple) {
                    layout.controls.push_back({ControlId::StatusPill,layout.statusPill});
                } else {
                    const auto action=view.statuses.front().action;
                    const auto id=action==StatusAction::RestartGateway?ControlId::RegisterRestartGateway
                        :(action==StatusAction::RestartConfig?ControlId::ChipWarn
                        :(action==StatusAction::RetryWebUi?ControlId::Chip:ControlId::ChipHelper));
                    layout.controls.push_back({id,layout.statusPill});
                    if(id==ControlId::RegisterRestartGateway) layout.registerRestartButton=layout.statusPill;
                    else if(id==ControlId::ChipWarn) layout.chipWarn=layout.statusPill;
                    else if(id==ControlId::Chip) layout.chip=layout.statusPill;
                    else layout.chipHelper=layout.statusPill;
                }
            }
        }
        // 状态中心浮层：右对齐在寄存器行下方，每行一个状态（整行可点，右侧是动作提示）。
        if(multiple&&view.statusCenterOpen&&layout.statusPill.right>layout.statusPill.left) {
            const float cardWidth=std::min(dip(272.0f),width-dip(kRegPadX*2));
            const float rowHeight=dip(30.0f);
            const float top=layout.lampRow.bottom+dip(4.0f);
            const float left=width-dip(kRegPadX)-cardWidth;
            layout.statusCard=Box(left,top,cardWidth,
                rowHeight*static_cast<float>(view.statuses.size())+dip(10.0f));
            float y=layout.statusCard.top+dip(5.0f);
            for(std::size_t index=0;index<view.statuses.size();++index) {
                PanelLayout::StatusRowBox row;
                row.row=Box(layout.statusCard.left+dip(4.0f),y,cardWidth-dip(8.0f),rowHeight);
                row.action=Box(row.row.right-dip(44.0f),y,dip(44.0f),rowHeight);
                layout.statusRows.push_back(row);
                y+=rowHeight;
            }
        }
        layout.busy=Box(lampEnd,layout.lampRow.top,std::max(dip(0.0f),syncLeft-lampEnd),dip(kLampRow));

    }

    // ---- 当前模型铭牌（hero：通栏条，非卡片；.hero padding 10 11，gap 10）----

    // 旧 index.html 里 .hero 在 #panel-models 内部：只有模型页有它，别的页面从寄存器直接到内容区。

    const bool modelPage=view.activePage==0&&view.offlineReason.empty();

    // hero 只在模型页出现（紧凑视图已删除，窗口高度由用户固定，不再有"装不下"的档位）。
    const bool heroFits=modelPage;
    layout.hero=heroFits?Box(0,dip(kTitleBar+kLampRow),width,dip(kHeroHeight)):D2D1::RectF();

    layout.heroDivider=Box(0,layout.hero.bottom-dip(1),width,dip(1));

    {

        const float lampSize=dip(8);

        layout.heroLamp=heroFits
            ?Box(dip(kHeroPadX+2),layout.hero.top+(dip(kHeroHeight)-lampSize)/2.0f,lampSize,lampSize)
            :D2D1::RectF();

        const float textLeft=layout.heroLamp.right+dip(kHeroGap);

        // 右侧留给 api_mode tag + 徽标（旧 .hero-r 是 flex:0 0 auto：tag 只有 ~46 宽，不要占太多）。

        const float heroRight=dip(72);

        layout.heroProvider=Box(textLeft,layout.hero.top+dip(10),

            layout.hero.right-layout.hero.left-heroRight-(textLeft-layout.hero.left),dip(16));

        const float plateHeight=dip(27);

        layout.heroModel=Box(textLeft,layout.hero.top+dip(29),

            layout.hero.right-layout.hero.left-heroRight-(textLeft-layout.hero.left),plateHeight);

        layout.heroModeTag=Box(layout.hero.right-dip(84),layout.hero.top+dip(8),dip(76),dip(20));

        layout.heroBadge=Box(layout.hero.right-dip(84),layout.hero.top+dip(34),dip(76),dip(20));

    }



    // ---- 内容区与底部导航 ----

    // ---- 窗口高度：标题栏 + 寄存器 + 页面内容 + 导航（旧 app:fit 量出来的就是这些和）----

    const auto pageHeight=[&]() {

        if(!view.offlineReason.empty()) return kOfflineMinHeight;                 // .offline min-height 532

        switch(view.activePage) {

        case 0: return kSearchPadTop+kSearchHeight+kSearchPadBottom               // .searchrow 9+30+6

            +kSegHeight+kFiltersPadBottom                                         // .filters 24+8

            +kListMinHeight+1.0f;                                                 // #model-list 384 + 上边框

        case 1: return kFallbackMinHeight;                                        // #panel-fallback 400

        case 2: return kAddMinHeight;                                             // #panel-add 452

        // .ph 的 9px 顶距 + 34 行高 + 过滤行 24+8 + #bkp-list 384 + 1 上边框：漏掉这 9px 会让列表

        // 比窗口高 9px，行压到底栏上、点底栏反而触发"回滚"（真机踩到过）。

        case 3: return kPageHeadTop+34.0f+kSegHeight+kFiltersPadBottom+kListMinHeight+1.0f;

        default: return 0.0f;                                                     // 设置页：按内容量

        }

    };

    const float chrome=dip(kTitleBar+kLampRow)+(modelPage?dip(kHeroHeight):0.0f);   // 标题栏+寄存器(+hero)

    const float fixedPage=pageHeight();

    layout.desiredHeight=fixedPage>0?chrome+dip(fixedPage)+dip(kNavHeight):0.0f;   // 0 = 按正文实测

    layout.nav=Box(0,height-dip(kNavHeight),width,dip(kNavHeight));

    layout.navDivider=Box(0,layout.nav.top,width,dip(1));

    const float contentTop=modelPage?layout.hero.bottom:dip(kTitleBar+kLampRow);

    // 右边界收进 0.5 DIP：控件描边是以边界为中心画的，贴着窗口边就会被裁掉右半像素

    // （真机反馈"输入框的边框显示不完全"）。左边界同理留 0.5。

    // 内容区高度夹到**非负**：窗口被工作区夹得极矮时（用户把高度拉到下限），
    // 直接相减会得到负数，后续各处 `std::max(96, …)` 之类的兜底又把它撑回 96+，
    // 最终控件被画到窗口外（真机上点不到）。
    const float availableContent=std::max(0.0f,layout.nav.top-contentTop);   // 已是像素，不能再乘 scale
    layout.content=Box(dip(kPagePadX),contentTop,

        width-dip(kPagePadX*2)-dip(kStrokeInset),availableContent);
    const bool tightContent=availableContent<dip(80.0f);

    layout.pageTitle=Box(layout.content.left,layout.content.top,layout.content.right-layout.content.left,dip(22));

    layout.pageNote=Box(layout.content.left,layout.content.top+dip(30),layout.content.right-layout.content.left,dip(40));

    // ---- 搜索行（DESIGN #10，模型页专属）：9px 上边距 + 30px 输入框 + 6px 下边距 ----

    layout.search=Box(layout.content.left,layout.content.top+dip(kSearchPadTop),

        layout.content.right-layout.content.left,dip(kSearchHeight));

    layout.searchIcon=Box(layout.search.left+dip(8),layout.search.top+dip(8),dip(14),dip(14));

    // v1.25：搜索框右端是「✕ + 刷新同步」两个按钮，互不重叠；文字区在它们左边。
    layout.syncModels=Box(layout.search.right-dip(92),layout.search.top+dip(5),dip(84),
        dip(kSearchHeight)-dip(10));

    layout.searchClear=Box(layout.search.right-dip(114),layout.search.top+dip(5),dip(20),dip(20));

    layout.searchHint=Box(layout.search.right-dip(134),layout.search.top+dip(6),dip(14),dip(18));

    layout.searchText=Box(layout.search.left+dip(29),layout.search.top,
        (view.searchHasText?layout.searchClear.left:layout.searchHint.left)
            -dip(3)-layout.search.left-dip(29),
        dip(kSearchHeight));

    if(view.activePage==0) {
        // 命中顺序：✕ → 同步按钮 → 搜索框（搜索框是整行大矩形，必须最后）。
        if(view.searchHasText) layout.controls.push_back({ControlId::SearchClear,layout.searchClear});
        layout.controls.push_back({ControlId::SyncModels,layout.syncModels});
        layout.controls.push_back({ControlId::SearchBox,layout.search});
    }

    // ---- 过滤行（.filters：三个 seg + 右侧 .hint）与列表（.list）----只属于模型页 ----

    if(view.activePage==0) {

        layout.filters=Box(layout.content.left,layout.search.bottom+dip(kSearchPadBottom),

            layout.content.right-layout.content.left,dip(kSegHeight));

        float x=layout.filters.left;

        for(int index=0;index<3;++index) {

            const auto& seg=view.filters[index];

            const float segWidth=dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale

                +dip(kSegCountGap)+EstimateWidth(std::to_string(seg.count),kSegCountFont)*scale;

            const ControlId ids[3]={ControlId::FilterAll,ControlId::FilterFav,ControlId::FilterRecent};

            layout.segments.push_back({ids[index],Box(x,layout.filters.top,segWidth,dip(kSegHeight))});

            layout.controls.push_back(layout.segments.back());

            x+=segWidth+dip(kSegGap);

        }

        layout.filterHint=Box(std::min(x+dip(kSegGap),layout.filters.right-dip(120)),layout.filters.top,

            std::max(0.0f,layout.filters.right-std::min(x+dip(kSegGap),layout.filters.right-dip(120))),

            dip(kSegHeight));

    }

    // 列表是固定 384 高的窗口（旧 .list max-height/min-height 384）：内容多了在内部滚，窗口不跟着长。

    // 但窗口被工作区夹短时（小屏/高 DPI），列表不能压到底部导航上：高度夹到可用空间。

    const float listBoxTop=layout.filters.bottom+dip(kFiltersPadBottom);

    const float listAvailable=tightContent?0.0f
        :std::max(dip(96.0f),layout.nav.top-listBoxTop-dip(1));

    // 模型页列表吃掉固定窗口的剩余高度（窗口可手动拉高）；其它页保持旧的 384 上限，
    // 备份页在自己的分支里再按可用空间夹取。
    const float listHeight=view.activePage==0?listAvailable
        :std::min(dip(kListMinHeight+1.0f),listAvailable);

    // 高度夹进窗口：紧凑档（或窗口被工作区夹短）时 listBoxTop 已经接近底部导航，
    // 直接给 listAvailable 会把列表画到底栏下面。
    const float listBottomLimit=std::max(listBoxTop,layout.nav.top-dip(1));
    // 右侧留白：滑条画在 content.right+2..+8（距窗口右缘 3.5 DIP），已经在卡片右缘之外，
    // 不会压住卡片 —— 所以**不要**为它额外收窄列表。列表盒就用 content 的左右边界，
    // 两者都是 kPagePadX，天然对称。
    // （曾在这里加过 dip(kPagePadX) 的右侧内缩，结果右缘距窗口 22.5 DIP、左边只有 11，
    //   真机立刻看出左右不对称 —— 那次修错了，已撤回。）
    layout.list=Box(layout.filters.left,listBoxTop,

        layout.filters.right-layout.filters.left,
        std::min(listHeight,std::max(0.0f,listBottomLimit-listBoxTop)));

    layout.listViewport=layout.list;

    layout.offlineCard=layout.content;

    {

        const float contentHeight=layout.nav.top-layout.hero.bottom;

        const float blockHeight=dip(34+19+36+30+9*3);

        float y=layout.hero.bottom+std::max(dip(8),(contentHeight-blockHeight)/2.0f);

        const float centreX=layout.hero.right/2.0f;

        layout.offlineIcon=Box(centreX-dip(17),y,dip(34),dip(34)); y+=dip(34+9);

        layout.offlineTitle=Box(layout.content.left-dip(kPagePadX),y,dip(kPagePadX*2)+(layout.content.right-layout.content.left),dip(19));

        y+=dip(19+9);

        layout.offlineBody=Box(centreX-dip(125),y,dip(250),dip(36)); y+=dip(36+9);

        layout.offlineButton=Box(centreX-dip(60),y,dip(120),dip(30));

        if(!view.offlineReason.empty()) layout.controls.push_back({ControlId::OfflineStart,layout.offlineButton});

    }

    // ---- 列表内容：组头（吸顶）、行、分组标题与空态。y 已含滚动偏移 ----

    if(view.activePage==0&&view.offlineReason.empty()) {

        // Pass 1: the item sequence and its height - independent of the scroll position, so the scroll

        // can be clamped before a single rect is placed (what is drawn and what is hit never diverge).

        float contentHeight=0;

        const auto append=[&](ListItemKind kind,int group,int row,float itemHeight,const std::string& text) {

            ListItem item;

            item.kind=kind;item.group=group;item.row=row;item.text=text;

            // 左边必须是列表自己的左边界：以前写 0，宽度又等于列表宽，于是整行从 x=0 开始画、
            // 被裁剪区切掉左边 kPagePadX，右端就短了一截 —— 真机上卡片左边距 11 DIP、右边 22.5 DIP。
            item.rect=Box(layout.list.left,0,layout.list.right-layout.list.left,dip(itemHeight));

            contentHeight+=dip(itemHeight);

            layout.items.push_back(item);

        };

        const auto hasActive=std::any_of(view.groups.begin(),view.groups.end(),

            [](const PanelViewModel::ModelGroup& group) { return group.active; });

        const auto hasOther=std::any_of(view.groups.begin(),view.groups.end(),

            [](const PanelViewModel::ModelGroup& group) { return !group.active; });

        bool sectDone=!hasActive||!hasOther;

        for(int groupIndex=0;groupIndex<static_cast<int>(view.groups.size());++groupIndex) {

            const auto& group=view.groups[static_cast<std::size_t>(groupIndex)];

            const auto groupStart=layout.items.size();

            if(!group.title.empty()) {

                // 「其他供应商」分隔（旧 renderTree：只在既有当前供应商又有其他供应商时出现）

                if(!group.active&&!sectDone) {

                    sectDone=true;

                    append(ListItemKind::Sect,-1,-1,kSect,"\xE5\x85\xB6\xE4\xBB\x96\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86");

                }

                append(ListItemKind::GroupHead,groupIndex,-1,kGroupHead,{});

            }

            if(group.open) {

                if(group.rows.empty()) {

                    append(ListItemKind::Empty,groupIndex,-1,kEmpty,

                        "\xE8\xBF\x99\xE4\xB8\xAA\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86\xE6\xB2\xA1\xE6\x9C\x89\xE5\xA3\xB0\xE6\x98\x8E"

                        "\xE6\xA8\xA1\xE5\x9E\x8B\xEF\xBC\x8C\xE7\x94\xA8 \xE2\x8B\xAF \xE2\x86\x92 \xE6\x8E\xA2\xE6\xB5\x8B\xE6\xA8\xA1"

                        "\xE5\x9E\x8B \xE6\x8B\x89\xE4\xB8\x80\xE6\xAC\xA1");

                } else {

                    for(int rowIndex=0;rowIndex<static_cast<int>(group.rows.size());++rowIndex)

                        append(ListItemKind::Row,groupIndex,rowIndex,kRow,{});

                }

            }

            // 吸顶范围：本组最后一项之后就是下一组（组头/分隔/空态都算在本组里）。

            for(auto index=groupStart;index<layout.items.size();++index)

                layout.items[index].groupEnd=static_cast<int>(layout.items.size());

        }

        if(layout.items.empty()&&!view.listEmpty.empty())

            append(ListItemKind::Empty,-1,-1,kEmpty,view.listEmpty);

        const float viewportHeight=layout.listViewport.bottom-layout.listViewport.top;

        layout.listMaxScroll=std::max(0.0f,contentHeight-viewportHeight);

        layout.listScroll=std::clamp(view.listScroll,0.0f,layout.listMaxScroll);

        // Pass 2: place the rects, then pin the captions. A group head or a section caption is

        // `position: sticky` in the old stylesheet: it holds the viewport top for as long as anything

        // of its own group is still on screen, and the caption arriving from below covers it (paint

        // order runs in item order, which is also why hit testing walks the items backwards).

        float y=layout.listViewport.top-layout.listScroll;

        for(auto& item:layout.items) {

            const float itemHeight=item.rect.bottom-item.rect.top;

            item.rect=Box(item.rect.left,y,item.rect.right-item.rect.left,itemHeight);

            y+=itemHeight;

        }

        for(std::size_t index=0;index<layout.items.size();++index) {

            auto& item=layout.items[index];

            if(item.kind!=ListItemKind::GroupHead&&item.kind!=ListItemKind::Sect) continue;

            if(item.rect.top>=layout.listViewport.top) continue;

            const auto end=item.groupEnd>static_cast<int>(index)

                ?static_cast<std::size_t>(item.groupEnd):index+1;

            const float groupBottom=layout.items[end-1].rect.bottom;

            if(groupBottom<=layout.listViewport.top) continue;

            item.sticky=true;

            const float itemHeight=item.rect.bottom-item.rect.top;

            item.rect=Box(item.rect.left,layout.listViewport.top,item.rect.right-item.rect.left,itemHeight);

        }

        layout.scrollVisible=layout.listMaxScroll>0;

        if(layout.scrollVisible) {

            const float barWidth=dip(kScrollBar);

            layout.scrollTrack=Box(layout.content.right+dip(2.0f),layout.listViewport.top,barWidth,

                viewportHeight);

            const float thumbHeight=std::max(dip(kScrollBarMinThumb),

                viewportHeight*viewportHeight/std::max(1.0f,viewportHeight+layout.listMaxScroll));

            const float travel=viewportHeight-thumbHeight;

            const float ratio=layout.listMaxScroll>0?layout.listScroll/layout.listMaxScroll:0;

            layout.scrollThumb=Box(layout.scrollTrack.left,layout.scrollTrack.top+travel*ratio,barWidth,

                thumbHeight);

        }

    }

    // ---- H5：其余四页的页体。每页只算当前可见的那一页，坐标系与模型页共用 content ----

    layout.ladder=Box(layout.content.left,layout.content.top,

        layout.content.right-layout.content.left,layout.content.bottom-layout.content.top);

    const auto fieldRow=[&](float top,float height) {

        PanelLayout::FieldBox box;

        box.label=Box(layout.content.left,top,layout.content.right-layout.content.left,dip(kFieldLabel));

        box.box=Box(layout.content.left,top+dip(kFieldLabel+kFieldGap),

            layout.content.right-layout.content.left,dip(height));

        return box;

    };

    if(view.activePage==1) {

        // 回退链：页头（标题 + 副题 + 逐条探测）+ 阶梯 + 追加一条

        layout.pageHead=Box(layout.content.left,layout.content.top+dip(kPageHeadTop),

            layout.content.right-layout.content.left,dip(kPageHeadHeight));

        // 旧 .ph 是 flex：.ph-t 自然宽度 + .ph-s 吃掉剩余宽度（固定 62 DIP 会把"每次写入前各留一份"

        // 这类副题挤成省略号）。

        const float headTitleWidth=EstimateWidth(view.fallback.title,kPageTitleFont)*scale;

        layout.pageHeadTitle=Box(layout.pageHead.left,layout.pageHead.top,headTitleWidth,

            layout.pageHead.bottom-layout.pageHead.top);

        layout.pageHeadAction=Box(layout.pageHead.right-dip(104),layout.pageHead.top,dip(104),dip(30));

        const float headTextLeft=layout.pageHead.left+headTitleWidth+dip(kPageHeadGap);

        layout.pageHeadSub=Box(headTextLeft,layout.pageHead.top,

            std::max(dip(40),layout.pageHeadAction.left-dip(8)-headTextLeft),

            layout.pageHead.bottom-layout.pageHead.top);

        layout.controls.push_back({ControlId::FbProbe,layout.pageHeadAction});

        const float addHeight=dip(kAddLineMargin+kAddLineHeight+kAddLineBottom);

        // 阶梯高度夹到非负：紧凑档（或窗口被夹得很矮）时 content 已经很矮，直接相减会是负数，
        // 后续 addLine/折叠控件就被算到窗口外面（audit 报 fbFoldToggle outside）。
        const float ladderTop=layout.pageHead.bottom+dip(7);
        const float ladderHeight=std::max(0.0f,
            layout.content.bottom-layout.pageHead.bottom-dip(7)-addHeight);
        layout.ladder=Box(layout.content.left,ladderTop,

            layout.content.right-layout.content.left,ladderHeight);

        layout.addLine=Box(layout.content.left,layout.ladder.bottom+dip(kAddLineMargin),

            layout.content.right-layout.content.left,dip(kAddLineHeight));

        // 连「追加一条」都放不下时（紧凑档）不注册控件：不然它落在窗口外，点不到却仍占命中表。
        const bool addLineVisible=layout.addLine.bottom<=layout.content.bottom+0.5f;
        if(addLineVisible) layout.controls.push_back({ControlId::FbFoldToggle,layout.addLine});
        else layout.addLine=D2D1::RectF();

        // 阶梯行：8px 竖距 + 20px 序号 + 两行文字（== 50 高），按钮在行尾。
        // 条目多于视口时在阶梯内部滚动（旧版 #fb-list 的 overflow-y:auto），否则第 9 条以后点不到。

        const float ladderContent=dip(kLadderPadTop)
            +static_cast<float>(view.fallback.rows.size())*dip(50)+dip(6);

        layout.fbMaxScroll=std::max(0.0f,ladderContent-(layout.ladder.bottom-layout.ladder.top));

        layout.fbScroll=std::clamp(view.fallbackScroll,0.0f,layout.fbMaxScroll);

        if(layout.fbMaxScroll>0.5f) {

            const float barWidth=dip(kScrollBar);

            layout.fbScrollTrack=Box(layout.ladder.right-barWidth-dip(1),
                layout.ladder.top,barWidth,layout.ladder.bottom-layout.ladder.top);

            const float viewportHeight=layout.ladder.bottom-layout.ladder.top;

            const float thumbHeight=std::max(dip(kScrollBarMinThumb),
                viewportHeight*viewportHeight/std::max(1.0f,viewportHeight+layout.fbMaxScroll));

            const float travel=viewportHeight-thumbHeight;

            const float ratio=layout.fbMaxScroll>0?layout.fbScroll/layout.fbMaxScroll:0.0f;

            layout.fbScrollThumb=Box(layout.fbScrollTrack.left,
                layout.fbScrollTrack.top+travel*ratio,barWidth,thumbHeight);

        }

        float rowY=layout.ladder.top+dip(kLadderPadTop)-layout.fbScroll;

        for(std::size_t index=0;index<view.fallback.rows.size();++index) {

            ListItem item;

            item.kind=ListItemKind::Row;

            item.row=static_cast<int>(index);

            item.rect=Box(layout.ladder.left,rowY,layout.ladder.right-layout.ladder.left,dip(50));

            layout.ladderRows.push_back(item);

            // 行尾三个动作 + 探测结果各自成为控件：悬停说明与命中测试同一份矩形。
            for(int which=1;which<=3;++which) {
                const auto id=which==1?ControlId::FbUp:(which==2?ControlId::FbDown:ControlId::FbRemove);
                layout.controls.push_back({id,LadderButtonBox(item.rect,which,scale),
                    static_cast<int>(index)});
            }
            {
                const float probeRight=LadderButtonBox(item.rect,1,scale).left-dip(9);
                layout.controls.push_back({ControlId::FbProbeDot,
                    Box(probeRight-dip(48),item.rect.top+dip(8),dip(48),dip(18)),
                    static_cast<int>(index)});
            }

            rowY+=dip(50);

        }

        if(view.fallback.formOpen) {
            const auto foldControlStart=layout.controls.size();

            // .fold：浮在阶梯上面（不把页面撑高），top 46、左右 11、padding 11。

            // 旧版 .fold 的 left/right:11px 相对 #panel-fallback（无水平 padding），与 .ladder 的

            // 11px 内边距、.addline 的 11px margin 同一条线 —— 卡片恰好盖住阶梯行（含行尾删除钮）。

            // content 已经是那条 11px 线，所以卡片直接取 content 宽；再内缩一层就会比阶梯窄，

            // 阶梯的红色删除钮会从卡片右缘外探出来（真机见过）。

            // 先定卡片盒子，再算工作区 —— 卡里所有东西都以工作区为基准排，不许混用整页坐标系

            //（真机反馈"追加一条乱七八糟、不对齐、超出"就是混用造成的）。

            layout.fold=Box(layout.content.left,layout.content.top+dip(kFoldTop),

                layout.content.right-layout.content.left,dip(0));

            const float cardLeft=layout.fold.left+dip(kFoldPad);

            const float cardRight=layout.fold.right-dip(kFoldPad);

            const float cardWidth=cardRight-cardLeft;

            float y=layout.fold.top+dip(kFoldPad);

            float foldHeight=dip(kFoldPad)*2;

            const auto cardField=[&](int fieldKind,float fieldHeight,float left,float width,int index) {

                PanelLayout::FieldBox field;

                field.kind=fieldKind;

                field.index=index;

                field.label=Box(left,y,width,dip(kFieldLabel));

                field.box=Box(left,y+dip(kFieldLabel+kFieldGap),width,dip(fieldHeight));

                layout.fields.push_back(field);

                layout.controls.push_back({ControlId::FormField,field.box,index});

                return field;

            };

            const auto advance=[&](float fieldHeight) {

                const float step=dip(kFieldLabel+kFieldGap+fieldHeight+kFieldRowGap);

                y+=step;

                foldHeight+=step;

            };

            // 0 来源供应商（下拉）、1 模型 id：整卡宽

            cardField(3,30,cardLeft,cardWidth,0);

            advance(30);

            cardField(3,30,cardLeft,cardWidth,1);

            // v1.25（用户要求）：模型只能选、不能手打——整块字段都算下拉命中区。
            {
                const auto fieldBox=layout.fields[1].box;
                layout.controls.insert(layout.controls.begin()+static_cast<std::ptrdiff_t>(foldControlStart),
                    {ControlId::FbModelSelect,Box(fieldBox.left,fieldBox.top,
                        fieldBox.right-fieldBox.left,fieldBox.bottom-fieldBox.top)});
            }

            advance(30);

            // 下拉框本身也是一个控件（点开/收起）

            layout.controls.push_back({ControlId::FbSelect,layout.fields[0].box});

            layout.primaryButton=Box(cardLeft,y,cardWidth,dip(30));

            layout.controls.push_back({ControlId::FbSubmit,layout.primaryButton});

            y+=dip(30+kFoldGap);

            foldHeight+=dip(30+kFoldGap);

            layout.errorLine=Box(cardLeft,y,cardWidth,dip(16));

            foldHeight+=dip(16+kFoldPad);

            layout.fold=Box(layout.fold.left,layout.fold.top,layout.fold.right-layout.fold.left,

                std::max(dip(60),foldHeight));

            layout.foldClose=Box(layout.fold.right-dip(kFoldX+24),layout.fold.top+dip(kFoldX),dip(24),dip(24));

            layout.controls.push_back({ControlId::FbFoldClose,layout.foldClose});
            // v1.25：折叠卡浮在阶梯之上——它的控件整体挪到控件表最前，让 ControlAt 先命中卡片。
            // 否则点卡片的 × / 保存 会被下边阶梯行的三个按钮抢走（真机反馈"收起按钮穿透"）。
            // v1.25：折叠卡浮在阶梯之上——它的控件整体挪到控件表最前，让 ControlAt 先命中卡片。
            if(foldControlStart<layout.controls.size())
                std::rotate(layout.controls.begin(),layout.controls.begin()+foldControlStart,
                    layout.controls.end());

            if(view.fallback.selectOpen) {
                // 下拉展开：贴在「来源供应商」字段下面，宽度与字段一致；可视 6 行，超出的滚动。
                constexpr std::size_t kRows=6;
                const auto total=view.fallback.sources.size();
                const int maxFirst=static_cast<int>(total>kRows?total-kRows:0);
                const int first=std::clamp(view.fallback.sourceScroll,0,maxFirst);
                const std::size_t count=std::min<std::size_t>(kRows,total-static_cast<std::size_t>(first));
                const float optionListHeight=dip(30.0f*static_cast<float>(count)+8);
                const float optionListTop=SelectListTop(layout.fields[0].box.bottom,
                    layout.fields[0].box.top,optionListHeight,layout.content.bottom);
                layout.selectList=Box(layout.fields[0].box.left,optionListTop,
                    layout.fields[0].box.right-layout.fields[0].box.left,optionListHeight);
                layout.selectLabels.clear();
                for(std::size_t i=0;i<count;++i)
                    layout.selectLabels.push_back(view.fallback.sources[static_cast<std::size_t>(first)+i]);
                layout.selectActive=view.fallback.source;
                layout.selectScroll=first;
                layout.selectTotal=static_cast<int>(total);
                layout.selectVisible=static_cast<int>(count);
                float optionY=layout.selectList.top+dip(4);
                for(std::size_t i=0;i<count;++i) {
                    ControlRect option;
                    option.id=ControlId::SelectOption;
                    option.index=first+static_cast<int>(i);
                    option.rect=Box(layout.selectList.left,optionY,
                        layout.selectList.right-layout.selectList.left,dip(30));
                    layout.selectOptions.push_back(option);
                    optionY+=dip(30);
                }
                if(total>count) {
                    const float trackTop=layout.selectList.top+dip(4);
                    const float trackHeight=dip(30.0f*static_cast<float>(count));
                    const float thumbHeight=trackHeight*static_cast<float>(count)/static_cast<float>(total);
                    const float thumbTop=trackTop+trackHeight*static_cast<float>(first)/static_cast<float>(total);
                    layout.selectBar=Box(layout.selectList.right-dip(4),trackTop,dip(3),trackHeight);
                    layout.selectThumb=Box(layout.selectBar.left,thumbTop,dip(3),thumbHeight);
                }
            }
            if(view.fallback.modelSelectOpen&&!view.fallback.modelOptions.empty()) {
                // v1.16：模型 ▾ 贴在「模型 id」下面，规则与来源供应商下拉一致（可视 6 行 + 滚动）。
                // 与来源列表互斥由应用层保证，两者不会同时写这一组布局槽位。
                constexpr std::size_t kRows=6;
                const auto total=view.fallback.modelOptions.size();
                const int maxFirst=static_cast<int>(total>kRows?total-kRows:0);
                const int first=std::clamp(view.fallback.sourceScroll,0,maxFirst);
                const std::size_t count=std::min<std::size_t>(kRows,total-static_cast<std::size_t>(first));
                const float optionListHeight=dip(30.0f*static_cast<float>(count)+8);
                const float optionListTop=SelectListTop(layout.fields[1].box.bottom,
                    layout.fields[1].box.top,optionListHeight,layout.content.bottom);
                layout.selectList=Box(layout.fields[1].box.left,optionListTop,
                    layout.fields[1].box.right-layout.fields[1].box.left,optionListHeight);
                layout.selectLabels.clear();
                int active=-1;
                for(std::size_t i=0;i<total;++i) {
                    if(view.fallback.modelOptions[i]==view.fallback.modelValue)
                        active=static_cast<int>(i);
                }
                for(std::size_t i=0;i<count;++i)
                    layout.selectLabels.push_back(view.fallback.modelOptions[static_cast<std::size_t>(first)+i]);
                layout.selectActive=active;
                layout.selectScroll=first;
                layout.selectTotal=static_cast<int>(total);
                layout.selectVisible=static_cast<int>(count);
                float optionY=layout.selectList.top+dip(4);
                for(std::size_t i=0;i<count;++i) {
                    ControlRect option;
                    option.id=ControlId::SelectOption;
                    option.index=first+static_cast<int>(i);
                    option.rect=Box(layout.selectList.left,optionY,
                        layout.selectList.right-layout.selectList.left,dip(30));
                    layout.selectOptions.push_back(option);
                    optionY+=dip(30);
                }
                if(total>count) {
                    const float trackTop=layout.selectList.top+dip(4);
                    const float trackHeight=dip(30.0f*static_cast<float>(count));
                    const float thumbHeight=trackHeight*static_cast<float>(count)/static_cast<float>(total);
                    const float thumbTop=trackTop+trackHeight*static_cast<float>(first)/static_cast<float>(total);
                    layout.selectBar=Box(layout.selectList.right-dip(4),trackTop,dip(3),trackHeight);
                    layout.selectThumb=Box(layout.selectBar.left,thumbTop,dip(3),thumbHeight);
                }
            }

        }

    }

    if(view.activePage==2||view.activePage==3||view.activePage==4) {

        // 三页共用一个可滚动的正文：窗口档位固定，而正文（三子表单 / 备份列表 / 设置行）比它高。

        layout.pageViewport=Box(layout.content.left,layout.content.top,

            layout.content.right-layout.content.left,layout.content.bottom-layout.content.top);

        const auto fieldHeight=[](int kind) {

            return kind==4?54.0f:(kind==5?24.0f:kInputHeight);

        };

        // ---- 页头（备份 = 标题+副题+刷新；添加/设置 = 标题）----

        float y=kPageHeadTop;

        if(view.activePage==3) {

            // 页头动作按钮（备份页「刷新」）必须落在**页头**里：之前纵坐标写的是 dip(0)，
            // 于是它被画到窗口最顶端，和标题栏的吸附/最小化按钮叠在一起（真机截图里那枚
            // 顶着 ↻ 和「刷新」字样的怪按钮就是它）。
            layout.pageHeadAction=Box(layout.content.right-dip(88),layout.content.top+dip(y),
                dip(88),dip(30));

            layout.controls.push_back({ControlId::BackupRefresh,layout.pageHeadAction});

        }

        layout.pageHead=Box(layout.content.left,layout.content.top+dip(y),

            layout.content.right-layout.content.left,dip(kPageHeadHeight));

        // 标题按实测宽度（旧 .ph-t 是自然宽度）；副题从标题右边一直排到动作按钮前。

        const float headTitleWidth=EstimateWidth(view.activePage==3?view.backup.title:view.add.title,

            kPageTitleFont)*scale;

        layout.pageHeadTitle=Box(layout.pageHead.left,layout.pageHead.top,headTitleWidth,dip(30));

        y+=kPageHeadHeight;

        if(view.activePage==3) {

            const float subLeft=layout.pageHeadTitle.right+dip(kPageHeadGap);

            layout.pageHeadSub=Box(subLeft,layout.pageHead.top,

                std::max(dip(40),layout.pageHeadAction.left-dip(8)-subLeft),

                layout.pageHead.bottom-layout.pageHead.top);

        }

        if(view.activePage==4) {

            // 设置页不画页头，直接从「连接」小节开始（旧面板设置页只有一个 .form）

            y=kPageHeadTop;

            layout.pageHead=D2D1::RectF();

        }

        // ---- 添加页：子页分段 ----

        if(view.activePage==2) {

            layout.subTabs=Box(layout.content.left,layout.content.top+dip(y),

                layout.content.right-layout.content.left,dip(kSubRow));

            const ControlId subIds[2]={ControlId::SubTabNew,ControlId::SubTabDel};

            float x=layout.subTabs.left;

            for(std::size_t index=0;index<view.add.subs.size()&&index<2;++index) {

                const float itemWidth=dip(kSegPadX*2+kSegSlack)+EstimateWidth(view.add.subs[index],kSegFont)*scale;

                layout.segments.push_back({subIds[index],Box(x,layout.subTabs.top,itemWidth,dip(kSubRow))});

                layout.controls.push_back(layout.segments.back());

                x+=itemWidth+dip(kSegGap);

            }

            y+=kSubRow+kSubRowBottom+kFormPadTop;

        } else if(view.activePage==3) {

            // 备份页：类型过滤段（全部/config/.env）+ 列表

            layout.filters=Box(layout.content.left,layout.content.top+dip(y),

                layout.content.right-layout.content.left,dip(kSubRow));

            const ControlId filterIds[3]={ControlId::FilterAll,ControlId::FilterFav,ControlId::FilterRecent};

            float x=layout.filters.left;

            for(std::size_t index=0;index<view.backup.filters.size()&&index<3;++index) {

                const auto& seg=view.backup.filters[index];

                const float itemWidth=dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale

                    +dip(kSegCountGap)+EstimateWidth(std::to_string(seg.count),kSegCountFont)*scale;

                layout.segments.push_back({filterIds[index],Box(x,layout.filters.top,itemWidth,dip(kSubRow))});

                layout.controls.push_back(layout.segments.back());

                x+=itemWidth+dip(kSegGap);

            }

            y+=kSubRow+kSubRowBottom;

        }

        // ---- 正文：字段 / 开关 / 分段 / 药丸 / 危险卡（或备份行）----

        const float bodyTop=layout.content.top+dip(y);

        float cursor=bodyTop-view.pageScroll;

        const auto placeField=[&](const FormFieldView& field,float width,float left,float height) {

            PanelLayout::FieldBox box;

            box.kind=field.kind;

            box.index=field.index;

            box.label=Box(left,cursor,width,dip(kFieldLabel));

            box.box=Box(left,cursor+dip(kFieldLabel+kFieldGap),width,dip(height));

            layout.fields.push_back(box);

            if(field.kind!=5)

                layout.controls.push_back({ControlId::FormField,box.box,field.index});

        };

        const auto rowWidth=[&]() { return layout.content.right-layout.content.left; };

        if(view.activePage==2) {

            const auto& page=view.add;

            const float gap=dip(7);

            if(page.danger) {

                // 删除页：危险卡片（红边 + 半透明红底 + 标签 + 字段 + 开关 + 说明 + 右对齐按钮）

                layout.dangerCard=Box(layout.content.left,bodyTop-cursor+bodyTop+0.0f,

                    layout.content.right-layout.content.left,dip(0));

            }

            std::size_t index=0;

            while(index<page.fields.size()) {

                const auto& field=page.fields[index];

                if(field.column==1&&index+1<page.fields.size()&&page.fields[index+1].column==2) {

                    const float columnWidth=(rowWidth()-gap)/2.0f;

                    placeField(field,columnWidth,layout.content.left,fieldHeight(field.kind));

                    placeField(page.fields[index+1],columnWidth,layout.content.left+columnWidth+gap,

                        fieldHeight(page.fields[index+1].kind));

                    cursor+=dip(kFieldLabel+kFieldGap+fieldHeight(field.kind)+kFormRowGap);

                    index+=2;

                    continue;

                }

                placeField(field,rowWidth(),layout.content.left,fieldHeight(field.kind));

                cursor+=dip(kFieldLabel+kFieldGap+fieldHeight(field.kind)+kFormRowGap);

                ++index;

                if(page.danger&&index==page.fields.size()) {

                    // 删除页的说明行插在字段后面（.note）

                    layout.notes.push_back({Box(layout.content.left,cursor,rowWidth(),dip(18)),0,

                        page.chipNote});

                    cursor+=dip(18+kFormRowGap);

                }

            }

            // 接口模式分段（新建供应商页）

            if(page.sub==0&&!page.modes.empty()) {

                layout.notes.push_back({Box(layout.content.left,cursor,rowWidth(),dip(kFieldLabel)),0,

                    "\xE6\x8E\xA5\xE5\x8F\xA3\xE6\xA8\xA1\xE5\xBC\x8F"});

                cursor+=dip(kFieldLabel+kFieldGap);

                float x=layout.content.left;

                for(std::size_t segIndex=0;segIndex<page.modes.size();++segIndex) {

                    const auto& seg=page.modes[segIndex];

                    const float itemWidth=dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale;

                    layout.segments.push_back({ControlId::ModeSeg,Box(x,cursor,itemWidth,dip(kSegHeight))});

                    layout.segments.back().index=static_cast<int>(segIndex);

                    layout.controls.push_back(layout.segments.back());

                    x+=itemWidth+dip(kSegGap);

                }

                cursor+=dip(kSegHeight+kFormRowGap);

                // v1.21（用户要求）：模型列表——一个可滚动的列表，不再是一行会溢出的药丸。
                // 表头一行「全选/全不选」，下面 5 行可视，超出用滚轮 + 右侧滑块。
                if(!page.chips.empty()) {
                    constexpr std::size_t kVisibleRows=5;
                    const float rowHeight=dip(28);
                    const float headerHeight=dip(26);
                    const auto total=page.chips.size();
                    const int maxFirst=static_cast<int>(total>kVisibleRows?total-kVisibleRows:0);
                    const int first=std::clamp(page.modelScroll,0,maxFirst);
                    const std::size_t count=std::min<std::size_t>(
                        kVisibleRows,total-static_cast<std::size_t>(first));
                    layout.modelList=Box(layout.content.left,cursor,rowWidth(),
                        headerHeight+rowHeight*static_cast<float>(count)+dip(4));
                    layout.controls.push_back({ControlId::ModelRowAll,
                        Box(layout.modelList.left,layout.modelList.top,rowWidth()-dip(4),headerHeight)});
                    float rowY=layout.modelList.top+headerHeight;
                    layout.modelRows.clear();
                    for(std::size_t slotIndex=0;slotIndex<count;++slotIndex) {
                        ControlRect row;
                        row.id=ControlId::ModelRow;
                        row.index=first+static_cast<int>(slotIndex);
                        row.rect=Box(layout.modelList.left,rowY,rowWidth()-dip(4),rowHeight);
                        layout.modelRows.push_back(row);
                        layout.controls.push_back(row);
                        rowY+=rowHeight;
                    }
                    if(total>kVisibleRows) {
                        const float trackTop=layout.modelList.top+headerHeight;
                        const float trackHeight=rowHeight*static_cast<float>(count);
                        const float thumbHeight=trackHeight*static_cast<float>(count)
                            /static_cast<float>(total);
                        const float thumbTop=trackTop+trackHeight*static_cast<float>(first)
                            /static_cast<float>(total);
                        layout.modelListBar=Box(layout.modelList.right-dip(3),trackTop,dip(3),trackHeight);
                        layout.modelListThumb=Box(layout.modelListBar.left,thumbTop,dip(3),thumbHeight);
                    }
                    cursor+=headerHeight+rowHeight*static_cast<float>(count)+dip(4)+dip(kFormRowGap);
                }

            }

            // 开关行

            for(const auto& row:page.switches) {

                layout.controls.push_back({ControlId::SwitchRowControl,

                    Box(layout.content.left,cursor,rowWidth(),dip(kSwitchHeight+6)),row.index});

                cursor+=dip(kSwitchHeight+6+kFormRowGap);

            }

            // 审计 F-012：API Key 字段右内侧的眼睛按钮（旧版 #btn-np-eye）。
            if(page.sub==0) {
                for(const auto& field:layout.fields) {
                    if(field.index!=2) continue;
                    const float size=dip(26);
                    const float pad=dip(5);
                    layout.controls.push_back({ControlId::KeyReveal,
                        Box(field.box.right-pad-size,
                            field.box.top+(field.box.bottom-field.box.top-size)/2.0f,size,size)});
                    break;
                }
            }

            // 供应商下拉展开：贴在「供应商」字段下面（加模型 / 删除两个子页共用），最多 8 行。

            if(page.selectOpen&&!page.providers.empty()) {
                // 供应商下拉展开（加模型 / 删除两个子页共用）：可视 8 行，超出的滚动
                // （真机：18 个供应商，后 10 个根本选不到）。
                constexpr std::size_t kRows=8;
                const auto fieldIndex=static_cast<std::size_t>(std::max(0,page.selectField));
                if(fieldIndex<layout.fields.size()) {
                    const auto& field=layout.fields[fieldIndex];
                    const auto total=page.providers.size();
                    const int maxFirst=static_cast<int>(total>kRows?total-kRows:0);
                    const int first=std::clamp(page.providerScroll,0,maxFirst);
                    const std::size_t count=std::min<std::size_t>(kRows,total-static_cast<std::size_t>(first));
                    const float optionListHeight=dip(30.0f*static_cast<float>(count)+8);
                    const float optionListTop=SelectListTop(field.box.bottom,field.box.top,
                        optionListHeight,layout.content.bottom-dip(kFootHeight));
                    layout.selectList=Box(field.box.left,optionListTop,
                        field.box.right-field.box.left,optionListHeight);
                    layout.selectLabels.clear();
                    for(std::size_t i=0;i<count;++i)
                        layout.selectLabels.push_back(page.providers[static_cast<std::size_t>(first)+i]);
                    layout.selectActive=page.provider;
                    layout.selectScroll=first;
                    layout.selectTotal=static_cast<int>(total);
                    layout.selectVisible=static_cast<int>(count);
                    float optionY=layout.selectList.top+dip(4);
                    for(std::size_t i=0;i<count;++i) {
                        ControlRect option;
                        option.id=ControlId::SelectOption;
                        option.index=first+static_cast<int>(i);
                        option.rect=Box(layout.selectList.left,optionY,
                            layout.selectList.right-layout.selectList.left,dip(30));
                        layout.selectOptions.push_back(option);
                        optionY+=dip(30);
                    }
                    if(total>count) {
                        const float trackTop=layout.selectList.top+dip(4);
                        const float trackHeight=dip(30.0f*static_cast<float>(count));
                        const float thumbHeight=trackHeight*static_cast<float>(count)/static_cast<float>(total);
                        const float thumbTop=trackTop+trackHeight*static_cast<float>(first)/static_cast<float>(total);
                        layout.selectBar=Box(layout.selectList.right-dip(4),trackTop,dip(3),trackHeight);
                        layout.selectThumb=Box(layout.selectBar.left,thumbTop,dip(3),thumbHeight);
                    }
                }
            }

            // 底部：说明 + 探测/保存（加模型页是 探测填充/追加）

            const float footTop=layout.content.bottom-dip(kFootHeight);

            layout.footBar=Box(layout.content.left,footTop,rowWidth(),dip(kFootHeight));

            const float buttonWidth=dip(96);

            layout.primaryButton=Box(layout.content.right-buttonWidth,footTop+dip(8),buttonWidth,dip(30));

            layout.controls.push_back({ControlId::PrimaryButton,layout.primaryButton});

            // 探测按钮在保存按钮左边（加模型页的文案更长，单独加宽）

            // v1.18：探测按钮只在「新建供应商」页（删除页没有可探测的对象）。
            if(view.activePage!=2||view.add.sub==0) {

                layout.secondaryButton=Box(layout.primaryButton.left-dip(7)-buttonWidth,
                    layout.primaryButton.top,buttonWidth,dip(30));

                layout.controls.push_back({ControlId::ProbeButton,layout.secondaryButton});

            }

            // 说明/错误行止于左侧按钮之前，避免文字压到按钮下面。

            layout.errorLine=Box(layout.content.left,footTop+dip(6),

                std::max(dip(60),rowWidth()-buttonWidth-buttonWidth-dip(24)),dip(34));

            // 危险卡的边框：把「删除供应商」按钮包进红框（旧 .danger 卡片）

            if(page.danger) layout.dangerCard=Box(layout.content.left,bodyTop-cursor+bodyTop+0.0f,

                rowWidth(),dip(0));

            // 正文超过视口时由页内滚动接住（底部动作栏固定在视口之外，不跟着滚）。

        } else if(view.activePage==3) {

            // 备份行：42 高，右端一个 .btn.ghost.tiny「回滚」。旧 .list 是 max-height 384 的**内部滚动**

            // 区域：只建视口内的行，否则视口外的行会压在底栏上、点底栏反而触发"回滚"（真机踩到过）。

            // 列表盒子是**固定的 384 高视口**（旧 .list overflow:auto）：盒子本身不跟着滚动走，

            // 只有里面的行按 pageScroll 移动。之前用 cursor（它已经减掉了 pageScroll）当盒子上边界，

            // 一滚动盒子连同裁剪区一起滑出屏幕 —— 真机上就是「备份页一滚就空」。

            const float listTop=bodyTop;

            // 视口取「384 与可用高度」的较小值：窗口被手动拉矮后，列表不能再压到底栏上。
            const float backupAvailable=tightContent?0.0f
                :std::max(dip(96.0f),layout.content.bottom-listTop);
            const float listViewportHeight=std::min(dip(kListMinHeight),backupAvailable);

            layout.list=Box(layout.content.left,listTop,rowWidth(),listViewportHeight);

            layout.listViewport=layout.list;

            float rowY=listTop-view.pageScroll;

            const float buttonWidth=dip(58);

            for(std::size_t index=0;index<view.backup.rows.size();++index) {

                if(rowY+dip(kBackupRow)<=layout.listViewport.top) { rowY+=dip(kBackupRow);continue; }

                // 只放"整行都在视口里"的行：半行露在外面会连它的按钮一起压到底栏上（真机踩到过）。

                if(rowY+dip(kBackupRow)>layout.listViewport.bottom) break;

                ListItem item;

                item.kind=ListItemKind::Row;

                item.row=static_cast<int>(index);

                item.rect=Box(layout.content.left,rowY,rowWidth(),dip(kBackupRow));

                layout.rows.push_back(item);

                layout.controls.push_back({ControlId::RestoreButton,

                    Box(item.rect.right-buttonWidth,rowY+dip((kBackupRow-21)/2),buttonWidth,dip(21)),

                    static_cast<int>(index)});

                rowY+=dip(kBackupRow);

            }

            // 脚注是列表里的一段 .note：宽文本要换行（旧 <p class="note">），高度给两行，

            // 也要算进列表的滚动范围，否则第二行被裁掉（真机上就是"脚注被切断"）。

            const float rowsHeight=dip(kBackupRow)*static_cast<float>(view.backup.rows.size());

            layout.pageMaxScroll=std::max(0.0f,rowsHeight-listViewportHeight);

            layout.pageScrollVisible=layout.pageMaxScroll>0;

            cursor=listTop+listViewportHeight;

        } else {

            // 设置页：小节标题 + 行（标签在左、控件在右）

            const auto& page=view.settings;

            std::size_t fieldIndex=0;

            for(std::size_t sectionIndex=0;sectionIndex<page.sections.size();++sectionIndex) {

                // 小节标题（.sect）

                layout.notes.push_back({Box(layout.content.left,cursor,rowWidth(),dip(kSectHeight)),

                    static_cast<int>(sectionIndex)+1,page.sections[sectionIndex]});

                cursor+=dip(kSectHeight);

                // 每个小节的行：连接 3 字段、行为 3 开关 + 关闭按钮下拉、快捷键 1 字段、其他 1 按钮 + meta

                if(sectionIndex==0) {

                    for(int count=0;count<3&&fieldIndex<page.fields.size();++count,++fieldIndex) {

                        const auto& field=page.fields[fieldIndex];

                        const float inputWidth=dip(128);

                        // 旧 .srow：左边是 label（说明 small 在标签**下面**一行），右边是控件，行高 = max(两者)。

                        // 旧 .srow 的高度 = max(标签+small, 控件)：小字可能换两行，行高按 15+2*13 给。

                        const float rowHeight=kInputHeight+24;

                        PanelLayout::FieldBox box;

                        box.kind=field.kind;

                        box.index=field.index;

                        box.label=Box(layout.content.left,cursor+dip(5),rowWidth()-inputWidth-dip(8),dip(16));

                        box.note=field.note;

                        box.box=Box(layout.content.right-inputWidth,cursor+dip(5),inputWidth,dip(kInputHeight));

                        layout.fields.push_back(box);

                        if(!field.note.empty())

                            layout.notes.push_back({Box(layout.content.left,cursor+dip(21),

                                rowWidth()-inputWidth-dip(8),dip(26)),0,field.note});

                        layout.controls.push_back({ControlId::FormField,box.box,field.index});

                        cursor+=dip(rowHeight);

                    }

                } else if(sectionIndex==1) {

                    for(const auto& row:page.switches) {

                        layout.controls.push_back({ControlId::SwitchRowControl,

                            Box(layout.content.left,cursor,rowWidth(),dip(22)),row.index});

                        cursor+=dip(22+12);

                    }

                    // 延迟探测间隔：三段（5/15/60 分钟），与主题分段同一套排版；排在主题段之前，
                    // 渲染层用 segments 的起始下标区分两组。
                    if(!page.latencySegments.empty()) {

                        const float segGap=dip(kSegGap);

                        std::vector<float> segWidths;

                        for(const auto& seg:page.latencySegments)

                            segWidths.push_back(dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale);

                        layout.labels.push_back({Box(layout.content.left,cursor,rowWidth(),dip(kFieldLabel)),0,

                            "\xE5\xBB\xB6\xE8\xBF\x9F\xE6\x8E\xA2\xE6\xB5\x8B\xE9\x97\xB4\xE9\x9A\x94"});

                        cursor+=dip(kFieldLabel+kFieldGap);

                        float x=layout.content.left;

                        float rowTop=cursor;

                        for(std::size_t latencyIndex=0;latencyIndex<page.latencySegments.size();++latencyIndex) {

                            const float itemWidth=segWidths[latencyIndex];

                            if(x>layout.content.left&&x+itemWidth>layout.content.right) {

                                x=layout.content.left;

                                rowTop+=dip(kSegHeight+kSegGap);

                            }

                            layout.segments.push_back({ControlId::LatencySeg,Box(x,rowTop,itemWidth,dip(kSegHeight))});

                            layout.segments.back().index=static_cast<int>(latencyIndex);

                            layout.controls.push_back(layout.segments.back());

                            x+=itemWidth+segGap;

                        }

                        cursor=rowTop+dip(kSegHeight+kFormRowGap);

                    }

                    // 贴边挂件样式：五段（胶囊/徽章/光环/声波/磁贴），与主题分段同一套排版。

                    if(!page.widgetSegments.empty()) {

                        const float segGap=dip(kSegGap);

                        std::vector<float> segWidths;

                        for(const auto& seg:page.widgetSegments)

                            segWidths.push_back(dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale);

                        layout.labels.push_back({Box(layout.content.left,cursor,rowWidth(),dip(kFieldLabel)),0,

                            "\xE8\xB4\xB4\xE8\xBE\xB9\xE6\x8C\x82\xE4\xBB\xB6"});

                        cursor+=dip(kFieldLabel+kFieldGap);

                        float x=layout.content.left;

                        float rowTop=cursor;

                        for(std::size_t widgetIndex=0;widgetIndex<page.widgetSegments.size();++widgetIndex) {

                            const float itemWidth=segWidths[widgetIndex];

                            if(x>layout.content.left&&x+itemWidth>layout.content.right) {

                                x=layout.content.left;

                                rowTop+=dip(kSegHeight+kSegGap);

                            }

                            layout.segments.push_back({ControlId::WidgetSeg,Box(x,rowTop,itemWidth,dip(kSegHeight))});

                            layout.segments.back().index=static_cast<int>(widgetIndex);

                            layout.controls.push_back(layout.segments.back());

                            x+=itemWidth+segGap;

                        }

                        cursor=rowTop+dip(kSegHeight+kFormRowGap);

                    }

                    // 配置档案：标签在左，ghost 按钮「管理」在右（F5）。

                    {
                        const float profileButtonWidth=EstimateWidth("管理",kButtonFont)*scale+dip(22);
                        layout.labels.push_back({Box(layout.content.left,
                            cursor+dip((30-kFieldLabel)/2.0f),
                            rowWidth()-profileButtonWidth-dip(8),dip(kFieldLabel)),0,"配置档案"});
                        layout.profilesButton=Box(layout.content.right-std::max(profileButtonWidth,dip(60)),
                            cursor,std::max(profileButtonWidth,dip(60)),dip(30));
                        layout.controls.push_back({ControlId::SetProfiles,layout.profilesButton});
                        cursor+=dip(30+12);
                    }

                    if(fieldIndex<page.fields.size()) {

                        const auto& field=page.fields[fieldIndex++];

                        const float inputWidth=dip(128);

                        PanelLayout::FieldBox box;

                        box.kind=field.kind;

                        box.index=field.index;

                        box.label=Box(layout.content.left,cursor+dip(5),rowWidth()-inputWidth-dip(8),dip(16));

                        box.box=Box(layout.content.right-inputWidth,cursor+dip(5),inputWidth,dip(kInputHeight));

                        layout.fields.push_back(box);

                        if(!field.note.empty())

                            layout.notes.push_back({Box(layout.content.left,cursor+dip(21),

                                rowWidth()-inputWidth-dip(8),dip(26)),0,field.note});

                        layout.controls.push_back({ControlId::FormField,box.box,field.index});

                        cursor+=dip(kInputHeight+16);

                    }

                    // 主题：一行放不下就换行（六套 + 跟随系统共七段，384 宽装不下 —— h-controls 门禁

                    // 抓到过"分段排到窗口外、点不到"）。旧 .chips 是 flex-wrap，这里按同样语义自己排。

                    {

                        const float segGap=dip(kSegGap);

                        std::vector<float> segWidths;

                        for(const auto& seg:page.themes)

                            segWidths.push_back(dip(kSegPadX*2+kSegSlack)+EstimateWidth(seg.label,kSegFont)*scale);

                        layout.labels.push_back({Box(layout.content.left,cursor,rowWidth(),dip(kFieldLabel)),0,

                            "主题"});

                        cursor+=dip(kFieldLabel+kFieldGap);

                        float x=layout.content.left;

                        float rowTop=cursor;

                        for(std::size_t themeIndex=0;themeIndex<page.themes.size();++themeIndex) {

                            const float itemWidth=segWidths[themeIndex];

                            if(x>layout.content.left&&x+itemWidth>layout.content.right) {

                                x=layout.content.left;

                                rowTop+=dip(kSegHeight+kSegGap);

                            }

                            layout.segments.push_back({ControlId::ThemeSeg,Box(x,rowTop,itemWidth,dip(kSegHeight))});

                            layout.segments.back().index=static_cast<int>(themeIndex);

                            layout.controls.push_back(layout.segments.back());   // 可点击：控件表里也要有

                            x+=itemWidth+segGap;

                        }

                        cursor=rowTop+dip(kSegHeight+kFormRowGap);

                    }

                } else if(sectionIndex==2) {

                    if(fieldIndex<page.fields.size()) {

                        const auto& field=page.fields[fieldIndex++];

                        const float inputWidth=dip(150);

                        PanelLayout::FieldBox box;

                        box.kind=field.kind;

                        box.index=field.index;

                        box.label=Box(layout.content.left,cursor+dip(5),rowWidth()-inputWidth-dip(8),dip(16));

                        box.box=Box(layout.content.right-inputWidth,cursor+dip(5),inputWidth,dip(kInputHeight));

                        layout.fields.push_back(box);

                        if(!field.note.empty())

                            layout.notes.push_back({Box(layout.content.left,cursor+dip(21),

                                rowWidth()-inputWidth-dip(8),dip(26)),0,field.note});

                        layout.controls.push_back({ControlId::FormField,box.box,field.index});

                        cursor+=dip(kInputHeight+16);

                    }

                } else {

                    // 诊断 / 用量：与 WebUI 行同款（标签在左，ghost 按钮在右、贴文字宽度）。

                    const float extraHeight=dip(30);

                    const float extraWidth=EstimateWidth("\xE6\x9F\xA5\xE7\x9C\x8B",kButtonFont)*scale+dip(22);

                    layout.labels.push_back({Box(layout.content.left,

                        cursor+dip((30-kFieldLabel)/2.0f),

                        layout.content.right-layout.content.left-extraWidth-dip(8),dip(kFieldLabel)),0,

                        "\xE8\xAF\x8A\xE6\x96\xAD"});

                    layout.diagnoseButton=Box(layout.content.right-std::max(extraWidth,dip(60)),cursor,

                        std::max(extraWidth,dip(60)),extraHeight);

                    layout.controls.push_back({ControlId::SetDiagnose,layout.diagnoseButton});

                    cursor+=dip(30+12);

                    layout.labels.push_back({Box(layout.content.left,

                        cursor+dip((30-kFieldLabel)/2.0f),

                        layout.content.right-layout.content.left-extraWidth-dip(8),dip(kFieldLabel)),0,

                        "\xE7\x94\xA8\xE9\x87\x8F\xE7\xBB\x9F\xE8\xAE\xA1"});

                    layout.usageButton=Box(layout.content.right-std::max(extraWidth,dip(60)),cursor,

                        std::max(extraWidth,dip(60)),extraHeight);

                    layout.controls.push_back({ControlId::SetUsage,layout.usageButton});

                    cursor+=dip(30+12);

                    // 手动重启网关：设置页常驻入口（换模型后的待重启胶囊在寄存器行，二者同一动作）。

                    {

                        const float restartWidth=std::max(EstimateWidth("重启网关",kButtonFont)*scale+dip(22),dip(84));

                        layout.labels.push_back({Box(layout.content.left,

                            cursor+dip((30-kFieldLabel)/2.0f),

                            rowWidth()-restartWidth-dip(8),dip(kFieldLabel)),0,

                            "网关"});

                        layout.restartGatewayButton=Box(layout.content.right-restartWidth,cursor,

                            restartWidth,dip(30));

                        layout.controls.push_back({ControlId::SetRestartGateway,layout.restartGatewayButton});

                        cursor+=dip(30+12);

                    }

                    // 其他：旧 <div class="srow"><label>WebUI</label><button class="btn ghost">

                    // 在浏览器打开</button></div>——标签在左，ghost 按钮在右并贴着文字宽度。

                    const float buttonHeight=dip(30);

                    const float buttonWidth=EstimateWidth(page.openWebUiLabel,kButtonFont)*scale+dip(22);

                    layout.labels.push_back({Box(layout.content.left,

                        cursor+dip((30-kFieldLabel)/2.0f),

                        layout.content.right-layout.content.left-buttonWidth-dip(8),dip(kFieldLabel)),0,

                        page.webUiRowLabel});

                    layout.secondaryButton=Box(layout.content.right-std::max(buttonWidth,dip(60)),cursor,

                        std::max(buttonWidth,dip(60)),buttonHeight);

                    layout.controls.push_back({ControlId::OpenWebUiButton,layout.secondaryButton});

                    cursor+=dip(30+12);

                    layout.cardFoot=Box(layout.content.left,cursor,rowWidth(),dip(70));

                    cursor+=dip(70);

                }

            }

            // 关闭按钮下拉的展开列表：贴在字段下面（真机反馈：点一下就直接换值了，应该像别的

            // 下拉一样弹出列表再选）。列表浮在页面之上，渲染层最后统一画。

            if(page.closeSelectOpen&&!page.closeOptions.empty()) {

                const D2D1_RECT_F* closeBox=nullptr;

                for(const auto& field:layout.fields)

                    if(field.index==3) { closeBox=&field.box;break; }

                if(closeBox) {

                    const auto count=page.closeOptions.size();

                    const float optionListHeight=dip(30.0f*static_cast<float>(count)+8);
                    // 关闭按钮常落在视口最后一行：下方放不下时向上翻，否则第二项会被底栏盖住、
                    // 点也点不到（真机实测只露出「隐藏到托盘」）。
                    const float optionListTop=SelectListTop(closeBox->bottom,closeBox->top,
                        optionListHeight,layout.content.bottom);

                    layout.selectList=Box(closeBox->left,optionListTop,

                        closeBox->right-closeBox->left,optionListHeight);

                    layout.selectLabels.clear();

                    float optionY=layout.selectList.top+dip(4);

                    for(std::size_t optionIndex=0;optionIndex<count;++optionIndex) {

                        ControlRect option;

                        option.id=ControlId::SelectOption;

                        option.index=static_cast<int>(optionIndex);

                        option.rect=Box(layout.selectList.left,optionY,

                            layout.selectList.right-layout.selectList.left,dip(30));

                        layout.selectOptions.push_back(option);

                        layout.selectLabels.push_back(page.closeOptions[optionIndex]);

                        optionY+=dip(30);

                    }

                    layout.selectActive=page.closeActive;

                }

            }

        }

        // ---- 页内滚动：正文超出视口时可见滚动条（旧 body.clamped .page 的等价物）----

        // 添加页底部是固定动作栏（探测/保存）：正文视口止于它的上沿，否则窗口拉矮后

        // 正文会被动作栏盖住（真机：「接口模式」与 .env 说明被按钮压住）。

        const float bodyBottom=(view.activePage==2&&layout.footBar.bottom>layout.footBar.top)

            ?layout.footBar.top:layout.content.bottom;

        // 裁剪起点必须是**页头之后、子页分段之前**，而不是 bodyTop（正文起点）：
        // 添加页的「新建/删除供应商」和「接口模式」两排分段都排在 bodyTop 之上，
        // 用 bodyTop 当裁剪上沿会把它们整排裁掉 —— 真机看到的就是"「添加」下面一大片空白"。
        // 没有子页分段的页（设置页）起点就是页头下沿，行为不变。
        const float viewportTop=layout.subTabs.bottom>layout.subTabs.top
            ?layout.subTabs.top
            :(layout.pageHead.bottom>layout.pageHead.top?layout.pageHead.bottom:bodyTop);
        layout.pageViewport=Box(layout.content.left,viewportTop,

            layout.content.right-layout.content.left,
            std::max(0.0f,bodyBottom-viewportTop));

        // 备份页的滚动范围由它自己的列表决定（上面已算好），其它页按正文实测。

        if(view.activePage!=3) {

            const float contentHeight=(cursor+view.pageScroll)-bodyTop;

            layout.pageMaxScroll=std::max(0.0f,contentHeight-(bodyBottom-bodyTop));

        }

        layout.pageScroll=std::clamp(view.pageScroll,0.0f,layout.pageMaxScroll);

        if(std::abs(layout.pageScroll-view.pageScroll)>0.01f) {

            const float delta=view.pageScroll-layout.pageScroll;

            for(auto& field:layout.fields) {

                field.label=Box(field.label.left,field.label.top+delta,field.label.right-field.label.left,

                    field.label.bottom-field.label.top);

                field.box=Box(field.box.left,field.box.top+delta,field.box.right-field.box.left,

                    field.box.bottom-field.box.top);

            }

            for(auto& row:layout.rows) row.rect=Box(row.rect.left,row.rect.top+delta,

                row.rect.right-row.rect.left,row.rect.bottom-row.rect.top);

            for(auto& control:layout.controls) {

                if(control.id!=ControlId::FormField&&control.id!=ControlId::RestoreButton) continue;

                control.rect=Box(control.rect.left,control.rect.top+delta,

                    control.rect.right-control.rect.left,control.rect.bottom-control.rect.top);

            }

        }

        layout.pageScrollVisible=layout.pageMaxScroll>0&&layout.pageViewport.bottom>layout.pageViewport.top;

        if(layout.pageScrollVisible) {

            const float viewportHeight=layout.pageViewport.bottom-layout.pageViewport.top;

            const float barWidth=dip(kScrollBar);

            // 在内容右缘**之外**（content.right 与面板右缘之间的空档）：旧位置压住输入框/下拉的右边框。
            layout.pageScrollTrack=Box(layout.content.right+dip(2.0f),bodyTop,barWidth,viewportHeight);

            const float thumbHeight=std::max(dip(kScrollBarMinThumb),

                viewportHeight*viewportHeight/std::max(1.0f,viewportHeight+layout.pageMaxScroll));

            const float travel=viewportHeight-thumbHeight;

            const float ratio=layout.pageMaxScroll>0?layout.pageScroll/layout.pageMaxScroll:0;

            layout.pageScrollThumb=Box(layout.pageScrollTrack.left,layout.pageScrollTrack.top+travel*ratio,

                barWidth,thumbHeight);

        }

        layout.cards=Box(layout.content.left,bodyTop,layout.content.right-layout.content.left,

            layout.content.bottom-bodyTop);

    }

    // 没有固定 min-height 的页面（设置页）：用正文实测高度补上窗口高度。

    if(layout.desiredHeight<=0.0f) {

        float contentBottom=layout.content.top;

        for(const auto& field:layout.fields) contentBottom=std::max(contentBottom,field.box.bottom);

        for(const auto& note:layout.notes) contentBottom=std::max(contentBottom,note.rect.bottom);

        contentBottom=std::max(contentBottom,layout.cardFoot.bottom);

        contentBottom=std::max(contentBottom,layout.secondaryButton.bottom);

        contentBottom=std::max(contentBottom,layout.pageScroll?layout.content.bottom:contentBottom);

        layout.desiredHeight=chrome+(contentBottom-layout.content.top)+dip(kNavHeight);

        // 设置页正文里最后一个可见元素就是 meta 行；旧版这里还有 #sheet-meta min-height 72。

        layout.desiredHeight=std::max(layout.desiredHeight,chrome+dip(kSheetMetaMinHeight)+dip(kNavHeight));

    }

    // ---- 模态 / Toast（DESIGN §4.8 #33/#34 + PLAN §4 #12/#13）----

    if(!view.modal.title.empty()) {

        // 模态：304 宽的卡片，但**宽度/高度按内容量**（正文按行数、按钮按自己的文字宽），

        // 并且整块夹进窗口 —— 真机上出现过"按钮文字被截断、正文溢出、模态顶到屏幕外"。

        const float pad=dip(kModalPad);

        const float textLimit=std::max(dip(180),width-pad*2-dip(6));

        const auto measureLine=[&](const std::string& text,float font) {

            return EstimateWidth(text,font)*scale;

        };

        // 正文：按行分隔符分行，每行再按可用宽度估算折行数（estimate 是近似，留一点余量）。

        float bodyWidth=dip(160);

        {

            std::size_t index=0;

            while(index<view.modal.body.size()) {

                const auto next=view.modal.body.find('\n',index);

                const auto line=view.modal.body.substr(index,next==std::string::npos

                    ?std::string::npos:next-index);

                bodyWidth=std::max(bodyWidth,measureLine(line,kModalBodyFont)+dip(2));

                if(next==std::string::npos) break;

                index=next+1;

            }

        }

        bodyWidth=std::min(bodyWidth,textLimit-pad*2);

        const auto bodyLines=[&]() {

            float count=0.0f;

            // 分点行（facts）每条占一行；正文按折行估算。

            count+=static_cast<float>(view.modal.facts.size());

            if(view.modal.body.empty()&&!view.modal.facts.empty()) count-=0.0f;

            std::size_t index=0;

            while(index<=view.modal.body.size()) {

                const auto next=view.modal.body.find('\n',index);

                const auto line=view.modal.body.substr(index,next==std::string::npos

                    ?std::string::npos:next-index);

                const auto lineWidth=measureLine(line,kModalBodyFont);

                count+=std::max(1.0f,std::ceil(lineWidth/std::max(dip(40),bodyWidth)));

                if(next==std::string::npos) break;

                index=next+1;

            }

            return std::max(1.0f,count);

        }();

        const float buttonFont=kButtonFont;

        const float confirmWidth=std::min(textLimit*0.62f,

            measureLine(view.modal.confirmLabel,buttonFont)+dip(24));

        const float cancelWidth=std::min(textLimit*0.4f,

            measureLine(view.modal.cancelLabel,buttonFont)+dip(20));

        const float buttonsWidth=confirmWidth+dip(7)+cancelWidth;

        const float modalWidth=std::min(width-dip(12),

            std::max(dip(kModalWidth),std::max(bodyWidth+dip(kModalPad*2)+dip(4),buttonsWidth+dip(kModalPad*2))));

        const float bodyHeight=dip(kModalBodyFont*1.7f)*bodyLines+dip(4);

        // F2/F3：事实行是单行绘制；实测最宽行超出正文宽时预留一条横向滑条（10 DIP）。
        float factsWidth=0.0f;
        for(const auto& fact:view.modal.facts)
            factsWidth=std::max(factsWidth,EstimateWidth(fact,kModalBodyFont)*scale+dip(6));
        const bool factsScrollable=factsWidth>modalWidth-pad*2.0f+0.5f;
        const float factsScrollHeight=factsScrollable?dip(10):0.0f;

        // F5：配置档案管理行（每行 30 + 6 间距）也要计入卡片高度。
        const float profileRowsHeight=static_cast<float>(view.modal.profiles.size())*(dip(30)+dip(6));

        const float modalHeight=pad*2.0f+dip(18)+dip(6)+bodyHeight+profileRowsHeight
            +factsScrollHeight+dip(11)+dip(30);

        const float cx=width/2.0f,cy=height/2.0f;

        float top=cy-modalHeight/2.0f;

        top=std::max(top,dip(6));                                   // 不许顶出窗口

        const float maxBottom=height-dip(kNavHeight+kToastBottom);

        const float modalBoxHeight=std::min(modalHeight,std::max(dip(60),maxBottom-top));

        layout.modalShade=Box(0,0,width,height);

        layout.modalBox=Box(cx-modalWidth/2.0f,top,modalWidth,modalBoxHeight);

        layout.modalTitle=Box(layout.modalBox.left+pad,layout.modalBox.top+pad,

            modalWidth-pad*2,dip(18));

        layout.modalBody=Box(layout.modalBox.left+pad,layout.modalBox.top+pad+dip(18)+dip(6),

            modalWidth-pad*2,std::max(dip(16),modalBoxHeight-pad*2-dip(18)-dip(6)-dip(11)-dip(30)));

        // 横向滚动条：贴正文盒底部；内容宽/正文宽 = 滑块占比。
        layout.modalMaxScroll=factsScrollable
            ?std::max(0.0f,factsWidth-(layout.modalBody.right-layout.modalBody.left)):0.0f;
        layout.modalScrollVisible=layout.modalMaxScroll>0.5f;
        layout.modalScrollX=std::clamp(view.modalScrollX,0.0f,layout.modalMaxScroll);
        if(layout.modalScrollVisible) {
            const float trackHeight=dip(6);
            layout.modalHScrollTrack=Box(layout.modalBody.left,
                layout.modalBody.bottom-dip(2)-trackHeight,
                layout.modalBody.right-layout.modalBody.left,trackHeight);
            const float trackWidth=layout.modalHScrollTrack.right-layout.modalHScrollTrack.left;
            const float thumbWidth=std::max(dip(24),
                trackWidth*trackWidth/std::max(1.0f,factsWidth));
            const float travel=std::max(0.0f,trackWidth-thumbWidth);
            const float ratio=layout.modalMaxScroll>0.0f
                ?layout.modalScrollX/layout.modalMaxScroll:0.0f;
            layout.modalHScrollThumb=Box(layout.modalHScrollTrack.left+travel*ratio,
                layout.modalHScrollTrack.top+dip(1),thumbWidth,trackHeight-dip(2));
        }

        // 档案行：名称/摘要占左侧，行尾「应用」「删除」两个 ghost 按钮。
        {
            float rowTop=layout.modalBody.top+dip(2)
                +static_cast<float>(view.modal.facts.size())*dip(17.0f)
                +(view.modal.facts.empty()?0.0f:dip(6.0f));
            for(std::size_t index=0;index<view.modal.profiles.size();++index) {
                const float rowHeight=dip(30);
                const auto row=Box(layout.modalBody.left,rowTop,
                    layout.modalBody.right-layout.modalBody.left,rowHeight);
                const float removeWidth=dip(44);
                const float applyWidth=dip(52);
                const auto removeBox=Box(row.right-removeWidth,row.top,removeWidth,rowHeight);
                const auto applyBox=Box(removeBox.left-dip(6)-applyWidth,row.top,applyWidth,rowHeight);
                layout.modalProfileRows.push_back({row,applyBox,removeBox});
                layout.controls.push_back({ControlId::ModalProfileApply,applyBox,
                    static_cast<int>(index)});
                layout.controls.push_back({ControlId::ModalProfileDelete,removeBox,
                    static_cast<int>(index)});
                rowTop+=rowHeight+dip(6);
            }
        }

        layout.modalConfirm=Box(layout.modalBox.right-pad-confirmWidth,

            layout.modalBox.bottom-pad-dip(30),confirmWidth,dip(30));

        layout.modalCancel=Box(layout.modalConfirm.left-dip(7)-cancelWidth,layout.modalConfirm.top,

            cancelWidth,dip(30));

        layout.controls.push_back({ControlId::ModalConfirm,layout.modalConfirm});

        layout.controls.push_back({ControlId::ModalCancel,layout.modalCancel});

    }

    if(!view.toast.text.empty()) {
        // 高度按文字实测行数（1-2 行）：长文案（如切换模型）不能省略，要完整显示。
        const float toastWidth=width-dip(kPagePadX*2);
        const float textWidth=EstimateWidth(view.toast.text,11.5f)*scale;
        const float available=std::max(1.0f,toastWidth-dip(20.0f));
        const int lines=std::min(2,std::max(1,static_cast<int>(std::ceil(textWidth/available))));
        const float toastHeight=std::max(dip(34.0f),
            dip(kToastPadY*2)+dip(11.5f*1.5f)*static_cast<float>(lines));
        const float toastBottom=height-dip(kNavHeight+kToastBottom);
        layout.toast=Box(layout.content.left,toastBottom-toastHeight,toastWidth,toastHeight);
    }

    {

        const auto count=view.nav.empty()?5:view.nav.size();

        const float itemWidth=width/static_cast<float>(count);

        const auto ids={ControlId::TabModels,ControlId::TabFallback,ControlId::TabAdd,

            ControlId::TabBackup,ControlId::TabSettings};

        int index=0;

        for(const auto id:ids) {

            if(index>=static_cast<int>(count)) break;

            NavLayout item;

            item.box=Box(itemWidth*static_cast<float>(index),layout.nav.top,itemWidth,dip(kNavHeight));

            const float centreX=item.box.left+itemWidth/2.0f;

            item.icon=Box(centreX-dip(8),layout.nav.top+dip(8),dip(16),dip(16));

            item.label=Box(item.box.left,layout.nav.top+dip(26),itemWidth,dip(15));

            item.badge=Box(centreX+dip(12),layout.nav.top+dip(10),dip(20),dip(12));

            layout.navItems.push_back(item);

            layout.controls.push_back({id,item.box});

            ++index;

        }

    }

    layout.contentHeight=height;

    return layout;

}

D2D1_RECT_F StarBox(const D2D1_RECT_F& row,float scale) {

    const auto size=kStar*scale;

    return Box(row.right-kRowRightPad*scale-size,row.top+(row.bottom-row.top-size)/2.0f,size,size);

}

D2D1_RECT_F GroupMenuBox(const D2D1_RECT_F& head,float scale) {

    const auto size=kMenuButton*scale;

    return Box(head.right-kGroupHeadPadX*scale-size,head.top+(head.bottom-head.top-size)/2.0f,size,size);

}

D2D1_RECT_F LadderButtonBox(const D2D1_RECT_F& row,int which,float scale) {

    // .fb-btns { display:flex; gap:2px } - three 22px hit boxes at the row's right end, right to left.

    const float size=22.0f*scale;

    const float pitch=size+2.0f*scale;                 // 22px button + the 2px gap of .fb-btns

    const float rightEdge=row.right-2.0f*scale;

    const float left=rightEdge-size-pitch*static_cast<float>(3-which);

    return Box(left,row.top+(row.bottom-row.top-size)/2.0f,size,size);

}

int LadderButtonAt(const D2D1_RECT_F& row,float x,float scale) {

    for(int which=1;which<=3;++which) {

        const auto box=LadderButtonBox(row,which,scale);

        if(x>=box.left&&x<box.right) return which;

    }

    return 0;

}

ListHit ListHitTest(const PanelLayout& layout,float x,float y) {

    ListHit hit;

    if(x<layout.list.left||x>=layout.list.right) return hit;

    if(y<layout.listViewport.top||y>=layout.listViewport.bottom) return hit;

    hit.inList=true;

    // 命中顺序 = 绘制顺序的逆序：粘性组头最后画（压在行上），所以先测它；rest 再按倒序

    // （后来者在上）。这样"行滚到组头下面"时点到的仍是组头，和眼睛看到的一致。

    const auto test=[&](const ListItem& item) {

        if(y<item.rect.top||y>=item.rect.bottom) return false;

        if(x<item.rect.left||x>=item.rect.right) return false;

        hit.kind=item.kind;

        hit.group=item.group;

        hit.row=item.row;

        if(item.kind==ListItemKind::Row) {

            const auto star=StarBox(item.rect,layout.scale);

            hit.star=x>=star.left&&x<star.right;

        } else if(item.kind==ListItemKind::GroupHead) {

            const auto menu=GroupMenuBox(item.rect,layout.scale);

            hit.menu=x>=menu.left&&x<menu.right;

        }

        return true;

    };

    for(auto item=layout.items.rbegin();item!=layout.items.rend();++item)

        if(item->sticky&&test(*item)) return hit;

    for(auto item=layout.items.rbegin();item!=layout.items.rend();++item)

        if(!item->sticky&&test(*item)) return hit;

    return hit;

}

namespace {
// 这些控件属于外壳/浮层，不参与"裁进内容区"的规则（底栏、标题栏、模态、下拉列表）。
bool ShellControl(ControlId id) {
    switch(id) {
    case ControlId::Snap: case ControlId::Pin: case ControlId::Minimize: case ControlId::Close:
    case ControlId::TabModels: case ControlId::TabFallback: case ControlId::TabAdd:
    case ControlId::TabBackup: case ControlId::TabSettings:
    case ControlId::Refresh: case ControlId::Chip: case ControlId::ChipWarn:
    case ControlId::ChipHelper: case ControlId::RegisterRestartGateway:
    case ControlId::StatusPill: case ControlId::LampWsl: case ControlId::LampWebui:
    case ControlId::LampGateway: case ControlId::ModalConfirm: case ControlId::ModalCancel:
    case ControlId::SelectOption: case ControlId::SyncModels:
    case ControlId::SearchClear: case ControlId::SearchBox:
        return true;
    default: return false;
    }
}
}

ControlId ControlAt(const PanelLayout& layout,float x,float y) {

    const auto hit=[&](const ControlRect& control) {

        return x>=control.rect.left&&x<control.rect.right

            &&y>=control.rect.top&&y<control.rect.bottom;

    };

    // v1.25（真机反馈"点设置会点到下边的按钮/很多按钮穿透"）：命中顺序必须跟**绘制层级**一致。

    // 底栏/标题栏/模态/下拉是最后画的，却因为登记得晚排在控件表尾部；旧实现按登记顺序取第一个

    // 命中，于是页面里滚出视口的控件会先被选中。现在：①外壳控件优先；②页面控件必须落在内容区

    // （滚到页脚/底栏下面的行不再吃点击）；③其余仍按登记顺序（折叠卡已提前）。

    for(const auto& control:layout.controls) {

        if(control.id==ControlId::None||!ShellControl(control.id)) continue;

        if(hit(control)) return control.id;

    }

    const bool insideContent=x>=layout.content.left-2.0f&&x<layout.content.right+2.0f

        &&y>=layout.content.top-2.0f&&y<layout.content.bottom+2.0f;

    if(!insideContent) return ControlId::None;

    for(const auto& control:layout.controls) {

        if(control.id==ControlId::None||ShellControl(control.id)) continue;

        if(hit(control)) return control.id;

    }

    return ControlId::None;

}

const ControlRect* ControlEntryAt(const PanelLayout& layout,ControlId id,float x,float y) {

    for(const auto& control:layout.controls) {

        if(control.id!=id) continue;

        if(x>=control.rect.left&&x<control.rect.right&&y>=control.rect.top&&y<control.rect.bottom)

            return &control;

    }

    return nullptr;

}

const D2D1_RECT_F* ControlRectFor(const PanelLayout& layout,ControlId id) {

    for(const auto& control:layout.controls) if(control.id==id) return &control.rect;

    return nullptr;

}

}

