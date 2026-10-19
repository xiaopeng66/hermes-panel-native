#include "data/log.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {
int passed=0,failed=0;
void Check(bool condition,const std::string& name) {
    if(condition) ++passed;
    else { ++failed;std::cerr<<"FAIL "<<name<<'\n'; }
}
struct Case { const char* name;const char* input;const char* expected; };
}
int main() {
    const std::vector<Case> cases{
        {"cookie header value masked","GET /x Cookie: share-session=eyJhbGciOiJIUzI1NiJ9.abc.def; other=1",
            "GET /x Cookie: share-session=<redacted>; other=1"},
        {"token json field masked","{\"userToken\":\"SUPERSECRET123\",\"ok\":true}",
            "{\"userToken\":\"<redacted>\",\"ok\":true}"},
        {"password pair masked","login password=P@ssw0rd! next","login password=<redacted> next"},
        {"harmless text untouched","geometry write panel dock target=(0,0,450,588)","geometry write panel dock target=(0,0,450,588)"},
        {"multiple secrets in one line","a=1 share-session=AAA b=2 userToken=BBB",
            "a=1 share-session=<redacted> b=2 userToken=<redacted>"},
        {"unrelated parameter is untouched","tokenizer=fast","tokenizer=fast"},
        {"bare key without value is left visible","userToken","userToken"},
        {"key with only a separator is left visible","userToken =","userToken ="},
        {"bearer header masked","Authorization: Bearer eyJhbGciOi.abc.def","Authorization: Bearer <redacted>"},
        {"lowercase bearer masked","authorization: bearer abcDEF123","authorization: bearer <redacted>"},
        {"access token query masked","url?access_token=abc123&x=1","url?access_token=<redacted>&x=1"},
        {"bare sk key masked","helper used sk-abcdefgh12345 here","helper used sk-<redacted> here"},
        {"server key masked","API_SERVER_KEY=sk-1234567890abcdef","API_SERVER_KEY=<redacted>"}
    };
    for(const auto& test:cases) Check(hermes::Log::Redact(test.input)==test.expected,test.name);

    wchar_t directory[MAX_PATH]{};
    GetTempPathW(MAX_PATH,directory);
    const std::wstring path=std::wstring(directory)+L"balance-panel-log-test.log";
    DeleteFileW(path.c_str());
    {
        hermes::Log log(path);
        log.Info("app","started");
        log.Geometry("write panel startup target=(200,120,450,588) actual=(200,120,450,588)");
        log.Error("site","http 401");
        Check(log.Lines()==3,"log counts appended lines");
        Check(log.Path()==path,"log reports its path");
    }
    {
        std::ifstream input(path);
        std::string line;
        std::vector<std::string> lines;
        while(std::getline(input,line)) lines.push_back(line);
        Check(lines.size()==3,"three lines persisted");
        Check(!lines.empty()&&lines[0].find("[info] app: started")!=std::string::npos,"info line format");
        Check(lines.size()>1&&lines[1].find("geometry: write panel startup")!=std::string::npos,"geometry audit line");
        Check(lines.size()>2&&lines[2].find("[error] site: http 401")!=std::string::npos,"error line format");
    }
    DeleteFileW(path.c_str());
    // 日志轮转：文件超过 5 MB 时把旧内容挪到 .1，新日志从空开始（只保留一份旧日志）。
    {
        const std::wstring rotated=std::wstring(directory)+L"balance-panel-log-rotate.log";
        DeleteFileW(rotated.c_str());
        DeleteFileW((rotated+L".1").c_str());
        {
            std::ofstream output(rotated,std::ios::binary|std::ios::trunc);
            const std::string chunk(1024*1024,'x');
            for(int index=0;index<6;++index) output<<chunk;
        }
        {
            hermes::Log log(rotated);
            log.Info("app","after rotation");
        }
        const auto previous=GetFileAttributesW((rotated+L".1").c_str());
        WIN32_FILE_ATTRIBUTE_DATA info{};
        GetFileAttributesExW(rotated.c_str(),GetFileExInfoStandard,&info);
        const auto size=(static_cast<unsigned long long>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
        Check(previous!=INVALID_FILE_ATTRIBUTES,"a 5 MB log is rotated to .1");
        Check(size<4096,"the new log starts small after rotation");
        DeleteFileW(rotated.c_str());
        DeleteFileW((rotated+L".1").c_str());
    }

    std::cout<<"log: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}
