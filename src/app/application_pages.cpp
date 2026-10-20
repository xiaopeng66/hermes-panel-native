#include "app/application.h"
#include "app/application_internal.h"
#include "control_plane.h"
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <stdexcept>
#include "data/http.h"
#include "data/paths.h"
#include "data/probe.h"
#include "render/widgets.h"
#include <windowsx.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

namespace hermes {
PanelLayout Application::LayoutForInput() const {
    RECT client{};
    if(!panel_||!GetClientRect(panel_,&client)) return {};
    auto* self=const_cast<Application*>(this);
{
    auto view=self->CurrentView();
    const auto layout=ComputeLayout(view,static_cast<float>(client.right),
        static_cast<float>(client.bottom),static_cast<float>(monitor_.geometry.dpi)/96.0f);
    if(view.tooltip.visible) {
        // 气泡**跟着指针**（旧版是浏览器原生 title 的行为）：锚点取指针位置而不是控件矩形，
        // 否则它会盖住控件旁边的东西 —— 真机反馈"悬浮时图标看不见了"（顶栏按钮的气泡压住了
        // 状态灯的圆点）。渲染层负责往下/往右偏一点，并在出界时翻到指针的另一侧。
        view.tooltip.left=self->pointerX_;view.tooltip.top=self->pointerY_;
        view.tooltip.right=self->pointerX_;view.tooltip.bottom=self->pointerY_;
    }
    self->tooltipView_=view.tooltip;
    return layout;
}
}
void Application::ListMoveCursor(int delta) {
    const auto view=CurrentView();
    if(view.visibleRows.empty()) { cursor_=-1;InvalidateAll();return; }
    const auto last=static_cast<int>(view.visibleRows.size())-1;
    cursor_=cursor_<0?(delta>0?0:last):std::clamp(cursor_+delta,0,last);
    // Keep the cursor row visible - the old panel called scrollIntoView({block:'nearest'}).
    const auto laid=LayoutForInput();
    for(const auto& item:laid.items) {
        if(item.kind!=ListItemKind::Row) continue;
        const auto& group=view.groups[static_cast<std::size_t>(item.group)];
        if(group.rows[static_cast<std::size_t>(item.row)].index!=cursor_) continue;
        if(item.rect.top<laid.listViewport.top) listScroll_-=laid.listViewport.top-item.rect.top;
        else if(item.rect.bottom>laid.listViewport.bottom)
            listScroll_+=item.rect.bottom-laid.listViewport.bottom;
        listScroll_=std::clamp(listScroll_,0.0f,laid.listMaxScroll);
        break;
    }
    InvalidateAll();
}
void Application::ListSelectCursor() {
    const auto view=CurrentView();
    if(cursor_<0||cursor_>=static_cast<int>(view.visibleRows.size())) return;
    const auto& ref=view.visibleRows[static_cast<std::size_t>(cursor_)];
    const auto& group=view.groups[static_cast<std::size_t>(ref.group)];
    const auto& row=group.rows[static_cast<std::size_t>(ref.row)];
    SwitchModel(row.providerId,row.model);
}
void Application::ClearSearch() {
    search_.SetText(L"");
    cursor_=-1;
    listScroll_=0;
    InvalidateAll();
}
void Application::CycleFilter(int kind) {
    filterKind_=std::clamp(kind,0,2);
    cursor_=-1;
    listScroll_=0;
    InvalidateAll();
}
void Application::ModelsPageWheel(int delta) {
    const auto laid=LayoutForInput();
    // One wheel notch is 120; the old list scrolled three 31px rows per notch.
    const float step=widgets::kRowHeight*3.0f*static_cast<float>(delta)/120.0f;
    listScroll_=std::clamp(listScroll_-step,0.0f,laid.listMaxScroll);
    InvalidateAll();
}
void Application::ScrollFromThumb(float y) {
    const auto laid=LayoutForInput();
    const float travel=(laid.listViewport.bottom-laid.listViewport.top)
        -(laid.scrollThumb.bottom-laid.scrollThumb.top);
    if(travel<=0.0f||laid.listMaxScroll<=0) return;
    const float offset=y-scrollGrabOffset_-laid.listViewport.top;
    listScroll_=std::clamp((offset/travel)*laid.listMaxScroll,0.0f,laid.listMaxScroll);
    InvalidateAll();
}
void Application::PageScrollFromThumb(float y) {
    const auto laid=LayoutForInput();
    if(!laid.pageScrollVisible) return;
    const float travel=(laid.pageScrollTrack.bottom-laid.pageScrollTrack.top)
        -(laid.pageScrollThumb.bottom-laid.pageScrollThumb.top);
    if(travel<=0.0f||laid.pageMaxScroll<=0) return;
    const float offset=y-pageScrollGrabOffset_-laid.pageScrollTrack.top;
    pageScroll_=std::clamp((offset/travel)*laid.pageMaxScroll,0.0f,laid.pageMaxScroll);
    InvalidateAll();
}
void Application::ScrollModalFrom(float x) {
    const auto laid=LayoutForInput();
    if(!laid.modalScrollVisible) return;
    const float trackWidth=laid.modalHScrollTrack.right-laid.modalHScrollTrack.left;
    const float thumbWidth=laid.modalHScrollThumb.right-laid.modalHScrollThumb.left;
    const float travel=std::max(1.0f,trackWidth-thumbWidth);
    const float ratio=std::clamp((x-laid.modalHScrollTrack.left-modalScrollGrab_)/travel,0.0f,1.0f);
    modalScrollX_=ratio*laid.modalMaxScroll;
    InvalidateAll();
}
int Application::FormFieldIndexOf(const TextField* field) const {
    if(!field) return -1;
    if(field==&addName_) return 0;
    if(field==&addKeyEnv_) return 1;
    if(field==&addUrl_) return 2;
    if(field==&addKey_) return 3;
    if(field==&addModels_) return 1;      // 加模型页的模型 textarea
    if(field==&addNewModels_) return 4;   // 新建供应商页的「模型（每行一个）」
    if(field==&addDanger_) return 0;      // 删除页的确认名
    if(field==&setDistro_) return 0;
    if(field==&setWebuiPort_) return 1;
    if(field==&setPollSec_) return 2;
    if(field==&setHotkey_) return 4;
    return -1;
}
TextField* Application::AddFieldAt(int index) {
    // 字段序号按子页解释（旧面板的三套表单各有一组 id）。
    if(addSub_==0) {
        switch(index) {
        case 0: return &addName_;
        case 1: return &addKeyEnv_;
        case 2: return &addUrl_;
        case 3: return &addKey_;
        case 4: return &addNewModels_;
        default: return nullptr;
        }
    }
    if(addSub_==1) return index==1?&addModels_:nullptr;   // 0 是供应商下拉
    return index==0?&addDanger_:nullptr;
}
void Application::HideTooltip() {
    if(panel_) KillTimer(panel_,kTooltipTimer);
    if(tooltipVisible_) { tooltipVisible_=false;InvalidateRect(panel_,nullptr,FALSE); }
}
bool Application::HandleModalClick(float x,float y) {
    // 模态是**全局**浮层（旧版一个 #modal 谁都能开）：确定/取消必须与当前页无关 —— 之前只有
    // 回退链/添加/备份三页处理它，于是从模型页开的「供应商菜单」点取消没反应（真机反馈）。
    if(pending_==Pending::None) return false;
    const auto laid=LayoutForInput();
    const auto* confirm=ControlRectFor(laid,ControlId::ModalConfirm);
    const auto inside=[&](const D2D1_RECT_F* box) {
        return box&&x>=box->left&&x<box->right&&y>=box->top&&y<box->bottom;
    };
    // 点确定 → 执行；点取消或遮罩 → 只关闭（旧版点遮罩不关，这里保持"取消/确定才关"）。
    // F5 管理模态：行尾的「应用 / 删除」先吃点击（行在正文区，不与底部按钮重叠）。
    if(pending_==Pending::ProfileMenu) {
        for(const auto& control:laid.controls) {
            if(control.index<0) continue;
            if(control.id!=ControlId::ModalProfileApply&&control.id!=ControlId::ModalProfileDelete)
                continue;
            if(x<control.rect.left||x>=control.rect.right||y<control.rect.top||y>=control.rect.bottom)
                continue;
            if(!profiles_||control.index>=static_cast<int>(profiles_->All().size())) return true;
            pendingProfileName_=profiles_->All()[static_cast<std::size_t>(control.index)].name;
            pending_=control.id==ControlId::ModalProfileApply
                ?Pending::ProfileApplyConfirm:Pending::ProfileDeleteConfirm;
            InvalidateAll();
            return true;
        }
    }
    const bool confirmed=inside(confirm);
    if(confirmed) RunPendingAction();
    if(keepModalOpen_) { keepModalOpen_=false;InvalidateAll();return true; }
    // 审计 F-005：旧版 confirmBox 没有"点遮罩关闭"，DESIGN §4.8 #32 也写"点遮罩不关"。
    // 只有确定/取消按钮或 Esc 才关闭；其余落在模态外的点击吞掉即可（模态期间背景不响应）。
    const auto* cancel=ControlRectFor(laid,ControlId::ModalCancel);
    if(!confirmed&&!inside(cancel)) { InvalidateAll();return true; }
    // 取消写入预览 = 什么都不写（旧版 confirmBox 返回 false 时给"已取消"）。
    if(pending_==Pending::PreviewWrite) {
        previewFacts_.clear();
        ShowToast(Narrow(L"\u5df2\u53d6\u6d88"),1);
    }
    pending_=Pending::None;
    pendingIndex_=-1;
    pendingProvider_.clear();
    pendingBackupFile_.clear();
    InvalidateAll();
    return true;
}
void Application::RunPendingAction() {
    // 每个 pending 各自的"确定"动作（旧版 modal 的 btn-modal-ok）。这里的文案用 Narrow(L"...")
    // 加 Unicode 转义写：源码全 ASCII，避免中文在这一处被二次编码（这个函数已经踩过两次）。
    if(pending_==Pending::DeleteProvider) { SubmitAdd();return; }
    if(pending_==Pending::RestoreBackup&&!pendingBackupFile_.empty()) {
        Json args;
        args["cmd"]="restore";
        args["file"]=pendingBackupFile_;
        args["kind"]=pendingBackupKind_;
        StartHelperAction("restore",args,Narrow(L"\u5DF2\u56DE\u6EDA\uFF08\u6062\u590D\u524D\u53C8\u5907\u4EFD\u4E86\u4E00\u4EFD\u5F53\u524D\u5185\u5BB9\uFF09"));
        return;
    }
    if(pending_==Pending::RemoveFallback&&pendingIndex_>=0) {
        Json args;
        args["cmd"]="fallback-remove";
        args["index"]=pendingIndex_;
        StartHelperAction("fallback-remove",args,
            Narrow(L"\u5DF2\u79FB\u9664\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
        return;
    }
    if(pending_==Pending::SwitchModel) {
        StartSwitch(pendingSwitchProvider_,pendingSwitchModel_);
        return;
    }
    if(pending_==Pending::PreviewWrite) {
        // 确认写入：用 dry_run:false 重发原请求（previewArgs_ 里已带上）。
        StartHelperAction(previewVerb_,previewArgs_,previewToast_,previewRefreshAfter_,previewProbe_);
        previewFacts_.clear();
        return;
    }
    if(pending_==Pending::DiagnoseInfo||pending_==Pending::UsageInfo) {
        // 刷新：**模态留在原地**显示"正在读取…"，结果回来原地换内容。
        // 之前是"关掉模态，读完再弹回来"——真机反馈那一下闪没了（keepModalOpen_ 让
        // HandleModalClick 不清 pending_）。读取在飞时忽略重复点击。
        modalScrollX_=0;
        readFacts_.clear();
        readFacts_.push_back(Narrow(L"正在读取…"));
        keepModalOpen_=true;
        if(!readBusy_) StartReadHelper(pending_==Pending::UsageInfo?"usage":"diagnose");
        InvalidateAll();
        return;
    }
    if(pending_==Pending::ProfileMenu) { SaveCurrentProfile();return; }
    if(pending_==Pending::ProfileApplyConfirm) { StartProfileApply(pendingProfileName_);return; }
    if(pending_==Pending::ProfileDeleteConfirm) {
        if(profiles_&&profiles_->Remove(pendingProfileName_)) {
            profiles_->Save();
            ShowToast(Narrow(L"已删除配置档案「")+pendingProfileName_+Narrow(L"」"),1);
        }
        return;
    }
    if(pending_==Pending::ProviderMenu&&!pendingProvider_.empty()) {
        const auto found=std::find_if(hermesState_.providers.begin(),hermesState_.providers.end(),
            [&](const ProviderView& provider) { return provider.name==pendingProvider_; });
        if(found!=hermesState_.providers.end()) {
            Json args;
            args["cmd"]="probe";
            args["base_url"]=found->baseUrl;
            if(!found->keyEnv.empty()) args["key_env"]=found->keyEnv;
            StartHelperAction("probe",args,Narrow(L"\u5DF2\u63A2\u5230\uFF0C\u5728\u300C\u6DFB\u52A0\u300D\u9875\u62C9\u6A21\u578B\u5217\u8868"));
        }
        return;
    }
}
void Application::AddPageClick(float x,float y) {
    HideTooltip();
    const auto laid=LayoutForInput();
    const auto view=CurrentView();
    if(pending_==Pending::None&&HandleChromeClick(x,y)) return;
    if(HandleModalClick(x,y)) return;   // 全局模态：所有页面共用同一套确定/取消
    // 审计 F-012：眼睛按钮压在 API Key 字段矩形内部，必须先于 FormField 命中。
    if(const auto* eye=ControlRectFor(laid,ControlId::KeyReveal);
        eye&&x>=eye->left&&x<eye->right&&y>=eye->top&&y<eye->bottom) {
        addKeyRevealed_=!addKeyRevealed_;
        InvalidateAll();
        return;
    }
    // 子页分段
    // 添加页的供应商下拉（加模型 / 删除）：展开时先吃点击（点空白即收起，与旧 select 一致）。
    if(view.activePage==2&&addSelectOpen_) {
        const auto option=SelectOptionAt(laid,x,y);
        if(option>=0) {
            addProvider_=option;
            addSelectOpen_=false;
            selectScroll_=0;
            // 删除页：选中的供应商名进 addDanger_（提交/确认都用它）。
            if(addSub_==2&&option<static_cast<int>(hermesState_.providers.size()))
                addDanger_.SetTextUtf8(hermesState_.providers[static_cast<std::size_t>(option)].name);
            InvalidateAll();
            return;
        }
        addSelectOpen_=false;
        selectScroll_=0;
        InvalidateAll();
        return;
    }
    for(const auto& control:laid.controls) {
        const auto id=control.id;
        const bool inRect=x>=control.rect.left&&x<control.rect.right
            &&y>=control.rect.top&&y<control.rect.bottom;
        if(!inRect) continue;
        // 眼睛按钮在 API Key 字段矩形内部：必须先于 FormField 命中（审计 F-012）。
        if(id==ControlId::KeyReveal) { addKeyRevealed_=!addKeyRevealed_;InvalidateAll();return; }
        if(id==ControlId::SubTabNew||id==ControlId::SubTabModels||id==ControlId::SubTabDel) {
            addSub_=id==ControlId::SubTabNew?0:(id==ControlId::SubTabModels?1:2);
            addError_.clear();
            BlurField();
            InvalidateAll();
            return;
        }
        if(id==ControlId::ModeSeg) { addMode_=std::max(0,control.index);InvalidateAll();return; }
        if(id==ControlId::ModelChip) {
            // 点掉一个探测回来的模型药丸（旧 renderChips 的 data-drop）——同时从「模型」多行框删掉该行。
            if(control.index>=0&&control.index<static_cast<int>(addChips_.size())) {
                const auto removed=addChips_[control.index];
                addChips_.erase(addChips_.begin()+control.index);
                std::wstring kept;
                std::size_t start=0;
                const auto text=addNewModels_.Text();
                while(start<=text.size()) {
                    const auto end=text.find(L'\n',start);
                    const auto line=text.substr(start,end==std::wstring::npos?std::wstring::npos:end-start);
                    if(Widen(removed)!=line) {
                        if(!kept.empty()||start>0) kept+=L"\n";
                        kept+=line;
                    }
                    if(end==std::wstring::npos) break;
                    start=end+1;
                }
                addNewModels_.SetText(kept);
            }
            InvalidateAll();
            return;
        }
        if(id==ControlId::FormField) {
            // kind==3 是下拉（加模型/删除页的供应商）：点它展开选项，不给文本焦点。
            const auto* entry=ControlEntryAt(laid,ControlId::FormField,x,y);
            const bool isSelect=entry&&entry->index>=0
                &&entry->index<static_cast<int>(view.add.fields.size())
                &&view.add.fields[static_cast<std::size_t>(entry->index)].kind==3;
            if(isSelect&&!view.add.providers.empty()) {
                addSelectOpen_=!addSelectOpen_;
                selectScroll_=0;
                if(addSelectOpen_&&entry) addSelectField_=entry->index;
                InvalidateAll();
                return;
            }
            auto* field=AddFieldAt(control.index);
            if(field) {
                FocusField(field);
                InvalidateAll();
            }
            return;
        }
        if(id==ControlId::SwitchRowControl) {
            switch(control.index) {
            case 0:
                if(addSub_==0) addProbeBefore_=!addProbeBefore_;
                else if(addSub_==2) addPurgeEnv_=!addPurgeEnv_;
                break;
            case 1:
                if(addSub_==0) addSetMainAfter_=!addSetMainAfter_;
                else if(addSub_==2) addDropFallbacks_=!addDropFallbacks_;
                break;
            default: break;
            }
            InvalidateAll();
            return;
        }
        if(id==ControlId::ProbeButton) { ProbeForAdd();return; }
        if(id==ControlId::PrimaryButton) {
            if(addSub_==2) {
                // 删除：下拉里选中的供应商才可删（按钮的禁用态由视图给，这里再兜一层），
                // 并且必须过「确定删除」模态（替代旧版手输名字那道护栏）。
                const auto selected=addDanger_.TextUtf8();
                bool matched=false;
                for(const auto& provider:hermesState_.providers)
                    if(provider.name==selected||provider.providerId==selected) matched=true;
                if(!matched) { addError_.clear();InvalidateAll();return; }
                pending_=Pending::DeleteProvider;
                InvalidateAll();
                return;
            }
            SubmitAdd();
            return;
        }
        break;
    }
    BlurField();
}
void Application::SubmitAdd() {
    addError_.clear();
    if(addSub_==0) {
        const auto name=addName_.TextUtf8();
        const auto url=addUrl_.TextUtf8();
        if(name.empty()||url.empty()) {
            addError_="\xE5\x90\x8D\xE7\xA7\xB0\xE5\x92\x8C Base URL \xE5\xBF\x85\xE5\xA1\xAB";
            InvalidateAll();
            return;
        }
        Json args;
        args["cmd"]="upsert-provider";
        args["name"]=name;
        args["base_url"]=url;
        args["api_mode"]=addMode_==0?"chat_completions":"codex_responses";
        const auto keyEnv=addKeyEnv_.TextUtf8();
        if(!keyEnv.empty()) args["key_env"]=keyEnv;
        const auto key=addKey_.TextUtf8();
        if(!key.empty()) args["key"]=key;
        // 审计 F-007：手动输入/编辑的模型（每行一个）优先；药丸只是同一份数据的快捷视图。
        // （探测会同时填文本框和药丸，用户在文本框里编辑后保存的必须是编辑后的内容。）
        Json modelList=Json::array();
        {
            std::string line;
            std::istringstream stream(addNewModels_.TextUtf8());
            while(std::getline(stream,line)) {
                if(!line.empty()&&line.back()=='\r') line.pop_back();
                if(!line.empty()) modelList.push_back(line);
            }
        }
        if(!modelList.empty()) args["models"]=modelList;
        else if(!addChips_.empty()) args["models"]=addChips_;
        if(addSetMainAfter_) args["set_main"]=true;
        StartHelperAction("upsert-provider",args,
            Narrow(L"\u5DF2\u5199\u5165 config.yaml\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
        return;
    }
    if(addSub_==1) {
        const auto models=addModels_.TextUtf8();
        if(models.empty()) { addError_="\xE5\xA1\xAB\xE4\xB8\xAA\xE6\xA8\xA1\xE5\x9E\x8B id";InvalidateAll();return; }
        if(addProvider_<0||addProvider_>=static_cast<int>(hermesState_.providers.size())) {
            addError_="\xE5\x85\x88\xE9\x80\x89\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86";
            InvalidateAll();
            return;
        }
        Json list=Json::array();
        std::string line;
        std::istringstream stream(models);
        while(std::getline(stream,line)) {
            if(!line.empty()&&line.back()=='\r') line.pop_back();
            if(!line.empty()) list.push_back(line);
        }
        if(list.empty()) { addError_="\xE5\xA1\xAB\xE4\xB8\xAA\xE6\xA8\xA1\xE5\x9E\x8B id";InvalidateAll();return; }
        Json args;
        args["cmd"]="add-models";
        args["provider"]=hermesState_.providers[static_cast<std::size_t>(addProvider_)].providerId;
        args["models"]=list;
        StartHelperAction("add-models",args,
            Narrow(L"\u5DF2\u8FFD\u52A0\u6A21\u578B\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
        addModels_.SetText(L"");
        return;
    }
    // 删除：名字必须来自下拉（即已存在的供应商），并且按**和视图同一份规则**判断能不能删
    // —— 视图把按钮置灰的那一条，这里必须同样拒绝，反过来也一样。
    const auto typed=addDanger_.TextUtf8();
    if(!ProviderDeletable(hermesState_,typed)) {
        addError_="\xE5\x90\x8D\xE7\xA7\xB0\xE4\xB8\x8D\xE5\x8C\xB9\xE9\x85\x8D\xEF\xBC\x9A\xE8\xBE\x93"
            "\xE5\x85\xA5\xE7\x9A\x84\xE5\x90\x8D\xE5\xAD\x97\xE4\xB8\x8E\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86"
            "\xE4\xB8\x8D\xE4\xB8\x80\xE8\x87\xB4";
        InvalidateAll();
        return;
    }
    for(const auto& provider:hermesState_.providers) {
        if(provider.name!=typed) continue;
        Json args;
        args["cmd"]="delete-provider";
        args["provider"]=provider.providerId;
        // 助手契约（旧 main.js 白名单）：purge_key / remove_fallbacks。曾经写成 purge_env /
        // drop_fallbacks，真机上「一并移除回退引用」被拒、`.env` 的 key 也清不掉。
        args["purge_key"]=addPurgeEnv_;
        args["remove_fallbacks"]=addDropFallbacks_;
        StartHelperAction("delete-provider",args,
            Narrow(L"\u5DF2\u5220\u9664\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
        addDanger_.SetText(L"");
        return;
    }
}
void Application::ProbeForAdd() {
    addError_.clear();
    if(addSub_==1) {
        // 从供应商探测填充（旧 btn-am-probe）：拿该供应商的 base_url 探测模型列表
        if(addProvider_<0||addProvider_>=static_cast<int>(hermesState_.providers.size())) {
            addError_="\xE5\x85\x88\xE9\x80\x89\xE4\xBE\x9B\xE5\xBA\x94\xE5\x95\x86";
            InvalidateAll();
            return;
        }
        const auto& provider=hermesState_.providers[static_cast<std::size_t>(addProvider_)];
        if(provider.baseUrl.empty()) {
            // 真机反馈"点了没有用"：这一条根本没有 base_url，助手的 probe 只会报"需要 base_url"。
            // 直接说清楚缺什么，别让按钮看起来没反应。
            addError_=Narrow(L"「")+provider.name+Narrow(L"」没有 base_url，先在 config.yaml 里补上");
            InvalidateAll();
            return;
        }
        Json args;
        args["cmd"]="probe";
        args["base_url"]=provider.baseUrl;
        if(!provider.keyEnv.empty()) args["key_env"]=provider.keyEnv;
        StartHelperAction("probe",args,std::string{},false,false);
        return;
    }
    // 新建供应商页：拿表单里的 Base URL 探测（旧 doProbe）
    const auto url=addUrl_.TextUtf8();
    if(url.empty()) { addError_="\xE5\x85\x88\xE5\xA1\xAB Base URL";InvalidateAll();return; }
    Json args;
    args["cmd"]="probe";
    args["base_url"]=url;
    const auto key=addKey_.TextUtf8();
    if(!key.empty()) args["key"]=key;
    else {
        const auto keyEnv=addKeyEnv_.TextUtf8();
        if(!keyEnv.empty()) args["key_env"]=keyEnv;
    }
    StartHelperAction("probe",args,std::string{},false,false);
}
void Application::LoadBackups() {
    backupBusy_=true;
    backupError_.clear();
    InvalidateAll();
    Json args;
    args["cmd"]="backup-list";
    StartHelperAction("backup-list",args,std::string{},false,false);
}
void Application::BackupPageClick(float x,float y) {
    const auto laid=LayoutForInput();
    if(pending_==Pending::None&&HandleChromeClick(x,y)) return;
    if(HandleModalClick(x,y)) return;
    for(const auto& control:laid.controls) {
        if(x<control.rect.left||x>=control.rect.right||y<control.rect.top||y>=control.rect.bottom) continue;
        if(control.id==ControlId::FilterAll||control.id==ControlId::FilterFav
            ||control.id==ControlId::FilterRecent) {
            backupFilter_=control.id==ControlId::FilterAll?0:(control.id==ControlId::FilterFav?1:2);
            InvalidateAll();
            return;
        }
        if(control.id==ControlId::BackupRefresh) { LoadBackups();return; }
        if(control.id==ControlId::RestoreButton&&control.index>=0) {
            // 回滚要过危险确认（旧 confirmBox('回滚这份备份？')）。行的序号是*当前过滤视图*里的序号，
            // 所以从视图取文件，而不是从全量列表按下标取。
            const auto view=CurrentView();
            if(control.index>=static_cast<int>(view.backup.rows.size())) return;
            const auto& entry=view.backup.rows[static_cast<std::size_t>(control.index)];
            pending_=Pending::RestoreBackup;
            pendingBackupFile_=entry.file;
            pendingBackupKind_=entry.kind;
            InvalidateAll();
            return;
        }
    }
    BlurField();
}
void Application::SettingsPageClick(float x,float y) {
    const auto laid=LayoutForInput();
    if(pending_==Pending::None&&HandleChromeClick(x,y)) return;
    if(HandleModalClick(x,y)) return;
    // 展开中的关闭按钮下拉：点选项就应用并收起（真机反馈：点一下就直接换值了，应该弹列表再选）。
    if(log_) log_->Info("ui","settings click: popup open="+std::to_string(settingsSelectOpen_)
        +" controls="+std::to_string(laid.controls.size())+" selectOptions="
        +std::to_string(laid.selectOptions.size())+" at "+std::to_string(static_cast<int>(x))
        +","+std::to_string(static_cast<int>(y)));
    // 选项矩形在 layout.selectOptions 里（不进 controls 表，ControlAt 点不到它们——和添加页同一套）。
    if(settingsSelectOpen_) {
        const auto option=SelectOptionAt(laid,x,y);
        if(option>=0) {
            SetSetting("closeAction",option==0?"hide":"quit");
            settingsSelectOpen_=false;
            InvalidateAll();
            return;
        }
    }
    for(const auto& control:laid.controls) {
        if(x<control.rect.left||x>=control.rect.right||y<control.rect.top||y>=control.rect.bottom) continue;
        if(control.id==ControlId::FormField) {
            TextField* field=nullptr;
            switch(control.index) {
            case 0: field=&setDistro_;break;
            case 1: field=&setWebuiPort_;break;
            case 2: field=&setPollSec_;break;
            case 4: field=&setHotkey_;break;
            default: break;
            }
            if(control.index==3) {
                // 关闭按钮：点开/收起选项列表（隐藏到托盘 / 退出程序），选了才应用。
                settingsSelectOpen_=!settingsSelectOpen_;
                InvalidateAll();
                return;
            }
            if(field) {
                FocusField(field);
                // 武装放在聚焦之后：FocusField 会先清捕获态（点别的字段=取消编辑），
                // 点回热键框才是"开始捕获"（与添加页同一套）。
                if(control.index==4) setHotkeyCapturing_=true;
            }
            return;
        }
        if(control.id==ControlId::SwitchRowControl&&control.index>=0) {
            switch(control.index) {
            case 0: SetSetting("previewDiff",stored_.previewDiff?"0":"1");break;
            case 1: SetSetting("autoReloadGateway",stored_.autoReloadGateway?"0":"1");break;
            case 2: SetSetting("alwaysOnTop",stored_.topmost?"0":"1");break;
            case 3: SetSetting("latencyProbe",stored_.latencyProbe?"0":"1");break;
            default: break;
            }
            return;
        }
        if(control.id==ControlId::WidgetSeg&&control.index>=0) {
            static const char* kWidgetIds[4]={"beacon","tag","flag","capsule"};
            SetSetting("widgetStyle",kWidgetIds[std::clamp(control.index,0,3)]);
            return;
        }
        if(control.id==ControlId::LatencySeg&&control.index>=0) {
            static const int kMinutes[3]={5,15,60};
            SetSetting("latencyMinutes",std::to_string(kMinutes[std::clamp(control.index,0,2)]));
            return;
        }
        if(control.id==ControlId::ThemeSeg&&control.index>=0) {
            static const char* kThemeIds[7]={"dark","light","system","slate","carbon","porcelain","sand"};
            const auto index=std::clamp(control.index,0,6);
            SetSetting("theme",kThemeIds[index]);
            return;
        }
        if(control.id==ControlId::OpenWebUiButton) { OpenWebUi();return; }
        if(control.id==ControlId::SetProfiles) {
            profileError_.clear();
            pending_=Pending::ProfileMenu;
            InvalidateAll();
            return;
        }
        if(control.id==ControlId::SetDiagnose||control.id==ControlId::SetUsage) {
            // F2/F3：先把模态打开显示「正在读取…」，助手结果回来后在原卡上填事实。
            const bool usage=control.id==ControlId::SetUsage;
            modalScrollX_=0;
            readTitle_=usage?Narrow(L"用量统计"):Narrow(L"诊断");
            readFacts_.clear();
            readFacts_.push_back(Narrow(L"正在读取…"));
            pending_=usage?Pending::UsageInfo:Pending::DiagnoseInfo;
            StartReadHelper(usage?"usage":"diagnose");
            return;
        }
        if(control.id==ControlId::SetRestartGateway) {
            // 手动重启网关（真机反馈：自动重启关掉后找不到手动重启的入口）。mode auto：优先
            // reload，失败再 restart——与托盘「重启网关」同一份动作。
            StartHelperAction("reload",{{"cmd","reload"},{"targets",{"gateway"}},{"mode","auto"}},
                Narrow(L"\u7F51\u5173\u5DF2\u91CD\u542F\uFF0C\u673A\u5668\u4EBA\u5DF2\u7528\u4E0A"
                    L"\u65B0\u6A21\u578B"),true);
            return;
        }
    }
    // 点在列表和字段之外：展开中的下拉收起（点选项才应用，点别处只收起）。
    if(settingsSelectOpen_) { settingsSelectOpen_=false;InvalidateAll(); }
    BlurField();
}
void Application::SetSetting(const std::string& key,const std::string& value) {
    // 设置页的动作（旧 setSetting）：写进本地设置文件，热键与主题要立刻生效。
    if(key=="theme") {
        stored_.theme=value;
        // 主题交叉淡入：当前调色板 → 新主题，180ms 内混合（绘制层只认一份 palette）。
        // 目标在这里显式交进去：StartThemeMotion 不再拿 palette_ 当目标。
        StartThemeMotion(ResolveTheme(stored_.theme));
    }
    else if(key=="distro") stored_.distro=value;
    else if(key=="webuiPort") { stored_.webuiPort=std::clamp(atoi(value.c_str()),1,65535); }
    else if(key=="pollSeconds") {
        stored_.pollSeconds=std::clamp(atoi(value.c_str()),5,3600);
        UpdatePollTimer();
    } else if(key=="previewDiff") stored_.previewDiff=value=="1";
    else if(key=="autoReloadGateway") stored_.autoReloadGateway=value=="1";
    else if(key=="latencyProbe") { stored_.latencyProbe=value=="1";UpdateLatencyTimer(); }
    else if(key=="latencyMinutes") {
        const auto minutes=atoi(value.c_str());
        stored_.latencyMinutes=minutes==15?15:(minutes==60?60:5);
        UpdateLatencyTimer();
    }
    else if(key=="widgetStyle") {
        stored_.widgetStyle=value;
        model_.widget=WidgetStyleFromName(value);
        // 换样式会改挂件尺寸：只要不是隐藏/拖动中就把两个窗口都按新尺寸落一次（贴边收回时
        // 挂件窗口才是新形状）。浮动态只重绘挂件窗口，不动面板。
        if(state_!=State::Hidden&&state_!=State::Dragging)
            geometry_.Place(model_,monitor_.geometry,state_,"widget-style");
        UpdateWidgetTimer();
        if(state_==State::Collapsed) RenderStrip();
        else if(strip_) InvalidateRect(strip_,nullptr,FALSE);
    }
    else if(key=="alwaysOnTop") {
        stored_.topmost=value=="1";
        SyncTopmostStyle();
    } else if(key=="closeAction") stored_.closeAction=value=="quit"?"quit":"hide";
    else if(key=="hotkey") {
        stored_.hotkey=value;
        RegisterHotkeys();          // 先注销同 id 再按新组合注册（同值不重注册也不会误报失败）
    }
    PersistSettings(true);
    if(log_) log_->Info("settings","set "+key+(key=="hotkey"?"":" = "+value));
    InvalidateAll();
}
void Application::CaptureHotkey(WPARAM key) {
    // 热键捕获（旧 #set-hotkey keydown）：记录修饰键 + 主键，形如 Control+Alt+H。
    if(!setHotkeyCapturing_) return;
    if(key==VK_ESCAPE) {
        // ESC = 放弃这次捕获（与点输入框外同义）：不写入，清旗标，调用方跟着 BlurField 还原显示。
        setHotkeyCapturing_=false;
        return;
    }
    const int synthetic=syntheticMods_;
    const bool ctrl=(synthetic&1)!=0||GetKeyState(VK_CONTROL)<0;
    const bool alt=(synthetic&2)!=0||GetKeyState(VK_MENU)<0;
    const bool shift=(synthetic&4)!=0||GetKeyState(VK_SHIFT)<0;
    const auto name=[&](WPARAM vk) -> std::string {
        if(vk>=VK_F1&&vk<=VK_F9) return "F"+std::to_string(vk-VK_F1+1);
        if(vk>='A'&&vk<='Z') return std::string(1,static_cast<char>(vk));
        if(vk>='0'&&vk<='9') return std::string(1,static_cast<char>(vk));
        return {};
    };
    const auto main=name(key);
    if(main.empty()) return;
    std::string combination;
    if(ctrl) combination+="Control+";
    if(alt) combination+="Alt+";
    if(shift) combination+="Shift+";
    if(combination.empty()) return;                       // 旧版要求至少一个修饰键
    combination+=main;
    setHotkeyCapturing_=false;
    setHotkey_.SetTextUtf8(combination);
    SetSetting("hotkey",combination);
}
bool Application::HandleChromeClick(float x,float y) {
    // 底栏与前四条标题/寄存器控件在每一页都要能用：页面自己的点击处理会吃掉落在正文里的点，
    // 所以先看一次"是不是外壳控件"。
    const auto laid=LayoutForInput();
    const auto id=ControlAt(laid,x,y);
    switch(id) {
    case ControlId::Snap: case ControlId::Pin: case ControlId::Minimize: case ControlId::Close:
    case ControlId::Refresh: case ControlId::Chip: case ControlId::ChipWarn:
    case ControlId::ChipHelper: case ControlId::RegisterRestartGateway: case ControlId::StatusPill:
    case ControlId::LampWsl: case ControlId::LampWebui: case ControlId::LampGateway:
    case ControlId::TabModels: case ControlId::TabFallback: case ControlId::TabAdd:
    case ControlId::TabBackup: case ControlId::TabSettings:
        InvokeControl(id);
        return true;
    default: return false;
    }
}
void Application::FallbackPageClick(float x,float y) {
    const auto laid=LayoutForInput();
    if(pending_==Pending::None&&HandleChromeClick(x,y)) return;
    // 模态优先：它盖在整页上（Enter/Esc 也在键盘路径里处理）。
    if(HandleModalClick(x,y)) return;
    // 展开中的下拉：先吃掉它的点击（点空白即收起，与旧 select 一致）。
    if(fbFormOpen_&&fbSelectOpen_) {
        const auto option=SelectOptionAt(laid,x,y);
        if(option>=0) {
            fbSource_=option;
            fbSelectOpen_=false;
            selectScroll_=0;
            // 选到具体供应商时把它的名字放进「provider 名」（手填模式才用得到）。
            if(option>0&&option-1<static_cast<int>(hermesState_.providers.size()))
                fbProvider_.SetTextUtf8(hermesState_.providers[static_cast<std::size_t>(option-1)].name);
            InvalidateAll();
            return;
        }
        fbSelectOpen_=false;
        selectScroll_=0;
        InvalidateAll();
        return;
    }
    const auto id=ControlAt(laid,x,y);
    switch(id) {
    case ControlId::FbProbe: {
        // 逐条探测（旧 btn-fb-probe）：一次跑完整条链；进行中忽略重复点击（按钮已显示进度）。
        if(probeRunning_.load()) return;
        const auto before=fbProbes_;
        (void)before;
        Json args;
        args["cmd"]="fallback-probe";
        args["index"]=0;
        StartHelperAction("fallback-probe",args,std::string{},false,true);
        return;
    }
    case ControlId::FbFoldToggle:
        fbFormOpen_=true;
        fbError_.clear();
        InvalidateAll();
        return;
    case ControlId::FbFoldClose: CloseForm();return;
    case ControlId::FbSubmit: SubmitFallback();return;
    case ControlId::FbSelect:
        fbSelectOpen_=!fbSelectOpen_;
        selectScroll_=0;
        InvalidateAll();
        return;
    case ControlId::FormField: {
        const auto* entry=ControlEntryAt(laid,ControlId::FormField,x,y);
        if(!entry) return;
        if(entry->index==0) {
            // 字段 0 就是「来源供应商」下拉：点它展开/收起（旧 select）。
            fbSelectOpen_=!fbSelectOpen_;
            InvalidateAll();
            return;
        }
        // 其余字段：焦点进去（API Key 按掩码显示，与旧 input[type=password] 一致）。
        auto* field=FormFieldAt(entry->index);
        if(field) FocusField(field);
        return;
    }
    default: break;
    }
    // 阶梯行：三个按钮（上移/下移/移除）；点空白处不做任何事（旧行没有点击行为）。
    if(x>=laid.ladder.left&&x<laid.ladder.right) {
        for(const auto& row:laid.ladderRows) {
            if(y<row.rect.top||y>=row.rect.bottom) continue;
            switch(LadderButtonAt(row.rect,x,laid.scale)) {
            case 1: {
                if(row.row==0) return;
                Json args;
                args["cmd"]="fallback-move";
                args["index"]=row.row;
                // 助手契约（与旧版 main.js 的白名单一致）只有 index/to；曾经发 delta，
                // 真实助手 int(payload.get("to")) 直接 TypeError，↑↓ 在真机上完全无效。
                args["to"]=row.row-1;
                StartHelperAction("fallback-move",args,
                    Narrow(L"\u5DF2\u4E0A\u79FB\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
                return;
            }
            case 2: {
                if(row.row+1>=static_cast<int>(hermesState_.fallbacks.size())) return;
                Json args;
                args["cmd"]="fallback-move";
                args["index"]=row.row;
                args["to"]=row.row+1;
                StartHelperAction("fallback-move",args,
                    Narrow(L"\u5DF2\u4E0B\u79FB\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
                return;
            }
            case 3:
                pending_=Pending::RemoveFallback;
                pendingIndex_=row.row;
                InvalidateAll();
                return;
            default: return;
            }
        }
    }
}
TextField* Application::FormFieldAt(int index) {
    switch(index) {
    case 1: return &fbModel_;
    case 2: return &fbProvider_;
    case 3: return &fbUrl_;
    case 4: return &fbKeyEnv_;
    case 5: return &fbKey_;
    default: return nullptr;
    }
}
void Application::SubmitFallback() {
    fbError_.clear();
    const auto model=fbModel_.TextUtf8();
    if(model.empty()) { fbError_="\xE5\xA1\xAB\xE4\xB8\xAA\xE6\xA8\xA1\xE5\x9E\x8B id";InvalidateAll();return; }
    Json args;
    args["cmd"]="fallback-add";
    args["model"]=model;
    if(fbSource_>0) {
        const auto index=fbSource_-1;
        if(index<static_cast<int>(hermesState_.providers.size()))
            args["from_custom"]=hermesState_.providers[static_cast<std::size_t>(index)].providerId;
    } else {
        const auto provider=fbProvider_.TextUtf8();
        const auto url=fbUrl_.TextUtf8();
        if(provider.empty()||url.empty()) {
            fbError_="\xE6\x89\x8B\xE5\xA1\xAB\xE6\xA8\xA1\xE5\xBC\x8F\xE8\xA6\x81 provider \xE5\x90\x8D + base_url";
            InvalidateAll();
            return;
        }
        args["provider"]=provider;
        args["base_url"]=url;
        const auto keyEnv=fbKeyEnv_.TextUtf8();
        if(!keyEnv.empty()) {
            args["key_env"]=keyEnv;
            const auto key=fbKey_.TextUtf8();
            if(!key.empty()) args["key"]=key;
        }
    }
    for(auto* field:{&fbModel_,&fbProvider_,&fbUrl_,&fbKeyEnv_,&fbKey_}) field->SetText(L"");
    fbProbes_.clear();
    StartHelperAction("fallback-add",args,
        Narrow(L"\u5DF2\u8FFD\u52A0\u5230\u56DE\u9000\u94FE\u672B\u5C3E\uFF08\u7F51\u5173\u70ED\u8BFB\u751F\u6548\uFF09"));
}
int Application::SelectOptionAt(const PanelLayout& layout,float x,float y) const {
    // 返回**列表里的绝对下标**（control.index 已经带上滚动量），不是"第几个可见行"。
    for(const auto& option:layout.selectOptions) {
        const auto& box=option.rect;
        if(x>=box.left&&x<box.right&&y>=box.top&&y<box.bottom) return option.index;
    }
    return -1;
}
void Application::ModelsPageClick(float x,float y) {
    const auto laid=LayoutForInput();
    const auto view=CurrentView();
    // 模态是全局浮层（切模型确认卡就在模型页弹出）：必须最先吃掉点击，否则确认按钮
    // 会被当成列表点击丢掉（h-models 门禁抓到"确认后没切换"）。
    if(HandleModalClick(x,y)) return;
    // The scrollbar is checked first: a drag that starts on the thumb must not also switch a model.
    if(laid.scrollVisible&&x>=laid.scrollTrack.left&&x<laid.scrollTrack.right
        &&y>=laid.listViewport.top&&y<laid.listViewport.bottom) {
        if(y>=laid.scrollThumb.top&&y<laid.scrollThumb.bottom) {
            scrollDrag_=true;
            scrollGrabOffset_=y-laid.scrollThumb.top;
        } else {
            const float centre=(laid.scrollThumb.top+laid.scrollThumb.bottom)/2.0f;
            listScroll_=std::clamp(listScroll_+(y<centre?-1.0f:1.0f)
                *(laid.listViewport.bottom-laid.listViewport.top)*0.9f,0.0f,laid.listMaxScroll);
            InvalidateAll();
        }
        return;
    }
    const auto hit=ListHitTest(laid,x,y);
    if(log_) log_->Info("ui","list click kind="+std::to_string(static_cast<int>(hit.kind))
        +" group="+std::to_string(hit.group)+" row="+std::to_string(hit.row)
        +" star="+(hit.star?"1":"0")+" inList="+(hit.inList?"1":"0"));
    if(!hit.inList) return;
    if(hit.kind==ListItemKind::GroupHead) {
        if(hit.group<0||hit.group>=static_cast<int>(view.groups.size())) return;
        if(hit.menu) {
            // 旧 openProviderMenu：弹出这个供应商的信息 + 动作（探测模型并追加 / 设为当前 / 复制名字 /
            // 重启网关 / 删除）。原生版用模态承载（旧版是底部 action sheet）。
            pendingProvider_=view.groups[static_cast<std::size_t>(hit.group)].title;
            pending_=Pending::ProviderMenu;
            if(log_) log_->Info("ui","provider menu opened for "+pendingProvider_);
            InvalidateAll();
            return;
        }
        const auto name=view.groups[static_cast<std::size_t>(hit.group)].title;
        const auto found=std::find(expanded_.begin(),expanded_.end(),name);
        if(found==expanded_.end()) expanded_.push_back(name);
        else expanded_.erase(found);
        cursor_=-1;
        InvalidateAll();
        return;
    }
    if(hit.kind==ListItemKind::Row) {
        if(hit.group<0||hit.row<0||hit.group>=static_cast<int>(view.groups.size())) return;
        const auto& group=view.groups[static_cast<std::size_t>(hit.group)];
        if(hit.row>=static_cast<int>(group.rows.size())) return;
        const auto& row=group.rows[static_cast<std::size_t>(hit.row)];
        if(hit.star) ToggleFavorite(row.key);
        else SwitchModel(row.providerId,row.model);
    }
}
bool Application::HandleListKey(WPARAM key) {
    switch(key) {
    case VK_DOWN: ListMoveCursor(1);return true;
    case VK_UP: ListMoveCursor(-1);return true;
    case VK_RETURN: ListSelectCursor();return true;
    case VK_ESCAPE:
        if(cursor_>=0) { cursor_=-1;InvalidateAll();return true; }
        return false;
    default: return false;
    }
}
void Application::OpenWebUi() {
    const int port=hermesState_.webui.port>0?hermesState_.webui.port:stored_.webuiPort;
    const std::wstring url=L"http://127.0.0.1:"+std::to_wstring(port);
    ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
void Application::InvokeControl(ControlId id) {
    // 离线时底栏整条不可用（旧 body.offline .nav .tab 的 pointer-events: none）：只有「启动 WSL」有效。
    const bool offline=hermesState_.wsl=="down";
    const auto isTab=id==ControlId::TabModels||id==ControlId::TabFallback||id==ControlId::TabAdd
        ||id==ControlId::TabBackup||id==ControlId::TabSettings;
    if(offline&&isTab) {
        if(log_) log_->Info("ui","tab ignored while offline");
        return;
    }
    switch(id) {
    case ControlId::Snap: InvokeMenu(static_cast<unsigned>(MenuCommand::Dock));return;
    case ControlId::Pin: InvokeMenu(static_cast<unsigned>(MenuCommand::AlwaysOnTop));return;
    case ControlId::Minimize: ShowWindow(panel_,SW_MINIMIZE);return;
    case ControlId::Close:
        // 旧版 win.on('close')：closeAction=quit 时退出，否则隐藏到托盘。
        // 审计 F-004：这里曾经无条件 ToggleVisibility，"退出程序"是个死设置。
        if(stored_.closeAction=="quit") {
            PersistSettings(true);
            if(control_) PostMessageW(control_,WM_CLOSE,0,0);
        } else {
            InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));
        }
        return;
    // Bottom navigation: the five pages of the old panel, in the same order.
    // 页面切换做一次内容淡入（140ms，无位移）：与余额面板的页面切换动效一致。
    case ControlId::TabModels: BlurField();statusCenterOpen_=false;activePage_=0;pageScroll_=0;PersistSettings();StartPanelMotion(false);InvalidateAll();return;
    case ControlId::TabFallback: statusCenterOpen_=false;activePage_=1;fallbackScroll_=0;PersistSettings();StartPanelMotion(false);InvalidateAll();return;
    case ControlId::TabAdd: statusCenterOpen_=false;activePage_=2;pageScroll_=0;PersistSettings();StartPanelMotion(false);InvalidateAll();return;
    case ControlId::TabBackup:
        statusCenterOpen_=false;
        activePage_=3;
        pageScroll_=0;
        LoadBackups();
        PersistSettings();
        StartPanelMotion(false);
        InvalidateAll();
        return;
    case ControlId::TabSettings: statusCenterOpen_=false;activePage_=4;pageScroll_=0;PersistSettings();StartPanelMotion(false);InvalidateAll();return;
    case ControlId::Refresh: InvokeMenu(static_cast<unsigned>(MenuCommand::Refresh));return;
    // The models-page controls: filter segments and the two action chips.
    case ControlId::FilterAll: CycleFilter(0);return;
    case ControlId::FilterFav: CycleFilter(1);return;
    case ControlId::FilterRecent: CycleFilter(2);return;
    case ControlId::Chip: RefreshData();return;                       // WebUI 不可达：点一下重试
    case ControlId::ChipHelper: RefreshData();return;                 // 助手出错：点一下重试
    case ControlId::ChipWarn: InvokeMenu(static_cast<unsigned>(MenuCommand::RestartGateway));return;
    case ControlId::RegisterRestartGateway: InvokeMenu(static_cast<unsigned>(MenuCommand::RestartGateway));return;
    // 审计 F-016：DESIGN §4.2 #4「异常时灯体可点击重试」；点击任一灯 = 重新读取并探活。
    case ControlId::LampWsl:
    case ControlId::LampWebui:
    case ControlId::LampGateway: RefreshData();return;
    case ControlId::StatusPill: statusCenterOpen_=!statusCenterOpen_;InvalidateAll();return;
    default: return;
    }
}
void Application::InvokeStatusAction(StatusAction action) {
    switch(action) {
    case StatusAction::RestartGateway:
    case StatusAction::RestartConfig:
        InvokeMenu(static_cast<unsigned>(MenuCommand::RestartGateway));
        return;
    case StatusAction::RetryWebUi:
    case StatusAction::RetryHelper:
        RefreshData();
        return;
    case StatusAction::None:
        return;
    }
}
}
