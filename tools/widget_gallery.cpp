// Widget gallery (PLAN §6 H2b): renders every basic control in both themes to PNG so the sheet can be
// compared against the old stylesheet line by line. No window, no app process - an offscreen D2D
// render target on a WIC bitmap, drawn with the very same widgets.* primitives the panel uses.
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <vector>
#include "render/icons.h"
#include "render/theme.h"
#include "render/widgets.h"

using Microsoft::WRL::ComPtr;
namespace hermes {
namespace {

std::wstring Widen(const std::string& value) {
    if(value.empty()) return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),nullptr,0);
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length);
    return result;
}
void Hr(HRESULT result,const char* what) {
    if(FAILED(result)) throw std::runtime_error(std::string(what)+" failed");
}
constexpr float kScale=1.5f;      // the sheet is drawn at notebook 150% so text is crisp
constexpr float kWidth=480.0f;    // DIP
constexpr float kHeroHeight=64.0f;  // .hero strip height (layout-level metric)

// One theme's sheet. All geometry is stated in DIP; R() maps it to the 150% bitmap.
class Sheet {
public:
    Sheet(ID2D1RenderTarget* target,IDWriteFactory* text,const ThemePalette& palette)
        :target_(target),text_(text),palette_(palette) {
        Hr(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),brush_.GetAddressOf()),"brush");
        const auto properties=D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND,D2D1_CAP_STYLE_ROUND,
            D2D1_CAP_STYLE_ROUND,D2D1_LINE_JOIN_ROUND,4.0f,D2D1_DASH_STYLE_SOLID,0.0f);
        ComPtr<ID2D1Factory> factory;
        target->GetFactory(factory.GetAddressOf());
        Hr(factory->CreateStrokeStyle(properties,nullptr,0,style_.GetAddressOf()),"stroke style");
        const auto make=[&](float size,DWRITE_FONT_WEIGHT weight,ComPtr<IDWriteTextFormat>& format,
            const wchar_t* family=L"Microsoft YaHei UI") {
            Hr(text->CreateTextFormat(family,nullptr,weight,DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,size*kScale,L"zh-CN",format.GetAddressOf()),"text format");
            format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        };
        make(10.0f,DWRITE_FONT_WEIGHT_NORMAL,lamp_);
        make(widgets::kChipFont,DWRITE_FONT_WEIGHT_MEDIUM,chip_);
        make(widgets::kButtonFont,DWRITE_FONT_WEIGHT_NORMAL,label_);
        make(10.5f,DWRITE_FONT_WEIGHT_NORMAL,sync_,L"Cascadia Mono");
        make(widgets::kSegCountFont,DWRITE_FONT_WEIGHT_NORMAL,count_,L"Cascadia Mono");
        make(widgets::kPlateFont,DWRITE_FONT_WEIGHT_MEDIUM,model_,L"Cascadia Mono");
        make(10.5f,DWRITE_FONT_WEIGHT_MEDIUM,provider_);
        make(10.0f,DWRITE_FONT_WEIGHT_NORMAL,navLabel_);
    }

    D2D1_RECT_F R(float x,float y,float w,float h) const {
        return D2D1::RectF(x*kScale,y*kScale,(x+w)*kScale,(y+h)*kScale);
    }
    void Ink(float heightDip) {
        brush_->SetColor(palette_.ink);
        target_->FillRectangle(R(0,0,kWidth,heightDip),brush_.Get());
    }
    void T(const std::string& value,D2D1_RECT_F rect,IDWriteTextFormat* format,const D2D1_COLOR_F& colour,
        DWRITE_TEXT_ALIGNMENT alignment=DWRITE_TEXT_ALIGNMENT_LEADING) {
        const auto wide=Widen(value);
        if(wide.empty()) return;
        brush_->SetColor(colour);
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        target_->DrawTextW(wide.c_str(),static_cast<UINT32>(wide.size()),format,rect,brush_.Get());
    }
    void Icon(const D2D1_RECT_F& rect,IconId id,const D2D1_COLOR_F& colour,float width=2.0f) {
        DrawIcon(target_,brush_.Get(),style_.Get(),rect,id,colour,width,kScale);
    }

    // The hero strip, at a fixed y (the first section, like the real panel).
    void Hero(float y,float hue,const std::string& provider,const std::string& model,
        const std::string& tag) {
        const auto hero=R(0,y,kWidth,kHeroHeight);
        widgets::DrawHeroBackdrop(target_,hero,hue,palette_,kScale);
        brush_->SetColor(palette_.line);
        target_->FillRectangle(D2D1::RectF(hero.left,hero.bottom-1.0f*kScale,hero.right,hero.bottom),brush_.Get());
        const auto lampBox=R(13.0f,y+(kHeroHeight-8.0f)/2.0f,8.0f,8.0f);
        const auto centre=D2D1::Point2F((lampBox.left+lampBox.right)/2.0f,(lampBox.top+lampBox.bottom)/2.0f);
        const auto colour=palette_.ok;
        const auto ring=[&](float inflate,float alpha) {
            brush_->SetColor(D2D1::ColorF(colour.r,colour.g,colour.b,alpha));
            target_->FillEllipse(D2D1::Ellipse(centre,4.0f*kScale+inflate*kScale,
                4.0f*kScale+inflate*kScale),brush_.Get());
        };
        ring(widgets::kHeroLampGlow,0.38f);
        ring(widgets::kHeroLampRing,0.17f);
        brush_->SetColor(colour);
        target_->FillEllipse(D2D1::Ellipse(centre,4.0f*kScale,4.0f*kScale),brush_.Get());
        const float textLeft=31.0f;
        T(provider,R(textLeft,y+10,200,16),provider_.Get(),palette_.dim);
        const auto plate=R(textLeft,y+29,220,27);
        widgets::DrawPlate(target_,brush_.Get(),plate,false,palette_,kScale);
        T(model,D2D1::RectF(plate.left+widgets::kPlatePadX*kScale,plate.top,
            plate.right-widgets::kPlatePadX*kScale,plate.bottom),model_.Get(),palette_.text);
        if(!tag.empty()) {
            const auto box=R(kWidth-112,y+8,100,20);
            brush_->SetColor(palette_.raise2);
            target_->FillRoundedRectangle(D2D1::RoundedRect(box,4.0f*kScale,4.0f*kScale),brush_.Get());
            T(tag,box,count_.Get(),palette_.dim,DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // Section: lamps (all four states) and the sync stamp, arranged like the .reg row.
    void Lamps(float y) {
        Lamp(14.0f,y,"WSL",LampState::Ok);
        Lamp(76.0f,y,"\xE7\xBD\x91\xE5\x85\xB3",LampState::Err);
        Lamp(140.0f,y,"WebUI",LampState::Warn);
        Lamp(204.0f,y,"\xE7\xBD\x91\xE5\x85\xB3",LampState::Idle);
        SyncStamp(380.0f,y,"\xE2\x80\x94");
    }

    // Section: chips (plain/warn/err + hover).
    void Chips(float y) {
        x_=14.0f;
        y_=y;
        ChipLabel("\xE7\xBD\x91\xE5\x85\xB3\xE5\xBE\x85\xE9\x87\x8D\xE5\x90\xAF",ChipKind::Warn,
            widgets::Visual::Normal,IconId::RefreshCw);
        ChipLabel("WebUI \xE4\xB8\x8D\xE5\x8F\xAF\xE8\xBE\xBE",ChipKind::Err,widgets::Visual::Hovered);
        ChipLabel("\xE9\x9D\x99\xE9\x9F\xB3 chip",ChipKind::Ok,widgets::Visual::Normal);
    }
    // Section: buttons in all variants (normal / primary / ghost / danger, hovered, disabled, tiny).
    void Buttons(float y) {
        x_=14.0f;
        y_=y;
        ButtonLabel("\xE6\x99\xAE\xE9\x80\x9A\xE6\x8C\x89\xE9\x92\xAE",IconId::None,
            widgets::ButtonVariant::Normal,widgets::Visual::Normal,false,100.0f);
        ButtonLabel("\xE4\xBF\x9D\xE5\xAD\x98",IconId::Check,widgets::ButtonVariant::Primary,
            widgets::Visual::Normal,false,80.0f);
        ButtonLabel("\xE5\x8F\x96\xE6\xB6\x88",IconId::None,widgets::ButtonVariant::Ghost,
            widgets::Visual::Hovered,false,80.0f);
        ButtonLabel("\xE5\x88\xA0\xE9\x99\xA4",IconId::Trash2,widgets::ButtonVariant::Danger,
            widgets::Visual::Normal,false,80.0f);
        y_+=38;
        x_=14.0f;
        ButtonLabel("\xE7\xA6\x81\xE7\x94\xA8",IconId::Check,widgets::ButtonVariant::Primary,
            widgets::Visual::Disabled,false,80.0f);
        ButtonLabel("\xE8\xBF\xBD\xE5\x8A\xA0\xE5\x88\xB0\xE9\x93\xBE\xE5\xB0\xBE",IconId::Plus,
            widgets::ButtonVariant::Primary,widgets::Visual::Hovered,false,110.0f);
        ButtonLabel("\xE8\xBF\xBD\xE5\x8A\xA0\xE4\xB8\x80\xE6\x9D\xA1",IconId::Archive,
            widgets::ButtonVariant::Normal,widgets::Visual::Normal,true,90.0f);
        // an .icb on hover
        const auto box=R(x_-8.0f,y_+3,24,24);
        brush_->SetColor(palette_.raise2);
        target_->FillRoundedRectangle(D2D1::RoundedRect(box,widgets::kIcbRadius*kScale,
            widgets::kIcbRadius*kScale),brush_.Get());
        Icon(box,IconId::Pin,palette_.text,1.8f);
        x_+=40.0f;
        const auto box2=R(x_-8.0f,y_+3,24,24);
        brush_->SetColor(palette_.err);
        target_->FillRoundedRectangle(D2D1::RoundedRect(box2,widgets::kIcbRadius*kScale,
            widgets::kIcbRadius*kScale),brush_.Get());
        Icon(box2,IconId::X,D2D1::ColorF(1.0f,1.0f,1.0f,1.0f),1.8f);
    }

    // Section: segmented filters with counts; switches; the hairline.
    void Rows(float y) {
        float x=14.0f;
        const auto skinOn=widgets::SegSkinOf(true,false,palette_);
        const auto skinHover=widgets::SegSkinOf(false,true,palette_);
        const auto skinOff=widgets::SegSkinOf(false,false,palette_);
        const auto seg1=R(x,y,84,widgets::kSegHeight);
        widgets::DrawSegShape(target_,brush_.Get(),seg1,skinOn,kScale);
        T("\xE5\x85\xA8\xE9\x83\xA8",seg1,label_.Get(),skinOn.text);
        T("128",seg1,count_.Get(),skinOn.count,DWRITE_TEXT_ALIGNMENT_TRAILING);
        x+=92;
        const auto seg2=R(x,y,84,widgets::kSegHeight);
        widgets::DrawSegShape(target_,brush_.Get(),seg2,skinHover,kScale);
        T("\xE6\x94\xB6\xE8\x97\x8F",seg2,label_.Get(),skinHover.text);
        T("12",seg2,count_.Get(),skinHover.count,DWRITE_TEXT_ALIGNMENT_TRAILING);
        x+=92;
        const auto seg3=R(x,y,84,widgets::kSegHeight);
        widgets::DrawSegShape(target_,brush_.Get(),seg3,skinOff,kScale);
        T("\xE6\x9C\x80\xE8\xBF\x91",seg3,label_.Get(),skinOff.text);
        T("3",seg3,count_.Get(),skinOff.count,DWRITE_TEXT_ALIGNMENT_TRAILING);
        y+=34;
        SwitchRow(y,true,false,"\xE5\x86\x99\xE5\x85\xA5\xE5\x89\x8D\xE6\x98\xBE\xE7\xA4\xBA\xE5\xB7\xAE\xE5\xBC\x82");
        SwitchRow(y+26,false,false,"\xE8\x87\xAA\xE5\x8A\xA8\xE9\x87\x8D\xE5\x90\xAF\xE7\xBD\x91\xE5\x85\xB3");
        SwitchRow(y+52,false,true,"\xE7\xA6\x81\xE7\x94\xA8\xE7\x9A\x84\xE5\xBC\x80\xE5\x85\xB3");
        widgets::DrawHairline(target_,R(12.0f,y+86,kWidth-24.0f,1.0f),palette_,kScale);
    }

    void SwitchRow(float y,bool on,bool disabled,const std::string& label) {
        const auto track=R(14.0f,y,widgets::kSwitchWidth,widgets::kSwitchHeight);
        widgets::DrawSwitchShape(target_,brush_.Get(),track,widgets::SwitchSkinOf(on,disabled,palette_),on,kScale);
        T(label,R(14.0f+widgets::kSwitchWidth+8.0f,y,260,widgets::kSwitchHeight),label_.Get(),
            disabled?palette_.faint:palette_.text);
    }

    void IconWall(float y) {
        float x=14.0f;
        int column=0;
        float row=y;
        for(const auto& shape:IconTable()) {
            Icon(R(x,row,24,24),shape.id,palette_.dim,1.8f);
            x+=30.0f;
            if(++column==15) { x=14.0f;row+=30.0f;column=0; }
        }
    }

private:
    void Lamp(float x,float y,const std::string& label,LampState state) {
        const auto skin=widgets::LampSkinOf(state,palette_);
        widgets::DrawLampDot(target_,brush_.Get(),R(x,y+8,widgets::kLampDot,widgets::kLampDot),state,
            palette_,kScale);
        T(label,R(x+widgets::kLampDot+widgets::kLampGap,y,52,widgets::kLampDot+16),lamp_.Get(),skin.label);
    }
    void ChipLabel(const std::string& label,ChipKind kind,widgets::Visual visual,
        IconId icon=IconId::WifiOff) {
        const auto skin=widgets::ChipSkinOf(kind,visual,palette_);
        const float width=widgets::kChipPadX*2.0f+widgets::kChipIcon+widgets::kChipGap
            +Measure(chip_.Get(),label);
        const auto box=R(x_,y_,width,widgets::kChipHeight);
        widgets::DrawChipShape(target_,brush_.Get(),box,skin,kScale);
        const auto iconBox=widgets::kChipIcon*kScale;
        Icon(D2D1::RectF(box.left+widgets::kChipPadX*kScale,box.top+(box.bottom-box.top-iconBox)/2.0f,
            box.left+widgets::kChipPadX*kScale+iconBox,box.bottom-(box.bottom-box.top-iconBox)/2.0f),
            icon,skin.text,1.6f);
        T(label,D2D1::RectF(box.left+(widgets::kChipPadX+widgets::kChipIcon+widgets::kChipGap)*kScale,
            box.top,box.right-widgets::kChipPadX*kScale,box.bottom),chip_.Get(),skin.text);
        x_+=width+10.0f;
    }
    void SyncStamp(float x,float y,const std::string& stamp) {
        const auto box=R(x,y+8,78,20);
        const auto iconBox=11.0f*kScale;
        Icon(D2D1::RectF(box.right-14.0f*kScale-iconBox,box.top+(box.bottom-box.top-iconBox)/2.0f,
            box.right-14.0f*kScale,box.bottom-(box.bottom-box.top-iconBox)/2.0f),IconId::RefreshCw,
            palette_.faint,1.7f);
        T(stamp,D2D1::RectF(box.right-14.0f*kScale,box.top,box.right-4.0f*kScale,box.bottom),
            sync_.Get(),palette_.faint);
    }
    void ButtonLabel(const std::string& label,IconId icon,widgets::ButtonVariant variant,
        widgets::Visual visual,bool iconOnly,float width) {
        const auto skin=widgets::ButtonSkinOf(variant,visual,palette_);
        const float height=iconOnly?widgets::kButtonIconOnly:widgets::kButtonHeight;
        const auto box=R(x_,y_,width,height);
        widgets::DrawButtonShape(target_,brush_.Get(),box,skin,kScale);
        const auto glyph=13.0f*kScale;
        const float textWidth=iconOnly?0.0f:Measure(label_.Get(),label)*kScale;
        const float total=(icon!=IconId::None?glyph+widgets::kButtonIconGap*kScale:0.0f)+textWidth;
        float cursor=box.left+(box.right-box.left-total)/2.0f;
        if(icon!=IconId::None) {
            Icon(D2D1::RectF(cursor,box.top+(box.bottom-box.top-glyph)/2.0f,cursor+glyph,
                box.bottom-(box.bottom-box.top-glyph)/2.0f),icon,skin.text,1.7f);
            cursor+=glyph+widgets::kButtonIconGap*kScale;
        }
        if(!iconOnly) T(label,D2D1::RectF(cursor,box.top,box.right,box.bottom),label_.Get(),skin.text);
        x_+=width+12.0f;
    }
    float Measure(IDWriteTextFormat* format,const std::string& value) {
        const auto wide=Widen(value);
        if(wide.empty()) return 0.0f;
        ComPtr<IDWriteTextLayout> layout;
        Hr(text_->CreateTextLayout(wide.c_str(),static_cast<UINT32>(wide.size()),format,4096.0f,4096.0f,
            layout.GetAddressOf()),"text layout");
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return metrics.width/kScale;
    }

    ID2D1RenderTarget* target_=nullptr;
    IDWriteFactory* text_=nullptr;
    ThemePalette palette_{};
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1StrokeStyle> style_;
    ComPtr<IDWriteTextFormat> lamp_,chip_,label_,sync_,count_,model_,provider_,navLabel_;
    float x_=14.0f;
    float y_=0.0f;
};

void SavePng(IWICImagingFactory* wic,IWICBitmap* bitmap,const std::wstring& path) {
    ComPtr<IWICStream> stream;
    Hr(wic->CreateStream(&stream),"wic stream");
    Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"stream init");
    ComPtr<IWICBitmapEncoder> encoder;
    Hr(wic->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"png encoder");
    Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    Hr(encoder->CreateNewFrame(&frame,&bag),"frame");
    Hr(frame->Initialize(bag.Get()),"frame init");
    UINT width=0,height=0;
    Hr(bitmap->GetSize(&width,&height),"bitmap size");
    Hr(frame->SetSize(width,height),"frame size");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppPBGRA;
    Hr(frame->SetPixelFormat(&format),"frame format");
    const UINT stride=width*4;
    std::vector<BYTE> pixels(static_cast<std::size_t>(stride)*height);
    Hr(bitmap->CopyPixels(nullptr,stride,static_cast<UINT>(pixels.size()),pixels.data()),"copy pixels");
    Hr(frame->WritePixels(height,stride,static_cast<UINT>(pixels.size()),pixels.data()),"write pixels");
    Hr(frame->Commit(),"frame commit");
    Hr(encoder->Commit(),"encoder commit");
}
}

int MainEntry(int argc,char** argv) {
    if(argc<2) { std::cerr<<"usage: widget-gallery <output-prefix>\n"; return 2; }
    const std::string narrow=argv[1];
    const int wideLength=MultiByteToWideChar(CP_UTF8,0,narrow.c_str(),static_cast<int>(narrow.size()),nullptr,0);
    std::wstring prefix(static_cast<std::size_t>(wideLength),L'\0');
    MultiByteToWideChar(CP_UTF8,0,narrow.c_str(),static_cast<int>(narrow.size()),prefix.data(),wideLength);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 2;
    int status=0;
    try {
        ComPtr<ID2D1Factory> factory;
        Hr(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,factory.GetAddressOf()),"d2d factory");
        ComPtr<IWICImagingFactory> wic;
        Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic)),"wic");
        ComPtr<IDWriteFactory> text;
        Hr(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(text.GetAddressOf())),"dwrite");
        // 四套主题各出一张总览：深色/浅色（旧面板原样）+ 拟态·暗/拟态·亮（新增风格）。
        // 六套主题各出一张总览：深色/浅色（旧面板原样）+ 四套现代化候选。
        for(const char* theme:{"dark","light","slate","carbon","porcelain","sand"}) {
            const auto palette=ResolveTheme(theme);
            const float canvasHeight=410.0f;   // DIP: hero 64 + lamps/chips/buttons/rows + icon wall
            const UINT32 width=static_cast<UINT32>(480*kScale);
            const UINT32 height=static_cast<UINT32>(canvasHeight*kScale);
            ComPtr<IWICBitmap> bitmap;
            Hr(wic->CreateBitmap(width,height,GUID_WICPixelFormat32bppPBGRA,WICBitmapCacheOnDemand,
                bitmap.GetAddressOf()),"bitmap");
            ComPtr<ID2D1RenderTarget> target;
            const auto properties=D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED));
            Hr(factory->CreateWicBitmapRenderTarget(bitmap.Get(),properties,target.GetAddressOf()),
                "render target");
            target->BeginDraw();
            Sheet sheet(target.Get(),text.Get(),palette);
            sheet.Ink(canvasHeight);
            sheet.Hero(0.0f,216.0f,"custom-deepseek","deepseek-v4.1-flash","chat_completions");
            sheet.Lamps(72.0f);
            sheet.Chips(104.0f);
            sheet.Buttons(142.0f);
            sheet.Rows(212.0f);
            sheet.IconWall(330.0f);
            target->EndDraw();
            const auto suffix=[&] {
                return std::wstring(L"-")+std::wstring(theme,theme+std::strlen(theme))+L".png";
            }();
            const std::wstring path=prefix+suffix;
            SavePng(wic.Get(),bitmap.Get(),path);
            std::wcout<<L"gallery: wrote "<<path<<'\n';
        }
    } catch(const std::exception& error) {
        std::cerr<<"gallery: "<<error.what()<<"\n";
        status=1;
    }
    CoUninitialize();
    return status;
}
}  // namespace hermes

int main(int argc,char** argv) {
    return hermes::MainEntry(argc,argv);
}
