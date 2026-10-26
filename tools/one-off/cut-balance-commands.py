"""H2a step 4: cut the balance control-plane commands and the audit line that no longer type-checks."""
import pathlib
import re

SOURCE = pathlib.Path("src/app/application.cpp")
MAIN = pathlib.Path("src/app/main.cpp")

# Commands that only existed for the balance product's data/login/diagnostics.
DEAD_COMMANDS = ["configure", "session", "cookies", "injectCookie", "pageEcho", "pageFetch", "login",
                 "closeLogin", "diagnose"]


def cut_block(text: str, command: str) -> str:
    """Remove `if(command=="<cmd>") { ... }` including a trailing return, balanced on braces."""
    marker = f'if(command=="{command}")'
    index = text.find(marker)
    if index < 0:
        print("MISS command:", command)
        return text
    start = text.rfind("\n", 0, index) + 1
    cursor = text.index("{", index)
    depth = 0
    while cursor < len(text):
        if text[cursor] == "{":
            depth += 1
        elif text[cursor] == "}":
            depth -= 1
            if depth == 0:
                break
        cursor += 1
    end = cursor + 1
    if text[end:end + 1] == "\n":
        end += 1
    return text[:start] + text[end:]


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    for command in DEAD_COMMANDS:
        text = cut_block(text, command)
    # The geometry audit list holds a different json shape in the shell; keep the fields the tests read.
    text = text.replace(
        '        {"audit",[this]{Json list=Json::array();for(const auto& entry:geometry_.Audit())list.push_back(entry);return list;}()},\n',
        '')
    SOURCE.write_text(text, encoding="utf-8", newline="")
    print("application.cpp commands cut")

    main_text = MAIN.read_text(encoding="utf-8")
    pattern = re.compile(r"[ \t]*if \(std::wstring\(arguments\[index\]\) == L\"--diagnose\"\)(?:.|\n)*?\n(?=[ \t]*\}|\n)")
    if pattern.search(main_text):
        main_text = pattern.sub("", main_text, count=1)
        print("main.cpp --diagnose branch removed")
    for old in ('        app.RunDiagnose(std::wstring(dataDirectory) + L"\\\\diagnose.json");\n',
                '        app.RunDiagnose(std::wstring(dataDirectory) + L"\\\\diagnose.json");\r\n'):
        if old in main_text:
            main_text = main_text.replace(old, "")
    main_text = main_text.replace('    const auto diagnose = argumentValue(arguments, argumentCount, L"--diagnose");\n', '')
    MAIN.write_text(main_text, encoding="utf-8", newline="")
    print("main.cpp patched")


if __name__ == "__main__":
    main()
