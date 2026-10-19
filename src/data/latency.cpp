#include "data/latency.h"
#include <fstream>
#include <json.hpp>
#include <windows.h>

namespace hermes {
namespace {
constexpr const char* kVersion= "1";
}
void LatencyStore::Load() {
    samples_.clear();
    std::ifstream input(path_);
    if(!input) return;
    try {
        nlohmann::json value;
        input>>value;
        if(!value.contains("samples")||!value["samples"].is_object()) return;
        for(const auto& item:value["samples"].items()) {
            if(!item.value().is_array()) continue;
            std::vector<LatencySample> samples;
            for(const auto& row:item.value()) {
                if(!row.is_object()) continue;
                samples.push_back({row.value("t",0LL),row.value("ms",0),row.value("ok",false)});
            }
            if(!samples.empty()) samples_[item.key()]=std::move(samples);
        }
    } catch(const std::exception&) {
        // 坏文件不阻塞启动：当成空历史，下一次探测重写它。
        samples_.clear();
    }
}
void LatencyStore::Save() const {
    nlohmann::json samples=nlohmann::json::object();
    for(const auto& item:samples_) {
        auto& rows=samples[item.first];
        rows=nlohmann::json::array();
        for(const auto& sample:item.second)
            rows.push_back({{"t",sample.t},{"ms",sample.ms},{"ok",sample.ok}});
    }
    nlohmann::json value={{"version",kVersion},{"samples",samples}};
    const auto temporary=path_+L".tmp";
    {
        std::ofstream output(temporary,std::ios::trunc|std::ios::binary);
        if(!output) return;
        output<<value.dump(2)<<'\n';
        output.flush();
        if(!output) return;
    }
    if(!MoveFileExW(temporary.c_str(),path_.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        DeleteFileW(temporary.c_str());
}
void LatencyStore::Record(const std::string& key,int ms,bool ok,long long now) {
    auto& samples=samples_[key];
    samples.push_back({now,ms,ok});
    if(samples.size()>kKeep) samples.erase(samples.begin(),
        samples.begin()+static_cast<std::ptrdiff_t>(samples.size()-kKeep));
}
const std::vector<LatencySample>* LatencyStore::Samples(const std::string& key) const {
    const auto found=samples_.find(key);
    return found==samples_.end()?nullptr:&found->second;
}
bool LatencyStore::Last(const std::string& key,int* ms,bool* ok) const {
    const auto* samples=Samples(key);
    if(!samples||samples->empty()) return false;
    if(ms) *ms=samples->back().ms;
    if(ok) *ok=samples->back().ok;
    return true;
}
int LatencyStore::Availability(const std::string& key,std::size_t count) const {
    const auto* samples=Samples(key);
    if(!samples||samples->empty()) return -1;
    const auto total=std::min(count,samples->size());
    std::size_t ok=0;
    for(std::size_t index=samples->size()-total;index<samples->size();++index)
        if((*samples)[index].ok) ++ok;
    return static_cast<int>(ok*100/total);
}
}
