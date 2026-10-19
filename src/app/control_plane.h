#pragma once
// DESIGN §8: the debug control plane (named pipe + WM_COPYDATA command channel) exists in debug
// builds only, so a shipped release cannot be driven by another local process. CMake defines the
// macro per configuration; the fallbacks keep ad-hoc builds sane.
#ifndef HERMES_CONTROL_PLANE
#ifdef _DEBUG
#define HERMES_CONTROL_PLANE 1
#else
#define HERMES_CONTROL_PLANE 0
#endif
#endif
