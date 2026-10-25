#pragma once
#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace hermes {
// Debug control plane required by DESIGN §8: a named pipe that carries one JSON request per
// message. Payloads are length-prefixed so requests may contain arbitrary UTF-8. The handler is
// invoked on the owning (UI) thread through a window message, so all window state stays
// single-threaded.
class ControlPipe {
public:
    using Handler = std::function<std::string(const std::string&)>;
    ControlPipe() = default;
    ~ControlPipe() { Stop(); }
    ControlPipe(const ControlPipe&) = delete;
    ControlPipe& operator=(const ControlPipe&) = delete;
    bool Start(HWND owner, UINT message, const std::wstring& pipeName, Handler handler);
    void Stop();
    unsigned Requests() const { return requests_; }
private:
    void Serve();
    HANDLE stopEvent_=nullptr;
    std::wstring name_;
    Handler handler_;
    std::thread thread_;
    std::atomic<unsigned> requests_{0};
};
}
