#pragma once
// A debug assertion must never open a modal dialog in a test binary: the run would sit on it until
// somebody clicks, which blocks the acceptance suite and the desktop with it. Report to stderr and let
// the process fail instead - the harness prints the failure and its scenario name.
//
// Call this first in every test main.
#include <crtdbg.h>

namespace harness {
inline void SilenceDebugAssertDialogs() {
#ifdef _DEBUG
    _CrtSetReportMode(_CRT_ASSERT,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT,_CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR,_CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR,_CRTDBG_FILE_STDERR);
    // abort() from an unhandled path should also stay off the desktop.
    _set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
#endif
}
}
