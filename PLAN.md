# hermes-panel-native · 详细方案（PLAN v2）

> 目标：把 Electron 版 `projects/hermes-panel`（HermesModelPanel v0.2.0）换成
> **与 balance-panel-native 同一套技术栈**：C++20 + 纯 Win32 + Direct2D/DirectWrite **手绘控件**，
> **不引入 WebView2 内容区**。信息、文案、观感、交互与旧版一致。
>
> 规格权威：`DESIGN.md`（视觉 token 表 + 36 项逐项对照表）。本文件是**执行方案**：契约、模块布局、
> 控件规格、切片顺序、门禁与验收方式。凡标注【实测】的都是从旧版源码读出来的，不是推测。

---

## 1. 已定前提

| 项 | 决定 |
|---|---|
| 技术栈 | C++20 / MSVC / Win32 / D2D + DWrite（与 balance-panel-native 相同，可执行文件单进程） |
| 内容区 | **全自绘**（无 WebView2） |
| 外壳 | 继承 balance-panel-native：双窗吸附、8 DIP 细条、悬停展开、拖拽、DPI、拓扑、托盘、热键、设置、主题、日志、控制面、测试框架（已过 A1–A7 门禁） |
| 唯一写入者 | WSL 里的 `wsl/hermes-panel-apply.py` **一行不改**；原生侧只做调用与解析 |
| 旧版 | Electron 面板与 `%APPDATA%\HermesModelPanel` 只读、可回退；新数据在 `%LOCALAPPDATA%\HermesModelPanelNative` |
| 面板尺寸 | 旧版是"内容自适应 + `app:fit`"；原生改固定档位，H2c 用实测内容高度定稿（预期宽 380 DIP，高按页面最大值） |

---

## 2. 契约（全部【实测】，原生端按此实现，不许猜）

### 2.1 助手调用协议

```
wsl.exe -d <distro> -- <pythonExe> <helperPath>      # 启动，windowsHide
stdin : 一行 JSON  {"cmd":"<verb>", "hermes_home":"…可选…", ...args}
stdout: 一行 JSON  {"ok":true,"data":{…}} | {"ok":false,"error":"…","trace":"…"}
退出码 : 0 成功 / 1 业务失败 / 2 协议错误（空请求、非 JSON、未知命令）
探活   : wsl.exe -d <distro> -- true
```

- 19 个 verb：`ping`、`health`、`list`、`webui-models`、`webui-refresh`、`backup-list`、`set-main`、
  `upsert-provider`、`add-models`、`delete-provider`、`set-provider-key`、`fallback-list`、`fallback-add`、
  `fallback-remove`、`fallback-move`、`fallback-probe`、`probe`、`reload`、`restore`。
- 响应里 `data` 可能缺 `ok` 字段（助手会自动补 `{"ok":true,"data":…}`）；原生解析按 `ok` 分支。
- 编码：请求与响应都按 UTF-8 读写；解析失败时把原始字节按 GBK 再试一次并记日志（**不记录任何 Key/密码值**）。

**原生实现要点**：`wsl.exe` 子进程 + 管道 + 单行读写 + 超时（读配置 15s / 写操作 60s）+ 可取消；
一进程一请求（与旧版一致，避免协议错位）。

### 2.2 状态模型（`state` 推送与 `panel:state` 的字段）

```jsonc
{
  "settings": { …见 §2.5… },              // 已脱敏
  "status": {
    "wsl": "up" | "down" | "unknown",     // unknown = 本轮探测未回：界面画"加载中"，不画离线卡
    "checkedAt": 0, "error": "",
    "config": { "mtime": 0, "mtime_text": "", "size": 0, "mode": "", "parses": true },
    "webui":   { "port": 8787, "reachable": false, "winReachable": false, "ports": [8787, 9119] },
    "gateway": { "port": 8642, "reachable": false, "since": "", "sinceUnix": 0, "active": "" }
  },
  "model":     { "provider": "custom:xxx", "model": "deepseek-v4.1-flash", "api_mode": "…" },
  "providers": [ { "name","provider_id","base_url","key_env","key_set","api_mode",
                   "default_model","models"(条数),"models_discovered","is_active","line" } ],
  "fallbacks": [ { …条目字段…, "index": 0 } ],
  "webui":     { "port": 8787, "groups": [ …模型目录… ] } | null,
  "recents":   ["provider|model", …],
  "favorites": ["provider|model", …],
  "busy": false,
  "loading": true                          // 首帧：列表未到位 → 界面禁用写操作
}
```

- `model` / `providers` / `fallbacks` / `files` / `gateway` 来自助手 `list`；`status.*` 来自 `health` + 本机 TCP 探活；
  `webui.groups` 来自 `webui-models`（WebUI 的模型目录，模型页三分组的数据源之一）。
- 模型页的行 = `providers[].default_model` + `providers[].models`（条数）+ `webui.groups` 合并去重，
  再叠加 `favorites` / `recents` 标记；分组标题是供应商名。
- 重绘去抖：旧版按 `signature(state)`（model/providers/fallbacks/favorites/recents/webui.groups/status.wsl）
  判断"内容真变了"才重画，避免打断滚动与输入 → 原生同样必须按签名比较，**不得每帧重建列表**。

### 2.3 动作（25 个旧通道 → 原生 handler）

| 旧通道 | 原生 | 说明 |
|---|---|---|
| `state` | `CollectState()` | 返回 §2.2 |
| `refresh` | `RefreshData(withWebui=true)` | `health` + 两个端口探活（并发）之后重建状态 |
| `helperHealth` / `backups` / `webuiModels` | 直通 `health` / `backup-list` / `webui-models` | 读路径 |
| `setModel` | `SetMain(provider,model,dryRun)` | 写路径 → `set-main` |
| `upsertProvider` / `addModels` / `deleteProvider` / `setProviderKey` | 同名 | 写路径 |
| `fallbackAdd` / `fallbackRemove` / `fallbackMove` / `fallbackProbe` | 同名 | 写路径 |
| `probe` | `Probe(provider…)` | 探测模型列表 |
| `reload` | `Reload()` | 热加载 WebUI（不重启网关） |
| `webuiRefresh` | `WebuiRefresh()` | WebUI 侧刷新后再拉一次状态 |
| `restore` | `Restore(name,kind)` | 备份恢复（模态确认） |
| `startWsl` | `StartWsl()` | `wsl.exe -d <distro> -- true`；失败给指引 |
| `openWebUI` | `OpenWebUI()` | ShellExecute `http://127.0.0.1:<port>` |
| `setSetting` | `SetSetting(key,value)` | §2.5；热键变更需重注册（同值不重注册，否则会被自己占用误报失败） |
| `close` / `minimize` / `snap` / `edgeEnter` / `edgeLeave` | 外壳动作 | 已实现 |
| 推送 `state` / `theme` / `docked` / `busy` | 同语义 | `busy` 带文案（如"切换中…"） |

**写操作的前置守卫**（旧版行为，逐条照搬）：`loading`（列表未到位）或 `status.wsl != up` 时禁用全部写按钮；
删除供应商要求输入名称匹配；当前模型正在用的供应商禁止删除。

### 2.4 探活

- WebUI：依次试 `8787 → 9119`（设置里的 `webuiPort` 是首选），命中即用并写回 `status.webui.port`；
- 网关：`status.gateway.port`（默认 8642）+ `/health`；
- WSL：`wsl.exe -d <distro> -- true` 的退出码；
- 刷新间隔 `pollSec`（默认 20s）；写操作后立即刷新一次。

### 2.5 设置键（沿用旧名，便于一次性迁移）

`distro`、`pythonExe`、`helperPath`、`webuiPort`、`gatewayPort`、`pollSec`、`hotkey`（字符串如 `Control+Alt+H`）、
`favorites`、`recents`、`previewDiff`、`autoReloadGateway`、`closeAction`、`alwaysOnTop`、`theme`、`pos`、`dockedSide`。
映射到原生：`closeAction→Settings.closeAction`、`alwaysOnTop→topmost`、`theme`、`pos/dockedSide→model_.floating/edge`，
其余进 hermes 自有设置段；`hotkey` 用 Modifiers 位 + VK 映射（`Control+Alt+H` → `MOD_CONTROL|MOD_ALT` + `'H'`）。

---

## 3. 模块与文件布局

```
src/
  core/            # 继承（几何/状态/几何控制器）—— 不改
  app/
    main.cpp              # 进程入口（继承；去掉 balance 的登录/数据初始化）
    application.{h,cpp}   # 外壳宿主：窗口/托盘/热键/设置/主题/日志/状态机/控制面（继承，删 balance 内容）
    panel_view.{h,cpp}    # 【新】hermes 视图模型：页面/控件树/文案与状态的纯函数构建
    pages.{h,cpp}         # 【新】5 个页面的内容模型（models/fallback/add/backup/settings）
    actions.{h,cpp}       # 【新】§2.3 的动作实现（守卫、忙态、模态与 Toast 的请求）
  render/
    theme.{h,cpp}         # 【改】hermes token（DESIGN §3 深/浅两套 + 等宽/UI 字体 + hero/plate 渐变）
    widgets.{h,cpp}       # 【新】控件层（§4）
    panel.{h,cpp}         # 【改】外壳绘制（标题栏/发丝线/铭牌）+ 分派到 widgets
    layout.{h,cpp}        # 【改】文档流式布局 + 控件命中框（沿用外壳那套"渲染与命中共用一份几何"）
    icons.{h,cpp}         # 【新】30 个 Lucide 图标（16×16 路径，1.5–2px 描边，圆头圆角）
    surface.{h,cpp}       # 继承
  data/
    settings.{h,cpp}      # 继承（加 hermes 段与旧设置迁移）
    log.{h,cpp}           # 继承（脱敏规则扩充：key/api_key/token/password/sk-）
    helper.{h,cpp}        # 【新】wsl.exe + stdin/stdout JSON 桥（超时/取消/编码回退）
    state.{h,cpp}         # 【新】§2.2 状态模型 + 签名比较 + 轮询
    probe.{h,cpp}         # 【新】WSL/TCP/HTTP 探活
tests/
  widget_tests.cpp        # 【新】控件单测：布局/命中/滚动几何/文本度量/IME 组合/换行截断
  helper_tests.cpp        # 【新】助手桥：假 wsl 脚本夹具（成功/失败/超时/坏 JSON/GBK/退出码）
  page_tests.cpp          # 【新】页面模型：分组/过滤/计数/禁用态/危险确认（纯函数，无窗口）
  h_*_tests.cpp           # 【新】端到端场景（假助手 + 假 WebUI + SendInput）
```

规模预估：控件层 1200–1800 行、5 个页面模型 700–1000、数据层与桥 600–900、图标 200–300、
布局与绘制 500–700、门禁与夹具 800–1200 → **合计约 4000–6000 行**（含测试）。

---

## 4. 控件规格（每项都要有单测 + 页面截图两重证据）

| # | 控件 | 状态 | 度量（DIP） | 交互 | 样式来源 |
|---|---|---|---|---|---|
| 1 | 状态灯 lamp | ok/warn/err/idle | 点 8，文案 12 | 异常时整灯可点（重试） | `.lamp` |
| 2 | chip | err/warn | 高 24，圆角 12，图标+文案 | 点击触发动作（重试 / 重启网关） | `.chip[data-kind]` |
| 3 | 按钮 | normal/hover/active/disabled | 高 30，圆角 9，主/幽灵/危险/图标-only | 悬停变亮、按下 0.97、禁用降饱 | `button.primary/ghost/danger-btn/icb` |
| 4 | 分段控件 seg | 选中/未选中 + 计数徽标 | 高 26，圆角 999 | 点击切换；选中描边 accent | `.seg[class=active]` |
| 5 | 单行输入框 | 空/聚焦/错误/禁用 | 高 30，圆角 9，左内边距 30（带图标时） | **IME 组合**、光标、选择、Ctrl+C/V/X/A、Home/End | `.inp`, `.fld input` |
| 6 | 密码框 + 眼睛 | 同 5，值掩码 | 同 5 | 眼睛切换明文/掩码 | `input[type=password]`, `#btn-np-eye` |
| 7 | 多行文本 | 同 5 | 行高 18，最少 3 行 | 同上 + 换行、滚动 | `textarea` |
| 8 | 开关 switch | 开/关/禁用 | 36×20，圆角 10，滑钮 16 | 点击切换 | `.switchrow`, `.sw` |
| 9 | 下拉 select | 收起/展开/禁用 | 高 30 | 点击展开列表、键盘上下+Enter、点外关闭 | `select` |
| 10 | 列表 | 行/悬停/选中/分组头 | 行高 31 | 滚轮、拖动条、点击行 | `#model-list`, `#bkp-list` |
| 11 | 梯级列表 | 同上 + 序号 | 行高 31 | 上移/下移/移除按钮 | `.ladder li` |
| 12 | 模态 | 显示/滚动正文（含 diff） | 宽 min(320, 面板宽−32) | Enter 确定 / Esc 取消 / 点遮罩不关 | `#modal` |
| 13 | Toast | 显示 2.6s | 右下角，圆角 9 | 无交互 | `#toast` |
| 14 | 忙碌条 | 隐藏/显示文案 | 行内 12px 文本 | 无 | `#busy` |
| 15 | 发丝线 / 铭牌 | — | 1px 渐变 / 渐变 + 顶部高光 | 无 | `--hx-hair`, `.plate`, `.hero` |

**滚动**：只绘制可见行 + 像素偏移；滚轮 `WM_MOUSEWHEEL` 3 行/格；拖动条宽 6、悬停加宽到 8。

**文本编辑最小可用集**（自绘输入框的验收范围）：插入（含 IME 提交）、退格/删除、左右移动、Home/End、
Shift+左右选择、Ctrl+A/C/V/X、点击定位与拖动选择；**不做** undo/redo、双击选词、右键菜单（与旧版差距记录在案）。

---

## 5. 中文 IME 方案（本次最大技术风险，单独立切片）

1. 输入框获取焦点 → `ImmAssociateContext` 绑定默认 IME 上下文；失焦解绑。
2. `WM_IME_STARTCOMPOSITION/COMPOSITION/ENDCOMPOSITION`：读 `GCS_COMPSTR/GCS_RESULTSTR`，
   组合串**内联绘制**在光标处（下划线 = 组合中），提交时插入正文。
3. 候选窗定位：`WM_IME_REQUEST` → `IMR_CANDIDATEFORM` 返回光标矩形（否则候选窗会飘到屏幕角落）。
4. 光标：仅在聚焦时以 530ms 闪烁——这是全应用**唯一**允许的定时器（设计原则"无常驻动画帧循环"仍成立：
   它只在聚焦时存在，失焦即停）。
5. 门禁：`h-ime` 场景用 SendInput 打拼音（`nihao`）后按空格提交，断言搜索框文本 == `你好` 且列表按此过滤；
   同时断言组合期间界面不抖动（尺寸/矩形不变）。

**降级阀门**：若 IME 组合串内联绘制在 2 次修复后仍不稳定，改为"组合期间只显示系统拼字窗、输入框内不画组合串"
（功能仍可用，观感略有差异），并把该决定写进 `DESIGN.md` 修订记录。

---

## 6. 切片（细化到可一次落地并验证）

> 每个切片必须：落地即可构建 + 对应门禁绿 + 附命令与输出；不允许把工程留在不可构建状态（H2a 的教训）。

| 片 | 范围 | 新增接口 | 门禁 / 证据 | 预估 |
|---|---|---|---|---|
| **H2a** ✅ | 剥离 + 桩 + 主题| 全部完成
| **H2b** ✅ 基础控件层（2026-10-15 完成）| 图标 30 个对齐 icons.js（search/refresh-cw/git-branch/pin 按原 path 重写）、`widgets.{h,cpp}`（灯/chip/按钮/seg/开关/发丝线/铭牌/hero 底色 + 每控件 SkinOf 纯函数）、chrome 度量全对齐 styles.css（34/27/47/24/22/31、.icb 24、灯点 6 方点）| `IconId` 表、`widgets::SkinOf/*Shape`、`HslColour/Lighten/hair token` | `widget-tests` **291 断言全绿**（≥40 达标）；`widget-gallery` 深浅两版控件总览截图对照 styles.css（`tmp/hermes-panel-native/evidence/widget-gallery-{dark,light}.png`）|
| **H2c** ✅ 文本框 + IME（2026-10-15，下拉/列表/模态/Toast/忙碌条随 H4/H5 页面切片交付；档位定稿随后）| 单行/密码/多行输入（含 IME）、下拉、可滚动列表、梯级列表、模态、Toast、忙碌条；实测内容高度定稿档位 | `TextField`、`ListBox`、`ModalHost`、`Toast` | `widget-tests`（文本度量/换行/滚动/选择）；`h-ime` 场景；尺寸写回 DESIGN | 2 片 |
| **H3** ✅ 数据层与桥（2026-10-15 完成，门禁 `helper`+`hermes-state` 14+20 断言；轮询接线随 H4）| `helper`（stdin/stdout JSON、超时、取消、编码回退）、`probe`（WSL/TCP/HTTP）、`state`（§2.2 + 签名比较 + 轮询） | `HelperCall(cmd,args)`、`Probe*()`、`HermesState` | `helper-tests`：假 wsl 脚本夹具（成功/失败/超时/坏 JSON/GBK/退出码 1/2） | 1–2 片 |
| **H4** ✅ 模型页（2026-10-15 完成）| 状态寄存器（三盏灯 + 两个 chip）、hero 铭牌（身份色 / codex 标签 / 在线点 / 切换中 `→`）、搜索（`/` 聚焦、✕ 清空、Esc 清空）、过滤段 + 计数 + 命中提示、分组树（当前组置顶、折叠、吸顶组头、「其他供应商」）、★ 收藏、点行切换、↑↓+↵ 游标、滚轮/拖动条滚动 | `AttachStatusView`、`AttachModelsPage`、`ListHitTest`、`Application::{StartRefresh,StartSwitch,ModelsPageClick,...}` | `h-models` 门禁：假助手（`tools/fake_hermes.cpp`）+ 隔离数据目录，10 段断言全过（分组/折叠/搜索/过滤/星标/切换/最近/矩形稳定）；`widget` 415 断言（状态接线 + 列表几何 + 命中）；证据 `tmp/hermes-panel-native/evidence/h4-models-{page,search,star}.png` | 1 片 |
| **H5** ✅ 其余四页（2026-10-15 完成）| 回退链（阶梯 + 折叠表单 + 逐条探测）、添加（三子表单 + 掩码 Key + 探测药丸）、备份（计数过滤 + 回滚）、设置（连接/行为/主题/热键捕获/其他） | `AttachFallbackPage`、`AttachAddPage`、`AttachBackupPage`、`AttachSettingsPage`；`StartHelperAction`（通用写动作 + 探测 + 备份列表） | `h-fallback` 7 段 / `h-add` 8 段 / `h-backup` 5 段 / `h-settings` 6 段全绿；`ModalHost`/`Toast` 为全局面板部件；页内滚动承接「旧窗口会变高」的表单 | 3 片（H5a/H5b/H5c）|
| **H6** ✅ 外壳接线（2026-10-15 完成）| 托盘全菜单（当前模型 +「切换到」子菜单=收藏/最近 + 主题/关闭按钮子菜单）、`closeAction`、离线整块 + 底栏不可点 + 启动 WSL、旧设置一次性迁移 | `BuildMenu()` 的子菜单、`SetSetting`、`StartWsl()`、`MigrateLegacySettings()` + `--legacy-dir` | `tray` 回归 + `h-offline` 3 段 + `h-migrate` 2 段；门禁一律带 `--legacy-dir <隔离>/no-legacy`（不得读用户真实旧设置） | 1 片 |
| **H7** 验收交付 | 端到端（假助手 + 假 WebUI 全链路）、打包、与 Electron 版对照、替换决策 | — | 全量门禁绿 + 对照表（内存/冷启动/体积）+ 决策文档；真机使用确认写成"待你执行"清单（非门禁） | 1 片 |

---

## 7. 门禁清单（ctest 名）

- 继承：`geometry`、`state`、`log`、`hover`、`drag`、`dpi`、`tray`、`shell`、`lifecycle`、`a-snap`、`a-pullout`、`a-topology`、`a-c1`。
  - `a-panelui` 为 balance 残留场景（断言 activeSite/trend/compact 等已随内容层删除的控件），2026-10-15 起从
    A 门禁移除；页面级点击覆盖由 H4–H6 的 `h-models`/`h-fallback`/`h-add`/`h-backup`/`h-settings`/`h-offline` 承接。
  - `drag` 于 2026-10-15 在本机与未动过的 balance-panel-native **同诊断行失败**（环境共性：系统移动循环
    对合成输入不再进入，见 `PROGRESS.md` H2b 节）；推迟到 H7 在空闲桌面复跑后再计入最终门禁。
- 新增：`widget`、`helper`、`hermes-state`、`h-ime`、`h-models`、`h-fallback`、`h-add`、`h-backup`、`h-settings`、`h-offline`、`h-migrate`。
- `h-models` 的输入路径（2026-10-15 实测决定）：**指针与键盘通过控制面 `tap` / `key` / `vk` 合成进真实窗口过程**
  （同样的 WM_LBUTTONDOWN / WM_CHAR / WM_KEYDOWN 处理链，含命中测试与字段编辑）；不用 SendInput，
  因为本机桌面经常被其它应用占住前台（`SetForegroundWindow` 失败、合成点击落到别处）——
  与 `drag` 记录的是同一类环境限制。真实输入路径仍由 `h-ime`（无 IME 时 SKIP）与 A 门禁覆盖。
- `h-ime` 的环境事实：桌面有中文 IME 时 `VK_ESCAPE` 会被输入法吞掉（面板自身的 Esc 逻辑有 widget/控制面覆盖，
  h-models 用 `vk` 命令验证）。
- 全部走假助手 + 假 WebUI + 隔离数据目录；**真实 WSL 不参与开发与验收**（真实写入只发生在你自己使用面板时）。
- 发布门禁 `Release`（不是 ctest）：构建 Release → `tools/package.ps1` 打包 → 启动产物，断言发布目录没有 DLL、
  进程不立即退出、且没有控制面窗口（控制面只在 Debug 编译进去）。
- 推进方式：用户已授权连续完成 H2b→H7，切片间不暂停；见 `GOAL.md` 的「授权与推进方式」。

## 8. 观感对照方法（本轮新发现的可复用手段）

旧版自带截图模式：`npm run capture`（`electron . --capture-out capture.png`）可渲染一张界面截图后退出。
因此对照流程为：

1. 用旧版按页生成参考图（若 `--script` 能驱动切页/切主题，则生成 5 页 × 2 主题的参考集；否则退化为"每页手动截"）；
2. 原生以相同尺寸、相同主题、相同数据（假助手夹具注入同一份 providers/models）截图；
3. 逐项核对 `DESIGN.md` §4 对照表，并把差异（间距/字号/色值）记进修订记录——**不以"看起来差不多"过关**。

## 9. 风险与触发式对策

| 风险 | 触发信号 | 对策 |
|---|---|---|
| IME 不稳定 | 组合串错位/候选窗飘 | §5 降级阀门（2 次修复失败即降级并记录） |
| 控件层超预算 | H2b/H2c 两片后仍缺 3 类以上控件 | 启用**最小可用交互集**：先只做 H4 主链路（搜索/过滤/点行切换，输入框只做 ASCII + IME 提交），表单页延后 |
| 列表性能 | 100+ 模型时掉帧 | 只画可见行；必要时给行加 31px 等高缓存 |
| wsl.exe 慢/挂 | 探活超时率 > 10% | 单请求超时 + 取消 + 退避；UI 保持旧版忙态语义 |
| 份数与旧版偏差 | 截图对照发现差异 | 逐项回写 `DESIGN.md` 并调整 token/度量 |

## 10. 完成定义（DoD）

- `DESIGN.md` §4 的 36 项逐项对照表全部有实现与证据（截图或门禁断言）；
- 上述门禁在最终产物上一遍全绿，且 `h-*` 场景覆盖：切模型、回退链增删排序、新建/加模型/删除、备份恢复、
  设置改动、离线恢复、旧设置迁移；
- 与 Electron 版的对照表（常驻内存 / 冷启动到可见 / 包体积）与替换决策交付；
- 全程未修改 WSL 助手、未写真实 hermes 配置（除用户手动确认的那一次）、旧版与其数据可回退。
