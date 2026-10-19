#pragma once
#include <string>
#include "data/http.h"

// Reachability probes (PLAN §2.4, H3): WebUI ports are tried 8787 -> 9119 with the chosen one
// winning, the gateway answers GET /health, and all of it runs off the UI thread in the caller.
namespace hermes {
// TCP connect check to host:port. True when the connect succeeds inside the timeout.
bool TcpReachable(const std::string& host,unsigned port,unsigned timeoutMs);
// GET http://host:port/health; transportOk + status 2xx means reachable.
HttpResult HealthCheck(const std::string& host,unsigned port,unsigned timeoutMs);
}
