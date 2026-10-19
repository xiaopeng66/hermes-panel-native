# 审查报告（阶段 2）

审查对象：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`
（SHA256 `b113b415d5cdbb3cb8ead1a2f0a307f94ccadb4e213be6b23a819db30ce14216`，943,616 B）。
审查方法：静态代码审查 + 现有门禁复跑 + 控制面行为探针 + 真实助手（只读/隔离夹具）契约实测 + 六主题截图目检。
逐项记录见 `audit/REVIEW-NOTES.md`；截图见 `audit/evidence/`；门禁证据见 `tmp/hermes-panel-native/evidence/`。

## 汇总

| 维度 | 检查项 | 通过 | 不通过 | 不适用 |
|---|---|---|---|---|
| A 功能正确性 | 12 | 9 | 3（A3/A4/A12） | 0 |
| B 界面与视觉 | 7 | 7 | 0 | 0 |
| C 交互与手感 | 8 | 7 | 1（C4） | 0 |
| D 流畅性与动画 | 7 | 7 | 0 | 0 |
| E 性能与资源 | 6 | 6 | 0 | 0 |
| F 稳定性与异常 | 8 | 6 | 2（F3 P3、F4 P2） | 0 |
| G 数据与持久化 | 5 | 5 | 0 | 0 |
| H 安全与隐私 | 6 | 4 | 2（H3 部分、H4） | 0 |
| I 可访问性与文案 | 6 | 3 | 3（I5/I6 与 I3 部分） | 0 |
| J 与旧版差异 | 4 | 3 | 1（J2） | 0 |

> “检查项”按 `AUDIT-PLAN.md` §5 的编号；A/B/… 每项可能含多个子断言。凡“通过”都在
> `REVIEW-NOTES.md` 里带命令/截图/门禁证据。

## findings（按严重度排序）

### F-001 [P1][A3] 回退链上移/下移对真实助手完全无效
- 现象：点回退链任意一行的 ↑/↓，界面无变化、无错误提示；真实助手返回协议错误，面板把它当“成功但顺序没变”（只刷新一次状态）。
- 复现：
  1) 隔离 `hermes_home` 放 2 条回退（Beta/Gamma）；
  2) 面板发 `{"cmd":"fallback-move","index":0,"delta":1}`（`src/app/application_pages.cpp:765-780`）；
  3) 真实助手 `cmd_fallback_move` 只读 `to`（`projects/hermes-panel/wsl/hermes-panel-apply.py:1237-1239`）→ `int(None)` → `TypeError`。
- 证据：
  - 真实助手（隔离夹具）实测：`{"ok": false, "error": "TypeError: int() argument ... NoneType"}`；
  - 同一夹具改发 `{"index":0,"to":1}` → `{"ok": true, ... "moved": true}`；
  - 旧版渲染器发的是 `W.fallbackMove({index:i, to})`（`projects/hermes-panel/renderer/app.js:974`）。
- 根因假设：原生侧把“位移”当成协议字段；假助手也按 `delta` 实现，导致 `h-fallback` 门禁自证（夹具与实现同错）。
- 影响面：回退链排序（主流程）在真机完全不可用；P1。
- 建议方向：发 `to = clamp(index ± 1)`；假助手改为只接受真助手契约（`to`），并在门禁里断言 `to`。

### F-002 [P1][A4] 删除供应商的两个勾选项在真实助手上失效
- 现象：勾“一并移除回退链里对它的引用”后删除仍报“回退链里还有 1 条引用它”；勾“同时清除 .env 里的 API Key”后 `.env` 里的 key 仍在。
- 复现：
  1) 隔离夹具：Beta 被回退链引用、`BETA_KEY` 在 `.env`；
  2) 面板发 `{"cmd":"delete-provider","provider":"custom:Beta","purge_env":true,"drop_fallbacks":true}`（`src/app/application_pages.cpp:420-421`）；
  3) 真实助手只读 `purge_key` / `remove_fallbacks`（`hermes-panel-apply.py:1094-1130`）。
- 证据：
  - 用面板参数名：`{"ok": false, "error": "回退链里还有 1 条引用它（第 1 条）..."}`；
  - 用助手参数名：`{"ok": true, ... "purged_key": "commented", "removed_fallbacks": 1}`，`.env` 中 `#BETA_KEY=...`；
  - 旧版 `main.js:1150` 的白名单是 `['name','purge_key','remove_fallbacks','force']`。
- 根因假设：与 F-001 同类——原生侧自造字段名，假助手（`tools/fake_hermes.cpp:409`）也跟着实现，门禁因此长期假绿。
- 影响面：删除供应商（写操作 + 密钥清理）在真机上部分不可用；P1。
- 建议方向：改用 `purge_key`/`remove_fallbacks`；假助手改为严格契约并在未知字段时报错，门禁断言请求字段名。

### F-003 [P1][H2/A12] `previewDiff`（写入前看 diff）对 4 个写命令不生效，且会**直接落盘**
- 现象：开启“写入前先看 diff”后，删除供应商 / 移除回退 / 移动回退 / 恢复备份**不会**弹预览卡；其中删除与移除会当场写盘（没有二次确认的机会）。用户以为“只是预览”。
- 复现：
  1) 打开设置页“写入前先看 diff”；
  2) 在回退链点 ×→确认（或删除供应商）；
  3) 面板给请求注入 `dry_run:true`（`application_data.cpp:233`），但真实助手只对 `set-main/upsert-provider/add-models/fallback-add` 实现 dry_run（`hermes-panel-apply.py` 对应函数），`fallback-remove`/`fallback-move`/`delete-provider`/`restore` 忽略该字段直接写。
- 证据：
  - 真实助手（隔离夹具）`{"cmd":"fallback-remove","index":1,"dry_run":true}` → 实际删除，返回 `"dry_run": false`；
  - `{"cmd":"delete-provider",...,"dry_run":true}` → 实际删除；
  - 原生侧只根据 `data.dry_run` 判断是否预览（`application_data.cpp:302`），真助手返回 false → 走“写入完成”分支。
- 根因假设：`IsWriteRequest` 把“所有非只读命令”都当成可预演，但助手契约只支持其中 4 个。
- 影响面：数据安全（用户明确要求“只看不写”时仍写盘）；P1。
- 建议方向：预览白名单 = 助手真正支持 dry_run 的 4 个命令；其余命令不注入 dry_run（由既有的危险确认模态承担确认）；若已请求 dry_run 而回复没有 `dry_run:true`，必须显式提示“该操作不支持预览，已直接写入”。

### F-004 [P2][A6/J] “关闭按钮：退出程序”是死设置
- 现象：设置页把关闭按钮设为“退出程序”后，点标题栏 × 仍然只是隐藏到托盘；进程继续运行。
- 复现：设置页 → 关闭按钮 → 退出程序；点 ×。
- 证据：行为探针 `behavior_probe2` → `A6 closeAction select writes quit` PASS、`A6 close button honours closeAction=quit` **FAIL（alive=True）**；
  代码 `application_pages.cpp:927` 固定走 `ToggleVisibility`；旧版 `main.js:1385` 在 `closeAction==='quit'` 时 `app.quit()`。
- 根因假设：关闭按钮只做了“隐藏/显示”动作，没有读取 `closeAction`。
- 影响面：设置项无效、用户无法用 × 退出；P2。
- 建议方向：`InvokeControl(Close)` 在 `closeAction=="quit"` 时持久化并退出（走 `MenuCommand::Quit`），否则隐藏。

### F-005 [P2][A10/J] 点模态遮罩会关闭模态（旧版不关、设计文档也写“点遮罩不关”）
- 现象：危险确认/诊断/档案等模态，点卡片外的暗色区域会直接关闭（等价取消）。
- 复现：备份页点“回滚”打开确认 → 点面板内但模态外的空白 → 模态消失。
- 证据：`behavior_probe2` → `A10 mask click keeps the modal` **FAIL**；
  `application_pages.cpp:147-186`：任何落在模态外的点击都会落到 `pending_=Pending::None`；
  DESIGN §4.8 #32 与 v0.4 修订记录均写“点遮罩不关”；旧版 `confirmBox` 没有遮罩点击处理（`renderer/app.js:94-119`）。
- 根因假设：`HandleModalClick` 用“非确认按钮 = 取消”的简化语义，未区分遮罩与取消按钮。
- 影响面：误触导致操作被取消（不丢数据，但与文档/旧版不一致）；P2。
- 建议方向：只有取消按钮（`ModalCancel`）或 Esc 才关闭；遮罩点击返回“已处理但不关闭”。

### F-006 [P2][A4/J] “加模型 → 从供应商探测填充”只显示药丸，不填模型 id 输入框
- 现象：探测成功（3 个模型）但“模型 id”多行框仍为空；点“追加”报“填个模型 id”。加模型页的药丸点了是“移除”，不是“填入”。
- 复现：添加 → 加模型 → 选供应商 → 从供应商探测填充。
- 证据：`behavior_probe2` → `modelsText=""`、`chips=['probe-a','probe-b','probe-c']`；
  旧版 `renderer/app.js:924-927` 把探测结果直接写进 `#am-models` 文本域。
- 根因假设：`ApplyDataResult` 对 `probe` 一律写 `addChips_`（为新建供应商页设计），没有按子页分流。
- 影响面：加模型主路径多一步手工输入；P2。
- 建议方向：`addSub_==1` 时把 `models` 写进 `addModels_`（每行一个），药丸仅在新建页使用。

### F-007 [P2][A4/J] 新建供应商页缺“手动输入/编辑模型”多行字段
- 现象：新建供应商只能靠“探测”得到模型；探测失败/离线时无法手动填模型 id，也无法编辑探测结果。
- 复现：添加 → 新建供应商：只有 名称/Key 变量名/Base URL/API Key 四个字段。
- 证据：`behavior_probe2` → `fields=4`；DESIGN §4.5 #19 明确要求“模型 chips +「手动输入/编辑」切换到多行”；
  旧版 `renderer/index.html` 有 `#np-models` 文本域 + `btn-np-manual` 切换（`renderer/app.js:895-901`）。
- 根因假设：H5b 落地时把“模型”只做成探测药丸，漏了文本域与切换。
- 影响面：无法在无网络/探测失败时创建供应商；P2。
- 建议方向：新建页增加多行“模型（每行一个）”字段；探测结果写入该字段；药丸与字段同源（点药丸=从字段删一行）。

### F-008 [P2][H1/F6/A7] 托盘气泡通知永远不显示（`NIM_MODIFY` 用了错误的 hWnd）
- 现象：灯变红/助手出错/待重启时 `notificationCount_` 增加，但系统托盘没有气泡；门禁只断言计数，因此长期假绿。
- 复现：制造 WebUI ok→err（假助手）；观察 `dump.notifications.count` +1，系统无气泡。
- 证据：
  - `AddTray` 用 `data.hWnd=control_`（`application.cpp:245`），`Notify` 用 `data.hWnd=panel_`（`application_data.cpp:65`）；
  - Win32 契约实测（`verify_tray_hwnd.py`）：同一进程里 `NIM_ADD` 到 hwnd1 后，`NIM_MODIFY`/`NIM_DELETE` 用 hwnd2 均返回 **False**，用 hwnd1 返回 True。
- 根因假设：通知与图标注册用了不同窗口句柄；uID 相同也不够（图标由 hWnd+uID 定位）。
- 影响面：F6 通知功能 0 可用；P2。
- 建议方向：统一用 `control_`（与 `UpdateTrayStatus` 一致）；控制面暴露 `Shell_NotifyIcon` 的返回值供门禁断言。

### F-009 [P2][H4/F4] 延迟探测把模型名拼进原始 JSON（未转义）
- 现象：模型 id 含 `"` 或 `\` 时，请求变成非法 JSON，助手返回协议错；该模型的延迟永远探测失败（其它模型不受影响）。
- 复现：`application_data.cpp:609` 用 `"{\"cmd\":\"probe\",\"model\":\""+target.model+"\"}"` 拼串。
- 证据：代码审查；同类拼接只有这一处（`StartRefresh`/`StartReadHelper` 用常量）。
- 根因假设：写成字符串拼接，未走 nlohmann。
- 影响面：特殊字符模型名的延迟徽章永远红色；P2（不影响配置写入）。
- 建议方向：用 `Json{{"cmd","probe"},{"model",target.model}}.dump()`。

### F-010 [P2][I5/C4] 键盘无法完成关键动作（Tab 无焦点链、F5 不刷新）
- 现象：按 Tab 不在控件间移动（`search.focused` 保持 false、页面不变）；F5 无反应；切页只能用鼠标。
- 复现：面板前台按 Tab ×5。
- 证据：`misc_probe.py` → `I5 keyboard can switch pages` FAIL、`F5` 未产生 `list` 请求；
  DESIGN §4.9 #36 要求“Tab 顺序”；旧版靠浏览器原生 Tab 顺序可达按钮/输入框。
- 根因假设：自绘 UI 没有焦点链，只有 `/` 与表单字段的键盘路径。
- 影响面：可访问性（只用键盘的用户无法切页/刷新）；P2。
- 建议方向：实现最小焦点链（Tab/Shift+Tab 遍历当前页控件与字段，Enter 激活，可见焦点环）；F5/Ctrl+R 绑定刷新。

### F-011 [P2][A2/F] 轮询在飞时点模型行切换被静默丢弃
- 现象：自动刷新（20 s 周期）或手动刷新期间点模型行，切换不会发生，也没有 toast；日志里只有一行 `switch refused`。
- 复现：`StartSwitch` 用 `workerBusy_.exchange(true)`，失败直接 `return`（`application_data.cpp:483-486`）；同一时刻 `StartHelperAction` 是**排队**的。
- 证据：代码审查；对照旧版 `writeInFlight` 只限制写并发（最多 3 个），读与写互不阻塞（`main.js:204-212`）。
- 根因假设：切换沿用“单飞”读路径的忙碌标志，没有排队。
- 影响面：约 1/20 概率点空（轮询 1 次约 0.3–1 s，实际概率取决于 WSL 时延）；P2。
- 建议方向：把切换也放进 `pendingActions_` 队列（或在忙时记一个 `pendingSwitch_`，当前结果落地后执行）。

### F-020 [P2][F4/F8] 最小化后任何几何动作都会抛 INV1
- 现象：审计的 800 轮随机点击命中“最小化”按钮后，后续主题/挂件切换/托盘显示都触发
  `INV1: absolute geometry write/read mismatch`，日志出现 `[error]`（`Audit` 门禁随即失败）。
- 复现：面板前台 → 点标题栏最小化 → 切主题/挂件或托盘隐藏/显示。
- 证据：`tmp/hermes-panel-native/audit-data/logs/app.log`（07:22:48 一批 `target=(200,120,480,793)
  actual=(200,120,199,34)`）；`-Gate Audit` 首跑 153/160。
- 根因假设：最小化窗口的 `GetWindowRect` 是任务栏小矩形；`GeometryController::Place/Write` 与
  `Visibility` 都把它当稳定矩形处理。
- 影响面：最小化 + 任何几何动作 = 错误日志/不变量异常（控制窗口路径已 catch，不会崩，但状态错）。
- 建议方向：几何写入前 `IsIconic` → `SW_RESTORE`；`Visibility(Expanded)` 对最小化用 `SW_RESTORE`。
- 状态：**已在 FIX-020 修复**（`-Gate Audit` 复跑 160/160）。

### F-012 [P3][H3/A4/J] API Key 没有显隐按钮
- 现象：Key 输入框永远掩码；`addKeyRevealed_` 存在但从未被任何控件切换（`page.keyRevealed` 也没被渲染使用）。
- 复现：添加 → 新建供应商 → 在 API Key 里输入任意值；控件表 `controls` 中没有眼睛按钮（`_controlsOrder` 无 `keyReveal`）。
- 证据：`behavior_probe2` → 控件表无 eye；`render/panel.cpp:997` 只按 `masked` 画圆点；DESIGN #19 要求“掩码+眼睛”，旧版有 `#btn-np-eye`。
- 影响面：粘贴/核对长 key 时不方便；P3。
- 建议方向：字段右侧加眼睛按钮（`ControlId::KeyReveal`），点击切换 `addKeyRevealed_`。

### F-013 [P3][I6/B] 窗口标题是乱码
- 现象：窗口标题为 `Hermes j21W8B…62g7F`（源码里被写坏的非 ASCII 字节序列）。
- 复现：启动面板 → `GetWindowTextW(panelHwnd)`，得到 `Hermes j21W8B…62g7F`（源码字节已损坏）。
- 证据：`src/app/application.cpp:123,125`；hexdump `4c224865726d6573206a3231573842c2973632673746`；
  JSON dump 的 `instance` 用的是控制窗口标题（正常），所以既有门禁没发现。
- 影响面：WS_EX_TOOLWINDOW 不在任务栏显示，但 Alt-Tab/窗口枚举/辅助技术可见；P3。
- 建议方向：改成 `L"Hermes \u6A21\u578B\u9762\u677F"`（纯 ASCII 转义）。

### F-014 [P3][F3/A6] `config.yaml` 解析失败时没有可见警告，写操作也不停
- 现象：`status.config.parses=false` 时界面无提示，写按钮仍可点（DESIGN §5.1 要求“写操作全停 + 设置页醒目提示”）。
- 复现：删除/改名隔离数据目录里的 `settings.json`（不迁移旧设置）→ 启动 → 设置页 WSL 发行版显示 `Ubuntu`；旧版默认是 `Ubuntu-24.04`。
- 证据：`state.config.parses` 只被解析（`data/state.cpp:43`），全项目无第二处读取；
  真实助手对坏 config 会拒绝写入，因此不会写坏数据，但用户看不到原因。
- 影响面：可诊断性；P3。
- 建议方向：设置页 meta 行显示“config.yaml 解析失败，写操作已停用”，并在 `StartHelperAction`/`StartSwitch` 前置守卫拒绝写。

### F-015 [P3][A6/J] 默认 WSL 发行版与旧版不同
- 现象：原生默认 `Ubuntu`，旧版默认 `Ubuntu-24.04`；没有旧设置可迁移的全新机器上会连错发行版。
- 复现：假助手夹具放 `<home>/config_broken` → `health` 返回 `parses:false` → 设置页 meta 无提示，新建供应商保存仍会发 `upsert-provider`。
- 证据：`src/data/settings.h:23` vs `projects/hermes-panel/main.js:37`。
- 影响面：新装环境首次使用失败；P3（本机已迁移，实机不受影响）。
- 建议方向：默认值改为 `Ubuntu-24.04`（旧设置迁移逻辑保持不变）。

### F-016 [P3][A7] 异常灯体不可点击重试
- 现象：DESIGN §4.2 #4 要求“异常时灯体可点击重试”；灯是控件但 `InvokeControl` 不处理 `LampWsl/LampWebui/LampGateway`，点击无反应。
- 复现：制造网关不可达 → 点网关灯 → 无任何请求（灯是控件但 `InvokeControl` 没有 lamp 分支）。
- 证据：`application_pages.cpp:920-950`（无 lamp 分支）；旧版灯也不可点（`index.html:29-31` 无处理器）。
- 影响面：与设计文档不一致；P3。
- 建议方向：点击任一灯 → `RefreshData()`（与刷新按钮同一动作）。

### F-017 [P3][I1/A7] 网关启动时间缺失时托盘提示出现“网关自  未重启”
- 现象：`gateway.since` 为空（网关不可达/unit 信息缺失）时，tooltip 仍拼“有外部改动，网关自  未重启”，中间双空格。
- 复现：让假助手返回 `gateway.since=""` 且 config mtime 晚于 gateway since → 托盘 tooltip 出现 `网关自  未重启`（双空格）。
- 证据：`behavior_probe1` 输出 `'有外部改动，网关自  未重启'`；`application.cpp:274-285`。
- 影响面：文案瑕疵；P3。
- 建议方向：`since` 为空时改文案为“有外部改动，网关尚未重启”。

### F-018 [P3][J1] DESIGN 文档滞后于实现的挂件厚度
- 现象：DESIGN §2/§4.8 #34 仍写“8 DIP 细条”，实现是 10/14/16/30 DIP 的挂件（G11 用户已验收）。
- 复现：对照 `DESIGN.md` §2/§4.8 #34（写 8 DIP 细条）与实际挂件尺寸（10/14/16/30 DIP）。
- 证据：`DESIGN.md:18,133`；`render/theme.h`/`WidgetMetricsFor`。
- 影响面：规格与实现不一致，后续审查会反复报同一差异；P3。
- 建议方向：更新 DESIGN（先改设计文档，符合硬规则 9）。

### F-019 [P3][G1] 保存设置时丢弃未知键
- 现象：`Settings::ToJson` 只写白名单键；来自更新版本的未知键在保存后消失（G1 要求“不丢未知键”）。
- 复现：在隔离 `settings.json` 写入 `{"futureKey":{"nested":42}}` → 启动并触发一次保存 → 重新读取文件，`futureKey` 已消失。
- 证据：`data/settings.cpp:23-51`；`settings-tests` 未覆盖未知键。
- 影响面：向前兼容性；P3。
- 建议方向：加载时保留原始 JSON 的未知键，保存时合并写回。

## 通过项清单（证明覆盖，不只是“没找到问题”）

- **门禁**：Unit 17,432+32+21+22；Widget 570+60；Lifecycle 3；Pages 9/9；Shell 6/6；Regression 23/23；ReadHelper 27；Audit 160；Release（单 exe、无 DLL、能启动）。
- **功能**：五页全部控件真点（h-controls 91 个中心命中 + 逐控件点击）；模型页全路径；回退链移除/追加/逐条探测；添加页三子表单；备份列表/过滤/回滚；设置四段；托盘 21 项菜单；热键冲突提示；贴条 4 样式 × 4 边；模态/Toast；诊断/用量原地刷新；previewDiff（set-main）。
- **视觉**：42 张 6 主题页面截图 + 挂件 32 格对照板；审计 0 越界；78 条对比度断言。
- **性能**：冷启动 0.36 s；空闲 CPU 1.24–1.56%；内存/GDI/USER/句柄/线程平台期；500 供应商/5000 模型 5.2 s；30 分钟长稳。
- **稳定**：异常助手 5 类；坏配置 3 个文件；跨 DPI/多显示器/工作区变化；强杀重启；多实例。
- **数据/安全**：原子写；隔离参数（真实设置 mtime_ns 不变）；迁移只读一次；日志脱敏；API Key 不落日志；Release 无控制面。
- **旧版对照**：36 项逐项有结论；差异均登记（刻意 vs 遗漏）。

## 与基线的对比

| 指标 | 基线（审查前） | 说明 |
|---|---|---|
| 门禁 | 全 PASS | 本报告中的 P1/P2 均未被现有门禁覆盖（夹具与实现同错 / 只断言计数） |
| 冷启动 | 0.36–0.40 s | 本次未变 |
| 空闲 CPU | 1.24–1.56% | 本次未变 |
| 工作集 | 51.5 MB | 5000 模型时 57.7 MB；30 分钟平台期 58–61 MB |
| 打包 | 943,616 B | 单 exe |

## 未覆盖 / 无法覆盖的部分与原因

1. **真实供应商联网探测**：只对现有真实配置做过只读 `list`/`health`；未对真实供应商发写请求（硬约束）。
   延迟探测/逐条探测的“计费与网关真实延迟”需用户真实使用确认。
2. **真实 WSL 写路径的 UI 全链路**：F-001/F-002/F-003 用**真实助手脚本 + 隔离 hermes_home** 验证了协议契约，
   但没有在真实 `~/.hermes` 上执行写（硬约束）；修复后的端到端写入由假助手门禁覆盖，真实写入留给用户。
3. **旧 Electron 面板的运行对照**：本会话未启动 Electron（避免与原生实例抢热键/置顶窗），J1/J2 以源码对照为准；
   J3 的旧版内存/冷启动沿用 PROGRESS H7 的历史实测并标注。
4. **系统托盘气泡的实际外观**：无头/远程会话无法保证通知中心可见；F-008 用 Win32 返回值契约证明“不会显示”，
   修复后用 `Shell_NotifyIcon` 返回值 + 控制面计数双断言，最终外观仍需用户真机确认。
5. **中文 IME 全输入法矩阵**：只覆盖系统默认拼音（`h-ime`）；第三方输入法（搜狗/微软五笔）未覆盖。
