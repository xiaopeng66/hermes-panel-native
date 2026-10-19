#include "data/probe.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib,"ws2_32.lib")

namespace hermes {
bool TcpReachable(const std::string& host,unsigned port,unsigned timeoutMs) {
    WSADATA data{};
    if(WSAStartup(MAKEWORD(2,2),&data)!=0) return false;
    bool reachable=false;
    ADDRINFOW hints{};
    hints.ai_family=AF_INET;
    hints.ai_socktype=SOCK_STREAM;
    const auto wide=std::wstring(host.begin(),host.end());
    ADDRINFOW* addresses=nullptr;
    const auto service=std::to_wstring(port);
    if(GetAddrInfoW(wide.c_str(),service.c_str(),&hints,&addresses)==0&&addresses) {
        SOCKET socket=::socket(addresses->ai_family,addresses->ai_socktype,addresses->ai_protocol);
        if(socket!=INVALID_SOCKET) {
            u_long nonBlocking=1;
            ioctlsocket(socket,FIONBIO,&nonBlocking);
            const auto connected=connect(socket,addresses->ai_addr,static_cast<int>(addresses->ai_addrlen));
            if(connected==0) reachable=true;
            else {
                fd_set writeSet{};
                FD_ZERO(&writeSet);
                FD_SET(socket,&writeSet);
                timeval wait{static_cast<long>(timeoutMs/1000),static_cast<long>(timeoutMs%1000)*1000};
                if(select(0,nullptr,&writeSet,nullptr,&wait)>0) {
                    int soError=0;
                    int length=sizeof(soError);
                    getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&soError),&length);
                    reachable=soError==0;
                }
            }
            closesocket(socket);
        }
        FreeAddrInfoW(addresses);
    }
    WSACleanup();
    return reachable;
}

HttpResult HealthCheck(const std::string& host,unsigned port,unsigned timeoutMs) {
    HttpRequest request;
    request.url=L"http://"+std::wstring(host.begin(),host.end())+L":"+std::to_wstring(port)+L"/health";
    request.timeoutMs=timeoutMs;
    return HttpFetch(request);
}
}
