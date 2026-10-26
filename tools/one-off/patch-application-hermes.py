"""H2a step 2: turn the remaining balance references in application.cpp into the hermes shell.

Every replacement is anchored on text that exists today, and the script reports misses instead of
silently skipping them.
"""
import pathlib

SOURCE = pathlib.Path("src/app/application.cpp")

REPLACEMENTS = [
    # 1) menu: the two site entries become the hermes actions
    ('''        {static_cast<unsigned>(MenuCommand::ViewChatAi),"site-chatai",L"查看 ChatAI"},
        {static_cast<unsigned>(MenuCommand::ViewDoulor),"site-doulor",L"查看 Doulor"},
        {static_cast<unsigned>(MenuCommand::CompactView),"compact",L"紧凑视图"},''',
     '''        {static_cast<unsigned>(MenuCommand::Refresh),"refresh",L"刷新状态"},
        {static_cast<unsigned>(MenuCommand::OpenWebUI),"open-webui",L"打开 WebUI"},
        {static_cast<unsigned>(MenuCommand::RestartGateway),"restart-gateway",L"重启网关"},
        {static_cast<unsigned>(MenuCommand::CompactView),"compact",L"紧凑视图"},'''),

    # 2) menu dispatch: hermes actions replace the site switches
    ('''    case MenuCommand::ViewChatAi: stored_.activeSite="chatai";Show();PersistSettings();InvalidateAll();return;
    case MenuCommand::ViewDoulor: stored_.activeSite="doulor";Show();PersistSettings();InvalidateAll();return;''',
     '''    case MenuCommand::Refresh: RefreshData();InvalidateAll();return;
    case MenuCommand::OpenWebUI: OpenWebUi();return;
    case MenuCommand::RestartGateway:
        // The gateway is never restarted automatically (old behaviour): the chip asks, the user clicks.
        if(log_) log_->Info("gateway","restart requested (wired in H6)");
        return;'''),

    # 3) startup: no stored sessions, no history store, no legacy import, no data layer
    ('''    palette_=ResolveTheme(stored_.theme);
    sessionPath_=Narrow(DirectoryOfPath(settings_?settings_->Path():L"")+L"\\\\session.dpapi");
    if(const auto restored=LoadSecret(std::wstring(sessionPath_.begin(),sessionPath_.end()))) {
        // A previous session is reused on startup; the cookie value stays in memory.
        sessionCookie_=*restored;
        data_.SetCookie(SiteId::ChatAi,sessionCookie_);
        sessionSaved_=true;
        if(log_) log_->Info("login","restored a stored session ("+std::to_string(sessionCookie_.size())+" bytes)");
    }
    history_=HistoryStore(DirectoryOfPath(settings_?settings_->Path():L"")+L"\\\\history");
    if(log_) log_->Info("app","history directory "+Narrow(history_.Directory()));
    // A second DPAPI file holds the Doulor session; both files are replaced atomically and never logged.
    doulorSessionPath_=Narrow(DirectoryOfPath(settings_?settings_->Path():L"")+L"\\\\session-doulor.dpapi");
    if(const auto restored=LoadSecret(std::wstring(doulorSessionPath_.begin(),doulorSessionPath_.end()))) {
        doulorCookie_=*restored;
        data_.SetCookie(SiteId::Doulor,doulorCookie_);
        if(log_) log_->Info("login","restored the Doulor session ("+std::to_string(doulorCookie_.size())+" bytes)");
    }
    if(settings_) {''',
     '''    // The hermes panel has no stored session of its own: the WSL helper owns every secret.
    palette_=ResolveTheme(stored_.theme);
    if(settings_) {'''),

    ('''    // The import runs after the settings are loaded (a load would otherwise overwrite what it set) and
    // before the geometry is placed, so the imported edge / tier / position are what the panel opens with.
    ImportLegacyData();
    const auto size=TierSize(model_.tier,monitor_.geometry.dpi);''',
     '''    const auto size=TierSize(model_.tier,monitor_.geometry.dpi);'''),

    ('''    ConfigureDataLayer(stored_.chatApiBase,stored_.doulorApiBase,stored_.pollSeconds);
    return true;''',
     '''    RefreshData();
    return true;'''),

    ('''Application::~Application() {
    data_.Stop();
    RemoveTray();''',
     '''Application::~Application() {
    RemoveTray();'''),

    # 4) window titles
    ('L"HermesModelPanelNative.Panel",L"BalancePanel Native",', 'L"HermesModelPanelNative.Panel",L"Hermes \x6A21\x578B\x9762\x677F",'),
    ('L"HermesModelPanelNative.Strip",L"BalancePanel Native Strip",', 'L"HermesModelPanelNative.Strip",L"Hermes \x6A21\x578B\x9762\x677F",'),
]

# The view, control dispatch and dump are replaced wholesale (they are short and fully balance-specific).
VIEW = r'''PanelViewModel Application::CurrentView() {
    PanelViewInput input;
    input.tier=model_.tier;
    input.lightTheme=palette_.light;
    input.activePage=activePage_;
    input.hoverControl=hoverControl_;
    input.pinned=stored_.topmost;
    return BuildPanelView(input);
}
void Application::RefreshData() {
    // Wired in H3 (helper bridge + probes). Until then the panel shows its chrome with idle lamps, and
    // every write path is disabled because loading stays true.
    if(log_) log_->Info("data","refresh requested (helper bridge lands in H3)");
}
void Application::OpenWebUi() {
    const int port=stored_.pollSeconds>0?8787:8787;      // webuiPort joins the settings in H3
    const std::wstring url=L"http://127.0.0.1:"+std::to_wstring(port);
    ShellExecuteW(nullptr,L"open",url.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
}
'''

CONTROL = r'''void Application::InvokeControl(ControlId id) {
    switch(id) {
    case ControlId::Snap: InvokeMenu(static_cast<unsigned>(MenuCommand::Dock));return;
    case ControlId::Pin: InvokeMenu(static_cast<unsigned>(MenuCommand::AlwaysOnTop));return;
    case ControlId::Minimize: ShowWindow(panel_,SW_MINIMIZE);return;
    case ControlId::Close: InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));return;
    // Bottom navigation: the five pages of the old panel, in the same order.
    case ControlId::TabChat: activePage_=0;PersistSettings();InvalidateAll();return;
    case ControlId::TabDoulor: activePage_=1;PersistSettings();InvalidateAll();return;
    case ControlId::Trend: activePage_=2;PersistSettings();InvalidateAll();return;
    case ControlId::Compact: activePage_=3;PersistSettings();InvalidateAll();return;
    case ControlId::TrendBack: activePage_=4;PersistSettings();InvalidateAll();return;
    case ControlId::Refresh: InvokeMenu(static_cast<unsigned>(MenuCommand::Refresh));return;
    default: return;
    }
}
'''

DUMP = r'''Json Application::Dump() const {
    const auto expected=ComputeRect(model_,monitor_.geometry);
    Json menu=Json::array();
    if(const auto handle=BuildMenu()) {
        const int count=GetMenuItemCount(handle);
        for(int i=0;i<count;++i) {
            MENUITEMINFOW info{sizeof(info)};
            info.fMask=MIIM_ID|MIIM_STATE|MIIM_STRING;
            wchar_t text[64]{};
            info.dwTypeData=text;info.cch=static_cast<UINT>(std::size(text));
            if(!GetMenuItemInfoW(handle,static_cast<UINT>(i),TRUE,&info)) continue;
            const MenuEntry* entry=nullptr;
            for(const auto& candidate:MenuEntries()) if(candidate.id==info.wID) entry=&candidate;
            if(!entry) continue;
            menu.push_back({{"id",info.wID},{"token",entry->token},{"text",Narrow(text)},
                {"checked",(info.fState&MFS_CHECKED)!=0},{"enabled",(info.fState&MFS_DISABLED)==0}});
        }
    }
    const auto rect=GeometryController::Read(panel_);
    const auto strip=GeometryController::Read(strip_);
    return Json{
        {"instance",Narrow(control_?[this]{wchar_t name[128]{};GetWindowTextW(control_,name,128);return std::wstring(name);}():L"")},
        {"state",static_cast<int>(state_)},{"edge",static_cast<int>(model_.edge)},
        {"anchor",model_.anchor},{"compact",model_.tier==Tier::Compact},
        {"pinned",stored_.topmost},{"activePage",activePage_},
        {"pollSeconds",stored_.pollSeconds},{"topmost",stored_.topmost},
        {"panel",{rect.x,rect.y,rect.width,rect.height}},
        {"strip",{strip.x,strip.y,strip.width,strip.height}},
        {"expectedPanel",{expected.panel.x,expected.panel.y,expected.panel.width,expected.panel.height}},
        {"expectedStrip",expected.strip?Json{expected.strip->x,expected.strip->y,expected.strip->width,expected.strip->height}:Json{}},
        {"work",{monitor_.geometry.work.x,monitor_.geometry.work.y,monitor_.geometry.work.width,monitor_.geometry.work.height}},
        {"dpi",static_cast<int>(monitor_.geometry.dpi)},
        {"visible",state_!=State::Hidden},{"stripVisible",IsWindowVisible(strip_)!=0},
        {"panelVisible",IsWindowVisible(panel_)!=0},
        {"clickThrough",(GetWindowLongPtrW(panel_,GWL_EXSTYLE)&WS_EX_TRANSPARENT)!=0},
        {"panelHwnd",reinterpret_cast<std::uintptr_t>(panel_)},
        {"stripHwnd",reinterpret_cast<std::uintptr_t>(strip_)},
        {"writes",geometry_.Writes()},{"violations",geometry_.Violations()},
        {"drops",drops_},{"tierWrites",tierWrites_},
        {"stripMoves",stripMoves_},{"panelMoves",panelMoves_},{"panelLeaves",panelLeaves_},{"graces",graces_},
        {"wakeCount",wakeCount_},{"themeLight",palette_.light},
        {"settingsPath",Narrow(settings_?settings_->Path():L"")},
        {"audit",[this]{Json list=Json::array();for(const auto& entry:geometry_.Audit())list.push_back(entry);return list;}()},
        {"menu",menu}};
}
'''

COMMAND_CUTS = [
    # control-plane commands that only made sense with the balance login/data layer
    ('''    if(command=="cookies") {
        login_.QueryCookies();
        return Dump();
    }''', None),
    ('''    if(command=="injectCookie") {
        login_.InjectCookie(request.value("name",std::string{}),request.value("value",std::string{}),
            request.value("domain",std::string{}));
        return Dump();
    }''', None),
]


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    for old, new in REPLACEMENTS:
        if old not in text:
            print("MISS:", old.strip().splitlines()[0][:70])
            continue
        text = text.replace(old, new, 1)
    # whole-function replacements
    import re
    for name, body in (("PanelViewModel Application::CurrentView", VIEW),
                       ("void Application::InvokeControl", CONTROL),
                       ("Json Application::Dump", DUMP)):
        pattern = re.compile(re.escape(name) + r"\([^)]*\)( const)? \{(?:.|\n)*?\n\}\n", re.MULTILINE)
        if not pattern.search(text):
            print("MISS function:", name)
            continue
        text = pattern.sub(lambda _match: body, text, count=1)
    SOURCE.write_text(text, encoding="utf-8", newline="")
    print("application.cpp patched")


if __name__ == "__main__":
    main()
