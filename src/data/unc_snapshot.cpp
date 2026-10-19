#include "unc_snapshot.h"
#include <windows.h>
#include <json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

namespace hermes {
namespace {
using Json=nlohmann::json;

std::wstring Utf8ToWide(const std::string& text) {
    if(text.empty()) return {};
    const int size=MultiByteToWideChar(CP_UTF8,0,text.c_str(),static_cast<int>(text.size()),nullptr,0);
    std::wstring wide(static_cast<std::size_t>(std::max(size,0)),L'\0');
    if(size>0) MultiByteToWideChar(CP_UTF8,0,text.c_str(),size,wide.data(),size);
    return wide;
}
// UNC 根形如 //wsl$/Ubuntu-24.04（正斜杠形态：本机实测 \\wsl$\ 反斜杠形态在 Win32 文件
// API 上不可见，而 //wsl$/ 可用——MSYS/gwsl 语义）；POSIX 路径逐段以正斜杠拼接。
std::wstring UncPath(const std::string& root,const std::string& posixPath) {
    std::string path=root;
    for(std::size_t i=0;i<posixPath.size();) {
        while(i<posixPath.size()&&posixPath[i]=='/') ++i;
        const std::size_t begin=i;
        while(i<posixPath.size()&&posixPath[i]!='/') ++i;
        if(i>begin) { path.push_back('/'); path+=posixPath.substr(begin,i-begin); }
    }
    return Utf8ToWide(path);
}
bool ReadWholeFile(const std::wstring& path,std::string& out) {
    std::ifstream file(path,std::ios::binary);
    if(!file) return false;
    std::ostringstream buffer;
    buffer<<file.rdbuf();
    out=buffer.str();
    return true;
}
// 返回 (exists, size, mtime 秒)。FileTime→epoch 用 UTC 换算（apply.py 的 int(st_mtime) 是
// epoch 秒，int() 之后的本地时间展示在面板侧由 UI 自己做）。
long long FileStat(const std::wstring& path,bool& exists,long long& size) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&info)) { exists=false;size=0;return 0; }
    exists=true;
    size=(static_cast<long long>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
    ULARGE_INTEGER ul{{info.ftLastWriteTime.dwLowDateTime,info.ftLastWriteTime.dwHighDateTime}};
    return static_cast<long long>((ul.QuadPart-116444736000000000ULL)/10000000ULL);
}

// ---------------- 行扫描器（apply.py 语义 1:1）----------------
using Lines=std::vector<std::string>;
// readlines() 等价：按 \n 切，保留空行；文末若无换行最后一行也算。
Lines SplitLines(const std::string& text) {
    Lines lines;
    std::string current;
    for(const char c:text) {
        if(c=='\n') { lines.push_back(current);current.clear(); }
        else if(c!='\r') current.push_back(c);
    }
    if(!current.empty()) lines.push_back(current);
    return lines;
}
bool TopKeyMatch(const std::string& line,std::string& key) {
    std::size_t i=0;
    if(line.empty()) return false;
    const auto c=static_cast<unsigned char>(line[0]);
    if(!(std::isalpha(c)||c=='_')) return false;
    i=1;
    while(i<line.size()) {
        const auto ch=static_cast<unsigned char>(line[i]);
        if(!(std::isalnum(ch)||ch=='_')) break;
        ++i;
    }
    if(i>=line.size()||line[i]!=':') return false;
    key=line.substr(0,i);
    return true;
}
struct Block { std::string name;std::size_t start=0,end=0; };
std::vector<Block> TopBlocks(const Lines& lines) {
    std::vector<Block> out;
    bool open=false;
    std::size_t begin=0;
    std::string name;
    for(std::size_t i=0;i<lines.size();++i) {
        std::string key;
        if(TopKeyMatch(lines[i],key)) {
            if(open) out.push_back({name,begin,i});
            name=key;begin=i;open=true;
        }
    }
    if(open) out.push_back({name,begin,lines.size()});
    return out;
}
bool FindBlock(const Lines& lines,const char* name,Block& out) {
    for(const auto& block:TopBlocks(lines))
        if(block.name==name) { out=block;return true; }
    return false;
}
// block_style：首个 "- " 行的缩进 = 条目缩进，字段缩进 = +2；空区块 → (2,4)。
void BlockStyle(const Lines& lines,const Block& block,std::size_t& itemIndent,std::size_t& fieldIndent) {
    for(std::size_t i=block.start+1;i<block.end;++i) {
        const auto& raw=lines[i];
        const auto first=raw.find_first_not_of(' ');
        if(first==std::string::npos) continue;
        if(raw.compare(first,2,"- ")==0) { itemIndent=first;fieldIndent=first+2;return; }
    }
    itemIndent=2;fieldIndent=4;
}
struct Entry { std::size_t start=0,end=0;std::string name;std::size_t indent=2,findent=4; };
std::vector<Entry> EntryRanges(const Lines& lines,const Block& block) {
    std::size_t itemIndent=2,fieldIndent=4;
    BlockStyle(lines,block,itemIndent,fieldIndent);
    const std::string prefix=std::string(itemIndent,' ')+"- ";
    std::vector<std::size_t> starts;
    for(std::size_t i=block.start+1;i<block.end;++i)
        if(lines[i].compare(0,prefix.size(),prefix)==0) starts.push_back(i);
    std::vector<Entry> out;
    for(std::size_t index=0;index<starts.size();++index) {
        Entry entry;
        entry.start=starts[index];
        entry.end=index+1<starts.size()?starts[index+1]:block.end;
        entry.indent=itemIndent;
        entry.findent=fieldIndent;
        const auto& line=lines[entry.start];
        const std::string after=line.substr(prefix.size());
        const auto colon=after.find(':');
        if(colon!=std::string::npos) {
            std::size_t v=colon+1,vend=after.size();
            while(v<vend&&(after[v]==' '||after[v]=='\t')) ++v;
            while(vend>v&&(after[vend-1]==' '||after[vend-1]=='\t')) --vend;
            entry.name=after.substr(v,vend-v);
        }
        out.push_back(entry);
    }
    return out;
}
std::string StripQuotes(const std::string& value) {
    if(value.size()>=2&&((value.front()=='"'&&value.back()=='"')||(value.front()=='\''&&value.back()=='\'')))
        return value.substr(1,value.size()-2);
    return value;
}
std::string Trim(const std::string& value) {
    const auto first=value.find_first_not_of(" \t");
    if(first==std::string::npos) return "";
    const auto last=value.find_last_not_of(" \t");
    return value.substr(first,last-first+1);
}
// entry_field：`<findent><field>:` 首行 → 剥引号的值；无 → ""（apply.py 是 None，落 JSON 时
// 由调用方决定 null/空串——list 里 entry_field 可为 None 的字段是 base_url/api_mode/default_model）。
std::string EntryField(const Lines& lines,const Entry& entry,const std::string& field,bool& present) {
    const std::string prefix=std::string(entry.findent,' ')+field+":";
    for(std::size_t i=entry.start+1;i<entry.end;++i)
        if(lines[i].compare(0,prefix.size(),prefix)==0) {
            present=true;
            return StripQuotes(Trim(lines[i].substr(prefix.size())));
        }
    present=false;
    return {};
}
// sub_range：`<findent><key>:` 行（rstrip 相等）后连续的「缩进更深 或 ('- ' 且缩进≥findent)」行。
bool SubRange(const Lines& lines,const Entry& entry,const std::string& key,std::size_t& start,std::size_t& end) {
    const std::string target=std::string(entry.findent,' ')+key+":";
    for(std::size_t i=entry.start;i<entry.end;++i) {
        std::string trimmed=lines[i];
        while(!trimmed.empty()&&(trimmed.back()==' '||trimmed.back()=='\t'||trimmed.back()=='\r')) trimmed.pop_back();
        if(trimmed!=target) continue;
        std::size_t j=i+1;
        while(j<entry.end) {
            const auto& line=lines[j];
            const auto first=line.find_first_not_of(" \t");
            if(first==std::string::npos) break;                      // 空行 → 停
            const auto indent=static_cast<std::size_t>(first);
            const bool deeper=indent>entry.findent;
            const bool listAtKey=indent>=entry.findent&&line.compare(first,2,"- ")==0;
            if(deeper||listAtKey) { ++j;continue; }
            break;
        }
        start=i;
        end=j;
        return true;
    }
    return false;
}
// yaml_key_of：`"key"`（JSON 转义）/`'key'`/裸 `key:`。
std::string YamlKeyOf(const std::string& raw) {
    std::string text=Trim(raw);
    if(!text.empty()&&text.front()=='"') {
        std::size_t end=1;
        while(end<text.size()) {
            if(text[end]=='\\') { end+=2;continue; }
            if(text[end]=='"') break;
            ++end;
        }
        if(end<=text.size()) return text.substr(1,std::min(end-1,text.size()-1));
        return text.substr(1);
    }
    if(!text.empty()&&text.front()=='\'') {
        const auto close=text.find('\'',1);
        return close==std::string::npos?text.substr(1):text.substr(1,close-1);
    }
    const auto colon=text.find(':');
    return Trim(colon==std::string::npos?text:text.substr(0,colon));
}
// count_models：models 子区块内的键（映射形态）或裸标量（列表形态）。
std::vector<std::string> CountModels(const Lines& lines,const Entry& entry) {
    std::size_t start=0,end=0;
    if(!SubRange(lines,entry,"models",start,end)) return {};
    std::vector<std::string> out;
    for(std::size_t i=start+1;i<end;++i) {
        std::string text=Trim(lines[i]);
        if(text.compare(0,2,"- ")==0) text=Trim(text.substr(2));
        if(text.empty()) continue;
        if(text.compare(text.size()-4,4,": {}")==0||text.back()==':')
            out.push_back(YamlKeyOf(text));
        else if(text.find(':')==std::string::npos)
            out.push_back(StripQuotes(text));
    }
    return out;
}
// snap_model：model 区块顶层字段（两空格前缀）；区块不存在时全 null + supports_vision=false。
struct ModelSnap { std::string defaultValue,provider,baseUrl,apiMode;bool supportsVision=false;bool present=false; };
ModelSnap SnapModel(const Lines& lines) {
    ModelSnap out;
    Block block;
    if(!FindBlock(lines,"model",block)) return out;
    out.present=true;
    const auto pick=[&](const char* key) {
        for(std::size_t i=block.start+1;i<block.end;++i) {
            const std::string prefix=std::string("  ")+key+":";
            if(lines[i].compare(0,prefix.size(),prefix)==0) return StripQuotes(Trim(lines[i].substr(prefix.size())));
        }
        return std::string{};
    };
    out.defaultValue=pick("default");
    out.provider=pick("provider");
    out.baseUrl=pick("base_url");
    out.apiMode=pick("api_mode");
    out.supportsVision=(pick("supports_vision")=="true");
    return out;
}
// env_read_all：KEY=VALUE，剥引号，跳过注释/空行。
std::vector<std::pair<std::string,std::string>> EnvReadAll(const std::wstring& envPath,bool& exists) {
    exists=false;
    std::string text;
    if(!ReadWholeFile(envPath,text)) return {};
    exists=true;
    std::vector<std::pair<std::string,std::string>> out;
    for(const auto& raw:SplitLines(text)) {
        std::string line=Trim(raw);
        if(line.empty()||line[0]=='#') continue;
        const auto eq=line.find('=');
        if(eq==std::string::npos) continue;
        out.push_back({Trim(line.substr(0,eq)),StripQuotes(Trim(line.substr(eq+1)))});
    }
    return out;
}
bool EnvKeySet(const std::vector<std::pair<std::string,std::string>>& env,const std::string& name) {
    if(name.empty()) return false;
    for(const auto& row:env) if(row.first==name) return true;   // apply.py: bool(value)，空串才算 false
    return false;
}
}

std::string WslHomeFromHelperPath(const std::string& helperPath) {
    // /home/<user>/... → /home/<user>。helperPath 在 /mnt/* 下时推不出（真机常态：
    // 脚本在 E: 盘，home 在发行版里），返回空由调用方做 /home 唯一目录探测。
    if(helperPath.compare(0,2,"~/")==0) return {};
    const std::string marker="/home/";
    const auto at=helperPath.find(marker);
    if(at!=std::string::npos) {
        const auto userAt=at+marker.size();
        const auto slash=helperPath.find('/',userAt);
        if(slash!=std::string::npos&&slash>userAt) return helperPath.substr(0,slash);
    }
    return {};
}

UncSnapshot TakeUncSnapshot(const std::string& distro,const std::string& homePosix,
    const std::string& gatewayPort) {
    (void)gatewayPort;
    UncSnapshot result;
    if(distro.empty()) { result.error="unc: distro unknown";return result; }
    // home 为空 = 调用方决定不走 UNC（门禁夹具/禁用旗标），这里不做任何探测——
    // 否则枚举 /home 会摸到真机的 .hermes，污染夹具（真机探测只属于 Release 产品路径）。
    std::string home=homePosix;
    if(home.empty()) { result.error="unc: home unknown (disabled)";return result; }
    const std::string root="//wsl$/"+distro;
    const std::wstring configPath=UncPath(root,home+"/config.yaml");
    bool configExists=false;
    long long configSize=0;
    const long long configMtime=FileStat(configPath,configExists,configSize);
    if(!configExists) {
        result.error="unc: config not visible (path="+root+home+"/config.yaml, err="+
            std::to_string(GetLastError())+")";
        return result;
    }
    const std::wstring envPath=UncPath(root,home+"/.env");
    bool envExists=false;
    long long envSize=0;
    FileStat(envPath,envExists,envSize);
    const auto env=EnvReadAll(envPath,envExists);
    std::string configText;
    if(!ReadWholeFile(configPath,configText)) { result.error="unc: read config failed";return result; }
    const Lines lines=SplitLines(configText);
    // ---- snap_model → list.model ----
    const auto snap=SnapModel(lines);
    Json model{{"default",snap.defaultValue.empty()?Json(nullptr):Json(snap.defaultValue)},
               {"provider",snap.provider.empty()?Json(nullptr):Json(snap.provider)},
               {"base_url",snap.baseUrl.empty()?Json(nullptr):Json(snap.baseUrl)},
               {"api_mode",snap.apiMode.empty()?Json(nullptr):Json(snap.apiMode)},
               {"supports_vision",snap.supportsVision}};
    // ---- snap_providers ----
    Json providers=Json::array();
    Block providersBlock;
    if(FindBlock(lines,"custom_providers",providersBlock)) {
        const std::string currentProvider=snap.provider;
        for(const auto& entry:EntryRanges(lines,providersBlock)) {
            bool present=false;
            const std::string baseUrl=EntryField(lines,entry,"base_url",present);
            const std::string apiMode=EntryField(lines,entry,"api_mode",present);
            const std::string defaultModel=EntryField(lines,entry,"model",present);
            const std::string keyEnv=EntryField(lines,entry,"key_env",present);
            const auto models=CountModels(lines,entry);
            providers.push_back(Json{
                {"name",entry.name},
                {"provider_id","custom:"+entry.name},
                {"base_url",baseUrl.empty()?Json(nullptr):Json(baseUrl)},
                {"key_env",keyEnv.empty()?Json(nullptr):Json(keyEnv)},
                {"key_set",EnvKeySet(env,keyEnv)},
                {"api_mode",apiMode.empty()?Json(nullptr):Json(apiMode)},
                {"default_model",defaultModel.empty()?Json(nullptr):Json(defaultModel)},
                {"models",models},
                {"models_discovered",EntryField(lines,entry,"models_discovered",present)=="true"},
                {"is_active",currentProvider=="custom:"+entry.name},
                {"line",entry.start+1},
            });
        }
    }
    // ---- snap_fallbacks ----
    Json fallbacks=Json::array();
    Block fallbackBlock;
    if(FindBlock(lines,"fallback_providers",fallbackBlock)) {
        int index=0;
        for(const auto& entry:EntryRanges(lines,fallbackBlock)) {
            bool present=false;
            const std::string keyEnv=EntryField(lines,entry,"key_env",present);
            fallbacks.push_back(Json{
                {"index",index++},
                {"provider",entry.name},
                {"model",EntryField(lines,entry,"model",present)},
                {"base_url",EntryField(lines,entry,"base_url",present)},
                {"api_mode",EntryField(lines,entry,"api_mode",present)},
                {"key_env",keyEnv.empty()?Json(nullptr):Json(keyEnv)},
                {"key_set",EnvKeySet(env,keyEnv)},
                {"line",entry.start+1},
            });
        }
    }
    Json list{
        {"model",model},
        {"providers",providers},
        {"fallbacks",fallbacks},
        {"files",Json{{"config",Json{
            {"mtime",configMtime},
            {"mtime_text",std::string{}},
            {"size",configSize},
            {"mode",std::string{}},
        }},{"env",Json{{"exists",envExists},{"size",envSize}}}}},
        {"backup_dir",home+"/backups/config-changes"},
        {"version","unc-1.0"},
    };
    result.configMtime=configMtime;
    result.configSize=configSize;
    result.listJson=list.dump();
    result.available=true;
    return result;
}
} // namespace hermes
