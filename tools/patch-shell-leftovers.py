"""H2a step 3: the last balance leftovers - menu flags, hotkeys, main.cpp CLI hooks."""
import pathlib

APPLICATION = pathlib.Path("src/app/application.cpp")
HEADER = pathlib.Path("src/app/application.h")
MAIN = pathlib.Path("src/app/main.cpp")

APP_EDITS = [
    # menu flags: the site checks are gone; the new actions carry no state
    ('''        case MenuCommand::ViewChatAi: if(stored_.activeSite=="chatai") flags|=MF_CHECKED;break;
        case MenuCommand::ViewDoulor: if(stored_.activeSite=="doulor") flags|=MF_CHECKED;break;
''', ''),
    # one hotkey, configurable later (H6); the old build had three chords
    ('''    // DESIGN §6.1: Ctrl+Alt+C (ChatAI), Ctrl+Alt+D (Doulor), Ctrl+Alt+B (toggle panel). Another
    // process may already own a chord, so each failure is recorded instead of assumed away.
    struct Attempt { Hotkey hotkey;WORD key; };
    const Attempt attempts[3]{{Hotkey::TogglePanel,'B'},{Hotkey::ViewChatAi,'C'},{Hotkey::ViewDoulor,'D'}};
    hotkeysRegistered_=true;
    for(int index=0;index<3;++index) {''',
     '''    // One chord: Ctrl+Alt+H toggles the panel (the old panel's default). Another process may already own
    // it, so the failure is recorded instead of assumed away; the settings page can change it later.
    struct Attempt { Hotkey hotkey;WORD key; };
    const Attempt attempts[1]{{Hotkey::TogglePanel,'H'}};
    hotkeysRegistered_=true;
    for(int index=0;index<1;++index) {'''),
    ('''    case Hotkey::TogglePanel: InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));return;
    case Hotkey::ViewChatAi: InvokeMenu(static_cast<unsigned>(MenuCommand::ViewChatAi));return;
    case Hotkey::ViewDoulor: InvokeMenu(static_cast<unsigned>(MenuCommand::ViewDoulor));return;''',
     '''    case Hotkey::TogglePanel: InvokeMenu(static_cast<unsigned>(MenuCommand::ToggleVisibility));return;'''),
]

HEADER_EDITS = [
    ('''    void InvalidateAll();''',
     '''    void InvalidateAll();
    // H3 wires these to the WSL helper and the probes; until then they only log, so the shell can run.
    void RefreshData();
    void OpenWebUi();'''),
]

MAIN_EDITS = [
    ('''    case WM_APP + 13:
        if (app) app->ApplyDataUpdate(reinterpret_cast<hermes::DataUpdate*>(lparam));
        return 0;''', ''),
    ('''    bool explicitDataDirectory = false;
    std::wstring dataDirectory = DataDirectory(argumentCount, arguments, &explicitDataDirectory);''',
     '''    bool explicitDataDirectory = false;
    std::wstring dataDirectory = DataDirectory(argumentCount, arguments, &explicitDataDirectory);
    static_cast<void>(explicitDataDirectory);'''),
    ('''    // --legacy-dir points the one-time import at a fixture (tests) or at a moved installation; without it
    // the importer reads %APPDATA%\\BalancePanel itself, and never for an isolated --data-dir run.
    std::wstring legacyDirectory;
    for (int index = 1; index + 1 < argumentCount; ++index) {
        if (std::wstring(arguments[index]) == L"--legacy-dir") legacyDirectory = arguments[index + 1];
    }
''', ''),
    ('''    app.SetIsolatedDataDirectory(explicitDataDirectory);
    app.SetInstanceName(id);
    if (!legacyDirectory.empty()) app.SetLegacyDirectory(legacyDirectory);
''', ''),
]


def apply(path, edits, label):
    text = path.read_text(encoding="utf-8")
    for old, new in edits:
        if old not in text:
            print("MISS", label + ":", old.strip().splitlines()[0][:64])
            continue
        text = text.replace(old, new, 1)
    path.write_text(text, encoding="utf-8", newline="")
    print("patched", label)


for path, edits, label in ((APPLICATION, APP_EDITS, "application.cpp"),
                           (HEADER, HEADER_EDITS, "application.h"),
                           (MAIN, MAIN_EDITS, "main.cpp")):
    apply(path, edits, label)
