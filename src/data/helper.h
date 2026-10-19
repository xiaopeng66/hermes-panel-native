#pragma once
#include <functional>
#include <string>

// The WSL helper bridge (PLAN §2.1, H3): spawns `wsl.exe -d <distro> -- <python> <helper>`, writes one
// JSON request line to stdin, reads one JSON reply line from stdout. One process per request, exactly
// like the old panel. INV-H1/H2: the bridge is transport only - verbs and payload shapes are the
// helper's contract, nothing is rewritten here.
namespace hermes {
struct HelperReply {
    bool transcriptOk=false;    // the process ran and the line exchange completed
    bool ok=false;              // the reply carried ok:true (or the helper exited 0 with data)
    int exitCode=-1;            // helper exit status: 0 ok / 1 business / 2 protocol
    bool timedOut=false;        // killed after the deadline
    std::string data="{}";      // raw JSON text of the reply's "data" object
    std::string error;          // reply "error", or the transport description when transcriptOk=false
    bool gbkRecovered=false;    // the raw bytes were not UTF-8 and were re-read as GBK
};

class HelperBridge {
public:
    void Configure(std::wstring distro,std::wstring pythonExe,std::wstring helperPath);
    // Tests swap in a fake wsl; production leaves wsl.exe on PATH.
    void SetWslExecutable(std::wstring path);
    // Diagnostics sink. NEVER receives key/password/token values - only verbs, exit codes, counts.
    void SetLogSink(std::function<void(const std::string&)> sink);
    // One request/response round. `requestLine` is the full JSON line ({"cmd":"...",...}).
    HelperReply Call(const std::string& requestLine,unsigned timeoutMs);
    // `wsl.exe -d <distro> -- true` - true when WSL answers at all.
    bool WslReachable();

private:
    std::wstring wslExe_=L"wsl.exe";
    std::wstring distro_,pythonExe_,helperPath_;
    std::function<void(const std::string&)> logSink_;
    unsigned long long lastReadStart_=0;
};
}
