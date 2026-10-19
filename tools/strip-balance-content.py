"""H2a step 1: remove the balance content layer from the copied application shell.

The shell (geometry, windows, tray, hotkeys, settings, theme, logging, control plane) stays; every
balance-specific function, member and include goes. The script deletes whole function bodies by line
range and reports what is left to fix by hand, so the tree never sits half-edited by guesswork.
"""
import pathlib
import re

SOURCE = pathlib.Path("src/app/application.cpp")

# Functions that only exist for the balance product (diagnostics, login window, quota polling,
# legacy import). Order does not matter; ranges are computed from the top-level definitions.
DEAD_FUNCTIONS = [
    "void Application::RunDiagnose",
    "void Application::ConfigureDataLayer",
    "void Application::OpenLogin",
    "void Application::OnSessionCaptured",
    "void Application::RefreshChatViaPage",
    "void Application::OnPagePayload",
    "void Application::ApplyDataUpdate",
    "void Application::ImportLegacyData",
    "void Application::NotifyMigration",
]

DEAD_INCLUDES = [
    '#include "data/datalayer.h"',
    '#include "data/history.h"',
    '#include "data/session.h"',
    '#include "data/credentials.h"',
    '#include "data/migration.h"',
    '#include "ui/login/login_window.h"',
]


def top_level_starts(lines):
    return [index for index, line in enumerate(lines) if re.match(r"^[A-Za-z].*Application::", line)]


def main() -> None:
    text = SOURCE.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    starts = top_level_starts(lines)
    removed = 0
    # Delete from the last dead function backwards so earlier indices stay valid.
    for name in DEAD_FUNCTIONS:
        index = next((i for i in starts if lines[i].startswith(name)), None)
        if index is None:
            print("MISS", name)
            continue
        following = next((i for i in starts if i > index), len(lines))
        del lines[index:following]
        starts = top_level_starts(lines)
        removed += 1
    text = "".join(lines)
    for include in DEAD_INCLUDES:
        text = text.replace(include + "\n", "")
    SOURCE.write_text(text, encoding="utf-8", newline="")
    print(f"removed {removed} function(s)")
    # Report the remaining references so the next edit is driven by facts, not guesses.
    leftovers = []
    for token in ["login_", "sessionCookie_", "doulorCookie_", "data_.", "history_", "site_",
                  "Credentials(", "MigrationResult", "chat_", "doulor_", "loginUrl_"]:
        for number, line in enumerate(text.splitlines(), 1):
            if token in line:
                leftovers.append((number, token, line.strip()[:110]))
    print(f"remaining references to fix: {len(leftovers)}")
    for number, token, line in leftovers[:40]:
        print(f"  {number:>5} [{token}] {line}")
    if len(leftovers) > 40:
        print(f"  ... and {len(leftovers) - 40} more")


if __name__ == "__main__":
    main()
