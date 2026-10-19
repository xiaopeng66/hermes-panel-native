#pragma once
#include <string>
#include <vector>

namespace hermes {
// Application log with the redaction rules carried over from the old build: session cookies
// (`share-session`) and `userToken` values must never reach disk (DESIGN §6.5). Geometry writes are
// appended to the same file through the audited writer so a defect can be traced after the fact.
class Log {
public:
    explicit Log(std::wstring path);
    void Info(const std::string& channel,const std::string& message);
    void Warn(const std::string& channel,const std::string& message);
    void Error(const std::string& channel,const std::string& message);
    void Geometry(const std::string& message) { Info("geometry",message); }
    static std::string Redact(std::string text);
    unsigned Lines() const { return lines_; }
    const std::wstring& Path() const { return path_; }
private:
    void Append(const char* level,const std::string& channel,const std::string& message);
    std::wstring path_;
    unsigned lines_=0;
};
}
