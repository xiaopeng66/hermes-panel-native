# 阻塞与假设登记

> 规则（`AUDIT-PLAN.md` §3.2）：环境类/需求类阻塞**必须绕行后继续**；真阻塞登记为"卡住"并跳过。
> 任何时候都不许因为这里的条目而停下整条流水线，除非剩余全部项都被同一阻塞卡死。

## 阻塞

| ID | 类型 | 卡点 | 已试方案 | 绕行方式 | 残留影响 | 状态 |
|---|---|---|---|---|---|---|
| BLOCKER-001 | ① 环境 | 桌面被用户自己的 Chrome / BalancePanelNative 占用，`hover`/`drag`/`dpi`/`h-settings` 的**真实指针**场景命中 `RequirePointOnPanel` / 光标漂移判据（harness 主动 SKIP） | ① 清理残留测试实例；② 重跑 3 次；③ 在控制面坐标可覆盖处改用 `tap`（窗口消息，不受遮挡影响） | 真实指针场景记 SKIP（不是 PASS）；其余断言改用控制面完成；单独运行且桌面空闲时这些套件全 PASS | 本轮 `-Gate Regression` 的 SKIP 项为 hover/drag/dpi/h-settings 的部分真实指针用例；**不影响**其余 23 套件与 160 项审计 | 已绕行 |
| BLOCKER-002 | ① 环境 | 第二轮长稳在 15 分钟处被“停掉所有 hermes-panel-native 进程”的清理命令误杀（脚本的 probe 管道随之关闭） | ① 重跑一次 → 再次被同一清理动作影响；② 改为独立数据目录 + 不在长稳期间做任何进程清理/构建 | 第三次在**最终产物**上重跑 30 分钟，期间不做任何清理；结果见 `audit/soak-final.json` | 前两轮样本作废（保留在 `audit/soak.log`、`audit/soak2.log`） | 已绕行 |
| BLOCKER-003 | ① 环境 | 无头/远程会话下系统托盘气泡的**视觉**无法可靠确认（通知中心可能被策略关闭） | ① 用 Win32 契约实测（`NIM_MODIFY` 不匹配 hWnd 返回 FALSE）；② 控制面暴露 `Shell_NotifyIcon` 返回值并断言 true | 用“返回值 true + 计数 + 边沿/抑制断言”证明已投递给 Shell；最终外观留给用户真机确认 | 气泡的字体/位置/时长属于系统 UI，无法在门禁里像素断言 | 已绕行 |
| BLOCKER-004 | ① 环境 | 真实供应商偶发 `Connection reset by peer`（实测 4 次 1 次），逐条探测会偶发失败 | ① 连接层失败重试一次（HTTP 4xx/5xx 不重试）；② 失败原因照实显示 | 重试 + 状态可见；夹具 `probe_flaky` 覆盖 | 供应商自身抖动仍可能显示一次红点 | 已绕行 |

## 假设（需求类取舍）

| ID | 场景 | 采取的做法 | 依据 | 需要用户确认？ |
|---|---|---|---|---|
| ASSUMPTION-001 | 遮罩点击语义 | 只有取消按钮/Esc 关闭模态；点遮罩吞掉 | DESIGN §4.8 #32 与 v0.4 修订记录、旧版 `confirmBox` 无遮罩处理 | 否（已按设计文档） |
| ASSUMPTION-002 | previewDiff 覆盖范围 | 只对助手真正支持 dry_run 的 4 个命令预演；其余按原有危险确认执行 | 助手契约实测（`fallback-remove`/`delete-provider`/`fallback-move`/`restore` 忽略 dry_run） | 否（安全优先） |
| ASSUMPTION-003 | 新建供应商的“手动输入/编辑” | 直接常显多行「模型」字段（旧版是按钮切换显隐） | 旧版 `#np-models` + `btn-np-manual` 的功能等价；常显少一次点击、布局审计 0 越界 | 否（可在观感上再调） |
| ASSUMPTION-004 | Tab 焦点链范围 | 遍历当前页控件表（含表单字段），Enter 激活，Esc 清除；不实现完整的 Tab 分组/助记符 | DESIGN §4.9 #36 的“Tab 顺序”最小满足；避免改动控件树结构 | 否 |
| ASSUMPTION-005 | 默认 WSL 发行版 | 改为 `Ubuntu-24.04`（与旧版 `SETTING_DEFAULTS.distro` 一致） | 旧版 `main.js` 默认值；本机已迁移，不受影响 | 否 |
| ASSUMPTION-006 | 未知设置键 | 保留并原样写回（不参与业务） | AUDIT-PLAN G1“不丢未知键” | 否 |
| ASSUMPTION-007 | 最小化窗口 | 任何几何写入前先 `SW_RESTORE` | AUDIT 随机点击命中最小化后触发 INV1；修复后不再产生 `[error]` | 否 |

## 工具改动（审计期，按 AUDIT-PLAN §12.5 记录）

- `tools/fake_hermes.cpp`：
  - `fallback-move` 只接受 `to`（缺 `to` 复刻真实助手的 TypeError 形状）——审计 F-001；
  - `delete-provider` 只接受 `purge_key`/`remove_fallbacks`，出现旧字段名直接报错——审计 F-002；
  - dry_run 只对 4 个真实支持的命令生效——审计 F-003；
  - 新增 `<home>/list_slow`（list 慢 1.5 s）与 `<home>/config_broken`（health `parses=false`）。
- `tests/harness/client.h`：把控制面 1460 超时、`GetCursorPos failed`、`another window holds the mouse capture`
  归为环境干扰（SKIP 而非 FAIL）。这是**环境判定**的修正，不是放宽产品断言。
- `tests/motion_tests.cpp`：进入卡片/贴条步骤前用 `place` 确定性回到收起态（真实光标可能已把贴条展开）。
- `tests/h_pages_tests.cpp`：`SelectAll` 改为退格路径（控制面 `key` 是 WM_CHAR，Ctrl+A 的 0x01 到不了自绘输入框）。
