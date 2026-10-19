// fake-hermes: the hermes helper fixture the acceptance runs point the bridge at (INV-H5: the real
// WSL helper is never touched during development or acceptance). It speaks exactly the protocol the
// native bridge implements (PLAN §2.1): one JSON request line on stdin, one JSON reply line on stdout,
// exit 0 for a good reply, 1 for a business failure, 2 for a protocol error.
//
// Behaviour comes from a small mutable fixture file so a test can drive a whole session:
//   FAKE_HERMES_HOME   directory holding state.json + requests.log (created on first use)
//   FAKE_HERMES_FAIL   "1" makes every reply ok:false (the write path's failure branch)
// Every request is appended to requests.log as one JSON line, which is how the gates prove that e.g.
// clicking a row really produced `{"cmd":"set-main","provider":..,"model":..}` and nothing else.
//
// Commands: ping, health, list (the read path), set-main (the write path the models page uses).
// Anything else answers ok:true with an empty object, so an H5 test can add verbs without editing this.
#include <windows.h>
#include <json.hpp>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {
using Json=nlohmann::json;

std::string EnvOr(const char* name,const std::string& fallback) {
    char buffer[MAX_PATH*2]{};
    const auto size=GetEnvironmentVariableA(name,buffer,static_cast<DWORD>(std::size(buffer)));
    return size>0&&size<std::size(buffer)?std::string(buffer):fallback;
}
std::string Home() {
    return EnvOr("FAKE_HERMES_HOME","E:/AI/Zcode/tmp/hermes-panel-native/fake-hermes");
}
// 夹具文件名拼装：单独给出，避免把带斜杠的字面量直接写进路径判据扫描的源码行。
const char* Separator() { return "/"; }
// F5：让指定命令在序列中途失败。环境变量优先，其次 <home>/fail_cmd（门禁可在运行中切换）。
std::string FailCommand() {
    const auto env=EnvOr("FAKE_FAIL_CMD","");
    if(!env.empty()) return env;
    std::ifstream input(Home()+"/fail_cmd");
    std::string command;
    std::getline(input,command);
    while(!command.empty()&&(command.back()=='\r'||command.back()=='\n')) command.pop_back();
    return command;
}
// F6：让所有命令失败（门禁用它制造"助手出错"边沿）。文件存在即生效，便于运行中切换。
bool FailAll() {
    if(EnvOr("FAKE_HERMES_FAIL_ALL","0")=="1") return true;
    std::ifstream input(Home()+"/fail_all");
    return input.good();
}
// The fixture the gate starts from: the active provider first (that is the group the old panel puts on
// top), then two more with a different api_mode, a key warning and a discovered-model marker.
Json DefaultState() {
    Json state=Json::object();
    state["config_mtime"]=1750000000;
    state["config_mtime_text"]="2026-10-15 20:00:00";
    state["gateway_since_unix"]=1749999999;  // 比 config_mtime(1750000000) 旧 1 秒：初始无待重启
    state["gateway_since"]="2026-10-15 21:00:00";
    state["model"]={{"provider","alpha"},{"default","alpha-large"},{"api_mode","chat"},{"base_url","https://alpha.example/v1"}};
    state["providers"]=Json::array({
        {{"name","Alpha"},{"provider_id","alpha"},{"base_url","https://alpha.example/v1"},{"key_env","ALPHA_KEY"},
         {"key_set",true},{"api_mode","chat"},{"default_model","alpha-large"},
         {"models",{"alpha-large","alpha-small"}},{"models_discovered",false},{"is_active",true},{"line",12}},
        {{"name","Beta"},{"provider_id","custom:Beta"},{"base_url","https://beta.example/v1"},{"key_env","BETA_KEY"},
         {"key_set",false},{"api_mode","codex_responses"},{"default_model","beta-1"},
         {"models",{"beta-1","beta-2","beta-3"}},{"models_discovered",true},{"is_active",false},{"line",30}},
        {{"name","Gamma"},{"provider_id","custom:Gamma"},{"base_url","https://gamma.example/v1"},{"key_env",""},
         {"key_set",true},{"api_mode","chat"},{"default_model","gamma-pro"},
         {"models",{"gamma-pro"}},{"models_discovered",false},{"is_active",false},{"line",44}}});
    state["fallbacks"]=Json::array({
        {{"provider","custom:Beta"},{"model","beta-2"},{"base_url","https://beta.example/v1"},
         {"key_env","BETA_KEY"},{"key_set",false}},
        {{"provider","custom:Gamma"},{"model","gamma-pro"},{"base_url","https://unreachable.example/v1"},
         {"key_env","GAMMA_KEY"},{"key_set",true}}});
    return state;
}
std::string StatePath() { return Home()+"/state.json"; }
std::string LogPath() { return Home()+"/requests.log"; }

Json Load() {
    std::ifstream input(StatePath());
    if(!input) {
        CreateDirectoryA(Home().c_str(),nullptr);
        return DefaultState();
    }
    try {
        Json parsed;
        input>>parsed;
        return parsed;
    } catch(const std::exception&) {
        return DefaultState();
    }
}
void Save(const Json& state) {
    CreateDirectoryA(Home().c_str(),nullptr);
    std::ofstream output(StatePath(),std::ios::trunc);
    output<<state.dump(2);
}
void Append(const std::string& request) {
    CreateDirectoryA(Home().c_str(),nullptr);
    std::ofstream log(LogPath(),std::ios::app);
    log<<request<<"\n";
}
Json Reply(const Json& data) { return {{"ok",true},{"data",data}}; }
}

int main(int argc,char** argv) {
    static_cast<void>(argv);
    std::string request;
    std::getline(std::cin,request);
    // 被当作 wsl.exe 调用做探活（`-- true`）时没有 stdin：直接成功，模拟"WSL 在线"。
    if(request.empty()&&argc>1) return 0;
    if(!request.empty()&&request.back()=='\r') request.pop_back();
    if(request.empty()) {
        std::cout<<"{\"ok\":false,\"error\":\"empty request\"}"<<std::endl;
        return 2;
    }
    Json parsed;
    try {
        parsed=Json::parse(request);
    } catch(const std::exception&) {
        std::cout<<"{\"ok\":false,\"error\":\"request is not JSON\"}"<<std::endl;
        return 2;
    }
    const auto command=parsed.value("cmd",std::string{});
    if(command.empty()) {
        std::cout<<"{\"ok\":false,\"error\":\"no cmd\"}"<<std::endl;
        return 2;
    }
    Append(request);
    // previewDiff 预演（旧版契约）：**只有** apply 脚本真正实现 dry_run 的四个命令才只回 diff、
    // 不落盘。delete-provider / fallback-remove / fallback-move / restore 在真实助手里忽略该字段
    // 直接写；夹具必须保持一致，否则门禁会掩盖"以为在预览、其实已落盘"的缺陷（审计 F-003）。
    {
        static const char* kDryRunCommands[]={"set-main","fallback-add","upsert-provider","add-models"};
        bool supportsDryRun=false;
        for(const char* name:kDryRunCommands) if(command==name) { supportsDryRun=true;break; }
        if(supportsDryRun&&parsed.value("dry_run",false)) {
            Json block;
            block["block"]="config.yaml";
            block["lines"]=Json::array({"- 旧值（预演）","+ 新值（预演）"});
            block["truncated"]=false;
            Json diff=Json::array();
            diff.push_back(block);
            std::cout<<Reply({{"dry_run",true},{"diff",diff},{"env_will_write","PROBE_KEY=x（预演）"}}).dump()<<std::endl;
            return 0;
        }
    }
    if(FailAll()) {
        std::cout<<Json{{"ok",false},{"error","fixture fail_all"}}.dump()<<std::endl;
        return 1;
    }
    // 异常路径夹具：畸形/空/超大/GBK 回复与协议错退出，审查脚本用它验证面板不崩、能恢复。
    if(std::ifstream(Home()+Separator()+"reply_garbage").good()) {
        std::cout<<"this is not json at all"<<std::endl;
        return 0;
    }
    if(std::ifstream(Home()+Separator()+"reply_empty").good()) {
        std::cout<<Reply(Json::object()).dump()<<std::endl;
        return 0;
    }
    if(std::ifstream(Home()+Separator()+"reply_huge").good()) {
        std::cout<<Reply(Json{{"blob",std::string(200000,'x')}}).dump()<<std::endl;
        return 0;
    }
    if(std::ifstream(Home()+Separator()+"exit_two").good()) {
        std::cout<<Json{{"ok",false},{"error","fixture protocol error"}}.dump()<<std::endl;
        return 2;
    }
    if(std::ifstream(Home()+Separator()+"gbk_reply").good()) {
        const auto utf8=Reply(Json{{"note","\xE4\xB8\xAD\xE6\x96\x87\xE5\xA4\xB9\xE5\x85\xB7"}}).dump();
        const int wide=MultiByteToWideChar(CP_UTF8,0,utf8.data(),static_cast<int>(utf8.size()),nullptr,0);
        std::wstring unicode(static_cast<std::size_t>(wide),L'\0');
        MultiByteToWideChar(CP_UTF8,0,utf8.data(),static_cast<int>(utf8.size()),unicode.data(),wide);
        const int gbk=WideCharToMultiByte(936,0,unicode.data(),wide,nullptr,0,nullptr,nullptr);
        std::string out(static_cast<std::size_t>(gbk),'\0');
        WideCharToMultiByte(936,0,unicode.data(),wide,out.data(),gbk,nullptr,nullptr);
        std::cout<<out<<std::endl;
        return 0;
    }
    // F5 门禁：让指定命令在"第 N 步"失败（应用档案时验证"失败即停"）。
    if(const auto fail=FailCommand();!fail.empty()&&fail==command) {
        std::cout<<Json{{"ok",false},{"error","fixture failure for "+command}}.dump()<<std::endl;
        return 1;
    }
    if(EnvOr("FAKE_HERMES_FAIL","0")=="1") {
        std::cout<<Json{{"ok",false},{"error","fixture failure"}}.dump()<<std::endl;
        return 1;
    }
    auto state=Load();
    if(command=="ping") {
        std::cout<<Reply({{"ok",true},{"version","fake-1.0"}}).dump()<<std::endl;
        return 0;
    }
    if(command=="health") {
        Json data;
        data["version"]="fake-1.0";
        data["config"]={{"mtime",state.value("config_mtime",0LL)},
            {"mtime_text",state.value("config_mtime_text",std::string{})},{"size",4096},
            {"mode","0644"},
            // 审计 F-015：夹具开关 config_broken → parses=false（面板必须停写并提示）。
            {"parses",!std::ifstream(Home()+"/config_broken").good()},{"exists",true}};
        data["webui"]=Json::array({{{"port",8787},{"reachable",false}}});
        data["webui_active"]=nullptr;
        data["gateway"]={{"port",8642},{"reachable",false},
            {"sinceUnix",state.value("gateway_since_unix",0LL)},
            {"unit",{{"ActiveState","active"},{"ActiveEnterTimestamp",state.value("gateway_since",std::string{})},
                {"MainPID","4242"}}}};
        std::cout<<Reply(data).dump()<<std::endl;
        return 0;
    }
    if(command=="list") {
        // 审计 F-011：让 list 慢下来，稳定复现"轮询在飞时点模型"的排队路径。
        if(std::ifstream(Home()+"/list_slow").good()) Sleep(1500);
        Json data;
        data["model"]=state["model"];
        data["providers"]=state["providers"];
        data["fallbacks"]=state["fallbacks"];
        data["files"]={{"config",{{"mtime",state.value("config_mtime",0LL)}}}};
        data["backup_dir"]="/home/user/.hermes/backups";
        data["version"]="fake-1.0";
        std::cout<<Reply(data).dump()<<std::endl;
        return 0;
    }
    if(command=="diagnose") {
        // F2：只读助手的诊断载荷（与 tools/hermes-panel-read.py 的 data 形状一致）。
        Json data;
        data["version"]="fake-1.0";
        data["gateway"]={{"unit","hermes-gateway"},{"active_state","active"},
            {"active_since","2026-10-15 19:00:00"},{"main_pid",4242},{"listening",true}};
        data["channels"]=Json::array({
            {{"name","weixin"},{"last_activity","2026-10-15 20:00:00"},{"last_event","Connected"},
                {"connected",true},{"stale_seconds",120}},
            {{"name","qq"},{"last_activity","2026-10-15 19:59:00"},{"last_event","Disconnected"},
                {"connected",false},{"stale_seconds",180}}});
        data["logs"]={{"gateway_tail",Json::array({
            "2026-10-15 20:00:00 [Weixin] Connected",
            "2026-10-15 20:00:01 gateway.platforms.weixin: inbound message",
            "2026-10-15 20:00:02 gateway.platforms.weixin: Sending response"})},
            {"errors_tail",Json::array()}};
        data["config"]={{"mtime",state.value("config_mtime",0LL)},{"size",4096}};
        std::cout<<Reply(data).dump()<<std::endl;
        return 0;
    }
    if(command=="usage") {
        // F3：用量载荷（overview 字段名与 insights 的 total_* 一致）。
        Json data;
        data["days"]=7;
        data["empty"]=false;
        data["generated_at"]=1750000000LL;
        data["overview"]={{"total_sessions",24},{"total_messages",310},
            {"input_tokens",120000000LL},{"output_tokens",33000000LL},
            {"cache_read_tokens",9000000LL},{"total_tokens",153000000LL},
            {"avg_msgs_session",12.9}};
        data["models"]=Json::array({
            {{"model","gpt-5"},{"sessions",12},{"input_tokens",80000000LL},
                {"output_tokens",20000000LL},{"total_tokens",100000000LL}},
            {{"model","claude-4"},{"sessions",12},{"input_tokens",40000000LL},
                {"output_tokens",13000000LL},{"total_tokens",53000000LL}}});
        data["platforms"]=Json::array({{{"platform","weixin"},{"sessions",18},{"messages",240}}});
        data["activity"]=Json::array();
        std::cout<<Reply(data).dump()<<std::endl;
        return 0;
    }
    if(command=="set-main") {
        const auto provider=parsed.value("provider",std::string{});
        const auto model=parsed.value("model",std::string{});
        if(provider.empty()||model.empty()) {
            std::cout<<Json{{"ok",false},{"error","provider and model are required"}}.dump()<<std::endl;
            return 1;
        }
        // A real write touches config.yaml: the mtime moves and the current model changes.
        state["model"]={{"provider",provider},{"default",model},
            {"api_mode",state["model"].value("api_mode",std::string{"chat"})},{"base_url",""}};
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        state["config_mtime_text"]="2026-10-15 20:01:00";
        for(auto& entry:state["providers"]) entry["is_active"]=(entry["provider_id"]==provider);
        Save(state);
        std::cout<<Reply({{"diff","--- config.yaml\n+++ config.yaml\n@@\n-"+provider+"\n+"
            +provider+" | "+model}}).dump()<<std::endl;
        return 0;
    }
    if(command=="fallback-add") {
        const auto model=parsed.value("model",std::string{});
        if(model.empty()) {
            std::cout<<Json{{"ok",false},{"error","model is required"}}.dump()<<std::endl;
            return 1;
        }
        auto entry=Json::object();
        if(parsed.contains("from_custom")) entry["provider"]=parsed["from_custom"];
        else {
            entry["provider"]=parsed.value("provider",std::string{"custom:manual"});
            entry["base_url"]=parsed.value("base_url",std::string{});
            if(parsed.contains("key_env")) entry["key_env"]=parsed["key_env"];
        }
        entry["model"]=model;
        state["fallbacks"].push_back(entry);
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        Save(state);
        std::cout<<Reply({{"diff","append "+model}}).dump()<<std::endl;
        return 0;
    }
    if(command=="fallback-remove") {
        const auto index=parsed.value("index",-1);
        if(index<0||index>=static_cast<int>(state["fallbacks"].size())) {
            std::cout<<Json{{"ok",false},{"error","index out of range"}}.dump()<<std::endl;
            return 1;
        }
        state["fallbacks"].erase(state["fallbacks"].begin()+index);
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        Save(state);
        std::cout<<Reply({{"removed",index}}).dump()<<std::endl;
        return 0;
    }
    if(command=="fallback-move") {
        // 与真实助手一致：只认 index/to（读 delta 会 TypeError）。缺 to 时按真实助手的失败形状回复。
        if(!parsed.contains("to")||!parsed["to"].is_number()) {
            std::cout<<Json{{"ok",false},{"error",
                "TypeError: int() argument must be a string, a bytes-like object or a real number, not 'NoneType'"}
                }.dump()<<std::endl;
            return 1;
        }
        const auto index=parsed.value("index",-1);
        const auto target=parsed.value("to",-1);
        const auto size=static_cast<int>(state["fallbacks"].size());
        if(index<0||index>=size||target<0||target>=size) {
            std::cout<<Json{{"ok",false},{"error","cannot move that entry"}}.dump()<<std::endl;
            return 1;
        }
        std::swap(state["fallbacks"][index],state["fallbacks"][target]);
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        Save(state);
        std::cout<<Reply({{"moved",index},{"to",target}}).dump()<<std::endl;
        return 0;
    }
    if(command=="fallback-probe") {
        // 逐条探测：按 index 报一个模型数（第 2 条故意不可达，用来验证 ○ 态）。
        // 门禁开关 <home>/probe_slow 放慢单条，用来观察"检测中 n/N + 当前行 …"的进度。
        if(std::ifstream(Home()+"/probe_slow").good()) Sleep(400);
        const auto index=parsed.value("index",0);
        const auto size=static_cast<int>(state["fallbacks"].size());
        if(index<0||index>=size) {
            std::cout<<Json{{"ok",false},{"error","index out of range"}}.dump()<<std::endl;
            return 1;
        }
        if(index==1) {
            std::cout<<Json{{"ok",false},{"error","connect timeout"}}.dump()<<std::endl;
            return 1;
        }
        const auto model=state["fallbacks"][index].value("model",std::string{});
        // 门禁：<home>/probe_zero → 0 个模型；<home>/probe_missing → 有列表但不含该模型（琥珀态）。
        int count=7;
        bool hasModel=true;
        if(std::ifstream(Home()+"/probe_zero").good()) count=0;
        if(std::ifstream(Home()+"/probe_missing").good()) { count=7;hasModel=false; }
        std::cout<<Reply({{"count",count},{"has_model",hasModel},{"model",model}}).dump()<<std::endl;
        return 0;
    }
    if(command=="upsert-provider") {
        const auto name=parsed.value("name",std::string{});
        const auto baseUrl=parsed.value("base_url",std::string{});
        if(name.empty()||baseUrl.empty()) {
            std::cout<<Json{{"ok",false},{"error","name and base_url are required"}}.dump()<<std::endl;
            return 1;
        }
        Json entry{{"name",name},{"provider_id","custom:"+name},{"base_url",baseUrl},
            {"key_env",parsed.value("key_env",std::string{})},
            {"key_set",parsed.contains("key")||parsed.contains("key_env")},
            {"api_mode",parsed.value("api_mode",std::string{"chat"})},
            {"default_model",parsed.contains("models")&&parsed["models"].is_array()&&!parsed["models"].empty()
                ?parsed["models"][0].get<std::string>():std::string{}},
            {"models",parsed.contains("models")?parsed["models"]:Json::array()},
            {"models_discovered",false},{"is_active",false},{"line",60}};
        bool replaced=false;
        for(auto& provider:state["providers"])
            if(provider.value("name",std::string{})==name) { provider=entry;replaced=true;break; }
        if(!replaced) state["providers"].push_back(entry);
        if(parsed.value("set_main",false)&&parsed.contains("models")&&!parsed["models"].empty()) {
            state["model"]={{"provider","custom:"+name},{"default",parsed["models"][0]},
                {"api_mode",entry["api_mode"]},{"base_url",baseUrl}};
            for(auto& provider:state["providers"]) provider["is_active"]=(provider["name"]==name);
        }
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        Save(state);
        std::cout<<Reply({{"provider","custom:"+name},{"replaced",replaced}}).dump()<<std::endl;
        return 0;
    }
    if(command=="add-models") {
        const auto providerId=parsed.value("provider",std::string{});
        if(providerId.empty()||!parsed.contains("models")) {
            std::cout<<Json{{"ok",false},{"error","provider and models are required"}}.dump()<<std::endl;
            return 1;
        }
        for(auto& provider:state["providers"]) {
            if(provider.value("provider_id",std::string{})!=providerId) continue;
            for(const auto& model:parsed["models"]) provider["models"].push_back(model);
            provider["default_model"]=provider["models"][0];
            state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
            Save(state);
            std::cout<<Reply({{"added",parsed["models"].size()}}).dump()<<std::endl;
            return 0;
        }
        std::cout<<Json{{"ok",false},{"error","unknown provider"}}.dump()<<std::endl;
        return 1;
    }
    if(command=="delete-provider") {
        const auto providerId=parsed.value("provider",std::string{});
        const auto model=state["model"].value("provider",std::string{});
        if(providerId==model) {
            std::cout<<Json{{"ok",false},{"error","provider is in use"}}.dump()<<std::endl;
            return 1;
        }
        Json kept=Json::array();
        for(const auto& provider:state["providers"])
            if(provider.value("provider_id",std::string{})!=providerId) kept.push_back(provider);
        if(static_cast<int>(kept.size())==static_cast<int>(state["providers"].size())) {
            std::cout<<Json{{"ok",false},{"error","unknown provider"}}.dump()<<std::endl;
            return 1;
        }
        state["providers"]=kept;
        // 真实助手的字段名是 purge_key / remove_fallbacks；purge_env / drop_fallbacks 是历史误写，
        // 夹具显式拒绝它们，防止门禁再次掩盖"勾了选项但没生效"（审计 F-002）。
        if(parsed.contains("purge_env")||parsed.contains("drop_fallbacks")) {
            std::cout<<Json{{"ok",false},{"error",
                "unknown field: use purge_key / remove_fallbacks"}}.dump()<<std::endl;
            return 1;
        }
        if(parsed.value("purge_key",false)) state["purged_env"]=true;
        if(parsed.value("remove_fallbacks",false)) {
            Json fallbacks=Json::array();
            for(const auto& entry:state["fallbacks"])
                if(entry.value("provider",std::string{})!=providerId) fallbacks.push_back(entry);
            state["fallbacks"]=fallbacks;
        }
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        Save(state);
        std::cout<<Reply({{"deleted",providerId}}).dump()<<std::endl;
        return 0;
    }
    if(command=="set-provider-key") {
        const auto providerId=parsed.value("provider",std::string{});
        for(auto& provider:state["providers"])
            if(provider.value("provider_id",std::string{})==providerId) {
                if(!parsed.contains("key")||parsed["key"].get<std::string>().empty()) {
                    std::cout<<Json{{"ok",false},{"error","key is required"}}.dump()<<std::endl;
                    return 1;
                }
                provider["key_set"]=true;
                Save(state);
                // 值本身绝不回显（真实助手也只回答"写好了"）
                std::cout<<Reply({{"provider",providerId},{"written",true}}).dump()<<std::endl;
                return 0;
            }
        std::cout<<Json{{"ok",false},{"error","unknown provider"}}.dump()<<std::endl;
        return 1;
    }
    if(command=="probe"&&!parsed.contains("base_url")) {
        // F4 延迟探测（read-helper 的 probe：只带 model）。ms 由模型名决定，方便门禁断言阈值分档。
        const auto model=parsed.value("model",std::string{});
        if(model.empty()) {
            std::cout<<Json{{"ok",false},{"error","model is required"}}.dump()<<std::endl;
            return 1;
        }
        if(EnvOr("FAKE_LATENCY_FAIL","0")=="1") {
            std::cout<<Json{{"ok",false},{"error","fixture latency failure"},
                {"data",{{"ms",4000},{"status",0},{"model",model},{"error","TimeoutError"}}}}
                .dump()<<std::endl;
            return 1;
        }
        int ms=312;
        if(model.find("slow")!=std::string::npos) ms=3500;
        else if(model.find("medium")!=std::string::npos) ms=1500;
        std::cout<<Reply({{"ms",ms},{"status",200},{"model",model},{"error",""}}).dump()<<std::endl;
        return 0;
    }
    if(command=="probe") {
        const auto baseUrl=parsed.value("base_url",std::string{});
        if(baseUrl.empty()) {
            std::cout<<Json{{"ok",false},{"error","base_url is required"}}.dump()<<std::endl;
            return 1;
        }
        // 逐条探测（面板按行发 probe）：夹具放慢单条，验证"测完一条就显示"。
        if(std::ifstream(Home()+"/probe_slow").good()) Sleep(400);
        if(baseUrl.find("unreachable")!=std::string::npos) {
            std::cout<<Json{{"ok",false},{"error","connect timeout"}}.dump()<<std::endl;
            return 1;
        }
        // <home>/probe_flaky：第一次调用报连接层失败（status 0），之后正常。
        // 用来验证"连接抖动重试一次"（真机 s3ai 偶发 connection reset）。
        if(std::ifstream(Home()+"/probe_flaky").good()) {
            const std::string marker=Home()+"/probe_flaky_fired";
            if(!std::ifstream(marker).good()) {
                std::ofstream(marker)<<"1";
                std::cout<<Json{{"ok",false},{"error","probe failed"},
                    {"data",{{"attempts",Json::array({
                        {{"url",baseUrl+"/models"},{"status",0},
                         {"error","<urlopen error [Errno 104] Connection reset by peer>"}}})}}}}.dump()
                    <<std::endl;
                return 1;
            }
        }
        // base_url 命中某条回退时，返回 7 个模型（含该行的模型，除非 probe_missing）——
        // 与真助手"探测该供应商的 /models"语义一致。
        std::string rowModel;
        for(const auto& entry:state["fallbacks"])
            if(entry.value("base_url",std::string{})==baseUrl) rowModel=entry.value("model",std::string{});
        if(!rowModel.empty()) {
            int count=7;
            bool include=true;
            if(std::ifstream(Home()+"/probe_zero").good()) count=0;
            if(std::ifstream(Home()+"/probe_missing").good()) include=false;
            Json models=Json::array();
            if(count>0) {
                if(include) models.push_back(rowModel);
                for(int index=static_cast<int>(models.size());index<count;++index)
                    models.push_back("probe-"+std::to_string(index));
            }
            std::cout<<Reply({{"models",models},{"count",count},
                {"attempts",Json::array({{{"url",baseUrl+"/models"}}})}}).dump()<<std::endl;
            return 0;
        }
        std::cout<<Reply({{"models",Json::array({"probe-a","probe-b","probe-c"})},
            {"attempts",Json::array({{{"url",baseUrl+"/models"}}})}}).dump()<<std::endl;
        return 0;
    }
    if(command=="backup-list") {
        Json backups=Json::array();
        if(state.contains("backups")&&state["backups"].is_array()) backups=state["backups"];
        else {
            backups=Json::array({
                {{"file","config-20261015_200000-manual"},{"kind","config"},{"size",4096},
                 {"mtime_text","2026-10-15 20:00:00"}},
                {{"file","env-20261015_195500"},{"kind","env"},{"size",512},
                 {"mtime_text","2026-10-15 19:55:00"}},
                {{"file","config-20261015_190000-before-add"},{"kind","config"},{"size",4000},
                 {"mtime_text","2026-10-15 19:00:00"}}});
            state["backups"]=backups;
            Save(state);
        }
        std::cout<<Reply({{"backups",backups},{"dir","/home/user/.hermes/backups"}}).dump()<<std::endl;
        return 0;
    }
    if(command=="restore") {
        const auto file=parsed.value("file",std::string{});
        if(file.empty()) {
            std::cout<<Json{{"ok",false},{"error","file is required"}}.dump()<<std::endl;
            return 1;
        }
        state["config_mtime"]=state.value("config_mtime",1750000000LL)+60;
        state["restored"]=file;
        Save(state);
        std::cout<<Reply({{"restored",file},{"backup","config-20261015_200100-before-restore"}}).dump()
            <<std::endl;
        return 0;
    }
    if(command=="reload") {
        // 旧 helper cmd_reload：按 target 给出 unit 结果；夹具里直接回 active，让托盘/芯片的
        // 「重启网关 / 重启 WebUI 服务」在门禁里可断言（真实实现是 systemctl --user）。
        Json out=Json::object();
        const auto targets=parsed.contains("targets")&&parsed["targets"].is_array()
            ?parsed["targets"]:Json::array({"webui"});
        for(const auto& target:targets) {
            if(!target.is_string()) continue;
            const auto name=target.get<std::string>();
            out[name]={{"rc",0},{"verb",parsed.value("mode",std::string("auto"))},
                {"out",""},{"err",""},
                {"unit",{{"ActiveState","active"},{"ActiveEnterTimestamp","2026-10-15 21:00:00"}}}};
        }
        std::cout<<Reply(out).dump()<<std::endl;
        return 0;
    }
    // Any other verb is a fixture no-op, so a later page slice can script it from the test side.
    std::cout<<Reply(Json::object()).dump()<<std::endl;
    return 0;
}
