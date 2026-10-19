#include "data/helper.h"
#include <windows.h>
#include <json.hpp>
#include <algorithm>
#include <cstring>

namespace hermes {
namespace {
using Json=nlohmann::json;

// wsl.exe 有自己的命令行解析器：给每个参数无条件加引号会让它解析失败（真机实测报"无法打开
// Wsl/Service"并立刻退出 -1），而同样的命令不加引号就正常。按 Windows 惯例只在必要时加引号
// （和 python 的 list2cmdline 一致）：含空格/制表/引号才引，内部引号用反斜杠翻倍。
std::wstring Quote(const std::wstring& value) {
    if(!value.empty()&&value.find_first_of(L" \t\"")==std::wstring::npos) return value;
    std::wstring quoted=L"\"";
    int backslashes=0;
    for(const auto character:value) {
        if(character=='\\') { ++backslashes;continue; }
        if(character=='\"') {
            quoted.append(static_cast<std::size_t>(backslashes*2+1),L'\\');
            quoted+=L'\"';
            backslashes=0;
            continue;
        }
        quoted.append(static_cast<std::size_t>(backslashes),L'\\');
        backslashes=0;
        quoted+=character;
    }
    quoted.append(static_cast<std::size_t>(backslashes*2),L'\\');
    quoted+=L'\"';
    return quoted;
}

// A console child (the real wsl.exe) misbehaves when it inherits invalid std handles: with no console
// and no STARTF_USESTDHANDLES it exits immediately with -1. Give every child a NUL device for stdin /
// stdout / stderr unless the caller wires a pipe, exactly like a shell would.
struct NullStdio {
    HANDLE read=nullptr;
    HANDLE write=nullptr;
    NullStdio() {
        SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
        read=CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);
        write=CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);
    }
    ~NullStdio() {
        if(read) CloseHandle(read);
        if(write) CloseHandle(write);
    }
    bool ok() const { return read!=nullptr&&write!=nullptr; }
};

struct ChildPipes {
    HANDLE childStdin=nullptr;
    HANDLE childStdout=nullptr;
    HANDLE process=nullptr;
    ~ChildPipes() {
        for(HANDLE handle:{childStdin,childStdout,process}) if(handle) CloseHandle(handle);
    }
};

// Overlapped read/write: the caller can hold a deadline because WaitForSingleObject is cancellable,
// and a blocked synchronous pipe read would hang the calling thread forever on a stuck helper.
bool WriteAll(HANDLE pipe,const char* data,DWORD size) {
    DWORD written=0;
    OVERLAPPED overlapped{};
    overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!overlapped.hEvent) return false;
    const bool ok=WriteFile(pipe,data,size,&written,&overlapped)==TRUE
        ?written==size
        :WaitForSingleObject(overlapped.hEvent,5000)==WAIT_OBJECT_0
            &&GetOverlappedResult(pipe,&overlapped,&written,FALSE)&&written==size;
    CloseHandle(overlapped.hEvent);
    return ok;
}

// 读 stdout 直到 EOF 或超时。CreatePipe 的句柄是同步的（ReadFile 会无限阻塞），
// 所以用 PeekNamedPipe 轮询 + 子进程退出检测：有数据才读、退出后读干、超时返回 false。
bool ReadToEof(HANDLE pipe,HANDLE process,std::string& out,unsigned timeoutMs) {
    const auto deadline=GetTickCount64()+timeoutMs;
    char buffer[4096];
    for(;;) {
        DWORD available=0;
        if(PeekNamedPipe(pipe,nullptr,0,nullptr,&available,nullptr)) {
            if(available>0) {
                DWORD read=0;
                if(!ReadFile(pipe,buffer,std::min<DWORD>(sizeof(buffer),available),&read,nullptr)||read==0)
                    return true;
                out.append(buffer,read);
                continue;
            }
        } else {
            return true;   // 写端已关：EOF
        }
        if(WaitForSingleObject(process,0)==WAIT_OBJECT_0) {
            for(;;) {
                DWORD read=0;
                if(!ReadFile(pipe,buffer,sizeof(buffer),&read,nullptr)||read==0) return true;
                out.append(buffer,read);
            }
        }
        if(GetTickCount64()>=deadline) return false;
        Sleep(5);
    }
}
}

void HelperBridge::Configure(std::wstring distro,std::wstring pythonExe,std::wstring helperPath) {
    distro_=std::move(distro);
    pythonExe_=std::move(pythonExe);
    helperPath_=std::move(helperPath);
}
void HelperBridge::SetWslExecutable(std::wstring path) { wslExe_=std::move(path); }
void HelperBridge::SetLogSink(std::function<void(const std::string&)> sink) { logSink_=std::move(sink); }

bool HelperBridge::WslReachable() {
    const std::wstring command=Quote(wslExe_)+L" -d "+Quote(distro_)+L" -- true";
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;
    startup.wShowWindow=SW_HIDE;
    // wsl.exe is a console program: without valid std handles it bails out with -1 before it even looks
    // at the distro. Give it the NUL device, exactly like a shell would.
    NullStdio stdio;
    if(!stdio.ok()) return false;
    startup.hStdInput=stdio.read;
    startup.hStdOutput=stdio.write;
    startup.hStdError=stdio.write;
    PROCESS_INFORMATION process{};
    std::wstring writable=command;
    if(!CreateProcessW(nullptr,writable.data(),nullptr,nullptr,TRUE,
        CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) return false;
    const HANDLE processHandle=process.hProcess;
    CloseHandle(process.hThread);
    if(WaitForSingleObject(processHandle,20000)!=WAIT_OBJECT_0) {
        // 冷启动的 WSL 可能正在起虚拟机：杀不掉就别急着判死，直接报告"没答上"。
        TerminateProcess(processHandle,5);
        CloseHandle(processHandle);
        if(logSink_) logSink_("wsl ping timed out after 20000ms");
        return false;
    }
    DWORD code=1;
    if(!GetExitCodeProcess(processHandle,&code)) { CloseHandle(processHandle);return false; }
    CloseHandle(processHandle);
    if(logSink_) logSink_("wsl ping exit "+std::to_string(code));
    return code==0;
}

HelperReply HelperBridge::Call(const std::string& requestLine,unsigned timeoutMs) {
    HelperReply reply;
    const std::wstring command=Quote(wslExe_)+L" -d "+Quote(distro_)+L" -- "
        +Quote(pythonExe_)+L" "+Quote(helperPath_);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags=STARTF_USESHOWWINDOW|STARTF_USESTDHANDLES;
    startup.wShowWindow=SW_HIDE;
    ChildPipes pipes;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    HANDLE parentRead=nullptr,parentWrite=nullptr;
    // Anonymous pipes for stdin/stdout: wsl.exe needs plain inherit-able handles (a *named* pipe client
    // for its stdout made the real wsl.exe hang forever, while a plain pipe - exactly what a shell gives
    // it - answers in milliseconds). The deadline is enforced on the CHILD now: we wait for it to exit
    // (killing it at the deadline) and only then drain the pipe, which is guaranteed to be at EOF.
    if(!CreatePipe(&pipes.childStdin,&parentWrite,&inherit,0)) { reply.error="stdin pipe failed";return reply; }
    if(!CreatePipe(&parentRead,&pipes.childStdout,&inherit,65536)) {
        reply.error="stdout pipe failed";
        CloseHandle(pipes.childStdin);CloseHandle(parentWrite);
        return reply;
    }
    SetHandleInformation(parentWrite,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(parentRead,HANDLE_FLAG_INHERIT,0);
    startup.hStdInput=pipes.childStdin;      // the child reads our request from the stdin read end
    startup.hStdOutput=pipes.childStdout;    // and writes the reply to the stdout write end
    startup.hStdError=pipes.childStdout;
    std::wstring writable=command;
    PROCESS_INFORMATION process{};
    if(!CreateProcessW(nullptr,writable.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,
        &startup,&process)) {
        reply.error="wsl.exe launch failed ("+std::to_string(GetLastError())+")";
        CloseHandle(pipes.childStdin);
        CloseHandle(parentRead);
        CloseHandle(pipes.childStdout);
        CloseHandle(parentWrite);
        return reply;
    }
    pipes.process=process.hProcess;
    CloseHandle(process.hThread);
    // The parent's copies of the child's ends must go, or the pipe never reports EOF.
    CloseHandle(pipes.childStdin);
    pipes.childStdin=nullptr;
    CloseHandle(pipes.childStdout);
    pipes.childStdout=nullptr;
    if(!WriteAll(parentWrite,(requestLine+"\n").c_str(),static_cast<DWORD>(requestLine.size()+1))) {
        reply.error="request write failed";
        TerminateProcess(pipes.process,3);
        CloseHandle(pipes.process);
        CloseHandle(parentRead);
        CloseHandle(parentWrite);
        return reply;
    }
    // The helper reads one line and writes one line; closing our write end signals EOF after it.
    CloseHandle(parentWrite);
    parentWrite=nullptr;
    // 必须先读 stdout 再等子进程退出：回复超过管道缓冲区（~64 KB，厂商/模型很多时很容易）
    // 时，子进程会阻塞在写端，父进程若先 WaitForSingleObject 就会死锁到超时。
    std::string raw;
    const bool readOk=ReadToEof(parentRead,pipes.process,raw,timeoutMs);
    if(!readOk) {
        reply.timedOut=true;
        reply.error="helper timed out after "+std::to_string(timeoutMs)+"ms";
        TerminateProcess(pipes.process,4);
    }
    WaitForSingleObject(pipes.process,5000);
    CloseHandle(parentRead);
    parentRead=nullptr;
    DWORD code=0;
    GetExitCodeProcess(pipes.process,&code);
    reply.exitCode=static_cast<int>(code);
    if(reply.timedOut) return reply;
    reply.transcriptOk=true;

    // The helper writes one JSON line. UTF-8 first; when that fails to parse, re-read the same bytes
    // as GBK and remember the rescue. Values are never logged - only the shape (ok/exit/lengths).
    std::string text=raw;
    // trim BOM/whitespace
    const auto notSpace=[](unsigned char c){return c!=' '&&c!='\r'&&c!='\n'&&c!='\t';};
    text.erase(text.begin(),std::find_if(text.begin(),text.end(),notSpace));
    text.erase(std::find_if(text.rbegin(),text.rend(),notSpace).base(),text.end());
    Json parsed;
    bool parsedOk=false;
    try { parsed=Json::parse(text);parsedOk=true; } catch(const std::exception& parseError) {
        if(logSink_) logSink_("utf8 parse refused: "+std::string(parseError.what()).substr(0,120)); }
    if(!parsedOk) {
        const int wide=MultiByteToWideChar(936,0,text.data(),static_cast<int>(text.size()),nullptr,0);
        if(wide>0) {
            std::wstring unicode(static_cast<std::size_t>(wide),L'\0');
            MultiByteToWideChar(936,0,text.data(),static_cast<int>(text.size()),unicode.data(),wide);
            const int utf8=WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,nullptr,0,nullptr,nullptr);
            if(utf8>0) {
                std::string converted(static_cast<std::size_t>(utf8),'\0');
                WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,converted.data(),utf8,nullptr,nullptr);
                try { parsed=Json::parse(converted);parsedOk=true;reply.gbkRecovered=true;
                    if(logSink_) logSink_("reply rescued via GBK ("+std::to_string(utf8)+" bytes)"); }
                catch(const std::exception&) {}
            }
        }
    }
    if(!parsedOk) {
        // wsl.exe 自己的错误是 UTF-16LE 文本（"没有已注册的分发版"之类）：解出来放进错误里，
        // 界面上就能看到真正的原因，而不是一句"不是 JSON"。
        std::string decoded;
        // UTF-16LE 的判据要严一点：前 8 个字节的奇数位都必须是 0，否则不要把 GBK 文本当 UTF-16 解。
        bool looksUtf16=text.size()>=8;
        for(std::size_t index=1;index<8&&index<text.size();index+=2)
            if(text[index]!=char(0)) looksUtf16=false;
        if(looksUtf16) {
            const int wide=static_cast<int>(text.size()/2);
            std::wstring unicode(static_cast<std::size_t>(wide),wchar_t(0));
            memcpy(unicode.data(),text.data(),static_cast<std::size_t>(wide)*2);
            const int utf8=WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,nullptr,0,nullptr,nullptr);
            if(utf8>0) {
                decoded.resize(static_cast<std::size_t>(utf8));
                WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,decoded.data(),utf8,nullptr,nullptr);
            }
        }
        if(decoded.empty()) {
            // 其次按 GBK（wsl.exe 的控制台输出就是系统代码页）解一次，最后才原样带上。
            const int wide=MultiByteToWideChar(936,0,text.data(),static_cast<int>(text.size()),nullptr,0);
            if(wide>0) {
                std::wstring unicode(static_cast<std::size_t>(wide),wchar_t(0));
                MultiByteToWideChar(936,0,text.data(),static_cast<int>(text.size()),unicode.data(),wide);
                const int utf8=WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,nullptr,0,nullptr,nullptr);
                if(utf8>0) {
                    decoded.resize(static_cast<std::size_t>(utf8));
                    WideCharToMultiByte(CP_UTF8,0,unicode.data(),wide,decoded.data(),utf8,nullptr,nullptr);
                }
            }
            if(decoded.empty()) decoded=text.substr(0,200);
        }
        // 诊断：把原始字节的前 80 个十六进制字符也带上（解析失败时才知道对面到底说了什么）。
        std::string hex;
        for(std::size_t index=0;index<text.size()&&index<24;++index) {
            char pair[4]{};
            std::snprintf(pair,sizeof(pair),"%02X ",static_cast<unsigned char>(text[index]));
            hex+=pair;
        }
        reply.error="reply is not JSON ("+std::to_string(text.size())+"B) [hex "+hex+"] "+decoded;
        if(logSink_) logSink_("protocol: unparsable reply, exit "+std::to_string(reply.exitCode));
        return reply;
    }
    reply.ok=parsed.value("ok",false);
    if(parsed.contains("data")&&!parsed["data"].is_null()) reply.data=parsed["data"].dump();
    reply.error=parsed.value("error",std::string());
    if(logSink_) {
        logSink_("helper exit="+std::to_string(reply.exitCode)+" ok="+(reply.ok?"1":"0")
            +" dataBytes="+std::to_string(reply.data.size()));
    }
    return reply;
}
}
