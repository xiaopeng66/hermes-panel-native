"""Rename the copied shell from the balance product to the hermes product.

The copy starts as the balance-panel-native shell; every product string has to become hermes' before the
content is swapped, otherwise the two apps would share mutexes, data directories and tray identities.
"""
import pathlib

RENAMES = [
    # exe / target name (used by CMake, the tools scripts and the test harness --app argument)
    ("balance-panel-native", "hermes-panel-native"),
    # product / window class / mutex / pipe / data directory
    ("BalancePanelNative", "HermesModelPanelNative"),
]

# .manifest/.rc 原来漏在这里：应用清单的 assemblyIdentity 名（BalancePanelNative）就没被改到，
# 一直到 2026-10-25 整理时才手工补上。保留这行是为了下次改名不再漏。
SUFFIXES = {".cpp", ".h", ".txt", ".ps1", ".md", ".cmake", ".manifest", ".rc"}


def main() -> None:
    changed = []
    for path in pathlib.Path(".").rglob("*"):
        if path.is_dir():
            continue
        if path.suffix.lower() not in SUFFIXES:
            continue
        if any(part in {"build", "dist"} for part in path.parts):
            continue
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        original = text
        for old, new in RENAMES:
            text = text.replace(old, new)
        if text != original:
            # Explicit newline keeps CRLF exactly as it is: text mode would double the CR and
            # MSVC reports that as "Mac file format" (C4335, an error under /WX).
            path.write_text(text, encoding="utf-8", newline="")
            changed.append(str(path))
    print(f"renamed in {len(changed)} file(s)")
    for name in changed[:10]:
        print("  ", name)
    if len(changed) > 10:
        print(f"   ... and {len(changed) - 10} more")


if __name__ == "__main__":
    main()
