# 最终报告（阶段 6）

> 任务：对 `E:\AI\Zcode\projects\hermes-panel-native` 做穷尽式质量工程
> （审查 → 审查报告 → 修复文档 → 修复 → 二次验收 → 结果报告）。
> 权威计划：`AUDIT-PLAN.md`。执行日期：2026-10-19。

## 一、审查对象与最终产物

| | 审查前 | 最终 |
|---|---|---|
| exe | `b113b415d5cdbb3c…14216`（943,616 B） | **`e19a074caa6895dd…28037`（956,416 B）** |
| 构建 | 2026-10-19 02:57 | 2026-10-19 09:44（Release） |
| 对应记录 | `audit/BASELINE.md` | `PROGRESS.md` G14 + `audit/ACCEPTANCE-R2.md` |

## 二、完成清单（每条附证据）

| # | 交付物 | 状态 | 证据 |
|---|---|---|---|
| 1 | 基线（exe 哈希、全门禁、性能） | ✅ | `audit/BASELINE.md`、`audit/coldstart-populated.json`、`baseline-perf.json` |
| 2 | 逐项审查记录（A–J） | ✅ | `audit/REVIEW-NOTES.md`（含每项结论 + 证据 + 修复后复核表） |
| 3 | 审查报告（findings/通过项/对比/未覆盖） | ✅ | `audit/REVIEW-REPORT.md`（P1×3、P2×9、P3×8） |
| 4 | 修复文档（每条 finding 一条 FIX） | ✅ | `audit/FIX-PLAN.md`（FIX-001…020） |
| 5 | 修复实施（一次一个根因） | ✅ | `PROGRESS.md` G14；每个 FIX 的改法/门禁列在 FIX-PLAN |
| 6 | 二次验收（A–J 全量，审查前 vs 现在） | ✅ | `audit/ACCEPTANCE-R2.md` |
| 7 | 阻塞与假设登记 | ✅ | `audit/BLOCKERS.md`（4 阻塞全部已绕行 + 7 假设） |
| 8 | 全门禁在最终产物上一遍绿 | ✅ | 见 §三 |
| 9 | 真机（真实输入）点按关键路径 | ✅ | 独立脚本 `tmp/hermes-panel-native/audit/real_input_check.py`：6/6（真实 SendInput 鼠标 + Unicode 键盘；切页/搜索/切模型确认/Esc/×） |
| 10 | 用最新 Release 重启真机实例 | ✅ | 见 §六 |

## 三、最终门禁（最终产物）

命令：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate <Gate>`
（构建 + ctest；证据在 `tmp/hermes-panel-native/evidence/`）

| 门禁 | 结果 | 关键输出 | 证据 |
|---|---|---|---|
| Unit | **PASS** | geometry 17,432 / state 32 / log 21 / settings 25 | `20261019-095112-347-Unit.txt` |
| Widget | **PASS** | widget 570 / icons 60 | `20261019-095113-917-Widget.txt` |
| Lifecycle | **PASS** | 3 | `20261019-095115-465-Lifecycle.txt` |
| Pages | **PASS** | 9/9（h-settings 因桌面被用户 Chrome 占用 SKIP；单跑全 PASS，含本轮新增断言） | `20261019-093822-412-Pages.txt` |
| Shell | **PASS** | 6/6（drag/dpi 的真实指针场景因桌面占用 SKIP；单跑全 PASS） | `20261019-095207-339-Shell.txt` |
| Regression | **PASS** | 23/23（hover/drag/h-models 因桌面占用 SKIP，见 §八） | `20261019-095740-347-Regression.txt` |
| ReadHelper | **PASS** | 27 | `20261019-095117-576-ReadHelper.txt` |
| Audit | **PASS** | 160/160（800 轮模糊、150 轮泄漏、2 显示器×4 挂件×4 边、异常路径、日志无 [error]） | `20261019-100329-167-Audit.txt` |
| Release | **PASS** | 956,416 B、无 DLL、无控制面、能启动 | `20261019-101143-972-Release.txt` |

`-Gate Audit` 的最终数值：CPU 展开 1.24% / beacon 1.24% / tag 0.94% / flag 1.26%；
150 轮泄漏循环 GDI 0 / USER −1 / 句柄 0 / 线程 0 / 内存 +748 KB。

**SKIP 明细（不是 PASS）**：`hover`/`drag`/`dpi`/`h-settings`/`h-models` 的部分真实指针用例，
在 `-Gate Shell/Pages/Regression` 的运行窗口内被用户自己的 Chrome 与 BalancePanelNative 占住
桌面/光标，harness 以 `RequirePointOnPanel` 或光标漂移判据主动 SKIP（`BLOCKER-001`）。
这些套件**单独在空闲桌面运行时全部 PASS**（`hover` 6/6、`drag` 全段、`dpi` 7/7、
`h-settings` 全段、`h-models` 全段）；控制面路径的断言不受影响。

## 四、修复摘要（用户可感知）

**P1（主流程/数据安全）**

1. **回退链 ↑/↓ 现在真的会改顺序**：改为助手契约的 `index/to`（真实助手复核 `moved:true`）。
2. **删除供应商的两个勾选项生效**：`purge_key` / `remove_fallbacks`（真实助手复核
   `purged_key=commented, removed_fallbacks=1`）。
3. **“写入前先看 diff”不再误写**：只对助手支持 dry_run 的 4 个命令预演；其余命令不会被注入
   dry_run，若未来助手契约漂移则明确提示“已直接写入”。

**P2（可用性/稳定性）**

- 关闭按钮遵守“退出程序”；点遮罩不再关闭模态；
- 加模型页“从供应商探测填充”真的写进输入框；新建供应商可手动输入/编辑模型；
- 托盘气泡真的能显示（`Shell_NotifyIcon` 返回值断言）；
- 延迟探测对含引号/反斜杠的模型名不再报非法 JSON；
- Tab 焦点链 + Enter 激活 + Esc 清除 + F5 刷新；
- 刷新期间点模型行不再“点空”（排队执行）；
- 审计随机点击命中最小化后不再触发 INV1（先 SW_RESTORE）。

**P3（打磨）**

- API Key 眼睛显隐；窗口标题恢复“Hermes 模型面板”；默认发行版 `Ubuntu-24.04`；
  `config.yaml` 解析失败时设置页有醒目提示且写操作停用；异常灯可点重试；
  托盘提示不再出现“网关自  未重启”；DESIGN 更新到 v1.5；未知设置键保存时保留。

## 五、与旧版 Electron 的差异

| 类别 | 内容 |
|---|---|
| **刻意差异（保留）** | 切模型前二次确认（真机要求：机器人需重启网关）；供应商“⋯”菜单用模态承载；新增诊断/用量/延迟探测/配置档案/托盘通知（旧版无）；固定窗口档位 + 页内滚动（替代 `app:fit` 的内容自适应）。 |
| **已消除的遗漏** | 回退链排序、删除供应商勾选项、previewDiff 语义、关闭按钮退出、模态遮罩、加模型探测填充、新建供应商手动模型、Key 显隐、Tab 键盘路径。 |
| **性能/体积** | 原生：956,416 B、冷启动 0.31–0.37 s、工作集约 56.5 MB（5000 模型 68 MB）；旧 Electron（历史实测）：约 366 MB 包、0.145 s、44.6 MB。 |
| **可回退性** | 旧版 `projects/hermes-panel` 与 `%APPDATA%\HermesModelPanel` 未修改；新旧可同时运行（独立 mutex/实例名/数据目录）；回退 = 启动旧版即可。 |

## 六、真机重启与新包状态

- 打包：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`，SHA256
  `e19a074caa6895dda0c4d0baaf4573ead2a7f9ece459b21fa587ecd2bd028037`，
  `manifest.sha256` 与实测一致。
- 真机重启：旧实例在阶段 0 停止；最终已用最新 Release（`dist`，SHA256 见上）启动
  （默认数据目录 `%LOCALAPPDATA%\HermesModelPanelNative`，真实设置 theme=sand / edge=left）。
  进程 PID 68304 存活，面板与贴条窗口都存在，面板标题为“Hermes 模型面板”
  （`GetWindowTextW` 输出），当前处于贴边收起态（贴条可见、面板隐藏）——与用户原来的
  “贴边胶囊”使用状态一致。`manifest.sha256` 与实测哈希一致。
- 真实配置只读：整个审计期间未写 `~/.hermes`；真实供应商只做过 `list`/`health` 与 `/models` 只读探测。

## 七、只有用户能做的事

1. **真实使用确认**：切换一次模型、在真实供应商上点一次“逐条探测”、开一次“写入前看 diff”
   再取消（确认没有落盘）。这些涉及真实 `~/.hermes` 写入，按硬约束留给用户。
2. **托盘气泡外观**：系统通知中心是否弹出取决于 Windows 通知设置；返回值已证明投递成功
   （`notifications.lastOk=true`）。
3. **第三方输入法**：本轮只覆盖系统默认拼音；搜狗/微软五笔的候选窗位置需真机确认。
4. **是否替换旧 Electron 版**：建议先用新包一周；旧版与其数据保持可回退。
5. **审美取舍**：新建供应商页的“模型”字段为常显（旧版是按钮切换显隐）；如需恢复切换式，
   改 `DESIGN.md` §4.5 #19 后再改代码。

## 八、阻塞与残留

| ID | 阻塞 | 绕行 | 残留影响 |
|---|---|---|---|
| BLOCKER-001 | 用户桌面被 Chrome / BalancePanelNative 占用，真实指针场景被 harness 判为环境干扰 | 记 SKIP；控制面 + 真机 SendInput（移到空白区）双路径验证 | `hover`/`drag`/`dpi`/`h-settings` 的部分真实指针用例在本轮 Regression 中为 SKIP（单独空闲时全 PASS） |
| BLOCKER-002 | 长稳脚本两次被“清理进程”的审计动作误杀 | 最终在全部构建/测试结束后单独跑 30 分钟，中途不动进程 | 前两轮样本作废（保留日志） |
| BLOCKER-003 | 无头会话无法目视托盘气泡 | Win32 契约 + 返回值断言 | 外观需用户确认 |
| BLOCKER-004 | 真实供应商偶发 `Connection reset` | 连接层失败重试一次 + 原因照实显示 | 供应商抖动时仍可能一次红点 |

最终 **P0 遗留：0；P1 遗留：0**。

## 九、可选增强（不实施，按性价比排序）

1. 设置页“恢复默认”按钮（当前只能逐项改）。
2. 备份页显示“写入前自动备份”的容量占用与一键清理。
3. 延迟探测的可用率趋势（最近 20 次已有数据，画一条 sparkline）。
4. 托盘菜单“诊断”直达入口（目前要开面板 → 设置 → 诊断）。
5. 模型列表的键盘 Home/End/PageUp/PageDown（当前 ↑↓ 逐行）。

## 十一、复验提示（给验收者）

- 想复验 P1：用隔离 `hermes_home` 直接跑真实助手脚本（命令见 `ACCEPTANCE-R2.md` §“J1 差异”）；
  **不要**在真实 `~/.hermes` 上做写测试。
- 想复验坏 config：在隔离数据目录放 `config_broken` 文件（假助手夹具），观察设置页 meta 与
  新建供应商保存被拒；真实环境不会用到这个夹具。
- 想复验真机交互：先关掉会遮挡面板的置顶窗（Chrome/BalancePanel），再跑
  `python E:/AI/Zcode/tmp/hermes-panel-native/audit/real_input_check.py`。

## 十、最终核验命令（可复跑）

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Unit
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Pages
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Shell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Regression
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate ReadHelper
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Audit
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Release
# 真实指针/键盘（点来点去、拖窗口、跨屏、IME）已挪出默认跑法：需要空闲桌面时单跑一次
powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Pointer
python E:/AI/Zcode/tmp/hermes-panel-native/audit/real_input_check.py     # 真机鼠标/键盘（需空闲桌面）
python E:/AI/Zcode/tmp/hermes-panel-native/audit/soak_run.py            # 30 分钟长稳（SOAK_SECONDS=1800）
```

## 十二、验收后的真机整改（2026-10-21，详见 `audit/ROUND-2026-10-21-BUSYBAR.md`）

本报告的验收结论仍然成立，但**交付物在此之后按真机反馈动过四处**，复验时以本节与
`ROUND-2026-10-21-BUSYBAR.md` 的新证据为准：

1. **挂件样式从四种收敛为两种**：删掉「旗标（Flag）」「信标（Beacon）」，只保留「铭牌（Tag）」
   「胶囊（Capsule）」（胶囊 30×136 DIP、铭牌 14×74 DIP）；旧设置里的 flag/halo/badge 一律归到铭牌。
   受影响的断言（`geometry`/`h-pages`/`h-controls` 的分段数与尺寸）已同步重算。
2. **忙碌条改单向循环**（原 `sin()` 往返），相位按时间算；`kSpinTimer` 增加自适应节流 +
   四个停顿探针（`busy tick gap`、`busy paint`（含 view+layout+draw 拆分）、`tray update`、`state apply`）。
3. **刷新章的同步时间戳**不再恒显占位符 `—`：助手输出 `mtime_text`、UNC 快照格式化
   `YYYY-MM-DD HH:MM:SS`、状态解析在 `mtime==0` 时回退 `list.files.config`；按钮宽度 92 → 60 DIP。
4. **收起不再重抓背景**（消除贴条/面板"闪一下/变色"），`DwmFlush` 一并移除；改在
   `ApplyDpiChange`/`RefreshTopology`/`DockNearest` 里 `InvalidateBackdrop`。另外把 UNC 探测/读取
   搬进 worker（原先在 UI 线程上跑 `FindFirstFileW("//wsl$/…")`，网关重启时会把消息循环连同忙碌条一起卡住）。

5. **铭牌（tag）补齐胶囊那套做法**（2026-10-21 同日第二轮，详见 `audit/ROUND-2026-10-21-TAG-GLASS.md`）：
   两样式共用同一份液态玻璃（桌面快照 1.06× 裁进形状 + 沿厚度六段明暗 + 贴边侧接触阴影 + 两道 1px
   渐变描边 + 长向单向受光）；铭牌形体升级为路径几何（圆角仍 4 DIP，**尺寸与命中区域不变**）；
   **内容排布统一为屏幕空间**（铭牌此前贴左/贴右会整块 180° 镜像：文字方向、三灯顺序、待重启
   琥珀条一起翻面），水平边「名字在左 → 三灯 → logo 在右」，竖直边「logo 在上 → 三灯 → 名字在下」；
   铭牌新增 11 DIP 品牌 logo。实测：胶囊同位置 **0/103360 px 变化**，铭牌 78.5% 像素变化。

6. **回退链「追加一条」的模型选择器**（v1.16，用户要求）：模型 id 右侧加 ▾，候选按
   「一键探测缓存 → 供应商条目自带的 models」取，选中只填文本框、**不发任何写请求**；
   两个下拉互斥（点另一个时一次点击完成关闭+打开）。门禁 `h-fallback` 断言
   「空缓存时给配置里的 models / 探测后缓存优先 / 选中后 modelValue 命中 / probe 调用次数不变」。

7. **所有配置写入统一交代「网关是否需要重启」**（v1.17，用户要求）：`IsConfigWrite` 的每个动作
   （切主模型 / 新建供应商 / 加模型 / 删供应商 / 回退链增删移 / 回滚备份）成功落盘后，按
   「自动重启网关」设置二选一——自动重启，或点亮可点的「重启网关」胶囊并用 toast 说明
   "需重启网关后机器人才用上"；写动作文案里"（网关热读生效）"的说法已删除。

**写盘链路实证**（2026-10-21）：把你真实 `config.yaml` 只读复制一份，用真实助手按面板实际 payload
把 7 条写路径（新建供应商 / 加模型 / 切主模型 / 追加回退 / 上移 / 移除 / 删除供应商）全部跑通并逐条读回
断言，副本最终还原到与基线一致；真实 `~/.hermes` 全程未被写入。证据：`tmp/hermes-panel-native/diag/verify-writes.py`。

8. **添加页收敛 + 供应商 ⋯「扫描并追加」**（v1.18，用户要求）：新建供应商不再要求模型与 Key 变量名
   （Key 变量名由助手推导；模型可空，写进配置后由 hermes 侧发现），删掉从未被读取的「保存前探测」
   死开关；「加模型」子页删除，改为模型页供应商 ⋯ → 只读探测 → 只把**新增**模型 `add-models` 写回，
   刷新后直接看到最新列表；无新增时只提示"已是最新"。同一轮还修了一个真 bug：模型药丸的命中框
   从未进入布局表（用了面板宽当药丸宽），导致真机上点掉药丸是空操作。

9. **「扫描并同步」（v1.19，用户反馈 s2api 列表没变）**：v1.18 只会追加，配置里已下线的模型
   不会消失。经用户授权，助手 `add-models` 新增可选 `prune:true`（不传时行为完全不变）：用探测结果
   替换该供应商的 `models:` 列表，新增的写入、已下线的删除，写前自动备份；面板 ⋯ 改为「扫描并同步」，
   探测为空时绝不 prune。实证：真实助手 + 真实配置副本，6 个模型同步成 4 个、二次同步不写盘、
   `dry_run` 不落盘、空列表被拒（`tmp/hermes-panel-native/diag/verify-prune.py`）。

10. **健康复查（v1.20）**：修掉一个既有的助手 bug——列表形态的 `models:` 元素缩进比字段深时，
    追加会写出挂错层级的 `- id`（写后校验挡住落盘，但老面板「加模型」会失败）；现在沿用已有元素
    缩进。面板侧的 ⋯ 探测缓存也改成只在成功且有结果时更新。

11. **输入框插入符 + 模型列表（v1.21，用户要求）**：所有表单输入框都画插入符（此前只有搜索框有）；
    新建供应商的模型区改成可滚动列表（探测列出全部模型并默认全选、表头全选/全不选、点行切换、
    滚轮 + 滑块），手动添加改单行回车并入；删除供应商页按钮文案改为「删除供应商」。
    门禁：`h-add` 四条新断言 + `h-controls` 的 caret 断言。

13. **原生输入框的三处收尾（v1.24，真机反馈）**：下拉展开时隐藏原生字段（否则列表被输入框盖住、
    点不到选项）；新建供应商页去掉「模型 id」输入框，模型只来自探测列表；忙碌条改独立子窗口绘制
    （主面板一帧 200ms → 子窗口 0.05ms/帧、60fps），门禁断言"忙碌期间持续重绘且单帧 <8ms"。

12. **输入框换成原生 EDIT（v1.23，用户要求「我要原生输入框」）**：面板主体是普通窗口，可以挂真正的
    `EDIT` 子控件；14 个单行字段（搜索/添加/回退链/设置）改为系统控件 + 子类化，光标定位、拖选、
    双击选词、I 形指针、右键菜单、IME、横向滚动全部由系统提供；自绘层只保留外框/标签/聚焦描边，
    文本经 `EN_CHANGE` 实时镜像回既有模型，业务逻辑零改动。过程中修掉自己引入的重绘自激
    （空闲 125fps → 8ms/s）与"自定义窗口类无编辑行为"两个问题。

14. **一键刷新同步 + 回退链只选不填 + 点击穿透（v1.25，真机反馈）**：模型页加「刷新同步」
    （只读探测全部供应商 + 差异写回，确认卡 + 自动备份）；⋯ 菜单说明行删除；回退链表单只剩
    来源/模型两个下拉；修掉三处"按钮点穿到底层控件"的根因（命中顺序改为绘制层级：外壳优先、
    页面控件限内容区、折叠卡前置、搜索框按钮排前、滚动条让位）与一个空供应商列表导致的
    Debug 断言崩溃。

15. **对 v1.25 的自查修复（v1.26）**：一键同步完成后刷新并交代网关重启（原来既不刷新也不提示）；
    退出时把一键探测/一键同步的 worker 一并 detach（原来关面板时可能 terminate）；忙碌条覆盖同步；
    删除失真的 `dump.fallback.manual`；`h-models` 新增一键同步门禁用例。

16. **第二轮自查（v1.27）**：跨 DPI 重建原生字段字体；换主题后让输入框重绘；`WM_CTLCOLOREDIT`
    刷子按颜色缓存（原会泄漏 GDI 句柄）；同步中重复点击只提示；删除手填模式死代码；同步按钮补提示。

17. **性能（v1.28）**：给 `Surface::Paint` 加三段计时归因——绘制本体 5–16ms，历史"200ms/帧"实为
    helper 抢 CPU 的墙钟等待；状态签名未变时轮询不再整窗重绘，对勾只给手动刷新；
    `dump.paintFrames` 显示空闲 4 秒帧数不变（5→5），`motion` 新增对应断言。

18. **热路径优化（v1.29）**：文本测量缓存（实测命中 99.2%，4766/4803）；原生字段摆放按布局签名
    短路（省掉每帧 2–6ms）；控制面 `dump.measure` + `h-models` 断言把"缓存确实生效"变成证据。

19. **悬停热路径缓存（v1.30）**：`TrackHover` 复用缓存的布局（30 次悬停命中 29 次），
    绘制不再重复计算布局；删除两处死成员。

20. **修正 v1.28 的跳过条件（v1.31，自查发现）**：状态签名补齐 webui/网关可达性、网关启动时刻、
    config mtime、助手错误横幅——否则这些变化不会触发重绘（灯不刷新、错误横幅不出现）。

21. **悬停气泡反相配色（v1.32）**：气泡按页面明暗反相（浅页深底亮字、深页亮底深字）并加淡阴影，
    与页面明确区分；证据 `evidence/tooltip-default.png` / `tooltip-dark.png`。

交付产物：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`，SHA256
`b4587270c87fc1231a06580d6ca3192dbb53636e1db069223ef253b8729ae33a`（真机实例跑的是同一份字节的
`dist-live/HermesModelPanelNative.exe`，PID 52684；历史见 ROUND-2026-10-21-TAG-GLASS.md §10.3/§11/§12/§19–§24、CODE-HYGIENE.md §9）。
同日历史：`5cd2ce51…` = 第七轮（添加页收敛 + ⋯ 扫描并追加）、`bd5e533d…` = 第六轮（⋯ 菜单补门禁）、
`2107ce82…` = 第五轮（写盘实证 + 统一网关重启交代）、`411ae7f4…` = 第四轮（回退链模型选择器）、
`ad730ff7…` = 第三轮（一键探测 + 悬停提示链）、`4220a769…` = 第一轮（忙碌条/时间戳/收起不重抓）、
`0a617e90…` = 第二轮（铭牌玻璃化）；第三轮之前的自查修复（玻璃快照过期 → `EnsureBackdrop` 改按窗口矩形判定），见
`audit/ROUND-2026-10-21-TAG-GLASS.md` §6。

