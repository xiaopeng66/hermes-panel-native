#pragma once
#include <string>
#include <vector>

// The hermes state model (PLAN §2.2, H3): exactly the fields the old `panel:state` push carried, plus
// the redraw signature - the old renderer only rebuilt the list when model/providers/fallbacks/
// favorites/recents/webui groups/status.wsl actually changed, and the native panel keeps that rule so
// scrolling and typing are never interrupted by a poll.
namespace hermes {
struct ConfigStatus { long long mtime=0;std::string mtimeText;long long size=0;std::string mode;bool parses=true; };
struct WebuiStatus { int port=8787;bool reachable=false,winReachable=false;std::vector<int> ports{8787,9119}; };
struct GatewayStatus { int port=8642;bool reachable=false;std::string since;long long sinceUnix=0;std::string active; };
struct ModelInfo { std::string provider,model,apiMode,baseUrl; };   // list.model: default/provider/...
struct ProviderView {
    std::string name,providerId,baseUrl,keyEnv,apiMode,line;
    std::vector<std::string> models;          // helper's "models": the /models list (an array)
    std::string defaultModel;
    bool keySet=false,modelsDiscovered=false,isActive=false;
};
struct FallbackView { std::string provider,model,state,baseUrl,apiMode,keyEnv;bool keySet=false;int index=0; };
struct WebuiGroups { int port=8787;bool present=false;std::string rawJson; };  // parsed by pages (H4)

struct HermesState {
    // status.* (from health + probes)
    std::string wsl="unknown";        // up / down / unknown - unknown paints a loading lamp, not offline
    std::string checkedAt,error;
    ConfigStatus config;
    WebuiStatus webui;
    GatewayStatus gateway;
    // content (from list)
    ModelInfo model;
    std::vector<ProviderView> providers;
    std::vector<FallbackView> fallbacks;
    WebuiGroups webuiGroups;
    std::vector<std::string> recents,favorites;
    bool busy=false,loading=true;
};

// Builds the state from the three helper verb replies (raw JSON strings, each already ok:true).
HermesState ParseState(const std::string& listJson,const std::string& healthJson,
    const std::string& webuiJson);
// 探测结果里"有没有这个模型"的判定：不区分大小写，并接受三种等价写法（真机踩过）：
//   ① 完全相同           deepseek-v4.1-flash == deepseek-v4.1-flash
//   ② 带/不带命名空间     deepseek-ai/deepseek-v4.1-flash ↔ deepseek-v4.1-flash
//   ③ 带/不带供应商前缀   workbuddy-deepseek-v4.1-flash ↔ deepseek-v4.1-flash
bool ModelListHasModel(const std::string& model,const std::vector<std::string>& models);
// Redraw signature: only these inputs may trigger a list rebuild (PLAN §2.2).
std::string Signature(const HermesState& state);
}
