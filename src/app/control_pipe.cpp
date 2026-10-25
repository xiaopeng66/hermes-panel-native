#include "control_pipe.h"

namespace hermes {
namespace {
constexpr unsigned kMaxPayload=65536;
bool ReadExact(HANDLE pipe,void* data,unsigned size) {
    auto* bytes=static_cast<unsigned char*>(data);
    unsigned remaining=size;
    while(remaining>0) {
        DWORD read=0;
        if(!ReadFile(pipe,bytes,remaining,&read,nullptr)||read==0) return false;
        bytes+=read;remaining-=read;
    }
    return true;
}
bool WriteExact(HANDLE pipe,const void* data,unsigned size) {
    const auto* bytes=static_cast<const unsigned char*>(data);
    unsigned remaining=size;
    while(remaining>0) {
        DWORD written=0;
        if(!WriteFile(pipe,bytes,remaining,&written,nullptr)||written==0) return false;
        bytes+=written;remaining-=written;
    }
    return true;
}
}
// owner/message 是历史参数：管道线程拿到请求后直接经 handler 里的 SendMessageW 回到 UI 线程
// （见 main.cpp 的 lambda），这两个参数从未被读取。保留签名不动调用方，用 (void) 标注意图。
bool ControlPipe::Start(HWND owner,UINT message,const std::wstring& pipeName,Handler handler) {
    (void)owner;
    (void)message;
    if(thread_.joinable()) return false;
    name_=pipeName;handler_=std::move(handler);
    stopEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!stopEvent_) return false;
    // 管道线程同样不能把异常漏出线程（std::terminate → __fastfail 会绕过崩溃探针）。
    thread_=std::thread([this] {
        try { Serve(); }
        catch(const std::exception& error) {
            OutputDebugStringA((std::string("control pipe failed: ")+error.what()).c_str());
        } catch(...) {
            OutputDebugStringA("control pipe failed: unknown exception");
        }
    });
    return true;
}
void ControlPipe::Stop() {
    if(!stopEvent_) return;
    SetEvent(stopEvent_);
    // Release a blocked ConnectNamedPipe so the serving thread can observe the stop event.
    const auto dummy=CreateFileW(name_.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(dummy!=INVALID_HANDLE_VALUE) CloseHandle(dummy);
    if(thread_.joinable()) thread_.join();
    CloseHandle(stopEvent_);
    stopEvent_=nullptr;
}
void ControlPipe::Serve() {
    while(WaitForSingleObject(stopEvent_,0)!=WAIT_OBJECT_0) {
        HANDLE pipe=CreateNamedPipeW(name_.c_str(),PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE|PIPE_WAIT,1,64*1024,64*1024,0,nullptr);
        if(pipe==INVALID_HANDLE_VALUE) return;
        const auto connected=ConnectNamedPipe(pipe,nullptr)!=FALSE||GetLastError()==ERROR_PIPE_CONNECTED;
        if(WaitForSingleObject(stopEvent_,0)==WAIT_OBJECT_0) {
            DisconnectNamedPipe(pipe);CloseHandle(pipe);return;
        }
        if(!connected) { CloseHandle(pipe);continue; }
        for(;;) {
            unsigned length=0;
            if(!ReadExact(pipe,&length,sizeof(length))) break;
            if(length==0||length>kMaxPayload) break;
            std::string payload(length,'\0');
            if(!ReadExact(pipe,payload.data(),length)) break;
            ++requests_;
            std::string response;
            if(handler_) response=handler_(payload);
            const auto replySize=static_cast<unsigned>(response.size());
            if(!WriteExact(pipe,&replySize,sizeof(replySize))) break;
            if(!WriteExact(pipe,response.data(),replySize)) break;
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}
}
