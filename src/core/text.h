#pragma once
// UTF-8 <-> UTF-16 转换：**全产品只此一份**。
// 之前 application_internal.h / settings.cpp / panel.cpp 各写了一份逻辑相同的 Widen/Narrow
// （我已核对三者逐行等价，没有行为漂移），但那正是编码类 bug 的温床——在其中一个里修、
// 另外两个照旧错（本项目历史上被 CJK 源码 / cp936 / "逐字节加宽"坑过多次）。
// 放在 core/ 下，产品侧统一 include 它；测试/工具各自的夹具副本不受影响（它们独立编译）。
#include <string>
#include <windows.h>

namespace hermes {
inline std::string Narrow(const std::wstring& value) {
    if(value.empty()) return {};
    const int length=WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),
        nullptr,0,nullptr,nullptr);
    std::string result(static_cast<std::size_t>(length),'\0');
    WideCharToMultiByte(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length,
        nullptr,nullptr);
    return result;
}
inline std::wstring Widen(const std::string& value) {
    if(value.empty()) return {};
    const int length=MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),
        nullptr,0);
    std::wstring result(static_cast<std::size_t>(length),L'\0');
    MultiByteToWideChar(CP_UTF8,0,value.data(),static_cast<int>(value.size()),result.data(),length);
    return result;
}
// 粗略文本宽度估算（DIP）：CJK 按 1em、ASCII 按 0.55em。
// 给"需要按内容定宽、但手边没有 IDWriteTextFormat"的地方用（例如贴条悬浮卡的宽度）。
// 渲染层自己的 EstimateWidth（layout.cpp）是同一套比例，只在那里给分段排宽用。
inline float EstimateTextWidth(const std::string& text,float fontSize) {
    float width=0.0f;
    for(std::size_t index=0;index<text.size();++index) {
        const auto byte=static_cast<unsigned char>(text[index]);
        if(byte>=0xC0) {
            // UTF-8 多字节：只按首字节记一次宽度（后续续字节跳过）。
            width+=fontSize;
            while(index+1<text.size()
                &&(static_cast<unsigned char>(text[index+1])&0xC0)==0x80) ++index;
            continue;
        }
        width+=byte==' '?fontSize*0.28f:fontSize*0.55f;
    }
    return width;
}
}
