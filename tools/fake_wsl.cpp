// Fake wsl.exe for the helper tests (INV-H5): stands in for `wsl.exe -d <distro> -- <python> <helper>`
// so the bridge is exercised without touching a real WSL. Behaviour is selected by environment:
//   FAKE_WSL_MODE = ok | fail | proto | bad | gbk | timeout | silent   (default ok)
//   FAKE_WSL_DELAY_MS = milliseconds to stall before answering (timeout mode)
//   FAKE_WSL_PING = down   -> `-- true` exits 3 instead of 0
// The ok/gbk replies echo the request line back inside data.request, plus a Chinese note, so the
// tests can verify UTF-8 handling and the GBK rescue end to end.
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

namespace {
std::string EnvOr(const char* name,const std::string& fallback) {
    char buffer[64]{};
    const auto size=GetEnvironmentVariableA(name,buffer,sizeof(buffer));
    return size>0&&size<sizeof(buffer)?std::string(buffer):fallback;
}
int Delay() {
    const auto value=EnvOr("FAKE_WSL_DELAY_MS","0");
    return atoi(value.c_str());
}
std::string OkReply(const std::string& request,const std::string& noteSuffix="") {
    std::string escaped;
    for(const char c:request) {
        switch(c) {
        case '\\': escaped+="\\\\";break;
        case '"': escaped+="\\\"";break;
        case '\r': break;
        case '\n': break;   // request is a single line anyway
        default: escaped+=c;
        }
    }
    return "{\"ok\":true,\"data\":{\"request\":\""+escaped+"\",\"note\":\"\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C"
        "\xE4\xB8\x96\xE7\x95\x8C"+noteSuffix+"\",\"answer\":42}}";
}
}

int wmain(int argc,wchar_t** argv) {
    // Detect the ping probe: `wsl.exe -d <distro> -- true`.
    bool ping=false;
    for(int index=1;index<argc;++index) {
        std::string flag;
        const auto length=WideCharToMultiByte(CP_UTF8,0,argv[index],-1,nullptr,0,nullptr,nullptr);
        flag.resize(static_cast<std::size_t>(std::max(length-1,0)));
        WideCharToMultiByte(CP_UTF8,0,argv[index],-1,flag.data(),length,nullptr,nullptr);
        if(flag=="true") ping=true;
    }
    const auto pingMode=EnvOr("FAKE_WSL_PING","up");
    if(ping) return pingMode=="down"?3:0;
    const auto mode=EnvOr("FAKE_WSL_MODE","ok");
    if(FILE* side=nullptr;fopen_s(&side,"E:/AI/Zcode/tmp/hermes-panel-native/fixture-seen.txt","w")==0&&side) {
        fprintf(side,"mode=%s delay=%d argc=%d",mode.c_str(),Delay(),argc);
        fclose(side);
    }
    std::string request;
    std::string line;
    while(std::getline(std::cin,line)) {
        if(!line.empty()&&line.back()=='\r') line.pop_back();
        if(!line.empty()) { request=line;break; }
    }
    if(mode=="timeout") {
        Sleep(static_cast<DWORD>(Delay()>0?Delay():10000));
        std::cout<<OkReply(request)<<std::endl;
        return 0;
    }
    if(mode=="silent") { Sleep(50);return 7; }
    if(mode=="fail") { std::cout<<"{\"ok\":false,\"error\":\"business failure\"}"<<std::endl;return 1; }
    if(mode=="proto") { std::cout<<"{\"error\":\"unknown command\"}"<<std::endl;return 2; }
    if(mode=="bad") { std::cout<<"not-json-at-all"<<std::endl;return 0; }
    if(mode=="gbk") {
        // The euro sign is a raw 0x80 byte: valid GBK, invalid UTF-8. nlohmann's parse rejects it,
        // so the bridge is forced onto its CP936 rescue path instead of silently mis-decoding.
        const auto utf8=OkReply(request,"\xE2\x82\xAC");   // the euro rides inside the note value
        const int wide=MultiByteToWideChar(CP_UTF8,0,utf8.data(),static_cast<int>(utf8.size()),nullptr,0);
        std::wstring unicode(static_cast<std::size_t>(wide),L'\0');
        MultiByteToWideChar(CP_UTF8,0,utf8.data(),static_cast<int>(utf8.size()),unicode.data(),wide);
        const int gbk=WideCharToMultiByte(936,0,unicode.data(),wide,nullptr,0,nullptr,nullptr);
        std::string encoded(static_cast<std::size_t>(gbk),'\0');
        WideCharToMultiByte(936,0,unicode.data(),wide,encoded.data(),gbk,nullptr,nullptr);
        std::cout<<encoded<<std::endl;
        return 0;
    }
    std::cout<<OkReply(request)<<std::endl;
    return 0;
}
