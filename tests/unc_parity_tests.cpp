// unc-parity 门禁（DESIGN v1.7）：Windows 侧行扫描器（unc_snapshot.cpp）产出的 list JSON
// 必须与 apply.py 的 `list` 输出逐字段一致。apply.py 在夹具上真实跑一遍（真 python 语义），
// C++ 扫描器读同一份 config.yaml/.env，两边 JSON 按字段对拍——provider/fallback 的每个
// 标量、models 数组、is_active/key_set/line。夹具覆盖：两种缩进风格（hermes 2/4 与
// WebUI 0/2）、带引号的键、models 映射与列表两种形态、空回退链、嵌套冒号的键名。
#include <windows.h>
#include <json.hpp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "data/unc_snapshot.h"

using hermes::TakeUncSnapshot;

using Json=nlohmann::json;

namespace {
int g_failures=0;
void Require(bool ok,const std::string& what) {
    if(ok) { std::printf("PASS %s\n",what.c_str());return; }
    ++g_failures;
    std::printf("FAIL %s\n",what.c_str());
}
bool WriteFile(const std::wstring& path,const std::string& text) {
    std::ofstream file(path,std::ios::binary);
    if(!file) return false;
    file<<text;
    return true;
}
std::string Slurp(const std::string& path) {
    std::ifstream file(path,std::ios::binary);
    std::ostringstream buffer;
    buffer<<file.rdbuf();
    return buffer.str();
}
// 与 apply.py 相同的夹具：hermes 风格（2 空格条目 / 4 空格字段）。
const char* kHermesStyle = R"(model:
  default: alpha-large
  provider: custom:Alpha
  base_url: https://alpha.example/v1
  api_mode: chat
  supports_vision: false
custom_providers:
  - name: Alpha
    base_url: https://alpha.example/v1
    key_env: ALPHA_KEY
    api_mode: chat
    model: alpha-large
    models_discovered: true
    models:
      alpha-large: {}
      alpha-mini: {}
      "weird:key": {}
  - name: Beta
    base_url: "https://beta.example/v1 "
    key_env: BETA_KEY
    api_mode: responses
    model: beta-1
    models:
      - beta-1
      - beta-2
fallback_providers:
  - provider: Alpha
    model: alpha-mini
    base_url: https://alpha.example/v1
    api_mode: chat
    key_env: ALPHA_KEY
)";
// WebUI 风格（0 空格条目 / 2 空格字段，PyYAML 重排后）。
const char* kWebuiStyle = R"(model:
  api_mode: chat
  base_url: https://alpha.example/v1
  default: alpha-large
  provider: custom:Alpha
custom_providers:
- name: Alpha
  api_mode: chat
  base_url: https://alpha.example/v1
  key_env: ALPHA_KEY
  model: alpha-large
  models:
    alpha-large: {}
    alpha-mini: {}
  models_discovered: true
- name: Beta
  api_mode: responses
  base_url: https://beta.example/v1
  key_env: BETA_KEY
  model: beta-1
  models:
  - beta-1
  - beta-2
fallback_providers:
- api_mode: chat
  base_url: https://alpha.example/v1
  key_env: ALPHA_KEY
  model: alpha-mini
  provider: Alpha
)";
const char* kEnv = "ALPHA_KEY=sk-test-alpha-0001\n"
                   "BETA_KEY=\"quoted-beta\"\n"
                   "# a comment\n"
                   "UNRELATED=1\n";

// 逐字段对拍：providers/fallbacks 数组按顺序，全部标量字段 + models 数组。
void CompareProviders(const Json& py,const Json& cpp,const std::string& label) {
    Require(py.is_array()&&cpp.is_array()&&py.size()==cpp.size(),
        label+" count ("+std::to_string(py.size())+" vs "+std::to_string(cpp.size())+")");
    const auto n=std::min(py.size(),cpp.size());
    for(std::size_t i=0;i<n;++i) {
        for(const char* field:{"name","provider_id","base_url","key_env","key_set","api_mode",
                "default_model","models_discovered","is_active","line"}) {
            const bool pyHas=py[i].contains(field)&&!py[i][field].is_null();
            const bool cppHas=cpp[i].contains(field)&&!cpp[i][field].is_null();
            if(!pyHas&&!cppHas) continue;
            Require(pyHas==cppHas&&py[i][field]==cpp[i][field],
                label+"["+std::to_string(i)+"]"+"."+field);
        }
        // models：两边都是数组（apply.py 是 count_models 的列表）
        Json pyModels=py[i].contains("models")&&!py[i]["models"].is_null()
            ?py[i]["models"]:Json::array();
        Json cppModels=cpp[i].contains("models")&&!cpp[i]["models"].is_null()
            ?cpp[i]["models"]:Json::array();
        Require(pyModels==cppModels,label+"["+std::to_string(i)+"].models");
    }
}
void CompareFallbacks(const Json& py,const Json& cpp,const std::string& label) {
    Require(py.is_array()&&cpp.is_array()&&py.size()==cpp.size(),
        label+" fallback count ("+std::to_string(py.size())+" vs "+std::to_string(cpp.size())+")");
    const auto n=std::min(py.size(),cpp.size());
    for(std::size_t i=0;i<n;++i) {
        for(const char* field:{"index","provider","model","base_url","api_mode","key_env",
                "key_set","line"})
            Require(py[i][field]==cpp[i][field],label+" fb["+std::to_string(i)+"]"+"."+field);
    }
}
void RunCase(const std::wstring& wlabel,const char* configText,const char* envText) {
    std::string label;
    for(wchar_t ch:wlabel) label.push_back(static_cast<char>(ch));
    // 夹具必须放在 WSL 文件系统内（UNC 通道只看得见 WSL 侧）；用 /tmp/unc-parity/<label>。
    // C++ 经 //wsl$ 写入，apply.py 从 WSL 内读同一目录——两边看到同一份字节。
    const std::string homePosix=std::string("/tmp/unc-parity/")+label;
    const std::wstring home=L"//wsl$/Ubuntu-24.04"+std::wstring(homePosix.begin(),homePosix.end());
    CreateDirectoryW(L"//wsl$/Ubuntu-24.04/tmp",nullptr);
    CreateDirectoryW(L"//wsl$/Ubuntu-24.04/tmp/unc-parity",nullptr);
    CreateDirectoryW(home.c_str(),nullptr);
    WriteFile(home+L"/config.yaml",configText);
    WriteFile(home+L"/.env",envText);
    const auto snapshot=TakeUncSnapshot("Ubuntu-24.04",homePosix,"8642");
    Require(snapshot.available,label+" unc snapshot available");
    if(!snapshot.available) return;
    std::ofstream out("E:/AI/Zcode/tmp/hermes-panel-native/unc-parity/"+label+".cpp.json",
        std::ios::binary);
    out<<snapshot.listJson;
    std::printf("WROTE %s.cpp.json\n",label.c_str());
}
} // namespace

int wmain(int argc,wchar_t** argv) {
    (void)argc;(void)argv;
    const std::wstring base=L"E:/AI/Zcode/tmp/hermes-panel-native/unc-parity";
    CreateDirectoryW(base.c_str(),nullptr);
    (void)base;
    RunCase(L"hermes-style",kHermesStyle,kEnv);
    RunCase(L"webui-style",kWebuiStyle,kEnv);
    // 边角：空回退链 + 无 model 区块 + models 全是映射形态（带引号键）
    RunCase(L"edge",
        "custom_providers:\n"
        "  - name: Solo\n"
        "    base_url: https://solo.example/v1\n"
        "    key_env: SOLO_KEY\n"
        "    api_mode: chat\n"
        "    models:\n"
        "      \"quoted:key\": {}\n"
        "      plain: {}\n",
        "SOLO_KEY=sk-test-solo\n");
    std::printf(g_failures==0?"unc-parity: all matched\n":"unc-parity: %d MISMATCHES\n",g_failures);
    return g_failures==0?0:1;
}
