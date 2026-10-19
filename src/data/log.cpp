#include "log.h"
#include <windows.h>
#include "paths.h"
#include <fstream>
#include <string>

namespace hermes {
namespace {
constexpr std::size_t kMaxValueLength=48;
// 单个日志文件上限：超过就轮转成 app.log.1（只留一份旧日志），长年运行不会撑爆磁盘。
constexpr unsigned long long kMaxLogBytes=5ull*1024ull*1024ull;
bool IsSeparator(char value) {
    return value==' '||value==':'||value=='='||value=='\x22'||value=='\x27';
}
// A value runs until a structural delimiter, so characters that are legal inside a password or a
// bearer token (for example '@' or '.') cannot leak by ending the scan early.
bool IsValueEnd(char value) {
    return value==' '||value=='\t'||value==','||value==';'||value=='&'||value=='}'||value==']'
        ||value=='\x22'||value=='\x27'||value=='\n'||value=='\r';
}
// Masks the value that follows a sensitive key, keeping the key and its separators so the log stays
// auditable. Returns the position the caller should continue from.
std::size_t MaskAfter(const std::string& text,std::size_t keyEnd,std::string& output) {
    std::size_t index=keyEnd;
    while(index<text.size()&&IsSeparator(text[index])) ++index;
    if(index==keyEnd) return keyEnd;                      // no separator: this is a different word
    const auto valueStart=index;
    while(index<text.size()&&index-valueStart<kMaxValueLength&&!IsValueEnd(text[index])) ++index;
    if(index==valueStart) return keyEnd;                  // separator only, no value to mask
    output+=text.substr(keyEnd,valueStart-keyEnd);        // keep the separator exactly as it was
    output+="<redacted>";
    return index;
}
std::string Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char buffer[32]{};
    std::snprintf(buffer,sizeof(buffer),"%04u-%02u-%02u %02u:%02u:%02u",
        now.wYear,now.wMonth,now.wDay,now.wHour,now.wMinute,now.wSecond);
    return buffer;
}
}
Log::Log(std::wstring path) : path_(std::move(path)) {
    const auto directory=DirectoryOfPath(path_);
    if(!directory.empty()) EnsureDirectory(directory);
}
std::string Log::Redact(std::string text) {
    // Never persist a session cookie or a bearer token, in any of the shapes the old build logged.
    // Exactly the keys the design forbids on disk, plus credential fields. Deliberately not the
    // bare words "session"/"token": redacting those would eat unrelated log text.
    for(const char* key:{"share-session","userToken","password","passwd","apiKey","api_key",
            "access_token","refresh_token","API_SERVER_KEY","api-key","apikey","client_secret"}) {
        const std::string needle(key);
        std::size_t position=0;
        while((position=text.find(needle,position))!=std::string::npos) {
            std::string rebuilt=text.substr(0,position+needle.size());
            const auto next=MaskAfter(text,position+needle.size(),rebuilt);
            rebuilt+=text.substr(next);
            if(rebuilt==text) { position+=needle.size();continue; }
            text=rebuilt;
            position+=needle.size()+std::string("<redacted>").size();
        }
    }
    // 标准鉴权头：Bearer 后面的一律当凭据（大小写两种写法都覆盖）。
    for(const char* prefix:{"Bearer ","bearer "}) {
        const std::string needle(prefix);
        std::size_t position=0;
        while((position=text.find(needle,position))!=std::string::npos) {
            std::string rebuilt=text.substr(0,position+needle.size()-1);
            const auto next=MaskAfter(text,position+needle.size()-1,rebuilt);
            rebuilt+=text.substr(next);
            if(rebuilt==text) { position+=needle.size();continue; }
            text=rebuilt;
            position+=needle.size()+std::string("<redacted>").size();
        }
    }
    // OpenAI 风格的裸 key（sk-...）：没有键名可依，只保留前缀。
    for(std::size_t position=0;(position=text.find("sk-",position))!=std::string::npos;) {
        auto end=position+3;
        while(end<text.size()&&end-position<kMaxValueLength&&!IsValueEnd(text[end])) ++end;
        if(end-position>=8) {
            text.replace(position+3,end-(position+3),"<redacted>");
            position+=3+std::string("<redacted>").size();
        } else {
            position+=3;
        }
    }
    return text;
}
void Log::Append(const char* level,const std::string& channel,const std::string& message) {
    {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if(GetFileAttributesExW(path_.c_str(),GetFileExInfoStandard,&info)) {
            const auto size=(static_cast<unsigned long long>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
            if(size>kMaxLogBytes) {
                const auto previous=path_+L".1";
                MoveFileExW(path_.c_str(),previous.c_str(),MOVEFILE_REPLACE_EXISTING);
            }
        }
    }
    std::ofstream output(path_,std::ios::app|std::ios::binary);
    if(!output) return;
    output<<Timestamp()<<" ["<<level<<"] "<<channel<<": "<<Redact(message)<<'\n';
    ++lines_;
}
void Log::Info(const std::string& channel,const std::string& message) { Append("info",channel,message); }
void Log::Warn(const std::string& channel,const std::string& message) { Append("warn",channel,message); }
void Log::Error(const std::string& channel,const std::string& message) { Append("error",channel,message); }
}
