#pragma once
#include <windows.h>
#include <string>

namespace hermes {
// Creates every missing level of a directory path. CreateDirectoryW only creates the last level, and
// a missing parent used to defeat persistence silently; the shell helper was unreliable for paths
// that mix forward slashes, so the chain is walked explicitly.
inline bool EnsureDirectory(const std::wstring& path) {
    if(path.size()<3) return false;
    for(std::size_t index=3;index<=path.size();++index) {
        if(index<path.size()&&path[index]!=L'\\'&&path[index]!=L'/') continue;
        CreateDirectoryW(path.substr(0,index).c_str(),nullptr);
    }
    CreateDirectoryW(path.c_str(),nullptr);
    const auto attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
}
// Windows accepts forward slashes in most calls but not reliably in wildcard searches, and the data
// directory arrives from --data-dir with forward slashes - so normalise before globbing.
inline std::wstring NormalizeSeparators(std::wstring path) {
    for(auto& character:path) if(character==L'/') character=L'\\';
    return path;
}
inline std::wstring DirectoryOfPath(const std::wstring& path) {
    const auto slash=path.find_last_of(L"\\/");
    return slash==std::wstring::npos?std::wstring{}:path.substr(0,slash);
}
}
