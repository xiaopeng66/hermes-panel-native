#pragma once
#include <string>

namespace harness {
// CreateProcessW wants a native (backslash) path. CMake passes $<TARGET_FILE:...> with whatever
// separators the configured project path used, so a run configured from Git Bash hands us forward
// slashes and the launch fails with a bare "CreateProcess failed". Normalise before every launch.
inline std::wstring NativePath(std::wstring path) {
    for (auto& character : path) if (character == L'/') character = L'\\';
    return path;
}
}
