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
| BLOCKER-005 | ① 环境 | 桌面存在**外部指针活动**：门禁合成拖拽期间，应用记下的真实光标与请求终点不符。实测 ① `drag-commit cursor=(1891,1163)` 而请求终点 `(853,345)`，`lastDrop.valid=false`（拖拽被外力打断，`CommitDrop` 没跑）；② A2 记录 `(1280,18)` 而请求 `(1280,601)`；③ 同一时段出现 `capture-lost-press` 与 `(-829,-297)` 这类越界光标读数。**2026-10-21 10:2x 抓到直接证据**：`SetCursorPos` 到 (700,700)/(300,300)/(1900,200) 三个点，250 ms 后读回**全部**是 (1313,59)；`GetClipCursor` 无裁剪、`GetAsyncKeyState(LBUTTON)`=false、`GetCapture()`=0（排除了"某窗口抓着鼠标"与系统裁剪）；同一分钟里 Explorer 窗口标题从 `session2-academic-v12\exports` 变成 `session2-paper-examples-v16\exports` —— **本机另有一个自动化会话在开着资源管理器跑任务**（`E:\AI\Codex\outputs\...` 下），它同时在动指针 | ① 判定与断言分离：`IsEnvironmentInterference` 把"光标漂移 / capture 被抢 / 桌面被占用 / `GetCursorPos failed` / 控制面 1460"归 SKIP(77)；② `Pointer::MoveTo` 落点校验 + 重试 4 次；③ A2/A3/A5 各补"落点漂移"判定；④ 空闲窗口里单跑复验（`a-crossdpi` 连跑 3 次 27.4–27.6 s 全 PASS；`a-snap`/`a-pullout`/`a-edge`/`drag` 各有 PASS 记录） | 真实指针场景记 SKIP（不是 PASS）；控制面能覆盖的改用控制面；单跑复验作为补充证据 | 干扰期间 `drag`/`hover`/`a-*` 全部立刻 SKIP（`MoveTo` 目标 (-30,-3)、(1280,690) 都被顶回 (1313,59)）；干扰停止时同一套件全 PASS。**不是产品回归**。2026-10-21 15:2x 干扰源收工后单跑 `-Gate Pointer`：10 项 9 过 1 SKIP（a-snap 被碰一次，重试即 PASS）——**最终产物上真实指针套件全部有通过记录** | 已绕行 |

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
- `tests/a_tests.cpp`（2026-10-21，A5 跨屏拖拽）：补上 A2/A3 已同源存在的**落点漂移判定**（`lastDrop.valid==false`
  或落点偏离请求终点 >2 px → 抛"the desktop is in use"由 `IsEnvironmentInterference` 归 SKIP）。落点正确时
  DPI / 尺寸 / 单次写入 / 单调采样四条断言照旧硬判——是**环境判定**补齐，不是放宽产品断言。
- 2026-10-22 真机验证的补充观察（均归 BLOCKER-005 同一类环境干扰）：①桌面上有第三方置顶窗口
  （交易软件）会盖住面板所在矩形，整屏截图取到的是它；②有外部程序在移动真实指针（实测 250 ms 内
  从 (1900,200) 漂到 (1979,254)），`click.ps1` 的落点校验因此报 "cursor interference" 并拒绝点击。
  措施：真机点击类验证改用控制面合成点击（`tap screen:true`）+ 隔离夹具实例，渲染证据取自
  夹具实例截图；这仍满足"真机点一遍"的意图（同一份渲染/命中代码），但**不**声称在用户当前
  桌面状态下完成了真实指针点击。
- 工具能力边界（非产品问题）：`computer-use` 的 UIA 通道看不到本面板窗口（自绘分层 tool window；
  `get_app_state(pid=…)` → `Windows UI Automation returned no tree for pid …`，`list_apps` 里也没有它）。
  真机点击因此改用与门禁同一机制的系统级 `SendInput`（`tmp/hermes-panel-native/diag/click.ps1`，带落点校验；
  注意 DPI：不设 awareness 的进程读到的是虚拟化坐标，虚拟桌面矩形必须从 PMv2 aware 进程取）。
