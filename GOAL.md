# 持续开发目标（GOAL）—— Hermes 模型面板原生版（hermes-panel-native）

> 本文件是跨会话的持续开发目标与工作规程。**每次开发会话开始时先读本文件 + `DESIGN.md` + `PLAN.md` + `PROGRESS.md`。**
> 规格冲突时以 `DESIGN.md` 为准（视觉与信息对照表在其 §3/§4）；要改规格，先改 `DESIGN.md` 并记入修订记录，再改代码。

## 授权与推进方式（2026-10-15，用户明确授权）

> **用户已授权连续推进 H2b → H7，中途不需要停下来等我检查。** 切片之间不请求确认、不做"给你看一眼再继续"
> 的停顿；把可自动验证的部分全部做完、门禁跑绿，**最后一次性汇报最终结果**。

- 汇报内容（一次性）：完成/未完成清单、每片的**实际命令与关键输出**、截图与证据路径、与旧版的行为差异清单、
  与 Electron 版的内存/冷启动/体积对照、以及"只有你本人能做"的待办（例如首次真实使用时的确认）。
- **不暂停的情形**（自行决定并继续，写进 `PROGRESS.md`）：设计细节取舍、控件观感微调、切片顺序微调、
  取舍不确定时的保守选择；WSL/hermes 当前不可用时**改用假助手夹具**完成该片并继续（限制写进汇报）。
- **顺延到最终汇报的情形**（不阻塞开发）：需要真实 WSL 写入或真实使用才能确认的项、替换路径选择、
  首次真实运行清单——一律列进最终汇报的"待你执行"，不作为开发暂停点。

## 一句话目标

把 Electron 版 Hermes 模型面板（`projects/hermes-panel`，HermesModelPanel v0.2.0）替换为
**与 `balance-panel-native` 同一套技术栈的原生应用**：C++20 + 纯 Win32 + Direct2D/DirectWrite 手绘控件，
**不引入 WebView2 内容区**；面板的**信息、文案、观感、交互与旧版一致**，功能等价或更强。
按 `PLAN.md` §6 的切片 **H2b → H7** 依次推进，每片以该切片自己的门禁全绿为唯一完成判据。

## 权威文件

| 文件 | 角色 |
|---|---|
| `DESIGN.md` | 规格唯一权威：技术栈与不变量、视觉系统 token、**36 项逐项对照表**、数据与动作映射、待定项 |
| `PLAN.md` | 执行方案：实测契约（助手协议/状态字段/设置键）、模块布局、控件规格、**切片清单 §6**、门禁清单、风险对策 |
| `PROGRESS.md` | 会话日志：已完成 / 证据 / 下一步 / 阻塞（**唯一状态源**） |
| `tools/e2e.ps1` | 门禁总脚本（`-Gate Unit|Widget|Lifecycle|Shell|Regression|A|All`） |
| `projects/hermes-panel/` | 旧版 Electron 实现：**只读**的观感与行为权威，不修改、不删除 |

## 环境与路径硬约束

- 全部文件只能写在 `E:\AI\Zcode\projects\hermes-panel-native\` 内；临时文件只写 `E:\AI\Zcode\tmp\hermes-panel-native\`。
- 写路径一律用 `E:/AI/Zcode/...` 或 `E:\AI\Zcode\...`（禁止 POSIX `/e/`、`/tmp/` 作为写入目标——历史上被误解析成盘根目录）。
- 工具链：VS Build Tools 2022 + MSVC 14.44 + Windows SDK 10.0.26100 + CMake（`C:\Program Files\CMake\bin\cmake.exe`）；
  构建：`cmake -S . -B build -G "Visual Studio 17 2022" -A x64` 后 `cmake --build build --config Debug --parallel`。
- 基线：x64、Per-Monitor V2 清单、Windows 11（兼容下限 Win10 1809，圆角降级）。

## 每个会话的标准循环（照此执行，不停顿）

1. **定位**：读 `PROGRESS.md` 最后一条 + `PLAN.md` §6 的切片表，确认下一片（未完成的最靠前一片）。
2. **实现**：只做**一个可验证切片**（能独立通过该切片门禁的最小增量）。
3. **门禁**：跑该切片门禁（`tools/e2e.ps1 -Gate <子集>` 或对应单测），**并回归受影响的历史门禁**。
4. **不绿 → 根因**：先证据后修复（systematic-debugging：一次一个假设、最小验证）；
   **禁止补丁式修复**——凡与 `DESIGN.md` 不变量（INV-H1…H6）冲突的改动，先改设计再改代码。
   同一问题 3 次修复失败 → 停下质疑架构，记录到 `PROGRESS.md`。
5. **全绿 → 记录**：更新 `PROGRESS.md`（做了什么 + **实际命令与输出摘要**）与 `PLAN.md` §6 的完成标记；继续下一片。
6. **任意时刻工程必须可构建**：切片落地时要么完整、要么回退到上一个绿状态；不允许把树留在"编译不过"的中间态
   （H2a 的教训：`application` 里 114 处引用的剥离必须与视图桩一起落地）。

## 任务队列（按序执行，细节见 `PLAN.md` §6）

- [x] **H2b 图标 + 基础控件层** ✅ 2026-10-15：30 个图标对齐 icons.js、`widgets.{h,cpp}` 控件原语、chrome 度量全对齐 styles.css
- [x] **H2c 文本与列表 + IME** ✅ 2026-10-15：`TextField`（选择/双向/代理对/掩码/IME 内联组合）、`h-ime` 门禁真键盘通过
- [x] **H3 数据层与桥** ✅ 2026-10-15：`helper`（异步管道超时是真的）、`probe`、`state` 解析 + 签名去抖；`helper` 14 / `hermes-state` 20 断言
- [x] **H4 模型页** ✅ 2026-10-15：状态寄存器 / hero / 搜索（`/`、✕、Esc）/ 过滤段 + 计数 / 分组树（吸顶、折叠、其他供应商）/ ★ / 点行切换 / ↑↓↵ 游标 / 滚动；`h-models` 门禁 10 段全过，`widget` 415 断言
- [x] **H5 其余四页** ✅ 2026-10-15：回退链（阶梯 + 折叠表单 + 逐条探测 + 危险确认）、添加（三子表单 + 掩码 Key +
      探测药丸 + 危险确认）、备份（计数过滤 + 回滚确认）、设置（连接/行为/主题/热键捕获/其他 + 生效即落盘）；
      `h-fallback` 7 段 / `h-add` 8 段 / `h-backup` 5 段 / `h-settings` 6 段全绿，四页截图在 evidence/
- [x] **H6 外壳接线与离线态** ✅ 2026-10-15：托盘全菜单（当前模型 / 「切换到」=收藏+最近切换 / 主题 / 关闭按钮）、
      `closeAction` 生效、离线整块 + 底栏不可点 + 启动 WSL、旧设置一次性迁移（只读、只搬一次）；
      `h-offline` 3 段 / `h-migrate` 2 段全绿
- [x] **H7 验收与交付** ✅ 2026-10-15：全量门禁（Unit/Widget/Lifecycle/Pages/Regression/A）全绿；`tools/package.ps1`
      出包（单 exe 609,792 B）；与 Electron 的对照表（体积实测 0.6 MB vs 366 MB，内存/冷启动 44.6 MB / 0.145 s，
      Electron 侧本会话无法运行需你在交互桌面确认）；替换决策写进 `README.md`；真机确认清单在 PROGRESS H7 节

## 硬性规则

1. **不要求用户手动验收**（除 H7 的真机确认）：门禁必须能在假助手 + 假 WebUI + 隔离数据目录下自证。
2. **一切"完成/通过"必须附证据**：实际执行的命令 + 关键输出摘要；无证据视为未完成。
3. **不写真实的 hermes 配置**：`~/.hermes/config.yaml` 与 `.env` 的唯一写入者是 WSL 里的
   `projects/hermes-panel/wsl/hermes-panel-apply.py`（一行都不改）；面板只做调用与解析。测试一律用假助手夹具。
4. **不修改旧版**：`projects/hermes-panel` 及其数据（`%APPDATA%\HermesModelPanel`）保持可用、不被修改、可回退。
5. **不引入 WebView2 内容区**：界面全自绘（用户明确要求与 `balance-panel-native` 同栈）。
6. **观感以对照为准**：任何页面上线前，按 `DESIGN.md` §4 的 36 项逐项核对，差异写进修订记录；
   参考图可用旧版自带的 `--capture-out` 截图模式生成。
7. **日志脱敏**：绝不记录 API Key / 密码 / token 值；助手响应里只记状态码、键名与计数。
8. 每次会话开始与结束都更新 `PROGRESS.md`（哪怕只写一行"无进展 + 原因"）。

## 何时停下来问用户（授权后已收窄）

只有这三类允许中断：

1. **真正阻塞**：缺权限 / 工具链损坏 / 目标磁盘不可写，且已尝试 2 种以上替代方案；
   （WSL 或 hermes 服务不可用**不算**——改用假助手夹具继续，限制写进最终汇报）
2. **破坏性操作**：删除或覆盖用户数据（`~/.hermes`、旧版 Electron 面板的数据）、对外发布、
   改动 WSL 里的 `hermes-panel-apply.py`；
3. **同一问题 3 次根因修复失败**（按标准循环第 4 步，先记录再停）。

其余一律自行推进；切片完成即继续下一片，不等待检查。

## 完成定义（Definition of Done）

- `DESIGN.md` §4 的 **36 项对照表逐项有实现与证据**（门禁断言或截图对照）；
- 门禁清单（`PLAN.md` §7）在最终产物上一遍全绿：`geometry/state/log/widget/helper/page/h-ime/h-models/h-fallback/h-add/
  h-backup/h-settings/h-offline/h-migrate` + 继承的 `shell/tray/hover/drag/dpi/lifecycle/a-*`；
- 与 Electron 版的对照表（常驻内存 / 冷启动到可见 / 包体积）与替换决策交付给用户；
- 全程未修改 WSL 助手、未写真实 hermes 配置（开发与验收一律用假助手夹具；真实写入只发生在你自己使用面板时）、旧版与其数据可回退；
- `PROGRESS.md` 完整可追溯（每片有命令与输出证据）。
