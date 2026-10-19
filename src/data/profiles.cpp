#include "data/profiles.h"
#include <algorithm>
#include <fstream>
#include <json.hpp>
#include <windows.h>

namespace hermes {
void ProfileStore::Load() {
    profiles_.clear();
    std::ifstream input(path_);
    if(!input) return;
    try {
        nlohmann::json value;
        input>>value;
        if(!value.contains("profiles")||!value["profiles"].is_array()) return;
        for(const auto& row:value["profiles"]) {
            if(!row.is_object()) continue;
            Profile profile;
            profile.name=row.value("name",std::string{});
            if(profile.name.empty()) continue;
            if(row.contains("main")&&row["main"].is_object()) {
                profile.mainProvider=row["main"].value("provider",std::string{});
                profile.mainModel=row["main"].value("model",std::string{});
            }
            if(row.contains("fallbacks")&&row["fallbacks"].is_array())
                for(const auto& entry:row["fallbacks"])
                    if(entry.is_object())
                        profile.fallbacks.push_back({entry.value("provider",std::string{}),
                            entry.value("model",std::string{})});
            if(row.contains("favorites")&&row["favorites"].is_array())
                for(const auto& entry:row["favorites"])
                    if(entry.is_string()) profile.favorites.push_back(entry.get<std::string>());
            profile.updated=row.value("updated",0LL);
            profiles_.push_back(std::move(profile));
        }
    } catch(const std::exception&) {
        // 坏文件按空列表处理（下次保存重写），不能阻塞面板启动。
        profiles_.clear();
    }
}
void ProfileStore::Save() const {
    nlohmann::json rows=nlohmann::json::array();
    for(const auto& profile:profiles_) {
        nlohmann::json fallbacks=nlohmann::json::array();
        for(const auto& entry:profile.fallbacks)
            fallbacks.push_back({{"provider",entry.provider},{"model",entry.model}});
        rows.push_back({{"name",profile.name},
            {"main",{{"provider",profile.mainProvider},{"model",profile.mainModel}}},
            {"fallbacks",fallbacks},{"favorites",profile.favorites},
            {"updated",profile.updated}});
    }
    const nlohmann::json value={{"version",1},{"profiles",rows}};
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
const Profile* ProfileStore::Find(const std::string& name) const {
    const auto found=std::find_if(profiles_.begin(),profiles_.end(),
        [&](const Profile& profile) { return profile.name==name; });
    return found==profiles_.end()?nullptr:&*found;
}
void ProfileStore::Upsert(const Profile& profile) {
    const auto found=std::find_if(profiles_.begin(),profiles_.end(),
        [&](const Profile& entry) { return entry.name==profile.name; });
    if(found==profiles_.end()) profiles_.push_back(profile);
    else *found=profile;
}
bool ProfileStore::Remove(const std::string& name) {
    const auto found=std::find_if(profiles_.begin(),profiles_.end(),
        [&](const Profile& entry) { return entry.name==name; });
    if(found==profiles_.end()) return false;
    profiles_.erase(found);
    return true;
}
}
