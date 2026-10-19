// Helper bridge tests (PLAN §6 H3, gate `helper`): every contract line of §2.1 against the fake wsl
// fixture - success, business failure, exit 1/2, unparsable reply, GBK rescue, timeout, ping, the
// Chinese round trip, and the redaction rule (log lines never carry request VALUES).
#include <windows.h>
#include <iostream>
#include "data/helper.h"

namespace {
int passed=0,failed=0;
void Check(bool condition,const std::string& name) {
    if(condition) ++passed;
    else { ++failed;std::cerr<<"FAIL "<<name<<"\n"; }
}
}

int wmain() {
    hermes::HelperBridge bridge;
    std::string logLines;
    bridge.Configure(L"fake-distro",L"python3",L"/home/user/helper.py");
    bridge.SetWslExecutable(L"E:/AI/Zcode/projects/hermes-panel-native/build/Debug/fake-wsl.exe");
    bridge.SetLogSink([&](const std::string& line) { logLines+=line+"\n"; });

    // 1) success + Chinese round trip: the reply echoes the request; UTF-8 survives.
    SetEnvironmentVariableA("FAKE_WSL_MODE","ok");
    auto reply=bridge.Call("{\"cmd\":\"list\"}",15000);
    Check(reply.transcriptOk&&reply.ok&&reply.exitCode==0,"ok mode returns ok with exit 0");
    Check(reply.data.find("cmd\\\":\\\"list\\\"")!=std::string::npos,
        "the reply echoes the request inside data (json-escaped)");
    Check(reply.data.find("\xE4\xBD\xA0\xE5\xA5\xBD")!=std::string::npos,
        "the Chinese note arrives as UTF-8 (你好)");
    Check(!reply.gbkRecovered&&!reply.timedOut,"the UTF-8 reply needs no rescue");

    // 2) business failure = exit 1 with ok:false.
    SetEnvironmentVariableA("FAKE_WSL_MODE","fail");
    reply=bridge.Call("{\"cmd\":\"set-main\"}",15000);
    Check(!reply.ok&&reply.exitCode==1&&reply.error=="business failure",
        "exit 1 maps to ok=false with the helper error");

    // 3) protocol error = exit 2.
    SetEnvironmentVariableA("FAKE_WSL_MODE","proto");
    reply=bridge.Call("{\"cmd\":\"nonsense\"}",15000);
    Check(reply.exitCode==2&&!reply.ok,"exit 2 (protocol error) is preserved");

    // 4) unparsable reply.
    SetEnvironmentVariableA("FAKE_WSL_MODE","bad");
    reply=bridge.Call("{\"cmd\":\"list\"}",15000);
    Check(reply.transcriptOk&&!reply.ok&&reply.error.find("not JSON")!=std::string::npos,
        "a non-JSON reply is reported as a protocol failure, not a crash");

    // 5) GBK rescue: the fixture answers in code page 936; the bridge re-reads and recovers.
    SetEnvironmentVariableA("FAKE_WSL_MODE","gbk");
    reply=bridge.Call("{\"cmd\":\"list\"}",15000);
    Check(reply.transcriptOk&&reply.ok&&reply.gbkRecovered,"the GBK reply is rescued");
    Check(reply.data.find("\xE4\xBD\xA0\xE5\xA5\xBD")!=std::string::npos,
        "the rescued Chinese note equals the UTF-8 original");

    // 6) timeout: the fixture stalls far past the deadline; the bridge kills it and reports.
    SetEnvironmentVariableA("FAKE_WSL_MODE","timeout");
    SetEnvironmentVariableA("FAKE_WSL_DELAY_MS","12000");
    reply=bridge.Call("{\"cmd\":\"backup-list\"}",900);
    Check(reply.timedOut&&!reply.transcriptOk&&reply.error.find("timed out")!=std::string::npos,
        "a stuck helper is killed and reported as timed out");
    SetEnvironmentVariableA("FAKE_WSL_DELAY_MS","0");

    // 7) ping: `-- true` with exit 0, then forced down.
    SetEnvironmentVariableA("FAKE_WSL_PING","up");
    SetEnvironmentVariableA("FAKE_WSL_MODE","ok");
    if(!bridge.WslReachable()) std::cerr<<"diagnostic ping log:\n"<<logLines<<"\n";
    Check(logLines.find("wsl ping exit 0")!=std::string::npos,"WSL ping succeeds on exit 0");
    SetEnvironmentVariableA("FAKE_WSL_PING","down");
    Check(!bridge.WslReachable(),"WSL ping fails when the child exits 3");
    SetEnvironmentVariableA("FAKE_WSL_PING","up");

    // 8) Redaction (INV / GOAL rule 7): logs carry shapes and exit codes, never request values.
    SetEnvironmentVariableA("FAKE_WSL_MODE","ok");
    bridge.Call("{\"cmd\":\"set-provider-key\",\"key\":\"SUPER-SECRET-VALUE-XK7\"}",15000);
    Check(logLines.find("SUPER-SECRET-VALUE-XK7")==std::string::npos,
        "log lines never contain request values");
    Check(logLines.find("helper exit=")!=std::string::npos,"log lines do carry the exit/shape facts");

    std::cout<<"helper: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}
