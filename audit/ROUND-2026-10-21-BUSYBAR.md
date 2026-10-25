# 真机反馈整改轮：忙碌条方向 + 刷新章 "-" 与空白 + 卡顿定性（2026-10-21）

> 触发：真机反馈三条 ——（a）刷新按钮里 logo 右边的 `-` 是什么、logo 左边一大块空白控件是什么；
> （b）切换模型 / 重启网关时贴条与面板会闪烁、变色；（c）"进度忙碌条偶尔卡住不动一会，然后才开始动"，
> 且"现在是左右移动的"。
> 产物：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`（SHA256
> `4220a769e062bec454a5c0eb28905a5d74d0205d9448ba5f3d2fcb0ad4c9e0af`），已替换真机实例运行。

## 1. 三条反馈的定性

| 反馈 | 定性 | 处理 |
|---|---|---|
| logo 右边的 `-` | 不是破折号，是**同步时间戳的占位**：配置 mtime 读不到时显示 `—`。真机经 UNC 读 `config.yaml` 时 `health.config.mtime` 恒为 0（该字段只有 `list` 路径填），于是永远显示占位符 | 三层补齐：助手脚本输出 `mtime_text`；UNC 快照按 mtime 格式化 `YYYY-MM-DD HH:MM:SS`；状态解析在 `mtime==0` 时回退到 `list.files.config` 的 mtime。真机状态行出现真实时间（`↻ 01:16`） |
| logo 左边的空白控件 | 刷新按钮宽度伪影：`kSyncWidth` 92 DIP 里内容（图标 + 时间文本）右对齐、按钮左半是空的，但整块都吃 hover/点击 | `kSyncWidth` 92 → 60 DIP（`src/render/layout.cpp`），内容宽度与热区一致 |
| 贴条/面板闪烁变色 | 收起时重新抓取背景快照：`CaptureBackdrop` 要把已显示的贴条 hide → 抓屏 → 再 show，中间那一帧就是"闪一下/变个色"。曾在折叠路径加 `DwmFlush` 挡住它，但 `DwmFlush` 会阻塞消息循环，直接把 a-edge 门禁点死 | 折叠路径**不再重新抓取**；改为在真正会移动/换屏的地方 `InvalidateBackdrop`（`ApplyDpiChange`、`RefreshTopology` 两支、`DockNearest`）。浮动占位尺寸（8 DIP 宽的灰竖块）用显式 `SetWindowPos` 到 `WidgetSize` 修正 |
| 忙碌条"左右移动" | 旧实现用 `sin()` 驱动单块，本质是往返，不是进度 | 改**单向循环**：相位 `fmod(elapsed/1600ms,1)`，半个轨道宽的渐变块持续右移、越过右端由左端接续（两端淡出，接缝不可见）。相位按**时间**算，定时器被挤晚也只让步进变大，不会"停住再跳" |
| 忙碌条"偶尔卡住一会才开始动" | 根因是 **UI 线程被 9P I/O 挡住**：`StartRefresh` 原先在 UI 线程上跑 `FindFirstFileW("//wsl$/…/home/*")` + `TakeUncSnapshot`。网关重启期 WSL 正在起虚拟机，单次 9P 调用可以阻塞数百毫秒到数秒 → 消息循环停摆 → 进度条定在上一帧 | UNC 探测与读取整体搬进 worker（`src/app/application_data.cpp`）。收尾核对：UI 线程上已无 `CreateProcessW`/`FindFirstFileW`/9P 读取，唯一长阻塞是用户主动点的「启动 WSL」按钮（单次 wsl.exe ping，上限 20s） |

## 2. 真机验收证据

### 2.1 单向运动（真机像素带采样）

真机实例（Release，120 DPI，面板 (882,0) 480×723），用 `HERMES_DISABLE_UNC=1` 把刷新逼上
**只读**的 helper 路径以获得 400–500 ms 的忙碌窗口，`tmp/hermes-panel-native/diag/busy-sample.ps1`
在窗口顶部横幅带逐帧找强调色块的左右边界：

```
episode 1: left 51 → 87 → 122 → 152
episode 2: left 34 → 68 → 122 → 157
episode 3: left 34 → 68 → 104 → 139      （全部单调右移，无反向）
```

证据：`tmp/hermes-panel-native/evidence/busy-live2.txt`（121 帧基线 + 上述 12 帧忙碌帧）。
速率 35 px / ~120 ms ≈ 292 px/s，与 480 px 轨道 / 1600 ms 周期（300 px/s）一致。
对照：改前同一手法测到的是右边界往返（Debug 控制实例：224 → 464 → 回落）。

> 说明：`HERMES_DISABLE_UNC=1` 只让面板走 helper 读路径（`list`/`health`，只读），不改 `config.yaml`、
> 不碰网关；采样后已用无该变量的方式重启实例，真机日志恢复 `refresh from UNC cache (config unchanged)`。

### 2.2 无停顿（应用自带探针）

Fix 后新增四个停顿探针：`busy tick gap`（kSpinTimer 两拍间隔 >200 ms）、`busy paint`
（一帧 >25 ms，附 view/layout/draw 拆分）、`tray update`（>80 ms）、`state apply`（>80 ms）。

真机日志（`%LOCALAPPDATA%\HermesModelPanelNative\logs\app.log`）在上述三段忙碌窗口期间：

```
grep -cE "busy paint|busy tick gap" app.log   →  0
```

即：没有一帧超过 25 ms，也没有一次 UI 线程停顿超过 200 ms。Debug 下测到的 172–194 ms/帧是
Debug 渲染开销（另加一台 Android 模拟器抢 CPU），Release 不成立；控制在 `kSpinTimer` 里的
自适应节流（paint > 20 ms 时按 2×paint 重绘，夹在 40–400 ms）仍保留，真机如出现慢帧会自动降频，
且因为相位按时间算，降频只让步进变大。

### 2.3 门禁

| 项 | 结果 |
|---|---|
| `tools/e2e.ps1 -Gate All -Configuration Debug`（最终产物，07:52） | **30 套件 100% 通过（exit 0）**，其中 5 项环境 SKIP：`drag`/`a-pullout`/`a-edge`/`a-crossdpi`/`h-ime`。证据 `tmp/hermes-panel-native/evidence/20261021-075232-958-All.txt`、`tmp/hermes-panel-native/gate-all-final.log` |
| 同轮里真实指针场景的 PASS | `a-snap` 29.38 s、`a-hover` 149.17 s（此前一轮 07:2x 里 `drag`/`a-pullout` 也是 PASS，见 `gate-all-after-busyfix.log`） |
| 环境 SKIP 项的单跑复验（同一最终二进制） | `a-crossdpi` 全 20 轮 PASS ×4（27.4–27.6 s）；`a-snap` PASS 28.56 s / 29.34 s；`a-pullout` PASS 19.41 s / 19.25 s；`a-edge` PASS 28.56 s；`drag` PASS |
| 残留 | 复验窗口内 `a-pullout` 出现过**一次**未复现的硬失败（其后 14 次尝试 0 复现，窗口内 PASS/SKIP 交替）；归到 `BLOCKER-005` 的观察范围，继续按环境抖动对待，但未当成 PASS |

## 3. 本轮工具改动（AUDIT-PLAN §12.5）

- `tests/a_tests.cpp`（A5 跨屏拖拽）：补上与 A2/A3 同源的**落点漂移判定**。应用记下的是真实光标
  （`GetCursorPos`），光标被外部挪走时读到的就是别人的坐标：
  - 实测 #5 失败时 `lastDrop.valid=false`（拖拽开始即失效，`CommitDrop` 从未运行）或
    `cursor=(1891,1163)` 而请求终点是 `(853,345)`（同一日志时段还伴随 `drag-commit cursor=(1891,1163)
    final=(-1176,1147) dpi=192`，即拖拽根本没落到目标显示器）——这一类里 DPI 断言要测的东西不存在；
  - 判据：`lastDrop.valid==false` 或落点与请求终点差 >2 px → 抛
    `the recorded drop point is not the requested end point (…); the desktop is in use`，由
    `IsEnvironmentInterference` 归为 SKIP(77)。
  - **不是放宽产品断言**：落点正确时 DPI/尺寸/单次写入/单调采样四条断言照旧硬判。
- `tests/a_tests.cpp`（A2/A3 之外的 hover/展开路径）：光标漂移与 capture 被抢的判定沿用既有守卫。
- 说明：`computer-use` 的 UIA 通道**看不到**本面板的窗口（自绘分层 tool window，
  `get_app_state(pid=…)` 返回 `Windows UI Automation returned no tree for pid …`，`list_apps` 里也没有它），
  因此真机点击改用与门禁同一机制的系统级 `SendInput`（`tmp/…/diag/click.ps1`，含落点校验）。
  这是输入通道的能力边界，不是产品缺陷。

## 4. 残留

- 真机忙碌窗口时长：UNC 命中路径 ~50 ms（用户正常轮询几乎看不到忙碌条），helper 路径 400–500 ms，
  切模型 / 重启网关时才明显可见。用户若在切模型时仍觉得"慢"，先看上述两个探针的日志行再定性。
- 桌面存在外部指针干扰源（见 `BLOCKERS.md` BLOCKER-005）：本轮 `-Gate All` 的 `a-snap`/`a-edge`/`h-ime`
  等真实指针场景按 SKIP 记录，不当作 PASS。
