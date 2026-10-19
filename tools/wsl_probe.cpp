// wsl-probe: 用与面板完全相同的桥（HelperBridge）打一次真实 wsl.exe，用来在没有界面的情况下看清
// 退出码/耗时/回包，避免"面板卡住"只能靠猜。只读：默认发 ping 与 list（list 不写任何文件）。
#include <windows.h>
#include <chrono>
#include <vector>
#include <iostream>
#include <string>
#include "data/helper.h"

namespace {
// 用四种 CreateProcess 参数组合跑同一条 wsl.exe 命令行：把"为什么 python 行、我们的桥不行"钉死。
int RunVariant(const std::wstring& command,const char* name,bool noWindow,bool hideWindow,bool handleList,
    const char* request) {
    HANDLE childIn=nullptr,parentOut=nullptr,childOut=nullptr,parentIn=nullptr;
    SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
    if(!CreatePipe(&childIn,&parentIn,&inherit,0)) return -1;
    if(!CreatePipe(&parentOut,&childOut,&inherit,65536)) return -1;
    SetHandleInformation(parentIn,HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(parentOut,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags=STARTF_USESTDHANDLES|(hideWindow?STARTF_USESHOWWINDOW:0);
    startup.wShowWindow=hideWindow?SW_HIDE:SW_SHOWNORMAL;
    startup.hStdInput=childIn;
    startup.hStdOutput=childOut;
    startup.hStdError=childOut;
    STARTUPINFOEXW extended{};
    extended.StartupInfo=startup;
    extended.StartupInfo.cb=sizeof(STARTUPINFOEXW);
    std::vector<unsigned char> buffer;
    SIZE_T size=0;
    bool listReady=false;
    if(handleList) {
        InitializeProcThreadAttributeList(nullptr,1,0,&size);
        buffer.resize(size);
        extended.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
        listReady=InitializeProcThreadAttributeList(extended.lpAttributeList,1,0,&size)!=FALSE;
        HANDLE inherited[2]{childIn,childOut};
        listReady=listReady&&UpdateProcThreadAttribute(extended.lpAttributeList,0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)!=FALSE;
    }
    const bool useList=handleList&&listReady;
    DWORD flags=(noWindow?CREATE_NO_WINDOW:0)|(useList?EXTENDED_STARTUPINFO_PRESENT:0);
    PROCESS_INFORMATION process{};
    std::wstring writable=command;
    const auto started=CreateProcessW(nullptr,writable.data(),nullptr,nullptr,TRUE,flags,nullptr,nullptr,
        listReady?&extended.StartupInfo:&startup,&process);
    if(listReady) DeleteProcThreadAttributeList(extended.lpAttributeList);
    if(!started) {
        std::cout<<name<<": CreateProcess failed ("<<GetLastError()<<")\n"<<std::flush;
        CloseHandle(childIn);CloseHandle(parentOut);CloseHandle(childOut);CloseHandle(parentIn);
        return -1;
    }
    CloseHandle(process.hThread);
    CloseHandle(childIn);
    CloseHandle(childOut);
    const std::string line=std::string(request)+"\n";
    DWORD written=0;
    WriteFile(parentIn,line.data(),static_cast<DWORD>(line.size()),&written,nullptr);
    CloseHandle(parentIn);
    const bool exited=WaitForSingleObject(process.hProcess,30000)==WAIT_OBJECT_0;
    std::string raw;
    char buf[4096];
    for(;;) {
        DWORD read=0;
        if(!ReadFile(parentOut,buf,sizeof(buf),&read,nullptr)||read==0) break;
        raw.append(buf,read);
        if(raw.size()>1u<<20) break;
    }
    DWORD code=0;
    GetExitCodeProcess(process.hProcess,&code);
    std::cout<<name<<": pid="<<process.dwProcessId<<" exited="<<(exited?"1":"0")
        <<" code="<<code<<" bytes="<<raw.size()<<" head="<<raw.substr(0,60)<<"\n"<<std::flush;
    CloseHandle(parentOut);
    CloseHandle(process.hProcess);
    return 0;
}
}

int wmain(int argc,wchar_t** argv) {
    if(argc<4) {
        std::cerr<<"usage: wsl-probe <distro> <pythonExe> <helperPath> [requestJson]\n";
        return 2;
    }
    const wchar_t* matrix=nullptr;
    for(int index=1;index<argc;++index)
        if(std::wstring(argv[index])==L"--matrix") matrix=L"yes";
    if(matrix) {
        const std::wstring quoted=L"\"wsl.exe\" -d \""+std::wstring(argv[1])+L"\" -- \""
            +std::wstring(argv[2])+L"\" \""+std::wstring(argv[3])+L"\"";
        // python 的 list2cmdline 在不需要时不会加引号：wsl.exe 有自己的命令行解析器，试无引号版。
        const std::wstring bare=L"wsl.exe -d "+std::wstring(argv[1])+L" -- "+std::wstring(argv[2])
            +L" "+std::wstring(argv[3]);
        const char* request="{\"cmd\":\"list\"}";
        RunVariant(quoted,"A quoted, USESTDHANDLES only",false,false,false,request);
        RunVariant(quoted,"B quoted +CREATE_NO_WINDOW",true,false,false,request);
        RunVariant(bare,"E bare (no quotes)",false,false,false,request);
        RunVariant(bare,"F bare +CREATE_NO_WINDOW",true,false,false,request);
        return 0;
    }
    hermes::HelperBridge bridge;
    bridge.Configure(argv[1],argv[2],argv[3]);
    bridge.SetLogSink([](const std::string& line) { std::cout<<"[bridge] "<<line<<"\n"<<std::flush; });
    const auto timeIt=[&](const char* what,const std::string& request,unsigned timeoutMs) {
        const auto start=std::chrono::steady_clock::now();
        const auto reply=bridge.Call(request,timeoutMs);
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-start).count();
        std::cout<<what<<": exit="<<reply.exitCode<<" ok="<<(reply.ok?"1":"0")
            <<" timedOut="<<(reply.timedOut?"1":"0")<<" transcript="<<(reply.transcriptOk?"1":"0")
            <<" bytes="<<reply.data.size()<<" in "<<ms<<"ms";
        if(!reply.error.empty()) std::cout<<" error="<<reply.error;
        std::cout<<"\n"<<std::flush;
        if(reply.ok) std::cout<<"  data head: "<<reply.data.substr(0,120)<<"\n"<<std::flush;
        return reply;
    };
    const auto pingStart=std::chrono::steady_clock::now();
    const auto reachable=bridge.WslReachable();
    std::cout<<"ping: reachable="<<(reachable?"1":"0")<<" in "
        <<std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now()-pingStart).count()<<"ms\n"<<std::flush;
    if(argc>4) {
        // 命令行给的请求是 UTF-8 的窄字符串（助手协议就是一行 JSON），不要按宽字符逐字符转。
        char buffer[4096]{};
        WideCharToMultiByte(CP_UTF8,0,argv[4],-1,buffer,sizeof(buffer),nullptr,nullptr);
        timeIt("call",std::string(buffer),60000);
    } else {
        timeIt("list","{\"cmd\":\"list\"}",60000);
        timeIt("health","{\"cmd\":\"health\"}",30000);
    }
    return 0;
}
