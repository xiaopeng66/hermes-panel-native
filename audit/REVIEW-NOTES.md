# 审查记录（阶段 1，边审边写）

> 每条：结论（通过/不通过/不适用）+ 证据 + 级别。维度定义见 `AUDIT-PLAN.md` §5。
> 审查对象：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`
> SHA256 `b113b415…14216`（见 `BASELINE.md`）。审查时间：2026-10-19 03:30–05:00。
> 除特别注明外，交互证据均由**隔离数据目录 + 假助手**（`tools/fake_hermes.cpp`）产生；
> 真实助手证据只用**只读**命令（`list`/`health`）或隔离 `hermes_home` 夹具。

## 进度

| 维度 | 覆盖情况 | 备注 |
|---|---|---|
| A 功能正确性 | A1–A12 全部执行 | 2 个 P1、6 个 P2、3 个 P3 |
| B 界面与视觉 | B1–B7 全部执行 | 42 张 6 主题页面截图 + 挂件对照板 |
| C 交互与手感 | C1–C8 全部执行 | 命中自洽由 h-controls 91 控件覆盖 |
| D 流畅性与动画 | D1–D7 全部执行 | 计时器起停有控制面证据 |
| E 性能与资源 | E1–E6 全部执行 | 含 30 分钟长稳 |
| F 稳定性与异常 | F1–F8 全部执行 | 含真实助手非零退出路径 |
| G 数据与持久化 | G1–G5 全部执行 | 隔离性与原子写有独立证据 |
| H 安全与隐私 | H1–H6 全部执行 | 日志/报告/截图密钥扫描 |
| I 可访问性与文案 | I1–I5 全部执行 | 1 个 P2（键盘）、2 个 P3 |
| J 与旧版差异 | J1–J4 全部执行 | 36 项对照逐项复核 |

## A. 功能正确性

| ID | 结论 | 证据 |
|---|---|---|
| A1 | 通过 | `h-controls` 91 个控件中心命中自洽 + 逐控件真点；本次复跑 Pages 9/9 PASS（`20261019-032553-391-Pages.txt`）。 |
| A2 | 通过 | `h-models` 10 段（分组/折叠/搜索/过滤/星标/点行/↑↓↵/滚动/矩形稳定）；`behavior_probe2` 复核搜索与过滤。 |
| A3 | **不通过（P1）** | 回退链上移/下移对真实助手完全无效，见 F-001。移除/追加/逐条探测本身通过（`h-fallback` + 真实助手实测）。 |
| A4 | **不通过（P1/P2）** | 删除供应商的两个勾选项参数名错（F-002，P1）；加模型页“从供应商探测填充”不填输入框（F-007，P2）；新建供应商缺手动模型输入（F-008，P2）；API Key 缺显隐按钮（F-009，P3）。 |
| A5 | 通过 | `h-backup` 5 段；列表/类型过滤/回滚确认/参数全部有断言。 |
| A6 | **不通过（P2/P3）** | 关闭按钮不遵守 `closeAction=quit`（F-005）；主题/挂件/高度/热键/延迟/档案/自启/迁移均通过。 |
| A7 | 通过 | 托盘注册、图标色、多行 tooltip、左键、右键菜单 21 项、`TaskbarCreated` 重挂（`readds 0→1`，独立 ctypes 探针）。 |
| A8 | 通过 | 默认 `Ctrl+Alt+H` 注册成功；冲突时 dump `hotkeys=false, hotkeyErrors[0]=1409`，设置页显示“注册失败：可能被其他程序占用（错误 1409）”（misc_probe）。 |
| A9 | 通过 | 单击展开（`strip-click` → state=3）、拖动阈值/吸附（`drag`/`a-*`）、悬浮卡（`strip-card` → window `[-2084,956,504,328]`，不压指针）、4 样式 × 4 边。 |
| A10 | **不通过（P2）** | 点遮罩会关闭模态，与 DESIGN §4.8 #32/旧版（遮罩无动作）不符，见 F-006。Enter/Esc/危险皮肤/Toast 计时正确。 |
| A11 | 通过 | 诊断弹窗读取中模态不消失、原地刷新；关掉后旧结果不弹回（`readToken`）；本次复测：刷新点击后标题仍为“诊断”、facts 原地更新。 |
| A12 | **不通过（P1/P2）** | previewDiff 对 4 个写命令不生效且会**直接落盘**（F-003/F-004）；对 `set-main` 正常（dry_run→确认→真写）。 |

## B. 界面与视觉

| ID | 结论 | 证据 |
|---|---|---|
| B1 | 通过 | 6 主题 × 5 页 + 添加页 3 子页共 42 张截图（`audit/evidence/B1-*-20261019-035038.png`），`audit` 门禁每页 0 越界；人工目检无重叠/越界/退化控件。 |
| B2 | 通过 | 长模型名/长路径省略号截断（`widget` Ellipsize 断言 + 5000 模型 180 字符名实测无溢出）；等宽数字对齐（截图）。 |
| B3 | 通过 | 灯色/语义色与 DESIGN §3 一致；`widget` 对比度与灯色断言。 |
| B4 | 通过 | 空态/加载态/错误态文案齐全（模型页空态、助手错误、备份空态、离线整块）。 |
| B5 | 通过 | 78 条 WCAG 断言（六主题 13 组前景/背景）+ 浅色截图目检；本次复核浅色正文/次要文字可读。 |
| B6 | 通过 | 4 样式 × 4 边 × 2 主题拼板 `B6-widget-sheet-20261019-035038.png`；尺寸/贴边由 `audit` 门禁逐项断言（120/192 DPI）。 |
| B7 | 通过 | 悬浮卡定位在屏幕内侧（`[-2084,956,504,328]` 全在工作区）、提示气泡跟随指针且有翻转；不复现闪烁（motion 断言“重复 show 不重启 cardStart”）。 |

## C. 交互与手感

| ID | 结论 | 证据 |
|---|---|---|
| C1 | 通过 | `h-controls`：91 个控件 `control-at` 与控件表一致。 |
| C2 | 通过 | 拖动阈值 `max(SM_CXDRAG,6 DIP)`、跟手、最近边吸附；`strip-drag`/`drag` 断言。 |
| C3 | 通过 | 90ms 交叉淡化；悬停不展开、单击展开；无抖动。 |
| C4 | **不通过（P2）** | Tab 不移动焦点（无焦点链），F5 不刷新；键盘只能覆盖 `/`、↑↓、↵、Esc、表单编辑。见 F-024。 |
| C5 | 通过 | `h-ime`：中文组合 `你好`、矩形稳定、候选窗锚定（本次 Pages 复跑 PASS）。 |
| C6 | 通过 | 滚轮归属：模型列表/回退阶梯/页内/下拉/模态横向各自独立（`h-pages`、`h-fallback`、500 供应商实测滚到底）。 |
| C7 | 通过 | 模态期间吞掉 WM_KEYDOWN/WM_CHAR/IME；`h-settings` 回归断言。 |
| C8 | 通过 | 500 供应商/5000 模型/300 回退：滚到 maxScroll=18688，末行可达；下拉滚动窗口可选最后一项（`h-add` 12 供应商 + 500 供应商实测）。 |

## D. 流畅性与动画

| ID | 结论 | 证据 |
|---|---|---|
| D1 | 通过 | 入场/贴条/卡片/提示/主题六处动画；`motion` 门禁 + 控制面时间戳（`panelStart`/`themeStart` 非零）。 |
| D2 | 通过 | 悬停交叉淡化 90ms，无“闪一下”；卡片已显示不重放入场。 |
| D3 | 通过 | 动画结束 `motion.timer=false`（展开 250ms 后、主题 300ms 后实测）；`SPI_GETCLIENTAREAANIMATION` 关闭时进度恒 1。 |
| D4 | 通过 | `audit` 门禁在动画路径后仍 0 几何违规；INV1/INV4/INV5 由 geometry 断言。 |
| D5 | 通过 | `motion` 断言重复 show 不重启 cardStart。 |
| D6 | 通过 | 交互到画面变化的主观延迟 < 1 帧（控制面 tap 后 dump 立即反映；截图序列无中间卡顿）。 |
| D7 | 通过 | 逐条探测：`检测中 n/N` + 当前行 `…`，每完成一行立即落地（`h-fallback` 中间态断言）。 |

## E. 性能与资源

| ID | 结论 | 证据 |
|---|---|---|
| E1 | 通过 | 空闲 CPU：展开 1.24% / beacon 1.56% / tag 1.24% / flag 1.56%（阈值 3%）。 |
| E2 | 通过 | 冷启动到可应答 0.333–0.374 s，到列表就绪 0.360–0.397 s（`audit/coldstart-populated.json`）；窗口创建到可见 0.012 s。 |
| E3 | 通过 | 工作集 51.5 MB / 私有 60.8 MB / GDI 16 / USER 16 / 句柄 371 / 线程 20；150 轮泄漏循环增量 GDI 0、USER −2、句柄 0、线程 −3、内存 +512 KB。 |
| E4 | 通过 | 500 供应商 / 5000 模型（含 180 字符模型名）/ 300 回退：加载 5.21 s，五页 `audit` 0 问题，工作集 57.7 MB，滚动到底无卡死。 |
| E5 | 通过 | 单 exe 943,616 B，发布目录无 DLL（Release 门禁）。 |
| E6 | 通过 | 30 分钟长稳：见 §E6 表。 |

### E6 长稳（30 分钟，1 s 交互循环 + 5 s 轮询）

证据：`tmp/hermes-panel-native/audit/soak-report.json`、`audit/soak.log`。

| 时间 | 工作集 KB | 私有 KB | GDI | USER | 句柄 | 线程 |
|---|---|---|---|---|---|---|
| 31 s | 59,248 | 64,468 | 16 | 17 | 372 | 23 |
| 160 s | 60,408 | 65,388 | 16 | 19 | 372 | 19 |
| 300 s | 58,4xx | 65,8xx | 16 | 17 | 368 | 19 |
| 600 s | 58,5xx | 65,9xx | 16 | 17 | 370 | 20 |
| 1800 s | 见 soak-report.json | | | | | |

结论：内存进入平台期（前期 +1 MB 后不再增长），GDI/USER/句柄/线程不涨，0 几何违规。

## F. 稳定性与异常

| ID | 结论 | 证据 |
|---|---|---|
| F1 | 通过 | 畸形/空/超大/GBK/非零退出：面板不崩、错误可见、清夹具恢复（`Audit` 门禁 error path 全 PASS）。 |
| F2 | 通过 | WSL down→up：灯色/离线整块/启动 WSL 路径（`h-offline`）。 |
| F3 | **部分不通过（P3）** | 损坏 settings/latency/profiles 能启动并回默认；但 `config.yaml parses=false` 时面板不显示警告也不禁用写（DESIGN §5.1 要求写操作全停）→ F-018。 |
| F4 | 通过 | Aero Snap/最大化后托盘隐藏/显示 reconcile 不崩（`h-controls`）；外部窗口状态不自动纠偏（dpi 门禁）。 |
| F5 | 通过 | `WM_DISPLAYCHANGE`/`SPI_SETWORKAREA` 两写刷新；跨 DPI 拖拽落点精确（`dpi` 7/7）。 |
| F6 | 通过 | 多实例互斥 + 唤醒；热键冲突 1409 可见；托盘被 Explorer 重建后恢复。 |
| F7 | 通过 | 强杀后重启：设置与 edge/anchor 保留，`legacyMigrated` 不重复迁移（`h-migrate` + 手工强杀）。 |
| F8 | 通过 | 极窄/极扁/负坐标/200%：`geometry` 17,432 断言 + `dpi` 202 DPI 用例 + 192 DPI 负坐标挂件实测。 |

## G. 数据与持久化

| ID | 结论 | 证据 |
|---|---|---|
| G1 | 通过 | `settings` 22 断言：逐键往返、坏字段只回落该字段、不丢未知键（未知键不在 ToJson 白名单内——**记为差异**，见 J2）。 |
| G2 | 通过 | 原子写（temp+rename）；坏文件不留 `.tmp`；备份恢复内容正确（`h-backup`）。 |
| G3 | 通过 | `--data-dir`/`--instance` 生效；真实 `%LOCALAPPDATA%\HermesModelPanelNative\settings.json` 在隔离运行前后 mtime/size 完全一致（mtime_ns 1792349112103714700 不变）。 |
| G4 | 通过 | 旧 Electron 设置只读迁移一次；`legacyMigrated` 语义正确（`h-migrate`）。 |
| G5 | 通过 | 日志 5 MB 轮转；`log` 21 断言脱敏；本次扫描 `audit/` 与真实日志无 `sk-`/Bearer 值。 |

## H. 安全与隐私

| ID | 结论 | 证据 |
|---|---|---|
| H1 | 通过 | 密钥只在 `.env`；日志只出现 key_env 名字；截图表单为空；`grep -r "sk-[A-Za-z0-9]{8,}" audit/` 无命中。 |
| H2 | 通过 | 探活/probe 只发只读请求；写命令只在用户点击路径（`h-*` 的 requests.log 断言）。 |
| H3 | **部分不通过（P3）** | API Key 默认掩码成立（`h-add` 断言 value 在字段、绘制掩码、日志无值）；但**没有显隐按钮**（F-009）。 |
| H4 | **不通过（P2）** | 延迟探测把模型名拼进原始 JSON（未转义）→ 含 `"`/`\` 的模型名让请求非法，见 F-012。其余字段走 nlohmann 序列化，无注入。 |
| H5 | 通过 | Release 门禁断言无控制面；发布包启动不暴露管道。 |
| H6 | 通过 | HttpFetch 使用 WinHTTP 默认 TLS 校验，无忽略选项；失败可见（`invalid url`/HRESULT）。 |

## I. 可访问性与文案

| ID | 结论 | 证据 |
|---|---|---|
| I1 | 通过 | 术语统一（网关/WebUI/WSL/待重启）；无中英混排错位；`widget` 文案断言。 |
| I2 | 通过 | 52 条 tooltip 全部单行（`h-controls`）；每条解释控件作用。 |
| I3 | 通过 | 状态不只靠颜色：灯有文案、chip 有图标+文字、待重启有胶囊文字。 |
| I4 | 通过 | 高 DPI（192/202）下字号行高保持 DIP 比例；截图无糊字。 |
| I5 | **不通过（P2）** | 关键动作无法纯键盘完成：Tab 无焦点链、F5 不刷新，切页只能靠鼠标；见 F-024。 |
| I6 | **不通过（P3）** | 窗口标题是乱码 `Hermes j21W8B…62g7F`（源码里被写坏的字节），Alt-Tab/辅助技术可见；见 F-011。 |

## J. 与旧版 Electron 的差异

| ID | 结论 | 证据 |
|---|---|---|
| J1 | 通过（36 项逐项复核，3 处差异记入 FINDINGS） | 见下表 |
| J2 | **不通过（P1/P2）** | 行为差异中属“遗漏”的：F-001/F-002/F-003/F-005/F-006/F-007/F-008/F-009。刻意差异：模型切换二次确认、供应商菜单用模态承载、档案/诊断/用量/延迟为新功能（旧版无）。 |
| J3 | 通过 | 本次对照：原生 943,616 B / 冷启动 0.36 s / 工作集 51.5 MB；旧版 Electron（历史实测）366 MB 包、44.6 MB 内存/0.145 s（PROGRESS H7，旧版本会话不运行）。 |
| J4 | 通过 | 旧版与 `%APPDATA%\HermesModelPanel` 未改；新数据在 `%LOCALAPPDATA%\HermesModelPanelNative`；可同时运行（不同 mutex/实例）、回退路径见 README。 |

### J1 36 项逐项复核

| # | 项 | 结论 | 说明 |
|---|---|---|---|
| 1 | 品牌标记 | 通过 | activity 图标 + Hermes，12 DIP 内边距（截图）。 |
| 2 | 右侧四按钮 | 通过 | pin/minimize-2/minus/x，顺序与语义一致；关闭悬停 err 色。 |
| 3 | 标题栏拖拽 | 通过 | HitTest 返回 HTCAPTION，按钮区 HTCLIENT。 |
| 4 | 三盏灯 | 部分 | 名称/顺序/颜色一致；“异常时灯体可点击重试”未实现（旧版灯也不可点）→ F-023(P3)。 |
| 5 | 忙碌条 | 通过 | 切换中/读取 WSL 配置… |
| 6 | WebUI 不可达 chip | 通过 | warn/err + wifi-off + 点击重试。 |
| 7 | 网关待重启 chip | 通过 | 文案与点击排空式重启。 |
| 8 | 同步按钮 | 通过 | refresh-cw + 等宽时间戳 + 旋转/对勾。 |
| 9 | hero 铭牌 | 通过 | 灯/供应商/等宽模型/hero 渐变/api_mode 标签。 |
| 10 | 搜索行 | 通过 | 放大镜/占位/`/` 提示/清空/Esc；IME 门禁。 |
| 11 | 过滤段 | 通过 | 全部/收藏/最近 + 计数。 |
| 12 | 列表提示 | 通过 | “N 条结果 · ↑↓ 选择 · ↵ 切换”。 |
| 13 | 模型列表 | 通过 | 行高 31/分组/滚动/悬停/点行/星标/切换中状态。 |
| 14 | 回退链页头 | 通过 | 标题/副题/逐条探测。 |
| 15 | 梯级列表 | **不通过** | 上移/下移在真实助手上无效（F-001）。 |
| 16 | 追加条 | 通过 | 折叠表单、来源下拉/手填。 |
| 17 | 折叠表单 | 通过 | 字段顺序/错误行/追加。 |
| 18 | 添加子分段 | 通过 | 三段。 |
| 19 | 新建供应商 | **不通过** | 缺“手动输入/编辑”模型多行（F-008）与 Key 显隐（F-009）；其余（自动推导/掩码/模式 chips/两个开关）通过。 |
| 20 | 加模型 | **部分** | 下拉/多行/追加/错误行通过；“从供应商探测填充”不填输入框（F-007）。 |
| 21 | 删除 | 通过 | 下拉选择 + 危险确认 + 在用供应商禁用；勾选项受 F-002 影响。 |
| 22 | 底部动作条 | 通过 | 备份说明 + 探测 + 保存。 |
| 23 | 备份页头 | 通过 | 标题/副题/刷新。 |
| 24 | 备份过滤 | 通过 | 全部(计数)/config/.env。 |
| 25 | 备份列表 | 通过 | 文件名等宽/时间/恢复 + 确认。 |
| 26 | 设置连接 | 通过 | 发行版/端口/刷新间隔（默认发行版差异见 F-014 P3）。 |
| 27 | 设置行为 | 通过 | diff/自动重启/置顶/关闭按钮/主题；关闭按钮语义见 F-005。 |
| 28 | 快捷键 | 通过 | 捕获/注册/冲突提示。 |
| 29 | 其他 | 通过 | WebUI 打开 + meta 行。 |
| 30 | 离线态 | 通过 | 整块替换 + 启动 WSL + 写操作消失。 |
| 31 | 底部导航 | 通过 | 五项 + 回退计数徽标。 |
| 32 | 模态 | **部分** | 标题/正文/取消确定/Enter/Esc；点遮罩语义不符（F-006）。 |
| 33 | Toast | 通过 | 右下角 2.6 s 成功/失败。 |
| 34 | 贴边细条 | 通过（文档滞后） | 实现为 10–30 DIP 挂件（G11 用户已验收）；DESIGN §2/§4 仍写 8 DIP → 文档待更新（F-038 P3）。 |
| 35 | 图标集 | 通过 | `icons` 60 断言（四主题 × 两底色）+ 截图。 |
| 36 | 键盘 | **部分** | `/`/Esc/Enter/↑↓ 可用；Tab 顺序缺失（F-024）。 |

## 逐项补充记录（非缺陷，供二次验收对照）

- A1：`Add` 页三个子页、`Backup` 页刷新、`Settings` 页四段全部逐点通过（`h-pages` 22.9 s 全断言）。
- A7：托盘菜单 21 条标签全部无乱码（dump 原文，见 `behavior_probe2` 输出）。
- A9：贴条 4 样式 × 4 边尺寸 = `ScaleDip(DIP, DPI)` 且贴边侧逐像素贴住（`audit` 32 项）。
- B5：浅色主题下正文/次要/弱色分别 ≥4.5/3.0/2.4（widget 78 断言）。
- D3：`animations=true` 时展开后 250 ms 计时器已停；`animations=false` 时所有 start=0。
- E6：30 分钟 0 崩溃、0 几何违规、资源平台期。
- F1：真实助手在隔离夹具上返回 `TypeError`（F-001 复现）时，面板显示错误且不崩（可恢复）。
- G3：真实设置文件 mtime_ns 在隔离会话前后完全一致。
- H1：`grep -r "sk-[A-Za-z0-9]\{8,\}" audit/` 与真实 `app.log` 均无命中。
- J3：旧 Electron 本会话未启动（只读源码对照），内存/冷启动沿用 H7 实测并标注来源。

## 检查点

**检查点 1（2026-10-19 04:00）**
- 已完成：阶段 0（基线全门禁 + 性能）、A1–A12、B1–B7、C1–C8、D1–D7、E1–E5、F1–F4、G1–G5、H1–H6、I1–I5、J1–J4 首轮。
- 进行中：E6 30 分钟长稳（后台）。
- 阻塞：无（A7 的 TaskbarCreated 首测因发错窗口判 FAIL，改用面板窗口后 `readds 0→1`，属测试脚本问题，已修正）。
- 发现：P1 × 3（F-001/F-002/F-003）、P2 × 7、P3 × 6。
- 下一步：写 REVIEW-REPORT / FIX-PLAN → 修复。

**检查点 2（2026-10-19 05:05）**
- 已完成：E6 长稳收尾、补充探针（托盘 hWnd 契约、Tab 焦点、悬浮卡、AI 模态、真实助手参数契约）。
- 进行中：无。
- 阻塞：无。
- 下一步：阶段 2/3 文档 → 阶段 4 修复。

## 审查期工具与改动

- 新增（均在 `tmp/hermes-panel-native/audit/`）：`measure_baseline.py`、`measure_coldstart.py`、`measure_populated.py`、
  `screenshot_sweep.py`、`verify_fade_cache.py`、`verify_fade_controlled.py`、`verify_strip_cache.py`、
  `behavior_probe.py`（首版，dump 形状写错，保留过程）、`behavior_probe2.py`、`inspect_probe.py`、
  `misc_probe.py`（首版，HWND_MESSAGE 写错）、`misc_probe2.py`、`bigdata_check.py`、`soak_check.py`、
  `real_readonly_check.py`、`verify_tray_hwnd.py`。
- 没有修改产品代码；真实 `~/.hermes` 只读；隔离夹具写在 `tmp/hermes-panel-native/audit-helper-fixture/`。

## 修复后复核（阶段 4 收尾，2026-10-19 08:40）

| Finding | 复核方式 | 结果 |
|---|---|---|
| F-001 | 真实助手（隔离 hermes_home）：`delta` 仍报 TypeError；`to` 返回 `moved:true` | 通过 |
| F-002 | 真实助手：`purge_key/remove_fallbacks` → `purged_key=commented, removed_fallbacks=1`；`.env` 中 `BETA_KEY` 被注释 | 通过 |
| F-003 | `h-fallback`：`fallback-add` 预演→确认→真写；`fallback-remove` 不注入 dry_run | 通过 |
| F-004 | `h-pages settings` 新段 + `verify_fixes_b`：`closeAction=quit` 点 × 进程退出 | 通过 |
| F-005 | `h-add` + 独立探针：遮罩保持、取消/Esc 关闭 | 通过 |
| F-006 | `h-add`：探测后 `modelsText` 含 `probe-a..c` | 通过 |
| F-007 | `h-add`：手输两行 → `upsert-provider.models == [manual-a, manual-b]` | 通过 |
| F-008 | `h-settings`：`notifications.lastOk == true`；独立探针触发 helper 边沿后 `lastOk=true` | 通过 |
| F-009 | 独立探针：模型名 `quote"model
ame` → 请求 `{'cmd':'probe','model':'quote"model\name'}` 合法，延迟记录成功 | 通过 |
| F-010 | `h-controls`：Tab 焦点变化、Esc 清除、F5 产生 list 请求；真机 `SendInput` 的 `/beta`、Esc 也通过 | 通过 |
| F-011 | `h-models`：`list_slow` 在飞时点行 → 确认后最终发出 `set-main` | 通过 |
| F-012 | `h-add`：眼睛切换 `keyRevealed` 与 `masked` | 通过 |
| F-013 | 外部探针：`GetWindowTextW(panel)` == `Hermes 模型面板` | 通过 |
| F-014 | `settings` 单测：默认 `Ubuntu-24.04` | 通过 |
| F-015 | `h-add`：`config_broken` → 设置页 meta 提示；独立探针：新建供应商保存被拒（未发 `upsert-provider`），toast「config.yaml 解析失败，写操作已停用」，日志 `write refused` | 通过 |
| F-016 | `h-add`：点 `lampGateway` 触发 list/health | 通过 |
| F-017 | 代码复核 + tooltip 文案（since 空 → “网关尚未重启”） | 通过 |
| F-018 | `DESIGN.md` v1.5 已更新 | 通过 |
| F-019 | `settings` 单测：`futureKey`/`another` 往返保留 | 通过 |
| F-020 | `-Gate Audit` 160/160；随机点击命中最小化后不再产生 `[error]` | 通过 |

## 追加复审（2026-10-19，真机闪退专案）

用户报告「点了几次贴条后闪退」。按 AUDIT-PLAN 模板追加一条 finding 与一轮暴力压测记录。

### F-021（P1）系统/安装器请求关闭时进程被 RestartManager 强杀（"闪退"）

- **现象**：真机 2026-10-19 上午，面板在安装程序运行期间从屏幕上消失；进程确认死亡。
- **复现**：`tmp/hermes-panel-native/stress/stress_violent.py` S5——向面板窗口
  `SendMessageTimeout(WM_QUERYENDSESSION, 1, ENDSESSION_CLOSEAPP)` → 应用回答 TRUE；
  随后 `WM_ENDSESSION(wParam=TRUE)` → 修复前进程**继续运行**（RM 等待超时后 TerminateProcess）。
- **证据**：
  - Windows 事件日志：11:31:26 MSI 安装完成（MsiInstaller 1040/1033/1042/11707）；
    11:34:24 RestartManager 会话启动 → 11:36:26 会话结束，面板进程在此前后消失；
    全天**无 Application Error**（WER 对本产品有效，02:42/05:52 两条是测试 exe）。
  - `%LOCALAPPDATA%\HermesModelPanelNative\logspp.log`（逐行 flush，可信）：最后一行
    11:32:44 `data: refresh started`，无 error、无 `message loop finished`、无 `exiting with`。
  - `logs/crash-probe.txt` 停留 2026-10-18 21:43（旧记录）→ 本次不是 SEH 崩溃。
- **根因（已证实）**：控制窗是 `HWND_MESSAGE` message-only 窗口，收不到
  `WM_QUERYENDSESSION/WM_ENDSESSION` 广播；面板/贴条窗口过程未处理这两个消息，
  `DefWindowProc` 对询问回答 TRUE（同意关闭）但 `WM_ENDSESSION` 后进程不退出 →
  RestartManager 等待超时后强杀。外部强杀 = 无 WER、无日志、无探针，与全部证据吻合。
- **影响面**：任何安装器/更新器（RestartManager）或系统关机都会强杀面板；被强杀时
  设置可能未保存、托盘图标可能残留。
- **建议方向**：顶层窗口处理两个会话消息（保存设置 + 干净退出）；lifecycle 门禁加回归。
- **另记（第一次死亡，约 11:25）**：其时间窗内无任何 RM/MSI 事件，UI 日志在 11:25:00 后
  停更——证据只支持"进程被外部结束（含任务管理器强杀）"，无法归因到产品缺陷；
  640+ 轮暴力压测未使产品崩溃/停摆。归入环境/外部类。

### 暴力压测（stress_violent.py，修复后全绿）

| 场景 | 轮次 | 结果 |
|---|---|---|
| S1 贴条单击展开/收起 | 300 | 300/300，进程存活，零 error |
| S2 显示/隐藏（托盘左键/热键同路径） | 200 | 200/200 |
| S3 展开→悬停设置标签 3.2s（提示气泡）→点击 | 60 | 60/60 |
| S4 贴条拖拽换边（四边轮转） | 20 | 20/20 |
| S5 RestartManager 关闭演练 | — | 修复后：同意询问 + 干净退出（exit 0） |
| S6 快速换页 + 轮询并发 | 60 | 60/60 |
| S7 慢助手（1.5s）+ 12 次连点排队切换 | — | 队列排空，busy=false pending=false |
| 全程心跳（UI 停摆探测 3s 超时） | 每 10 轮 | 零停摆 |

修复后复核：`lifecycle` 门禁新增用例「RestartManager session end exits cleanly」通过；
Lifecycle 4/4、Regression 23/23、Audit 160/160、Release 打包可启动（见 evidence/）。
