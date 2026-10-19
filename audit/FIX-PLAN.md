# 修复文档（阶段 3）

> 每条 finding 一条 FIX。排序：P1 → P2（按影响面）→ P3。
> 原则：一次只改一个根因；每条都要能通过“该项门禁 + 受影响历史门禁”；违反 DESIGN 不变量的先改设计文档。
> 回滚方式默认：`git` 不可用（工作区非 git 仓库），回滚 = 还原本条列出的文件改动（每条已列文件）。

## 批次划分

| 批次 | 内容 | 批末门禁 |
|---|---|---|
| A | FIX-001/002/003（3 条 P1，同一类根因：原生侧自造助手字段名 / 误判 dry_run 能力） | Unit + Pages + Regression |
| B | FIX-004/005/006/008/009（关闭语义、遮罩、加模型填充、托盘气泡、JSON 注入） | Unit + Pages + Shell + Regression |
| C | FIX-007/010/011/012（手动模型输入、Tab 焦点链、切换排队、灯可点） | Unit + Pages + Regression |
| D | FIX-013…019（P3：标题、默认发行版、解析失败提示、tooltip 文案、设计文档、未知键） | Unit + Pages + Audit + Release |

---

### FIX-001 ← F-001（P1）回退链上移/下移发错字段
- 根因（已证实）：面板发 `delta`，真实助手只认 `to`；假助手按 `delta` 实现，门禁因此假绿。
- 方案：`Application::FallbackPageClick` 里发 `to = index + delta`（边界已由 `row.row` 判断）；假助手改为只接受 `to`（缺 `to` 时按真实助手的行为返回 `ok:false` 与 `TypeError` 风格错误），并把 `delta` 视为未知字段忽略。
- 涉及文件：`src/app/application_pages.cpp`、`tools/fake_hermes.cpp`、`tests/h_fallback_tests.cpp`。
- 测试：`h-fallback` 断言请求携带 `index` 与 `to`（不再断言 `delta`）；真实助手（隔离夹具）手工复核。
- 门禁：`tools/e2e.ps1 -Gate Pages`、`-Gate Regression`；预期全 PASS。
- 回滚：还原上述三文件。
- 用户可感知：回退链 ↑/↓ 真正改变顺序。
- 违反 DESIGN 不变量：否（INV-H2 是“照旧契约”，本修复正是回到旧契约）。

### FIX-002 ← F-002（P1）删除供应商勾选项字段名错
- 根因（已证实）：面板发 `purge_env` / `drop_fallbacks`，真实助手读 `purge_key` / `remove_fallbacks`。
- 方案：改为真实字段名；假助手只接受真实字段名（出现 `purge_env`/`drop_fallbacks` 时返回 `ok:false`，让门禁能抓住同类回归）。
- 涉及文件：`src/app/application_pages.cpp`、`tools/fake_hermes.cpp`、`tests/h_pages_tests.cpp`。
- 测试：`h-add` 断言删除请求带 `purge_key`/`remove_fallbacks`；勾选项的真实语义用隔离夹具复核。
- 门禁：Pages + Regression。
- 回滚：还原上述三文件。
- 用户可感知：勾“一并移除回退引用”真的能删；“清除 .env key”真的清。
- 违反 DESIGN 不变量：否。

### FIX-003 ← F-003（P1）previewDiff 对不支持的写命令直接落盘
- 根因（已证实）：`IsWriteRequest()` 把 9 个写命令都当可预演；真实助手只对 `set-main` / `upsert-provider` / `add-models` / `fallback-add` 实现 `dry_run`。
- 方案：
  1) 新增 `SupportsDryRun(cmd)` 白名单；只有这 4 个命令注入 `dry_run:true`；
  2) 其余写命令不注入（它们各自已有危险确认模态：删除/回退移除/恢复；移动为低风险）；
  3) 防御：若请求里带了 `dry_run:true` 而助手回复 `ok:true` 且 `data.dry_run != true`，记 warn 并 toast「该操作不支持预览，已直接写入」，避免未来助手变更时静默落盘。
- 涉及文件：`src/app/application_data.cpp`、`src/app/application.h`（DataResult 增加 `previewIgnored`）、`tools/fake_hermes.cpp`（不支持的写命令忽略 dry_run，与真实助手一致）、`tests/h_fallback_tests.cpp` / `tests/h_pages_tests.cpp`。
- 测试：把原“↓ 触发预览”的断言改为“追加回退（fallback-add）触发预览”；新增“移除回退即使开着 previewDiff 也不发 dry_run”的断言。
- 门禁：Pages + Regression。
- 回滚：还原上述文件；DESIGN §4.8 #32 与旧版语义不变。
- 用户可感知：开启“写入前看 diff”后，只有真正支持预览的写操作会弹预览；其余按原有确认流程执行。
- 违反 DESIGN 不变量：否（INV-H2 助手契约优先；DESIGN §6 的 previewDiff 描述本就以旧版 set-main 为准）。

### FIX-004 ← F-004（P2）“关闭按钮：退出程序”未生效
- 根因：`InvokeControl(Close)` 固定走 `ToggleVisibility`，从不读 `closeAction`。
- 方案：`closeAction=="quit"` → `PersistSettings(true)` + `PostMessageW(control_,WM_CLOSE)`（与托盘“退出”同路径）；否则隐藏。
- 涉及文件：`src/app/application_pages.cpp`、`tests/h_shell_tests.cpp`（或 `h_pages`）。
- 测试：新增场景：设置 closeAction=quit → 点 × → 进程在 2 s 内退出；closeAction=hide → 进程存活且 `panelVisible=false`。
- 门禁：Pages + Shell + Regression。
- 回滚：还原两文件。
- 用户可感知：设置页的“退出程序”真的退出。
- 违反 DESIGN 不变量：否。

### FIX-005 ← F-005（P2）点遮罩关闭模态
- 根因：`HandleModalClick` 把“没点中确定”一律当取消。
- 方案：显式区分 `ModalConfirm` / `ModalCancel` / 其它：只有取消与 Esc 关闭；其它点击吞掉但不关闭（ProfileMenu 的行点击保留）。
- 涉及文件：`src/app/application_pages.cpp`、`tests/h_pages_tests.cpp`（新增遮罩点击断言）。
- 测试：打开回滚确认 → 点面板空白 → 模态仍在；点取消 → 关闭；Esc → 关闭。
- 门禁：Pages + Regression。
- 回滚：还原两文件。
- 用户可感知：危险确认不再被误触关闭。
- 违反 DESIGN 不变量：否（回到 DESIGN §4.8 #32 与旧版行为）。

### FIX-006 ← F-006（P2）加模型探测不填输入框
- 根因：`ApplyDataResult` 对所有 `probe` 结果写 `addChips_`，未按子页分流。
- 方案：`addSub_==1` 时把探测到的模型按行写进 `addModels_`；其它子页维持药丸逻辑。
- 涉及文件：`src/app/application_data.cpp`、`tests/h_pages_tests.cpp`。
- 测试：加模型页探测后 `modelsText` 包含探测结果；追加请求携带这些模型。
- 门禁：Pages + Regression。
- 回滚：还原两文件。
- 用户可感知：“从供应商探测填充”真的填好可编辑的列表。
- 违反 DESIGN 不变量：否。

### FIX-007 ← F-007（P2）新建供应商缺手动模型输入
- 根因：H5b 只落地了“探测药丸”，漏了旧版的多行模型字段。
- 方案：新增 `addNewModels_` 多行字段（第 5 个字段，默认收起→由“手动输入 / 编辑”按钮展开？旧版是切换；为降低布局风险，直接常显多行字段，标题“模型（每行一个）”）；
  探测结果写入该字段；药丸由字段行派生（点药丸=删该行）；保存时以字段行为 `models`。
- 涉及文件：`src/app/application.h`、`src/app/application.cpp`（视图）、`src/app/application_pages.cpp`（点击/提交）、`src/app/panel_view.h/.cpp`、`src/render/layout.cpp`、`src/render/panel.cpp`、`tests/h_pages_tests.cpp`。
- 测试：手动输入两行 → 保存请求 `models` 两行；探测 → 字段填好；点药丸 → 对应行被删。
- 门禁：Pages + Widget + Regression（布局审计必须 0 问题）。
- 回滚：还原上述文件。
- 用户可感知：探测失败也能建供应商，探测结果可编辑。
- 违反 DESIGN 不变量：否。

### FIX-008 ← F-008（P2）托盘气泡句柄不匹配
- 根因：图标注册在 `control_`，通知用 `panel_`；Win32 要求 hWnd+uID 一致（实测不一致返回 FALSE）。
- 方案：`Notify` 改用 `control_`；把 `Shell_NotifyIconW` 的返回值记到 `lastNotifyOk_`，控制面 `notifications.lastOk` 暴露；门禁同时断言计数与返回值。
- 涉及文件：`src/app/application_data.cpp`、`src/app/application.h`、`src/app/application_control.cpp`、`tests/h_pages_tests.cpp`（F6 断言）。
- 测试：WebUI ok→err → 计数 +1 且 `lastOk=true`；10 分钟抑制仍生效。
- 门禁：Pages + Shell + Regression。
- 回滚：还原上述文件。
- 用户可感知：灯变红/助手出错/待重启时真的弹托盘气泡。
- 违反 DESIGN 不变量：否。

### FIX-009 ← F-009（P2）延迟探测的 JSON 拼接
- 根因：`"{\"cmd\":\"probe\",\"model\":\""+target.model+"\"}"` 未转义。
- 方案：改用 `Json{{"cmd","probe"},{"model",target.model}}.dump()`。
- 涉及文件：`src/app/application_data.cpp`。
- 测试：新增 `hermes-state`/`h-models`? 延迟探测的请求构造是私有的；在 `h-models` 用含 `"` 的模型名 + 控制面 `latency-probe`，断言请求是合法 JSON 且 `model` 原样到达。
- 门禁：Pages + Regression。
- 回滚：还原该文件。
- 用户可感知：特殊字符模型名的延迟徽章不再永远失败。
- 违反 DESIGN 不变量：否。

### FIX-010 ← F-011（P2）切换在忙时被丢弃
- 根因：`StartSwitch` 用单飞标志，忙则 return；其它写动作是排队。
- 方案：忙时把目标存入 `queuedSwitch_`（provider/model）；当前结果落地后（`ApplyDataResult` 末尾与早退分支统一调用 `StartQueuedSwitch()`）执行；同一目标合并。
- 涉及文件：`src/app/application.h`、`src/app/application_data.cpp`、`tests/h_models_tests.cpp`、`tools/fake_hermes.cpp`（新增 `<home>/list_slow` 让 list 慢 1.5 s，便于稳定复现）。
- 测试：poll 在飞时点行 → 最终发出 `set-main` 且 hero 跟随。
- 门禁：Pages + Regression。
- 回滚：还原上述文件。
- 用户可感知：刷新期间点模型不再“点空”。
- 违反 DESIGN 不变量：否。

### FIX-011 ← F-010（P2）Tab 焦点链 / F5 刷新
- 根因：自绘 UI 没有焦点链；只有 `/` 与字段编辑。
- 方案：新增 `focusId_/focusIndex_` 与“当前页可聚焦控件序”（来自布局控件表，过滤 None/禁用）；Tab/Shift+Tab 移动，Enter 激活（复用 `InvokeControl` / 页面点击入口），Esc 清除；绘制 accent 焦点环；F5 与 Ctrl+R 触发 `RefreshData()`。
- 涉及文件：`src/app/application.h`、`src/app/application_input.cpp`、`src/app/application.cpp`、`src/app/panel_view.h/.cpp`、`src/render/panel.cpp`、`tests/h_controls_tests.cpp`。
- 测试：Tab 后 `dump.focusId` 变化且有焦点环；Enter 激活当前项（例如切页）；F5 产生 `list` 请求。
- 门禁：Pages + Regression。
- 回滚：还原上述文件。
- 用户可感知：能只用键盘切页/刷新/操作。
- 违反 DESIGN 不变量：否（补齐 DESIGN §4.9 #36）。

### FIX-012 ← F-016（P3）异常灯体不可点
- 根因（已证实）：`InvokeControl` 没有 lamp 分支，灯只是装饰控件。
- 方案：`InvokeControl` 增加三个 lamp → `RefreshData()`；tooltip 已存在。
- 涉及文件：`src/app/application_pages.cpp`。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 测试：`h-controls` 增“点网关灯触发 list”的断言。
- 门禁：Pages + Regression。
- 用户可感知：灯异常时点灯=重试。

### FIX-013 ← F-012（P3）API Key 显隐按钮
- 根因（已证实）：`addKeyRevealed_` 与 `page.keyRevealed` 存在但从未接线；绘制层只认 `masked`。
- 方案：新增 `ControlId::KeyReveal`，位置在 Key 字段右内侧（30×30）；点击切换 `addKeyRevealed_`；渲染 `masked && !keyRevealed`，按钮图标 eye/eye-off。
- 涉及文件：`src/app/panel_view.h`（ControlId/ControlName）、`src/render/layout.cpp`、`src/render/panel.cpp`、`src/app/application_pages.cpp`、`src/render/icons.h/.cpp`（若无 eye 图标则加）。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 测试：`h-add` 断言控件存在、点击后 `keyRevealed=true` 且字段显示明文。
- 门禁：Pages + Widget + Regression。

### FIX-014 ← F-013（P3）窗口标题乱码
- 根因（已证实）：窗口标题的 Unicode 转义在早期改名脚本里被写坏成非 ASCII 字节。
- 方案：`L"Hermes \u6A21\u578B\u9762\u677F"`。
- 涉及文件：`src/app/application.cpp`。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 测试：`dump` 增加 `windowTitle`（可选）或用 `GetWindowTextW` 外部探针；门禁断言等于“Hermes 模型面板”。
- 门禁：Shell + Regression。

### FIX-015 ← F-014（P3）默认发行版
- 根因（已证实）：原生 `Settings.distro` 默认值写成 `Ubuntu`，与旧版 `SETTING_DEFAULTS.distro` 不一致。
- 方案：`Settings.distro` 默认 `Ubuntu-24.04`（与旧版一致）；迁移逻辑不变。
- 涉及文件：`src/data/settings.h`、`tests/settings_tests.cpp`（若有默认断言）。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 门禁：Unit + Regression。

### FIX-016 ← F-015（P3）config 解析失败无提示、写不停
- 根因（已证实）：`state.config.parses` 只被解析、从未被读取；写路径没有前置守卫。
- 方案：`StartHelperAction` / `StartSwitch` 前置守卫：`hermesState_.config.parses==false` 且是写命令 → toast「config.yaml 解析失败，写操作已停用」并 return；设置页 meta 追加醒目提示。
- 涉及文件：`src/app/application_data.cpp`、`src/app/application.cpp`、`tests/h_pages_tests.cpp`、`tools/fake_hermes.cpp`（`<home>/config_broken` → health 的 `parses=false`）。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 门禁：Pages + Regression。

### FIX-017 ← F-017（P3）tooltip 双空格
- 根因（已证实）：`since` 为空时仍走“网关自 <since> 未重启”的拼接。
- 方案：`gateway.since` 为空时用「有外部改动，网关尚未重启」。
- 涉及文件：`src/app/application.cpp`。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 门禁：Unit（无直接单测）+ Shell。

### FIX-018 ← F-018（P3）DESIGN 文档滞后
- 根因（已证实）：G11 改挂件尺寸时只更新了 PROGRESS，没有回写 DESIGN。
- 方案：更新 DESIGN §2 与 §4.8 #34 的挂件厚度描述（10/14/16/30 DIP，G11 已验收），并在修订记录加 v1.5 一行。
- 涉及文件：`DESIGN.md`（先改文档，符合硬规则 9）。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 门禁：Audit（无代码影响）。

### FIX-019 ← F-019（P3）未知设置键丢失
- 根因（已证实）：`Settings::ToJson` 只输出白名单键，保存时丢弃未知键。
- 方案：`SettingsStore` 在 `Load` 时保留原始 JSON 中未识别的键（`extras_`），`ToJson` 输出前合并；已知键覆盖。
- 涉及文件：`src/data/settings.h/.cpp`、`tests/settings_tests.cpp`。
- 回滚：还原上述文件（工作区非 git 仓库）。
- 测试：写入含 `futureKey` 的设置文件 → Load/Save → 文件中仍有 `futureKey`。
- 门禁：Unit + Regression。

---

## 不修 / 延后（P3，附理由与建议）

| Finding | 决定 | 理由 | 建议 |
|---|---|---|---|
| F-018 文档滞后 | **本轮修** | 硬规则 9 要求先改设计文档 | 已列入 FIX-018 |
| 其它 P3 | 全部本轮修 | 见上表 | — |

## 验收映射

- 每条 FIX 完成后：Debug/Release 构建 → 该 finding 的门禁 → 受影响历史门禁 → `PROGRESS.md` 记 Gxx。
- 批次末：`-Gate Regression`。
- 全部完成后：`-Gate Regression` + `-Gate Audit` + `-Gate Release`，打包并在真机重启。


### FIX-020 ← F-020（P2）最小化后几何写入抛 INV1
- 根因（已证实）：最小化窗口的 `GetWindowRect` 是任务栏小矩形（实测 199×34），
  `GeometryController::Place/Write` 与 `Visibility` 未先恢复窗口。
- 方案：`Place` 前 `IsIconic(panel_)` → `SW_RESTORE`；`Visibility(Expanded/Hidden 之外)` 同样先恢复。
- 涉及文件：`src/core/geometry_controller.cpp`。
- 测试：`-Gate Audit` 的 800 轮随机点击（会命中最小化）+ 既有 `dpi`/`h-controls` 回归。
- 门禁：Audit + Regression；预期 160/160、23/23。
- 回滚：还原该文件。
- 用户可感知：最小化后再操作面板，不会留下错误日志/异常几何。
- 违反 DESIGN 不变量：否（这是让 INV1 重新成立）。


### FIX-021 ← F-021（P1）系统/安装器请求关闭时必须优雅退出
- 根因（已证实）：`WM_QUERYENDSESSION/WM_ENDSESSION` 只发给普通顶层窗口（控制窗是
  message-only），面板/贴条窗口过程未处理 → 同意询问后不退出 → RestartManager 超时强杀。
- 方案：`Application::WindowMessage` 增加两个 case——`WM_QUERYENDSESSION`（面板窗口：
  `PersistSettings(true)` + 返回 TRUE）、`WM_ENDSESSION(wParam=TRUE)`（日志 + 
  `PostMessageW(control_, WM_CLOSE)` → 走正常退出路径：销毁窗口 → 消息循环退出 →
  保存设置 → 移除托盘）。DESIGN.md v1.6 先记录契约。
- 涉及文件：`src/app/application.cpp`、`DESIGN.md`、`tests/lifecycle_tests.cpp`（回归）。
- 测试：lifecycle 新用例（按 PID 过滤找面板窗口，发两个会话消息，断言 5s 内 exit 0）；
  压测 S5 同场景。
- 门禁：Lifecycle（4/4，证据 `20261019-121529-241-Lifecycle.txt`）+ Regression
  （23/23，`20261019-121539-936-Regression.txt`）+ Audit（160/160，
  `20261019-121813-225-Audit.txt`）+ Release（`20261019-130606-323-Release.txt`）。
- 回滚：还原上述两个源文件。
- 用户可感知：安装器/关机时面板优雅退出（保存设置、托盘图标正常移除），不再被强杀。
- 违反 DESIGN 不变量：否（新增外壳契约，DESIGN v1.6 先行记录）。
