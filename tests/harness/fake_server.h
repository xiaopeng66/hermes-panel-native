#pragma once
// Minimal loopback HTTP server for the data-layer acceptance runs. It binds an OS-chosen port on
// 127.0.0.1 and answers each request with whatever the current script says, so every test stays off
// the real sites (DESIGN §6.4: isolated data directory and fake data only).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <atomic>
#include <functional>
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>

namespace harness {
struct FakeResponse { unsigned status=200;std::string body="{}";unsigned delayMs=0;std::string extraHeaders;std::string contentType="application/json"; };
class FakeServer {
public:
    using Handler=std::function<FakeResponse(const std::string& method,const std::string& path,const std::string& body)>;
    explicit FakeServer(Handler handler) {
        WSADATA data{};
        if(WSAStartup(MAKEWORD(2,2),&data)!=0) return;
        socket_=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(socket_==INVALID_SOCKET) return;
        sockaddr_in address{};
        address.sin_family=AF_INET;
        address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        address.sin_port=0;
        if(bind(socket_,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0) return;
        int length=sizeof(address);
        if(getsockname(socket_,reinterpret_cast<sockaddr*>(&address),&length)!=0) return;
        port_=ntohs(address.sin_port);
        if(listen(socket_,8)!=0) return;
        worker_=std::thread([this,handler]() mutable {
            while(!stopping_&&socket_!=INVALID_SOCKET) {
                const auto client=accept(socket_,nullptr,nullptr);
                if(client==INVALID_SOCKET) break;
                // One thread per connection: a client that connects first and sends later (or keeps a
                // socket open) must not stall the requests that follow it.
                std::thread([this,client,handler]{
                    Serve(client,handler);
                    closesocket(client);
                }).detach();
            }
        });
    }
    ~FakeServer() {
        stopping_=true;
        if(socket_!=INVALID_SOCKET) { closesocket(socket_);socket_=INVALID_SOCKET; }
        if(worker_.joinable()) worker_.join();
        WSACleanup();
    }
    FakeServer(const FakeServer&)=delete;
    FakeServer& operator=(const FakeServer&)=delete;
    unsigned short Port() const { return port_; }
    std::string BaseUrl() const { return "http://127.0.0.1:"+std::to_string(port_); }
    unsigned Requests() const { return requests_; }
    // Optional gate: when set, any request whose headers lack this exact cookie is answered 401, which
    // is how the login scenario proves that the captured session is what makes the poll work.
    void RequireCookie(std::string cookie) { requiredCookie_=std::move(cookie); }
    unsigned Unauthorized() const { return unauthorized_; }
    bool Running() const { return port_!=0; }
private:
    // Chromium is picky about response framing: a well-formed HTTP/1.1 reply needs a Date, and a
    // navigation that Chromium should turn into a document must not look like a cached download.
    static std::string DateHeader() {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        static const char* days[]{"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
        static const char* months[]{"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        char buffer[40]{};
        std::snprintf(buffer,sizeof(buffer),"%s, %02u %s %04u %02u:%02u:%02u GMT",
            days[now.wDayOfWeek%7],now.wDay,months[(now.wMonth>0?now.wMonth-1:0)%12],now.wYear,
            now.wHour,now.wMinute,now.wSecond);
        return buffer;
    }
    void Serve(SOCKET client,Handler handler) {
        DWORD timeout=3000;
        setsockopt(client,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&timeout),sizeof(timeout));
        std::string request;
        char buffer[2048]{};
        while(request.find("\r\n\r\n")==std::string::npos) {
            const auto read=recv(client,buffer,sizeof(buffer),0);
            if(read<=0) return;
            request.append(buffer,read);
            if(request.size()>64*1024) break;
        }
        const auto firstLine=request.substr(0,request.find("\r\n"));
        const auto methodEnd=firstLine.find(' ');
        const auto pathEnd=firstLine.find(' ',methodEnd+1);
        const std::string method=firstLine.substr(0,methodEnd);
        const std::string path=firstLine.substr(methodEnd+1,pathEnd-methodEnd-1);
        const auto headerEnd=request.find("\r\n\r\n");
        const std::string body=headerEnd==std::string::npos?std::string{}:request.substr(headerEnd+4);
        ++requests_;
        if(!requiredCookie_.empty()&&request.find(requiredCookie_)==std::string::npos) {
            ++unauthorized_;
            const std::string denied="{\"code\":401,\"message\":\"login required\"}";
            std::string reply="HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n";
            reply+="Content-Length: "+std::to_string(denied.size())+"\r\nConnection: close\r\n\r\n"+denied;
            send(client,reply.data(),static_cast<int>(reply.size()),0);
            return;
        }
        const auto response=handler(method,path,body);
        if(response.delayMs) std::this_thread::sleep_for(std::chrono::milliseconds(response.delayMs));
        std::string reply="HTTP/1.1 "+std::to_string(response.status)+" "+
            (response.status==200?"OK":response.status==500?"Internal Server Error":"Error")+"\r\n";
        reply+="Content-Type: application/json\r\n";
        reply+=response.extraHeaders;
        reply+="Content-Length: "+std::to_string(response.body.size())+"\r\n";
        reply+="Connection: close\r\n\r\n";
        reply+=response.body;
        send(client,reply.data(),static_cast<int>(reply.size()),0);
    }
    SOCKET socket_=INVALID_SOCKET;
    unsigned short port_=0;
    std::atomic<bool> stopping_{false};
    std::atomic<unsigned> requests_{0};
    std::atomic<unsigned> unauthorized_{0};
    std::string requiredCookie_;
    std::thread worker_;
};
}
