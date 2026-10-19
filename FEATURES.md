# 新功能开发文档：诊断 / 延迟 / 用量 / 档案 / 通知

> 2026-10-17 定稿。五个功能全做，按 F1→F6 切片推进，每片独立可验证、独立门禁、独立 PROGRESS 记录。
> 本文档是唯一的实施依据；与 GOAL/DESIGN/PLAN 冲突时，以那三份的硬规则为准（尤其：**不修改
> WSL 里的 hermes-panel-apply.py**、**不写真实 hermes 配置**、**日志不落 Key/密码/token**）。

---

## 0. 总览

| 编号 | 功能 | 一句话 | 预估 |
|---|---|---|---|
| F1 | 只读助手脚本 `hermes-panel-read.py` | 三个只读命令：diagnose / usage / probe，作为 F2/F3/F4 的数据源 | 1 片 |
| F2 | 诊断面板 | 网关单元状态 + 日志尾部 + 各通道连接状态，一眼看到"机器人为什么不动" | 1 片 |
| F3 | 用量统计 | InsightsEngine 的结构化 JSON → 面板内今日/近 7 天 token 与按模型分解 | 1 片 |
| F4 | 延迟探测 | 定时探针经网关测延迟，列表行显示徽章 + 近 N 次可用率 | 1–2 片 |
| F5 | 配置档案 | 命名档案（主模型+回退链+收藏）一键整套切换 | 1–2 片 |
| F6 | 系统通知 | 灯变红 / 助手出错 / 待重启 首次出现时发 Windows 托盘气泡 | 1 片 |

**依赖顺序**：F1 → F2 → F3 → F4 → F5 → F6。F2/F3/F4 共用 F1 的只读脚本；F5/F6 独立。
**不引入**：WebView2、新写入者、真实 WSL 写入（全部走只读脚本 + 既有 apply 脚本的既有命令）。

---

## 1. F1：只读助手脚本 `tools/hermes-panel-read.py`

### 1.1 定位

- 与 `hermes-panel/wsl/hermes-panel-apply.py` **并列**的**只读**脚本（apply 是唯一写入者，本脚本
  **永不写 config/.env**，只读 + 发网络探针）。
- 协议与 apply 相同：stdin 读一行 JSON `{"cmd":"..."}`，stdout 回一行 JSON `{"ok":true,"data":{...}}`。
- 放在 Windows 侧 `projects/hermes-panel-native/tools/`，WSL 通过 `/mnt/e/...` 执行（与 apply 同机制）。
- 新增设置项 `readHelperPath`（默认
  `/mnt/e/AI/Zcode/projects/hermes-panel-native/tools/hermes-panel-read.py`），设置页"连接"区
  只读展示（不提供编辑框，避免误改）。

### 1.2 命令 `diagnose`

返回结构化诊断（全部只读）：

```json
{
  "ok": true,
  "data": {
    "gateway": {
      "unit": "hermes-gateway",
      "active_state": "active",            // systemctl --user show -p ActiveState
      "active_since": "2026-10-17 01:57:37",
      "main_pid": 324,
      "listening": true                    // 127.0.0.1:8642 有 LISTEN
    },
    "channels": [
      {"name": "weixin", "connected": true, "last_activity": "2026-10-17 00:42:34",
       "last_event": "Sending response (3907 chars) to o9cq...", "stale_seconds": 72000},
      {"name": "qqbot",  "connected": true, "last_activity": "2026-10-17 01:31:47",
       "last_event": "Session resumed", "stale_seconds": 0}
    ],
    "logs": {
      "gateway_tail": ["...最近 40 行，已按 KEY= 掩码..."],
      "errors_tail":  ["...最近 20 行..."]
    },
    "config": {"mtime": 1750000480, "mtime_text": "2026-10-17 14:34:40", "size": 24910}
  }
}
```

实现要点：

1. **通道状态**：解析 `~/.hermes/logs/gateway.log` 尾部，按 `gateway.platforms.<name>` 聚合
   最近一条含 `Connected/Disconnected/inbound/Sending response/rate limited` 的行；
   `connected` = 最近一条连接事件是 Connected 且其后无 Disconnected。
   `weixin` 的"失联检测"：`sync.json` mtime 持续更新但 `context-tokens.json` 不动 —— 两个
   mtime 都返回，面板侧判 `stale`（这是 2026-10-17 那次机器人故障的判据，原样产品化）。
2. **日志脱敏**：脚本内对 `API_KEY=...`/`token=...`/`Bearer ...` 做正则替换为 `***`；
   门禁断言输出里不含已知夹具 Key。
3. **超时**：每条命令 ≤ 5 s；systemctl 用 `--no-pager show -p ... --value`。

### 1.3 命令 `usage`

```json
{"cmd":"usage","days":7}
→ {"ok":true,"data":{
  "generated_at": 1792240332,
  "overview": {"total_sessions":23,"total_messages":679,"input_tokens":12009189,
               "output_tokens":512345,"total_tokens":12521534,"avg_msgs_session":29.5},
  "models": [{"model":"deepseek-v4.1-flash","sessions":20,"input_tokens":...,"output_tokens":...}, ...],
  "platforms": [{"platform":"cli","sessions":18,...}],
  "activity": {"by_day":[{"day":"Mon","count":20},...]}
}}
```

实现：`sys.path.insert(0,'~/.hermes/hermes-agent')` → `from hermes_state import SessionDB` +
`from agent.insights import InsightsEngine` → `SessionDB(read_only=True)` → `engine.generate(days,
source=None)`（已实测返回 dict）→ 只挑面板要的字段，剔除 session 内容与 tool 明细。
**只读打开**（`read_only=True`），不写库。超时 15 s（1 GB 库的聚合）。

### 1.4 命令 `probe`

```json
{"cmd":"probe","provider":"custom:doulor","model":"workbuddy-glm-5.3-flash"}
→ {"ok":true,"data":{"ms":312,"status":200,"model":"...","error":""}}
```

实现：从 `~/.hermes/.env` 读 `API_SERVER_KEY`（**不打印**），`POST http://127.0.0.1:8642/v1/chat/
completions`，messages=`[{"role":"user","content":"ping"}]`，`max_tokens=1`，`timeout=20`；
记录 `elapsed_ms` 与 HTTP 状态；失败返回 `ok:false, error:"<简短原因>"`（不含响应体全文）。
**只在 WSL 内发**（Windows 侧访问不到 WSL 的 127.0.0.1）。

### 1.5 门禁 `read-helper`（新 ctest）

- 假数据夹具：`FAKE_HERMES_HOME` 指向临时目录（放伪造的 gateway.log / .env / 两个 mtime 文件）
  → `diagnose` 断言：通道解析正确、`stale_seconds` 计算正确、日志尾部含掩码后的 `***` 且不含
  夹具 Key 原文；
- `usage`：跳过（需要真实 1 GB 库；在真机由用户确认，不在门禁）；
- `probe`：用 `FAKE_READ_PROBE_URL` 指向 tests 内的 loopback 假服务器（复用 harness/fake_server.h
  的模式）→ 断言 ms 字段与失败路径。
- 全部命令：断言 stdout 是**单行合法 JSON**、退出码 0、超时 < 5 s。

---

## 2. F2：诊断面板

### 2.1 交互

- **入口**：设置页"其他"区新增一行 `诊断` + ghost 按钮 `打开`（不占底部导航，避免 5 个 tab 的
  布局与断言连锁改动）。
- **呈现**：全局面板部件 `ModalHost` 大卡（标题"诊断"），内容分区：
  1. **网关**：单元状态（active/failed）+ 启动时间 + 端口监听；
  2. **通道**：每通道一行：名称 · 连接灯（绿/黄/红）· 最后活动（相对时间）· 最近事件（省略号）；
     `stale` 时在该行显示"长时间无消息（可能失联）"的警示文案；
  3. **日志**：`gateway_tail` 与 `errors_tail` 各一个可滚动区域（等宽字体、行内已掩码）；
  4. 底部按钮：`刷新`（重新拉取 diagnose）+ `关闭`。
- **数据流**：打开时经 `HelperBridge` 调 `readHelperPath diagnose`（与 apply 相同的 wsl.exe 通路，
  独立可执行文件路径）；失败显示错误行与重试按钮。

### 2.2 布局与渲染

- 模态宽度沿用 `min(340, width-2*pad)`；高度 = 窗口内容区 - 上下留白，内部两段滚动
  （日志用现有 `ListBox` 视口做法，只画可见行）。
- 渲染新增：`DiagnosticView`（纯视图结构体）→ `panel.cpp` 新函数 `DiagnosticCard()`；
  颜色只用既有 token（灯色复用 `LampColour`，警示用 `palette.err`），六主题自动适配。

### 2.3 门禁 `h-diagnose`

- 假 read-helper（`--read-helper` 夹具参数注入一个返回固定 JSON 的假脚本）→ 断言：
  打开入口存在、模态标题正确、通道行数与连接灯状态、日志区显示掩码文本、刷新按钮后请求计数 +1、
  关闭后模态消失、六主题下渲染无崩溃（截图证据）。
- 回归：`Pages`/`Regression` 全绿；`h-controls` 的控件可达性不变。

---

## 3. F3：用量统计

### 3.1 交互

- **入口**：设置页"其他"区同一行 `用量` + ghost 按钮 `查看`（与诊断入口并列）。
- **模态**：标题"用量统计"，内容：
  - 顶部：`今日 / 近 7 天 / 近 30 天` 三段（复用 SegRow，bordered）；
  - 概览：会话数 · 消息数 · 输入/输出 token · 平均每会话消息数；
  - **按模型**：列表（模型名 + token 数 + 占比条），按 token 降序取前 8；
  - 底部：生成时间 + `刷新`。
- 数据：`usage --days N`；缓存 60 s（同一模态内切段不重复请求，跨段用后端结果切分不可行——
  days 由脚本按参数算，切换段重新请求一次）。

### 3.2 门禁 `h-usage`

- 假 read-helper 返回固定 usage JSON → 断言：三段切换后的请求 days 参数（1/7/30）、
  概览数字、按模型前 8 排序、占比条宽度与 token 比例一致（纯 view model 断言 + 截图）。
- 数据为空（`empty:true`）时显示"暂无会话数据"。

---

## 4. F4：延迟探测

### 4.1 设计

- **探针对象**：当前主模型 + 全部 ★ 收藏模型（去重，上限 8 个，避免刷爆）。
- **频率**：面板侧计时器，默认 5 分钟；设置页"行为"区新增开关 `延迟探测` + 间隔段
  （5/15/60 分钟，默认关）；**只在面板可见且 WSL 绿灯时探测**。
- **执行**：串行（一次一个 probe，间隔 200 ms），经 `readHelperPath probe`（WSL 内发请求）。
- **存储**：面板数据目录 `latency.json`：`{"<provider>|<model>": [{"t": unix, "ms": 312,
  "ok": true}, ...]}`，每条保留最近 20 次。
- **展示**：
  - 模型列表行尾：收藏行 + 当前行显示 `~312ms`（等宽小字，颜色按阈值：<1s 绿 / <3s 黄 /
    >=3s 或失败 红）；
  - hero 当前模型：延迟徽章（最近一次结果 + 近 5 次可用率）；
  - 悬停列表行显示最近 5 次明细气泡。

### 4.2 门禁 `h-latency`

- 假 read-helper（probe 返回固定 ms / 失败交替）→ 断言：单选/收藏去重、上限 8、串行顺序、
  `latency.json` 轮转（21 次后保留 20）、阈值分档颜色、开关关闭时零请求。
- 渲染：截图证明徽章在行尾不挤压模型名（`h-controls` 的点击可达性不变）。

### 4.3 风险

- 探针会计费（1 token × 8 模型 × 每 5 分钟）——**默认关闭**，开启时在设置页文案注明；
- 网关不可达时探针全失败——不弹通知（见 F6 的抑制规则），只在徽章显示红色。

---

## 5. F5：配置档案

### 5.1 存储

- 面板数据目录 `profiles.json`：`{"version":1,"profiles":[{"name":"日常","main":{"provider":
  "custom:doulor","model":"workbuddy-glm-5.3-flash"},"fallbacks":[{"provider":"...","model":"..."}],
  "favorites":["..."],"updated":1792240332}]}`。
- 档案**只存引用与摘要**，不存任何 Key；`favorites` 来自面板设置。

### 5.2 交互

- **入口**：设置页"行为"区新增一行 `配置档案` + ghost 按钮 `管理`。
- **管理模态**：列表（名称 + 主模型 + 回退链条数）+ 三个动作：
  - `保存当前`：读当前状态（list 响应）生成/覆盖同名档案；
  - `应用`：二次确认后依次经 apply 脚本写：`set-main` → `fallback` 增删到目标集合 →
    完成后 toast + 触发刷新；**每一步都走既有命令**，串行、失败即停并在模态里标出错步；
  - `删除`：危险确认。
- **应用语义**：档案应用是**写操作**，遵守既有规则：写前自动备份（apply 脚本已有）、不写 .env、
  失败可回滚（备份页）。应用后如果 `自动重启网关` 关，则置待重启（复用昨天的胶囊逻辑）。

### 5.3 门禁 `h-profiles`

- 假助手夹具：保存当前 → 档案文件生成且字段正确；应用另一个档案 → 断言 set-main + fallback
  序列请求与顺序、toast、待重启胶囊；中途失败（假助手第 2 步返回错误）→ 停止且模态显示错误步；
  删除流程走危险确认。

---

## 6. F6：系统通知

### 6.1 触发规则（只在**边沿**触发，不重复轰炸）

| 事件 | 条件 | 通知 |
|---|---|---|
| 网关灯变红 | `gateway` 从 ok→err（且 WSL 仍在） | "网关不可达"（含端口） |
| WebUI 灯变红 | `webui` ok→err | "WebUI 不可达（端口 x）" |
| 助手出错 | `helperError` 从空→非空 | "助手出错：<首行 120 字>" |
| 待重启出现 | `pendingGatewayRestart` false→true | "已切换模型，网关待重启" |
| 刷新连续失败 | 连续 3 次刷新失败 | "连续刷新失败，请检查 WSL" |

**抑制**：
- 面板隐藏/托盘时也发（这正是价值）；
- 同事件 10 分钟内不重复（内存表 `lastNotified[event]=tick`）；
- **探针失败不发通知**（F4 的日常失败不打扰）；
- 面板正在前台且可见时，灯红已有界面反馈——仍发（用户可能没看），但 10 分钟抑制兜底。

### 6.2 实现

- `Shell_NotifyIcon(NIM_MODIFY, NIF_INFO)` 托盘气泡（Windows 10/11 会转成系统通知）；
- 图标沿用现有托盘图标；点击气泡打开面板（`NIN_BALLOONUSERCLICK` → `Show()`）；
- 文案与既有 toast 同源（避免两套措辞）；日志只记事件名不记内容。

### 6.3 门禁 `tray-notify`

- 假助手让 WebUI 先 ok 后 err → 断言托盘通知计数 +1（控制面暴露 `notifications` 计数与最后文本）；
- 10 分钟内再次 err → 计数不变；恢复 ok 再 err → 计数 +1（边沿）；
- 隐藏面板时仍计数；点击回调路径（控制面直接调 `TrayMessage(NIN_BALLOONUSERCLICK)`）→
  `panelVisible` 变 true。

---

## 7. 切片与里程碑

| 片 | 内容 | 门禁 | 依赖 |
|---|---|---|---|
| F1 | read-helper 脚本 + 设置项 + read-helper 门禁 | `read-helper` | — |
| F2 | 诊断模态 + 设置页入口 | `h-diagnose` + Pages | F1 |
| F3 | 用量模态 | `h-usage` | F1 |
| F4 | 延迟探测（开关/计时器/历史/徽章） | `h-latency` | F1 |
| F5 | 配置档案（存储/管理/应用） | `h-profiles` | — |
| F6 | 托盘通知（边沿/抑制/点击） | `tray-notify` + tray | — |

每片完成即：全量门禁（Unit/Widget/Lifecycle/Shell/Pages/Regression/Release）+ PROGRESS 记录 +
真机重启。**任一门禁不绿先查根因，禁止补丁式修复**（GOAL 规程）。

---

## 8. 与既有不变量的关系

- **INV-H1…H6**（DESIGN §5）：新组件只用既有 token 与控件原语，不引入拟态/新阴影体系；
- **唯一写入者**：read-helper **只读**；档案应用走既有 apply 命令；不新增写入路径；
- **数据隔离**：全部门禁用假助手/假 read-helper/隔离数据目录；真机写入只发生在用户操作时；
- **日志脱敏**：read-helper 输出前掩码；面板日志只记事件名；
- **任意时刻可构建**：每片独立落地；F1 先落地（脚本 + 设置项 + 门禁）再动 UI 层。

---

## 9. 风险与对策

| 风险 | 触发信号 | 对策 |
|---|---|---|
| InsightsEngine 接口漂移 | usage 命令报 import/字段错 | 脚本内做 `data.get()` 防御 + 面板显示"数据源不可用"；`read-helper` 门禁用假数据，不锁死 hermes 内部 |
| state.db 1 GB 聚合慢 | usage 超时 | 脚本超时 15 s + 面板显示"统计中"；可加 `--days` 上限 |
| 探针计费 | 用户账单 | 默认关闭 + 文案注明 + 上限 8 模型 |
| 档案应用半途失败 | 第 N 步报错 | 失败即停 + 模态标出错步 + 备份页可回滚（apply 每步都有备份） |
| 通知轰炸 | 用户反馈 | 10 分钟抑制 + 探针失败不通知 |
| 6 页导航挤压 | — | 诊断/用量/档案都走设置页入口 + 模态，不动底部导航 |

---

## 10. 待用户确认（不阻塞开发）

1. 延迟探测的默认间隔（草案：5 分钟，默认关闭）；
2. 用量统计是否需要费用估算（当前 insights 的 pricing 数据为 "Unknown"，只能显示 token）；
3. 配置档案的“套用是否自动重启网关”（草案：沿用 `自动重启网关` 开关）。
