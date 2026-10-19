#include "http.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>

namespace hermes {
namespace {
bool SplitUrl(const std::wstring& url,std::wstring& host,std::wstring& path,bool& secure,unsigned short& port) {
    URL_COMPONENTS parts{};
    parts.dwStructSize=sizeof(parts);
    wchar_t hostName[256]{};
    wchar_t urlPath[2048]{};
    parts.lpszHostName=hostName;
    parts.dwHostNameLength=static_cast<DWORD>(std::size(hostName));
    parts.lpszUrlPath=urlPath;
    parts.dwUrlPathLength=static_cast<DWORD>(std::size(urlPath));
    if(!WinHttpCrackUrl(url.c_str(),static_cast<DWORD>(url.size()),0,&parts)) return false;
    host.assign(hostName,parts.dwHostNameLength);
    path.assign(urlPath,parts.dwUrlPathLength);
    if(path.empty()) path=L"/";
    secure=parts.nScheme==INTERNET_SCHEME_HTTPS;
    port=parts.nPort;
    return true;
}
void CloseHandles(HINTERNET session,HINTERNET connect,HINTERNET request) {
    if(request) WinHttpCloseHandle(request);
    if(connect) WinHttpCloseHandle(connect);
    if(session) WinHttpCloseHandle(session);
}
}
std::string HttpResult::DurationText() const { return std::to_string(elapsedMs)+"ms"; }

HttpResult HttpFetch(const HttpRequest& request) {
    HttpResult result;
    const auto started=GetTickCount64();
    std::wstring host,path;
    bool secure=false;
    unsigned short port=0;
    if(!SplitUrl(request.url,host,path,secure,port)) { result.error="invalid url";return result; }
    const auto session=WinHttpOpen(L"HermesModelPanelNative/0.1",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!session) { result.error="WinHttpOpen failed";return result; }
    WinHttpSetTimeouts(session,static_cast<int>(request.timeoutMs),static_cast<int>(request.timeoutMs),
        static_cast<int>(request.timeoutMs),static_cast<int>(request.timeoutMs));
    const auto connect=WinHttpConnect(session,host.c_str(),port,0);
    if(!connect) { result.error="WinHttpConnect failed";CloseHandles(session,nullptr,nullptr);return result; }
    const auto flags=secure?WINHTTP_FLAG_SECURE:0;
    const auto handle=WinHttpOpenRequest(connect,request.method.c_str(),path.c_str(),nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags);
    if(!handle) { result.error="WinHttpOpenRequest failed";CloseHandles(session,connect,nullptr);return result; }
    std::wstring headers;
    if(!request.cookie.empty()) {
        const std::wstring cookie(request.cookie.begin(),request.cookie.end());
        headers=L"Cookie: "+cookie+L"\r\n";
    }
    if(!request.body.empty()) {
        const std::wstring type(request.contentType.begin(),request.contentType.end());
        headers+=L"Content-Type: "+type+L"\r\n";
    }
    const auto sent=WinHttpSendRequest(handle,headers.empty()?WINHTTP_NO_ADDITIONAL_HEADERS:headers.c_str(),
        headers.empty()?0:static_cast<DWORD>(headers.size()),
        request.body.empty()?WINHTTP_NO_REQUEST_DATA:const_cast<char*>(request.body.data()),
        static_cast<DWORD>(request.body.size()),static_cast<DWORD>(request.body.size()),0);
    if(!sent) {
        result.error="WinHttpSendRequest failed ("+std::to_string(GetLastError())+")";
        CloseHandles(session,connect,handle);return result;
    }
    if(!WinHttpReceiveResponse(handle,nullptr)) {
        result.error="WinHttpReceiveResponse failed ("+std::to_string(GetLastError())+")";
        CloseHandles(session,connect,handle);return result;
    }
    DWORD statusSize=sizeof(DWORD);
    DWORD status=0;
    WinHttpQueryHeaders(handle,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&statusSize,WINHTTP_NO_HEADER_INDEX);
    result.status=status;
    result.transportOk=true;
    for(;;) {
        DWORD available=0;
        if(!WinHttpQueryDataAvailable(handle,&available)||available==0) break;
        std::vector<char> buffer(available);
        DWORD read=0;
        if(!WinHttpReadData(handle,buffer.data(),available,&read)||read==0) break;
        result.body.append(buffer.data(),read);
        if(result.body.size()>4*1024*1024) break;
    }
    CloseHandles(session,connect,handle);
    result.elapsedMs=static_cast<unsigned>(GetTickCount64()-started);
    return result;
}
}
