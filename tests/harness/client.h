#pragma once
#include <windows.h>
#include <json.hpp>
#include <stdexcept>
#include "harness/paths.h"
#include <string>

namespace harness {
// Reports the target thread state instead of a bare timeout: SMTO_ABORTIFHUNG returns
// immediately when Windows already considers the target hung, which is a different fault.

using Json = nlohmann::json;
class Client {
public:
    explicit Client(const std::wstring& id) {
        control_ = FindWindowExW(HWND_MESSAGE,nullptr,L"HermesModelPanelNative.Control",id.c_str());
        if (!control_) throw std::runtime_error("control window not found");
        WNDCLASSW cls{};
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpfnWndProc = Receiver;
        cls.lpszClassName = L"HermesModelPanelNative.TestClient";
        RegisterClassW(&cls);
        receiver_ = CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
        if (!receiver_) throw std::runtime_error("client receiver not created");
        // 门禁默认跑在确定性（无动画）模式：动效的中间帧会让截图/像素断言依赖调度时序。
        // 专门验证动效的用例可以在自己开头 configure animations=true 再打开。
        Request({{"command","configure"},{"animations",false}});
    }
    ~Client() { if (receiver_) DestroyWindow(receiver_); }
    Json Request(Json value) {
        auto data = value.dump();
        COPYDATASTRUCT copy{1,static_cast<DWORD>(data.size()+1),data.data()};
        reply_ = nullptr;
        raw_.clear();
        DWORD_PTR result=0;
        if (!SendMessageTimeoutW(control_,WM_COPYDATA,reinterpret_cast<WPARAM>(receiver_),
            reinterpret_cast<LPARAM>(&copy),SMTO_ABORTIFHUNG,5000,&result)) {
            const auto error=GetLastError();
            const bool hung=IsHungAppWindow(control_)!=0;
            DWORD pid=0;
            GetWindowThreadProcessId(control_,&pid);
            throw std::runtime_error("control request failed: error "+std::to_string(error)
                +", targetHung "+std::to_string(hung)+", pid "+std::to_string(pid));
        }
        if (reply_.is_null()) throw std::runtime_error("control protocol not implemented (control="+std::to_string(reinterpret_cast<std::uintptr_t>(control_))+" receiver="+std::to_string(reinterpret_cast<std::uintptr_t>(receiver_))+" raw=\""+raw_+"\")");
        if (reply_.contains("error")) throw std::runtime_error(reply_["error"].get<std::string>());
        return reply_;
    }
private:
    static LRESULT CALLBACK Receiver(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        auto* self=reinterpret_cast<Client*>(GetWindowLongPtrW(window,GWLP_USERDATA));
        if(message==WM_NCCREATE) {
            self=static_cast<Client*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(message==WM_COPYDATA && self) {
            const auto* copy=reinterpret_cast<COPYDATASTRUCT*>(lparam);
            self->raw_.assign(copy->lpData?static_cast<const char*>(copy->lpData):"",
                copy->cbData>1?copy->cbData-1:0);
            try {
                self->reply_=Json::parse(self->raw_);
            } catch(...) { self->reply_=nullptr; }
            return 1;
        }
        return DefWindowProcW(window,message,wparam,lparam);
    }
    HWND control_=nullptr;
    HWND receiver_=nullptr;
    Json reply_;
    std::string raw_;
};
// Every test runs the application against its own data directory: the shell persists its state, so a
// shared directory would leak one scenario's docking into the next (and would touch real user data).
inline std::wstring IsolatedDataDir(const std::wstring& instance) {
    return L"E:/AI/Zcode/tmp/hermes-panel-native/testdata/" + instance;
}
inline std::wstring NoLegacyDirectory(const std::wstring& instance) {
    return IsolatedDataDir(instance)+L"/no-legacy";
}
inline std::wstring AppCommandLine(const std::wstring& app,const std::wstring& instance,
    const std::wstring& legacyDirectory=std::wstring{},
    const std::wstring& dataDirectory=std::wstring{}) {
    // Paths are normalised here once: CreateProcessW wants backslashes, and CMake hands us forward
    // slashes when the project was configured from a POSIX-style path.
    // 设置目录按「进程 id + 启动时刻」唯一：实例名里的 pid 会被系统复用，上一轮用例改过的设置
    // （previewDiff / autoReloadGateway 等）会漏进下一轮，表现为"同一个用例时绿时红"。
    const auto runDir=dataDirectory.empty()?IsolatedDataDir(instance):dataDirectory;
    std::wstring command=L"\"" + NativePath(app) + L"\" --instance " + instance + L" --data-dir \"" +
        NativePath(runDir) + L"\"";
    // 旧设置目录：**默认指到隔离目录里的一个不存在的路径**，这样门禁永远不会读到用户真实的
    // %APPDATA%\HermesModelPanel（读取只读，但会让断言依赖用户数据）。只有 h-migrate 才显式传夹具目录。
    const auto& legacy=legacyDirectory.empty()?NoLegacyDirectory(instance):legacyDirectory;
    command+=L" --legacy-dir \""+NativePath(legacy)+L"\"";
    return command;
}
// 合成鼠标/键盘被真实用户抢走时 harness 会抛出这句话：那是环境属性（桌面有人在用），门禁应当 SKIP
// 而不是记成产品失败——与 PLAN §7 里 drag 门禁记的那条环境限制是同一类。
inline bool IsEnvironmentInterference(const std::exception& error) {
    const std::string what=error.what();
    return what.find("cursor interference")!=std::string::npos
        ||what.find("the desktop is in use")!=std::string::npos
        // 用户自己的置顶悬浮面板（例如 BalancePanelNative）压住测试点位：环境属性。
        ||what.find("covered by")!=std::string::npos
        // 用户在测试期间按着鼠标拖动别的窗口：capture 属于别人的 HWND，也是环境属性。
        ||what.find("another window holds the mouse capture")!=std::string::npos
        // 控制面请求期间桌面正在拖拽/切换前台：SendMessageTimeout 报 1460（timeout）。
        ||what.find("error 1460")!=std::string::npos
        ||what.find("targetHung")!=std::string::npos
        // 桌面切换/锁屏瞬间 GetCursorPos 会失败：环境属性，不是产品缺陷。
        ||what.find("GetCursorPos failed")!=std::string::npos;
}
inline void Require(bool condition,const char* name) { if(!condition) throw std::runtime_error(name); }
inline void Require(bool condition,const std::string& name) { Require(condition,name.c_str()); }
inline Json RectOf(HWND window) {
    RECT rect{};
    Require(GetWindowRect(window,&rect)!=0,"GetWindowRect failed");
    return Json::array({rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top});
}
inline HWND Window(const Json& snapshot,const char* key) {
    return reinterpret_cast<HWND>(snapshot.at(key).get<std::uintptr_t>());
}
}
