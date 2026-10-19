// Independent geometry probe (DESIGN §8): samples GetWindowRect in physical pixels on its own
// timer, captures per-monitor screenshots with BitBlt, and diffs a region against a stored
// baseline. It never asks the application for geometry while sampling, so the evidence is
// external to the code under test. Commands arrive on stdin, results leave as one JSON line.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <json.hpp>
#include <atomic>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include "no_modal_asserts.h"

using Json=nlohmann::json;
using Microsoft::WRL::ComPtr;

namespace {
constexpr unsigned kMaxIntervalMs=200;

struct MonitorInfo { RECT bounds{};std::wstring device; };
struct BandBaseline { bool valid=false;int x=0;int y=0;int width=0;int height=0;std::vector<std::uint32_t> pixels; };

HANDLE OpenPipe(const std::wstring& instance) {
    const auto name=LR"(\\.\pipe\HermesModelPanelNative.)"+instance;
    if(!WaitNamedPipeW(name.c_str(),3000)) return INVALID_HANDLE_VALUE;
    return CreateFileW(name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
}
Json PipeRequest(const std::wstring& instance,const Json& request) {
    const auto pipe=OpenPipe(instance);
    if(pipe==INVALID_HANDLE_VALUE) throw std::runtime_error("control pipe unavailable");
    const auto payload=request.dump();
    const auto length=static_cast<unsigned>(payload.size());
    DWORD written=0;
    if(!WriteFile(pipe,&length,sizeof(length),&written,nullptr)||!WriteFile(pipe,payload.data(),length,&written,nullptr)) {
        CloseHandle(pipe);throw std::runtime_error("control pipe write failed");
    }
    unsigned replyLength=0;DWORD read=0;
    if(!ReadFile(pipe,&replyLength,sizeof(replyLength),&read,nullptr)||replyLength==0||replyLength>(64u<<20)) {
        CloseHandle(pipe);throw std::runtime_error("control pipe reply failed");
    }
    std::string reply(replyLength,'\0');
    if(!ReadFile(pipe,reply.data(),replyLength,&read,nullptr)) { CloseHandle(pipe);throw std::runtime_error("control pipe reply truncated"); }
    CloseHandle(pipe);
    return Json::parse(reply);
}
Json RectJson(HWND window,bool& visible) {
    RECT rect{};
    GetWindowRect(window,&rect);
    visible=IsWindowVisible(window)!=0;
    return Json::array({rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top});
}
std::vector<MonitorInfo> Monitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,PRECT,LPARAM param) -> BOOL {
        MONITORINFOEXW info{};info.cbSize=sizeof(info);
        if(GetMonitorInfoW(monitor,&info)) reinterpret_cast<std::vector<MonitorInfo>*>(param)->push_back({info.rcMonitor,info.szDevice});
        return TRUE;
    },reinterpret_cast<LPARAM>(&monitors));
    return monitors;
}
bool CaptureBand(int x,int y,int width,int height,std::vector<std::uint32_t>& pixels) {
    HDC screen=GetDC(nullptr);
    HDC memory=CreateCompatibleDC(screen);
    HBITMAP bitmap=CreateCompatibleBitmap(screen,width,height);
    const auto previous=SelectObject(memory,bitmap);
    const bool copied=BitBlt(memory,0,0,width,height,screen,x,y,SRCCOPY|CAPTUREBLT)!=FALSE;
    BITMAPINFOHEADER header{};
    header.biSize=sizeof(header);header.biWidth=width;header.biHeight=-height;header.biPlanes=1;header.biBitCount=32;header.biCompression=BI_RGB;
    pixels.assign(static_cast<std::size_t>(width)*height,0u);
    const bool read=GetDIBits(memory,bitmap,0,static_cast<UINT>(height),pixels.data(),reinterpret_cast<BITMAPINFO*>(&header),DIB_RGB_COLORS)!=0;
    SelectObject(memory,previous);
    DeleteObject(bitmap);DeleteDC(memory);ReleaseDC(nullptr,screen);
    return copied&&read;
}
bool SavePng(const std::wstring& path,int x,int y,int width,int height) {
    std::vector<std::uint32_t> pixels;
    if(!CaptureBand(x,y,width,height,pixels)) return false;
    ComPtr<IWICImagingFactory> factory;
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))) return false;
    ComPtr<IWICStream> stream;
    if(FAILED(factory->CreateStream(&stream))) return false;
    if(FAILED(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE))) return false;
    ComPtr<IWICBitmapEncoder> encoder;
    if(FAILED(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder))) return false;
    if(FAILED(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache))) return false;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    if(FAILED(encoder->CreateNewFrame(&frame,&properties))) return false;
    if(FAILED(frame->Initialize(properties.Get()))) return false;
    if(FAILED(frame->SetSize(static_cast<UINT>(width),static_cast<UINT>(height)))) return false;
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;
    if(FAILED(frame->SetPixelFormat(&format))) return false;
    if(FAILED(frame->WritePixels(static_cast<UINT>(height),static_cast<UINT>(width)*4,
        static_cast<UINT>(width)*4*static_cast<UINT>(height),reinterpret_cast<BYTE*>(pixels.data())))) return false;
    if(FAILED(frame->Commit())||FAILED(encoder->Commit())) return false;
    return true;
}
std::string Trim(std::string value) {
    while(!value.empty()&&(value.back()=='\r'||value.back()=='\n'||value.back()==' ')) value.pop_back();
    std::size_t start=0;
    while(start<value.size()&&value[start]==' ') ++start;
    return value.substr(start);
}
std::vector<std::string> Split(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    for(const auto character:value) {
        if(character==' ') { if(!current.empty()) parts.push_back(current);current.clear(); }
        else current.push_back(character);
    }
    if(!current.empty()) parts.push_back(current);
    return parts;
}
}

int wmain(int argc,wchar_t** argv) {
    harness::SilenceDebugAssertDialogs();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED))) return 2;
    std::wstring instance;
    for(int i=1;i+1<argc;++i) if(std::wstring(argv[i])==L"--id") instance=argv[i+1];
    if(instance.empty()) { std::cerr<<"usage: geometry-probe --id <instance>\n";return 2; }
    try {
        const auto snapshot=PipeRequest(instance,{{"command","dump"}});
        const auto panel=reinterpret_cast<HWND>(snapshot.at("panelHwnd").get<std::uintptr_t>());
        const auto strip=reinterpret_cast<HWND>(snapshot.at("stripHwnd").get<std::uintptr_t>());
        const auto monitors=Monitors();
        std::cout<<Json{{"ready",true},{"monitors",monitors.size()},
            {"panel",reinterpret_cast<std::uintptr_t>(panel)},{"strip",reinterpret_cast<std::uintptr_t>(strip)}}.dump()<<'\n'<<std::flush;

        std::atomic<bool> sampling{false};
        std::thread sampler;
        std::atomic<unsigned long long> samples{0};
        BandBaseline baseline;
        std::string line;
        while(std::getline(std::cin,line)) {
            const auto parts=Split(Trim(line));
            if(parts.empty()) continue;
            if(parts[0]=="sample") {
                if(sampling) { std::cout<<Json{{"error","already sampling"}}.dump()<<'\n'<<std::flush;continue; }
                const std::string file=parts[1];
                unsigned interval=parts.size()>2?static_cast<unsigned>(std::stoul(parts[2])):50;
                if(interval==0||interval>kMaxIntervalMs) interval=kMaxIntervalMs;
                samples=0;
                sampling=true;
                sampler=std::thread([&,file,interval]{
                    std::ofstream out(file,std::ios::trunc);
                    out<<"{\"interval\":"<<interval<<"}\n";
                    const auto start=GetTickCount64();
                    while(sampling) {
                        bool panelVisible=false,stripVisible=false;
                        const auto panelRect=RectJson(panel,panelVisible);
                        const auto stripRect=RectJson(strip,stripVisible);
                        out<<Json{{"t",GetTickCount64()-start},{"panel",panelRect},{"strip",stripRect},
                            {"panelVisible",panelVisible},{"stripVisible",stripVisible}}.dump()<<'\n';
                        out.flush();
                        ++samples;
                        Sleep(interval);
                    }
                });
                std::cout<<Json{{"sampling",true},{"interval",interval},{"file",file}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="stop") {
                if(sampling) { sampling=false;sampler.join(); }
                std::cout<<Json{{"stopped",true},{"samples",samples.load()}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="pipe") {
                const auto request=Json::parse(line.substr(5));
                std::cout<<PipeRequest(instance,request).dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="shotmon") {
                const auto index=static_cast<std::size_t>(std::stoul(parts[2]));
                if(index>=monitors.size()) { std::cout<<Json{{"error","monitor index out of range"}}.dump()<<'\n'<<std::flush;continue; }
                const auto& monitor=monitors[index];
                const int width=monitor.bounds.right-monitor.bounds.left,height=monitor.bounds.bottom-monitor.bounds.top;
                const bool saved=SavePng(std::wstring(parts[1].begin(),parts[1].end()),monitor.bounds.left,monitor.bounds.top,width,height);
                std::cout<<Json{{"saved",saved},{"file",parts[1]},{"width",width},{"height",height},
                    {"left",monitor.bounds.left},{"top",monitor.bounds.top}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="baseline") {
                const auto index=static_cast<std::size_t>(std::stoul(parts[1]));
                if(index>=monitors.size()) { std::cout<<Json{{"error","monitor index out of range"}}.dump()<<'\n'<<std::flush;continue; }
                int x=0,y=0,width=0,height=0;
                sscanf_s(parts[2].c_str(),"%d,%d,%d,%d",&x,&y,&width,&height);
                if(width<=0||height<=0) { std::cout<<Json{{"error","band required"}}.dump()<<'\n'<<std::flush;continue; }
                baseline.valid=CaptureBand(x,y,width,height,baseline.pixels);
                baseline.x=x;baseline.y=y;baseline.width=width;baseline.height=height;
                std::cout<<Json{{"baseline",baseline.valid},{"x",x},{"y",y},{"width",width},{"height",height}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="colorcompare") {
                if(!baseline.valid) { std::cout<<Json{{"error","no baseline"}}.dump()<<'\n'<<std::flush;continue; }
                int red=0,green=0,blue=0,limit=12;
                sscanf_s(parts[2].c_str(),"%d,%d,%d",&red,&green,&blue);
                if(parts.size()>3) limit=std::stoi(parts[3]);
                std::vector<std::uint32_t> current;
                if(!CaptureBand(baseline.x,baseline.y,baseline.width,baseline.height,current)) {
                    std::cout<<Json{{"error","capture failed"}}.dump()<<'\n'<<std::flush;continue;
                }
                const auto matches=[](std::uint32_t pixel,int red,int green,int blue,int limit) {
                    return std::abs(static_cast<int>((pixel>>16)&0xFF)-red)<=limit
                        &&std::abs(static_cast<int>((pixel>>8)&0xFF)-green)<=limit
                        &&std::abs(static_cast<int>(pixel&0xFF)-blue)<=limit;
                };
                unsigned long long gained=0;
                int minX=baseline.width,maxX=-1,minY=baseline.height,maxY=-1;
                // Largest 4-connected run of gained pixels. A window that paints out of bounds lays
                // down a block or a long edge, while desktop noise and a cursor passing through leave
                // a handful of isolated pixels - the run length separates the two without trusting a
                // global count.
                std::vector<unsigned char> mask(static_cast<std::size_t>(baseline.width)*baseline.height,0);
                for(std::size_t i=0;i<current.size();++i) {
                    if(!matches(current[i],red,green,blue,limit)) continue;
                    if(matches(baseline.pixels[i],red,green,blue,limit)) continue;
                    ++gained;
                    mask[i]=1;
                    const int x=static_cast<int>(i%static_cast<std::size_t>(baseline.width));
                    const int y=static_cast<int>(i/static_cast<std::size_t>(baseline.width));
                    if(x<minX) minX=x;
                    if(x>maxX) maxX=x;
                    if(y<minY) minY=y;
                    if(y>maxY) maxY=y;
                }
                std::size_t maxComponent=0;
                if(gained>0) {
                    std::vector<int> stack;
                    for(int y=0;y<baseline.height;++y) {
                        for(int x=0;x<baseline.width;++x) {
                            const auto index=static_cast<std::size_t>(y)*static_cast<std::size_t>(baseline.width)
                                +static_cast<std::size_t>(x);
                            if(!mask[index]) continue;
                            std::size_t size=0;
                            stack.push_back(static_cast<int>(index));
                            mask[index]=0;
                            while(!stack.empty()) {
                                const int seed=stack.back();stack.pop_back();
                                ++size;
                                const int cx=seed%baseline.width,cy=seed/baseline.width;
                                const int neighbours[4][2]={{cx-1,cy},{cx+1,cy},{cx,cy-1},{cx,cy+1}};
                                for(const auto& neighbour:neighbours) {
                                    const int nx=neighbour[0],ny=neighbour[1];
                                    if(nx<0||ny<0||nx>=baseline.width||ny>=baseline.height) continue;
                                    const auto next=static_cast<std::size_t>(ny)*static_cast<std::size_t>(baseline.width)
                                        +static_cast<std::size_t>(nx);
                                    if(!mask[next]) continue;
                                    mask[next]=0;
                                    stack.push_back(static_cast<int>(next));
                                }
                            }
                            if(size>maxComponent) maxComponent=size;
                        }
                    }
                }
                std::cout<<Json{{"colorMatchPixels",gained},{"maxComponent",maxComponent},{"total",current.size()},
                    {"minX",maxX<0?-1:minX},{"maxX",maxX},{"minY",maxY<0?-1:minY},{"maxY",maxY},
                    {"red",red},{"green",green},{"blue",blue},{"tolerance",limit}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="compare") {
                if(!baseline.valid) { std::cout<<Json{{"error","no baseline"}}.dump()<<'\n'<<std::flush;continue; }
                std::vector<std::uint32_t> current;
                if(!CaptureBand(baseline.x,baseline.y,baseline.width,baseline.height,current)) {
                    std::cout<<Json{{"error","capture failed"}}.dump()<<'\n'<<std::flush;continue;
                }
                unsigned long long changed=0;unsigned maxDelta=0;
                // Bounding box of the changed pixels: "out-of-bounds drawing" is a block on the
                // neighbour screen, while a thin column at the seam is a compositor window shadow.
                int minX=baseline.width,maxX=-1,minY=baseline.height,maxY=-1;
                for(std::size_t i=0;i<current.size();++i) {
                    const auto before=baseline.pixels[i],after=current[i];
                    const int delta=std::abs(static_cast<int>((before>>16)&0xFF)-static_cast<int>((after>>16)&0xFF))
                        +std::abs(static_cast<int>((before>>8)&0xFF)-static_cast<int>((after>>8)&0xFF))
                        +std::abs(static_cast<int>(before&0xFF)-static_cast<int>(after&0xFF));
                    if(static_cast<unsigned>(delta)>maxDelta) maxDelta=static_cast<unsigned>(delta);
                    if(delta<=24) continue;
                    ++changed;
                    const int x=static_cast<int>(i%static_cast<std::size_t>(baseline.width));
                    const int y=static_cast<int>(i/static_cast<std::size_t>(baseline.width));
                    if(x<minX) minX=x;
                    if(x>maxX) maxX=x;
                    if(y<minY) minY=y;
                    if(y>maxY) maxY=y;
                }
                const int changedWidth=maxX<0?0:maxX-minX+1;
                std::cout<<Json{{"changedPixels",changed},{"total",current.size()},{"maxDelta",maxDelta},
                    {"changedWidth",changedWidth},{"changedMinX",maxX<0?-1:minX},{"changedMaxX",maxX},
                    {"changedMinY",maxY<0?-1:minY},{"changedMaxY",maxY},
                    {"x",baseline.x},{"y",baseline.y},{"width",baseline.width},{"height",baseline.height}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="monitors") {
                Json list=Json::array();
                for(const auto& monitor:monitors) list.push_back({{"bounds",{monitor.bounds.left,monitor.bounds.top,
                    monitor.bounds.right-monitor.bounds.left,monitor.bounds.bottom-monitor.bounds.top}}});
                std::cout<<Json{{"monitors",list}}.dump()<<'\n'<<std::flush;
                continue;
            }
            if(parts[0]=="quit") break;
            std::cout<<Json{{"error","unknown probe command"}}.dump()<<'\n'<<std::flush;
        }
        if(sampling) { sampling=false;sampler.join(); }
    } catch(const std::exception& error) {
        std::cout<<Json{{"error",error.what()}}.dump()<<'\n'<<std::flush;
        return 1;
    }
    return 0;
}
