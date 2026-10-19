#pragma once
#include <string>
#include <vector>

namespace hermes {
// F5 配置档案：只存引用与摘要（provider/model/favorites），不存任何 Key。
struct ProfileFallback { std::string provider,model; };
struct Profile {
    std::string name;
    std::string mainProvider,mainModel;
    std::vector<ProfileFallback> fallbacks;
    std::vector<std::string> favorites;
    long long updated=0;
};
class ProfileStore {
public:
    explicit ProfileStore(std::wstring path) : path_(std::move(path)) {}
    void Load();
    void Save() const;
    const std::vector<Profile>& All() const { return profiles_; }
    const Profile* Find(const std::string& name) const;
    // 同名覆盖（保存当前），新档案追加。
    void Upsert(const Profile& profile);
    bool Remove(const std::string& name);
    std::size_t Size() const { return profiles_.size(); }
    const std::wstring& Path() const { return path_; }
private:
    std::wstring path_;
    std::vector<Profile> profiles_;
};
}
