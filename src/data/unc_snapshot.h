#pragma once
// UNC 只读快照（DESIGN v1.7）：轮询的读路径从「每次 2 个 wsl.exe」换成 `\\wsl$` 9P 直读。
// 这里只读不写——写路径 100% 仍走 hermes-panel-apply.py（硬规则 1）。
// 行扫描器 1:1 复刻 apply.py 的 top_blocks/find_block/block_style/entry_ranges/entry_field/
// sub_range/snap_model/snap_providers/snap_fallbacks/env_read_all 语义（含两种缩进风格），
// 产出与 `list` 相同字段的 JSON，喂给既有 ParseState；对拍由 unc-parity 门禁钉住。
#include <string>

namespace hermes {

// 快照结果：listJson 与 helper 的 {"cmd":"list"} 回复同构（字段级对拍见 unc-parity 门禁）。
// available=false 表示 UNC 通道不可用（VM 未运行等），调用方应降级回 wsl.exe 路径。
struct UncSnapshot {
    bool available=false;
    std::string listJson;        // 与 apply.py cmd_list 输出同构
    long long configMtime=0;     // 变化检测：两次快照之间 mtime+size 都没变 → 调用方可直接复用缓存
    long long configSize=0;
    std::string error;           // available=false 时的原因（日志用，绝不包含文件内容）
};

// 探测 UNC 根（\\wsl$\<distro>\home\<user>\.hermes）是否可达；不可达时 error 说明原因。
// home 由调用方从 helperPath 推导（见 settings 里的约定），这里只做可达性 + 拼装。
UncSnapshot TakeUncSnapshot(const std::string& distro,const std::string& homePosix,
    const std::string& gatewayPort);

// helperPath（~/hermes-panel-apply.py 或 /mnt/e/.../hermes-panel-apply.py）→ 用户的 WSL home。
// 推不出（路径不含 /home/<user>/ 或不是 ~ 开头）时返回空串：调用方放弃 UNC、走 wsl.exe。
std::string WslHomeFromHelperPath(const std::string& helperPath);

} // namespace hermes
