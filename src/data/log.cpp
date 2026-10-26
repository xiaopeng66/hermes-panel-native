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
std::string Log::Sanitize(const std::string& text) {
    // 日志是 UTF-8 文本文件，但写进去的东西有一部分来自子进程：wsl.exe 与读助手在异常时会吐
    // UTF-16 或其它编码的原始字节。直接落盘就会出现乱码，更要命的是载荷里的 0x0A/0x00 会把一行
    // 切成好几段——真机日志实测混进 5445 行二进制，其中一条 11.8 KB 的载荷把后面整段冲散，
    // 想按行读日志的排查工具全部失效。
    // 规则：可打印 ASCII 与合法 UTF-8（含中文）原样保留；`\t` 保留（不破坏行结构）；其余
    // 全部转成 \xNN 转义 —— 日志始终是单行合法 UTF-8，载荷也仍然可以逐字节还原。
    std::string output;
    output.reserve(text.size());
    for(std::size_t index=0;index<text.size();) {
        const auto byte=static_cast<unsigned char>(text[index]);
        if(byte=='\t'||(byte>=0x20&&byte<0x7F)) { output+=static_cast<char>(byte);++index;continue; }
        std::size_t length=0;
        if(byte>=0xC2&&byte<=0xDF) length=2;
        else if(byte>=0xE0&&byte<=0xEF) length=3;
        else if(byte>=0xF0&&byte<=0xF4) length=4;
        bool valid=length!=0&&index+length<=text.size();
        if(valid) {
            for(std::size_t offset=1;offset<length;++offset) {
                if((static_cast<unsigned char>(text[index+offset])&0xC0)!=0x80) { valid=false;break; }
            }
            if(valid) {
                // RFC 3629：挡掉过长编码、UTF-16 代理区与超出 U+10FFFF 的码位。
                const auto second=static_cast<unsigned char>(text[index+1]);
                if(length==3) valid=!(byte==0xE0&&second<0xA0)&&!(byte==0xED&&second>=0xA0);
                if(length==4) valid=!(byte==0xF0&&second<0x90)&&!(byte==0xF4&&second>=0x90);
            }
        }
        if(valid) { output.append(text,index,length);index+=length;continue; }
        char escape[8]{};
        std::snprintf(escape,sizeof(escape),"\\x%02X",byte);
        output+=escape;
        ++index;
    }
    return output;
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
    // 先遮敏感值、再转义编码：两步都在这个唯一的收口处，任何调用方都不可能把坏字节写进日志。
    output<<Timestamp()<<" ["<<level<<"] "<<channel<<": "<<Sanitize(Redact(message))<<'\n';
    ++lines_;
}
void Log::Info(const std::string& channel,const std::string& message) { Append("info",channel,message); }
void Log::Warn(const std::string& channel,const std::string& message) { Append("warn",channel,message); }
void Log::Error(const std::string& channel,const std::string& message) { Append("error",channel,message); }
}
