#pragma once
#include <map>
#include <string>
#include <vector>

namespace hermes {
// F4 延迟探测的持久化：每个模型保留最近 kLatencyKeep 次探测（成功/失败都记，失败也有 ms）。
// 只由 UI 线程读写（工作线程只回传结果），所以没有锁。
struct LatencySample { long long t=0;int ms=0;bool ok=false; };
class LatencyStore {
public:
    static constexpr std::size_t kKeep=20;
    explicit LatencyStore(std::wstring path) : path_(std::move(path)) {}
    void Load();
    void Save() const;
    void Record(const std::string& key,int ms,bool ok,long long now);
    const std::vector<LatencySample>* Samples(const std::string& key) const;
    // 最近一次：有记录时返回 true 并填 ms/ok。
    bool Last(const std::string& key,int* ms,bool* ok) const;
    // 最近 count 次的可用率（0..100）；没有记录返回 -1。
    int Availability(const std::string& key,std::size_t count=5) const;
    std::size_t Size() const { return samples_.size(); }
    std::vector<std::string> Keys() const {
        std::vector<std::string> keys;
        for(const auto& item:samples_) keys.push_back(item.first);
        return keys;
    }
    const std::wstring& Path() const { return path_; }
private:
    std::wstring path_;
    std::map<std::string,std::vector<LatencySample>> samples_;
};
}
