#pragma once
#include <string>
#include <vector>
#include <json.hpp>
#include "core/geometry.h"

namespace hermes {
// 收藏 / 最近使用的条数上限（旧 main.js 的 FAVORITES_MAX / RECENTS_MAX）。
// 放在数据层：**加载时**也要按它截断 —— 被篡改或损坏的 settings.json 里可能有几十万条，
// 只靠写入路径限长会让启动时把整份数组读进内存（实测 2MB/20 万条不会崩，但没有意义）。
constexpr int kFavoritesMax=200,kRecentsMax=12;
// Persisted shell state (DESIGN §6.4 mapping). Keys mirror the old Electron settings.json where the
// meaning is unchanged, so a migration tool can translate one into the other.
struct Settings {
    Edge edge=Edge::None;
    Tier tier=Tier::Full;
    Point floating{200,120};
    // 浮起胶囊（DESIGN v1.8）：收起态不贴边时停在 stripFloatPos；与浮动面板的 floating 分开。
    bool stripFloat=false;
    Point stripFloatPos{200,120};
    int anchor=0;
    // 完整档的固定窗口高度（DIP）：拖底边可改，落盘后切页/重启都保持同一高度。
    int heightDip=kPanelDefaultHeightDip;
    std::string monitorId;
    bool topmost=true;
    int pollSeconds=60;
    std::string theme="system";
    std::string closeAction="hide";
    // Hermes 助手连接（PLAN §2.5 沿用旧键名，便于一次性迁移）
    // 与旧版主进程的 SETTING_DEFAULTS.distro 一致（审计 F-014）。
    std::string distro="Ubuntu-24.04";
    std::string pythonExe="python3";
    std::string helperPath="~/hermes-panel-apply.py";
    // 只读助手（diagnose/usage/probe）：与 apply 并列，永不写配置。
    std::string readHelperPath="/mnt/e/AI/Zcode/projects/hermes-panel-native/tools/hermes-panel-read.py";
    int webuiPort=8787;
    int gatewayPort=8642;
    std::vector<std::string> favorites;               // the old setSetting(favorites) list - panel-local
    std::vector<std::string> recents;
    long long hotLoadedMtime=0;      // config mtime this panel wrote (main.js hotLoadedMtime)
    // 行为与热键（旧设置键名，便于一次性迁移）
    std::string hotkey="Control+Alt+H";
    bool previewDiff=false;          // 写入前先看 diff（dry_run 预演）
    bool autoReloadGateway=false;    // 默认关：只在状态条提示
    // F4 延迟探测：默认关（会周期性经网关发 1-token 请求）；间隔 5/15/60 分钟。
    bool latencyProbe=false;
    int latencyMinutes=5;
    // G11 贴边挂件样式：beacon（三段灯柱）/ tag（铭牌）/ flag（旗标）。
    // 旧值 capsule / badge / halo / pulse / magnet 读回来时映射到最接近的新方案。
    std::string widgetStyle="beacon";
    bool legacyMigrated=false;       // 旧 Electron 设置已经搬过一次（不再读它）
};

// Atomic, throttled writer: temp file + rename so a crash can never leave a truncated settings file,
// and at most one write per second so a drag storm cannot thrash the disk (DESIGN §5.3).
class SettingsStore {
public:
    explicit SettingsStore(std::wstring path);
    Settings Load();
    void Save(const Settings& settings,bool force=false);
    void Flush();
    unsigned Writes() const { return writes_; }
    const std::wstring& Path() const { return path_; }
private:
    bool WriteAtomically(const Settings& settings);
    std::wstring path_;
    std::wstring directory_;
    // 未知键保留（审计 F-019）：来自更新版本的设置键不能在保存时被丢掉。
    nlohmann::json extras_=nlohmann::json::object();
    Settings pending_;
    bool dirty_=false;
    unsigned long long lastWriteTick_=0;
    unsigned writes_=0;
};
}
