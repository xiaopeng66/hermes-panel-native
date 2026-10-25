// Hermes state model tests (H3): parse the §2.2 shape out of fixture replies and prove the redraw
// signature only moves when the model/providers/fallbacks/favorites/recents/groups/wsl inputs move.
#include "data/state.h"
#include <functional>
#include <iostream>

namespace {
int passed=0,failed=0;
void Check(bool condition,const std::string& name) {
    if(condition) ++passed;
    else { ++failed;std::cerr<<"FAIL "<<name<<"\n"; }
}
std::string ListFixture(const std::string& model) {
    std::string fixture="{";
    fixture+="\"model\":{\"default\":\""+model+"\",\"provider\":\"custom:deepseek\","
        "\"base_url\":\"https://api.deepseek.example/v1\",\"api_mode\":\"chat_completions\"},";
    fixture+="\"providers\":[";
    fixture+="{\"name\":\"DeepSeek\",\"provider_id\":\"custom:deepseek\","
        "\"base_url\":\"https://api.deepseek.example/v1\",\"key_env\":\"DEEPSEEK_API_KEY\","
        "\"api_mode\":\"chat_completions\",\"default_model\":\""+model+"\","
        "\"models\":[\""+model+"\",\"deepseek-r2\"],\"models_discovered\":true,"
        "\"is_active\":true,\"line\":\"custom:deepseek\"},";
    fixture+="{\"name\":\"Local\",\"provider_id\":\"custom:local\","
        "\"base_url\":\"http://127.0.0.1:8642/v1\",\"key_env\":\"LOCAL_API_KEY\","
        "\"api_mode\":\"chat_completions\",\"default_model\":\"qwen-3-32b\","
        "\"models\":[],\"models_discovered\":false,\"is_active\":false,\"line\":\"custom:local\"}],";
    fixture+="\"fallbacks\":[{\"provider\":\"DeepSeek\",\"model\":\"deepseek-v4.1-flash\","
        "\"key_env\":\"DEEPSEEK_API_KEY\",\"key_set\":true,\"index\":0}],";
    fixture+="\"recents\":[\"custom:deepseek|deepseek-v4.1-flash\"],";
    fixture+="\"favorites\":[\"custom:deepseek|deepseek-v4.1-flash\"],";
    fixture+="\"busy\":false,\"loading\":false}";
    return fixture;
}
std::string HealthFixture(const std::string& wsl) {
    std::string fixture="{\"ok\":true,\"status\":{";
    fixture+="\"wsl\":\""+wsl+"\",\"checkedAt\":\"2026-10-15 18:30\",\"error\":\"\",";
    fixture+="\"config\":{\"mtime\":1760514611,\"mtime_text\":\"2026-10-15 18:30\",\"size\":2048,"
        "\"mode\":\"0644\",\"parses\":true},";
    fixture+="\"webui\":{\"port\":8787,\"reachable\":true,\"winReachable\":true,\"ports\":[8787,9119]},";
    fixture+="\"gateway\":{\"port\":8642,\"reachable\":true,\"since\":\"18:00\","
        "\"sinceUnix\":1760512800,\"active\":\"deepseek\"}}}";
    return fixture;
}
std::string WebuiFixture() {
    return "{\"ok\":true,\"data\":{\"port\":8787,\"groups\":["
        "{\"title\":\"DeepSeek\",\"models\":[\"deepseek-v4.1-flash\",\"deepseek-r2\"]}]}}";
}
}

int wmain() {
    using namespace hermes;
    const auto state=ParseState(ListFixture("deepseek-v4.1-flash"),HealthFixture("up"),WebuiFixture());
    // status
    Check(state.wsl=="up","wsl comes from the health reply");
    Check(state.webui.port==8787&&state.webui.reachable&&state.webui.winReachable,
        "webui port/reachability parse");
    Check(state.webui.ports.size()==2&&state.webui.ports[1]==9119,"the port candidate list parses");
    Check(state.gateway.port==8642&&state.gateway.sinceUnix==1760512800,"gateway status parses");
    Check(state.config.parses&&state.config.size==2048,"config status parses");
    // content
    Check(state.model.provider=="custom:deepseek"&&state.model.model=="deepseek-v4.1-flash",
        "the current model parses");
    Check(state.providers.size()==2&&state.providers[0].defaultModel=="deepseek-v4.1-flash"
        &&state.providers[1].modelsDiscovered==false,"providers parse with their fields");
    Check(state.fallbacks.size()==1&&state.fallbacks[0].index==0,"the fallback ladder parses with indexes");
    Check(state.favorites.size()==1&&state.recents.size()==1,"favorites/recents parse");
    Check(state.webuiGroups.present&&state.webuiGroups.rawJson.find("deepseek-r2")!=std::string::npos,
        "webui.groups survive as raw json for the models page");
    Check(!state.loading&&!state.busy,"list flags parse");

    // signature: stable when nothing changes
    const auto same=ParseState(ListFixture("deepseek-v4.1-flash"),HealthFixture("up"),WebuiFixture());
    Check(Signature(state)==Signature(same),"the signature is stable across identical states");
    // each tracked input moves it
    auto mutate=[](HermesState value,const std::function<void(HermesState&)>& patch) {
        patch(value);
        return value;
    };
    const auto differentModel=mutate(state,[](HermesState& value) { value.model.model="qwen-3-32b"; });
    Check(Signature(differentModel)!=Signature(state),"a model swap moves the signature");
    const auto wslDown=mutate(state,[](HermesState& value) { value.wsl="down"; });
    Check(Signature(wslDown)!=Signature(state),"wsl up->down moves the signature");
    const auto differentProviders=mutate(state,[](HermesState& value) {
        value.providers[1].defaultModel="glm-5-flash"; });
    Check(Signature(differentProviders)!=Signature(state),"a provider change moves the signature");
    const auto favoritesChanged=mutate(state,[](HermesState& value) { value.favorites.pop_back(); });
    Check(Signature(favoritesChanged)!=Signature(state),"a favorite change moves the signature");
    const auto groupsChanged=mutate(state,[](HermesState& value) { value.webuiGroups.rawJson="[]"; });
    Check(Signature(groupsChanged)!=Signature(state),"a webui-groups change moves the signature");
    // v1.31（契约变更，断言按新契约改写）：签名原本只决定"列表要不要重建"，因此可达性不计入；
    // 现在它还决定**整窗要不要重绘**（v1.28 的跳过逻辑），可达性/网关启动时刻/mtime/错误横幅
    // 都会改变画面（三盏灯、同步时间戳、待重启胶囊、错误横幅），必须计入——否则灯变了界面不刷新。
    const auto reachable=mutate(state,[](HermesState& value) { value.webui.reachable=false; });
    Check(Signature(reachable)!=Signature(state),"webui reachability moves the signature (it repaints lamps)");
    const auto gatewayDown=mutate(state,[](HermesState& value) { value.gateway.reachable=false; });
    Check(Signature(gatewayDown)!=Signature(state),"gateway reachability moves the signature");
    const auto gatewayRestarted=mutate(state,[](HermesState& value) { value.gateway.sinceUnix+=60; });
    Check(Signature(gatewayRestarted)!=Signature(state),"the gateway start time moves the signature");
    const auto mtimeChanged=mutate(state,[](HermesState& value) { value.config.mtime+=60; });
    Check(Signature(mtimeChanged)!=Signature(state),"config mtime moves the signature (sync stamp)");
    const auto helperBroken=mutate(state,[](HermesState& value) { value.error="helper failed"; });
    Check(Signature(helperBroken)!=Signature(state),"the helper error banner moves the signature");
    Check(Signature(HermesState{})==Signature(HermesState{}),"the empty state has a stable signature");
    Check(ParseState("{bad json",HealthFixture("up"),WebuiFixture()).providers.empty(),
        "unparsable list input yields an empty state instead of throwing");

    // ---- 恶意/畸形回复：逐字段类型检查必须兜住，任何一处都不能抛异常 ----
    {
        const auto hostileHealth=ParseState("{}",
            "{\"status\":{\"webui\":{\"ports\":[\"8787\",null,8787,true]}}}","{}");
        Check(hostileHealth.webui.ports.size()==1&&hostileHealth.webui.ports[0]==8787,
            "a mixed-type ports array keeps only the numeric entry");
        const auto hostileLists=ParseState(
            "{\"recents\":[1,null,\"a\"],\"favorites\":[true,\"b\"],\"providers\":"
            "[{\"name\":\"X\",\"models\":[1,null,\"m1\"]}]}","{}","{}");
        Check(hostileLists.recents.size()==1&&hostileLists.recents[0]=="a",
            "a mixed-type recents array keeps only strings");
        Check(hostileLists.favorites.size()==1&&hostileLists.favorites[0]=="b",
            "a mixed-type favorites array keeps only strings");
        Check(hostileLists.providers.size()==1&&hostileLists.providers[0].models.size()==1
            &&hostileLists.providers[0].models[0]=="m1","a mixed-type models array keeps only strings");
        const auto wrongTypes=ParseState("{\"model\":{\"default\":5,\"provider\":null}}",
            "{\"status\":[]}","[]");
        Check(wrongTypes.model.model.empty()&&wrongTypes.model.provider.empty(),
            "wrong-typed model fields fall back to empty strings");
        Check(wrongTypes.wsl=="unknown","a status that is not an object falls back to unknown");
        const auto topArrays=ParseState("[]","[]","[]");
        Check(topArrays.providers.empty()&&topArrays.fallbacks.empty()&&topArrays.wsl=="unknown",
            "top-level arrays parse as empty state");
        std::string huge="{\"providers\":[";
        for(int index=0;index<5000;++index) {
            if(index) huge+=",";
            huge+="{\"name\":\"P"+std::to_string(index)+"\",\"models\":[\"m\"]}";
        }
        huge+="]}";
        Check(ParseState(huge,"{}","{}").providers.size()==5000,
            "a 5000-provider reply parses without throwing");
    }

    {
        const auto hugePort=ParseState("{}",
            "{\"status\":{\"webui\":{\"port\":999999999},\"gateway\":{\"port\":-5}}}","{}");
        Check(hugePort.webui.port==65535,"a huge webui port clamps to 65535");
        Check(hugePort.gateway.port==1,"a negative gateway port clamps to 1");
    }
    // 探测结果的"有没有这个模型"判定（真机：明明有却说没有）。
    {
        const std::vector<std::string> listed{"deepseek-v4.1-flash","glm-5.3","nvidia/llama-4"};
        Check(ModelListHasModel("deepseek-v4.1-flash",listed),
            "an exact match is found");
        Check(ModelListHasModel("DeepSeek-V4.1-Flash",listed),
            "the match ignores case");
        Check(ModelListHasModel("workbuddy-deepseek-v4.1-flash",listed),
            "a supplier prefix on the config side still matches the listed id");
        Check(ModelListHasModel("llama-4",listed),
            "a namelesspace prefix on the listed id still matches");
        Check(ModelListHasModel("nvidia/llama-4",listed),
            "the namespaced form on the config side matches too");
        Check(!ModelListHasModel("gpt-5.9",listed),"a really missing model says missing");
        Check(!ModelListHasModel("",listed),"an empty model never matches");
        // 行探测的 base_url 解析：逐行 probe 依赖它（没有它只能退回助手的整链探测）。
        const auto parsed=ParseState(
            "{\"fallbacks\":[{\"provider\":\"custom:Beta\",\"model\":\"beta-2\","
            "\"base_url\":\"https://beta.example/v1\",\"key_env\":\"BETA_KEY\",\"index\":3}]}","{}","{}");
        Check(parsed.fallbacks.size()==1&&parsed.fallbacks[0].baseUrl=="https://beta.example/v1"
            &&parsed.fallbacks[0].keyEnv=="BETA_KEY"&&parsed.fallbacks[0].index==3,
            "a fallback row keeps its base_url/key_env (per-row probing needs them)");
    }
    std::cout<<"hermes-state: "<<passed<<" passed, "<<failed<<" failed\n";
    return failed==0?0:1;
}
