#include "data/state.h"
#include <json.hpp>
#include <algorithm>

namespace hermes {
namespace {
using Json=nlohmann::json;
Json ParseOrEmpty(const std::string& text) {
    try { return Json::parse(text); } catch(const std::exception&) { return Json::object(); }
}
std::string Str(const Json& value,const char* key,const std::string& fallback={}) {
    if(value.contains(key)&&value[key].is_string()) return value[key].get<std::string>();
    return fallback;
}
long long Int(const Json& value,const char* key,long long fallback=0) {
    if(value.contains(key)&&value[key].is_number()) return value[key].get<long long>();
    return fallback;
}
bool Bool(const Json& value,const char* key,bool fallback=false) {
    if(value.contains(key)&&value[key].is_boolean()) return value[key].get<bool>();
    return fallback;
}
}

HermesState ParseState(const std::string& listJson,const std::string& healthJson,
    const std::string& webuiJson) {
    HermesState state;
    const auto list=ParseOrEmpty(listJson);
    const auto health=ParseOrEmpty(healthJson);
    const auto webuiRoot=ParseOrEmpty(webuiJson);
    // ---- status.*（health 回复）----
    const auto status=health.contains("status")&&health["status"].is_object()
        ?health["status"]:health;
    state.wsl=Str(status,"wsl","unknown");
    state.checkedAt=Str(status,"checkedAt");
    state.error=Str(status,"error");
    if(status.contains("config")&&status["config"].is_object()) {
        const auto& config=status["config"];
        state.config.mtime=Int(config,"mtime");
        state.config.mtimeText=Str(config,"mtime_text");
        state.config.size=Int(config,"size");
        state.config.mode=Str(config,"mode");
        state.config.parses=Bool(config,"parses",true);
    }
    if(status.contains("webui")&&status["webui"].is_object()) {
        const auto& webui=status["webui"];
        state.webui.port=static_cast<int>(std::clamp<long long>(Int(webui,"port",8787),1,65535));
        state.webui.reachable=Bool(webui,"reachable");
        state.webui.winReachable=Bool(webui,"winReachable");
        state.webui.ports.clear();
        if(webui.contains("ports")&&webui["ports"].is_array())
            for(const auto& port:webui["ports"])
                if(port.is_number()) state.webui.ports.push_back(static_cast<int>(port.get<long long>()));
    }
    if(status.contains("gateway")&&status["gateway"].is_object()) {
        const auto& gateway=status["gateway"];
        state.gateway.port=static_cast<int>(std::clamp<long long>(Int(gateway,"port",8642),1,65535));
        state.gateway.reachable=Bool(gateway,"reachable");
        state.gateway.since=Str(gateway,"since");
        state.gateway.sinceUnix=Int(gateway,"sinceUnix");
        state.gateway.active=Str(gateway,"active");
    }
    // ---- 内容（list 回复）----
    if(list.contains("model")&&list["model"].is_object()) {
        const auto& model=list["model"];   // helper: {"default":..,"provider":..,"api_mode":..,"base_url":..}
        state.model.provider=Str(model,"provider");
        state.model.model=Str(model,"default");
        state.model.apiMode=Str(model,"api_mode");
        state.model.baseUrl=Str(model,"base_url");
    }
    if(list.contains("providers")&&list["providers"].is_array()) {
        for(const auto& entry:list["providers"]) {
            ProviderView provider;
            provider.name=Str(entry,"name");
            provider.providerId=Str(entry,"provider_id");
            provider.baseUrl=Str(entry,"base_url");
            provider.keyEnv=Str(entry,"key_env");
            provider.apiMode=Str(entry,"api_mode");
            provider.defaultModel=Str(entry,"default_model");
            provider.line=Str(entry,"line");
            if(entry.contains("models")&&entry["models"].is_array())
                for(const auto& item:entry["models"])
                    if(item.is_string()) provider.models.push_back(item.get<std::string>());
            provider.keySet=Bool(entry,"key_set");
            provider.modelsDiscovered=Bool(entry,"models_discovered");
            provider.isActive=Bool(entry,"is_active");
            state.providers.push_back(provider);
        }
    }
    if(list.contains("fallbacks")&&list["fallbacks"].is_array()) {
        for(const auto& entry:list["fallbacks"]) {
            FallbackView fallback;
            fallback.provider=Str(entry,"provider");
            fallback.model=Str(entry,"model");
            // base_url/api_mode 之前被丢掉了：逐条探测要按行发 probe（带这一行的 base_url/key_env），
            // 没有它就只能退回"助手把所有行一次性探完"的老接口（没有逐行进度）。
            fallback.baseUrl=Str(entry,"base_url");
            fallback.apiMode=Str(entry,"api_mode");
            fallback.keyEnv=Str(entry,"key_env");
            fallback.keySet=Bool(entry,"key_set");
            fallback.index=static_cast<int>(Int(entry,"index"));
            state.fallbacks.push_back(fallback);
        }
    }
    if(list.contains("recents")&&list["recents"].is_array())
        for(const auto& entry:list["recents"])
            if(entry.is_string()) state.recents.push_back(entry.get<std::string>());
    if(list.contains("favorites")&&list["favorites"].is_array())
        for(const auto& entry:list["favorites"])
            if(entry.is_string()) state.favorites.push_back(entry.get<std::string>());
    state.busy=Bool(list,"busy");
    state.loading=Bool(list,"loading",true);
    // ---- webui.groups（webui-models 回复）----
    const auto webui=webuiRoot.contains("data")&&webuiRoot["data"].is_object()
        ?webuiRoot["data"]:webuiRoot;
    if(webui.contains("port")) state.webuiGroups.port=static_cast<int>(Int(webui,"port",state.webuiGroups.port));
    if(webui.contains("groups")&&webui["groups"].is_array()) {
        state.webuiGroups.present=true;
        state.webuiGroups.rawJson=webui["groups"].dump();
    }
    return state;
}

namespace {
std::string LowerAscii(std::string value) {
    for(auto& character:value) if(character>='A'&&character<='Z') character=static_cast<char>(character+32);
    return value;
}
// 归一化：去掉命名空间（最后一个 '/' 之前）与常见供应商包装前缀。
std::string NormalizeModelId(const std::string& model) {
    std::string value=LowerAscii(model);
    if(const auto slash=value.find_last_of('/');slash!=std::string::npos) value=value.substr(slash+1);
    for(const auto* prefix:{"workbuddy-","custom-","custom:","openai/","anthropic/"}) {
        const std::string head(prefix);
        if(value.rfind(head,0)==0) { value=value.substr(head.size());break; }
    }
    return value;
}
}
bool ModelListHasModel(const std::string& model,const std::vector<std::string>& models) {
    if(model.empty()) return false;
    const auto wanted=NormalizeModelId(model);
    for(const auto& candidate:models) {
        const auto normalized=NormalizeModelId(candidate);
        if(normalized==wanted) return true;
        // 尾巴匹配：一边是另一边去掉前缀后的形式（deepseek-v4.1-flash vs v4.1-flash）。
        if(normalized.size()>wanted.size()&&normalized.ends_with("-"+wanted)) return true;
        if(wanted.size()>normalized.size()&&wanted.ends_with("-"+normalized)) return true;
    }
    return false;
}
std::string Signature(const HermesState& state) {
    std::string signature=state.model.provider+"|"+state.model.model+"|"+state.model.apiMode
        +"|wsl="+state.wsl;
    for(const auto& provider:state.providers) {
        signature+="|p:"+provider.providerId+":"+provider.defaultModel+":"
            +(provider.isActive?"a":"-")+":"+std::to_string(provider.models.size());
        for(const auto& item:provider.models) signature+=":"+item;
    }
    for(const auto& fallback:state.fallbacks)
        signature+="|f:"+std::to_string(fallback.index)+":"+fallback.provider+":"+fallback.model;
    for(const auto& recent:state.recents) signature+="|r:"+recent;
    for(const auto& favorite:state.favorites) signature+="|v:"+favorite;
    if(state.webuiGroups.present) signature+="|g:"+state.webuiGroups.rawJson;
    return signature;
}
}
