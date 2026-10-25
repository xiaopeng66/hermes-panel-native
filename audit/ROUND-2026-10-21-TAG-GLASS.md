# 真机整改轮：铭牌补齐胶囊那套（方案 3 + 1）——2026-10-21

> 触发：用户问"铭牌挂件能不能优化"，我给了 5 个方案（玻璃化 / 加厚 / 屏幕空间排布 / 信息位 / 交互反馈），
> 用户选定 **先做 3（内容改屏幕空间排布 + 加 logo）与 1（同一套液态玻璃）**，不实施 2/4/5。
> 产物：`dist/HermesModelPanelNative/HermesModelPanelNative.exe`，SHA256
> `0a617e906c070c01525e3765c510b3aa6495e2256c0c92636cfba071a9ca4570`（已替换真机实例运行）。

## 1. 设计先行（仓库规则：违反/改动设计契约先改 DESIGN.md）

`DESIGN.md` 加 **v1.10（2026-10-21）** 条目，并同步两处已过期的挂件描述：

- §2 架构树与 §4.8 #34：`10/14/16/30 DIP（beacon/tag/flag/capsule）` → **`14/30 DIP（tag/capsule）`**
  （旗标/信标按用户要求删除是上一轮的事，文档这次一并补上）。
- v1.10 契约：①两样式同一份液态玻璃；②铭牌形体升级为路径几何（圆角仍 4 DIP，**尺寸与命中区域不变**）；
  ③**内容排布统一为屏幕空间**（取代 v1.8 "整条挂件是同一个刚体"里关于 logo/灯/文字的那半条）——
  竖直边恒为「logo 在上 → 三灯 网关/WebUI/WSL 从上到下 → 模型名在下、从下往上读」，
  水平边恒为「名字在左 → 三灯 从左到右 → logo 在右」；④待重启琥珀条改画在**屏幕空间起点端**
  （竖直边=顶边、水平边=左边）并裁进轮廓；⑤身份色轨仍留贴边侧（这条必须随形体）。

改前就存在的**不一致**（本轮修掉）：铭牌贴左/贴右会整块 180° 镜像——文字方向、三灯顺序、琥珀条
一起翻面；胶囊不会（它的内容一直是屏幕空间）。

## 2. 实现（`src/render/panel.cpp` / `panel.h`）

- `PanelRenderer::Widget`：形状构造抽成两样式共用一条几何（胶囊=按 `capsuleDocked` 插值的路径，
  铭牌=`CreateRoundedRectangleGeometry`），随后**同一段玻璃管线**：桌面快照 1.06× 裁进形状 →
  沿厚度六段明暗烘焙 → 贴边侧接触阴影 → 两道 1px 渐变描边 → 长向单向受光。
- 铭牌专属装饰（身份色轨 + 琥珀条）在**屏幕空间**、用变换后的形状做 layer 遮罩，所以
  圆角处不再探出，且琥珀条不再随形体翻到另一端。
- 新增 `PanelRenderer::WidgetContent`（两样式共用，屏幕空间）：尺寸档只有 logo（铭牌 10.5 DIP /
  胶囊 14 DIP）与灯点半径（2.2 / 3.4 DIP）不同，排布规则完全一致；`LampDot` 从原 lambda 提成成员函数。
- 删除挂件那条已经走不到的"通用旋转文字"路径（两样式都在屏幕空间画完内容），顺带清掉未使用的
  `worst` / `fill` / `glow` 三个死 lambda。

## 3. 证据

### 3.1 前后对照图（隔离实例 + 控制面，同一台机器同一主题）

`tmp/hermes-panel-native/evidence/`：

| 文件 | 内容 |
|---|---|
| `tag-before-porcelain-edge{1,2,3,4}.png` / `tag-after-…` | 铭牌四边 × 白瓷主题，前后对照（4× 放大） |
| `tag-before-dark-edge{1,2,3,4}.png` / `tag-after-…` | 同上，深色主题 |
| `capsule-before-porcelain-edge1.png` / `capsule-after-…` / `capsule-final-…` | 胶囊对照（用户当前样式） |
| `widget-tag-porcelain-edge2-painted.png` / `-diff.png` | 干净位置（anchor 150 DIP）重拍的铭牌 + 与"隐藏后"的差值图 |
| `edge2-context.png` | 抓图现场：右边缘那块第三方悬浮窗压在测试点上的样子 |

**定量**（`tmp/…/diag/png-diff.py`，阈值：单像素三通道和差 >24 才算变化）：

```
capsule-before-porcelain-edge1 -> capsule-final-porcelain-edge1 : 0 / 103360 px changed, max delta 27 (≈0.03/px)
capsule-before-dark-edge1      -> capsule-final-dark-edge1      : 48 / 103360 px changed (0.05%)
tag-before-porcelain-edge1     -> tag-after-porcelain-edge1     : 21024 / 26784 px changed (78.5%)
tag-after-porcelain-edge1      -> tag-final-porcelain-edge1     : 0 / 26784 px changed   ← 两次同代码渲染确定
```

即：**胶囊逐像素不变**（那 48 px 是灯点光晕在变化的玻璃背景上的抗锯齿），**铭牌整个换材质换排布**。
结构报告（`diag/widget-structure.py`）也一致：胶囊的 rail/accent/text 像素数与包围盒前后完全相同。

### 3.2 排查过程中踩到并定性的一个假象（值得记下来）

右边缘（edge2）前后两张图都显示"铭牌上半截没画"：屏幕上有桌面、没有板子，看着像渲染 bug。
排查路径（全部只读）：

1. 与"应用隐藏后"的差值图（`diag/widget-diff.py`）→ 上半截只有 ~1 px/行 的差异 → 那块像素被**别的东西盖住**；
2. 临时给 `Surface::PaintStrip` 加一行 DIB 像素轨迹（环境变量 `HERMES_DIAG_WIDGET` 指向文件），
   逐帧打印中轴像素 → **DIB 本身完全正确**（y≈12 是 logo、y≈24/32/40 是三个灯、y≈56 起是模型名）；
   排查完已删除该临时代码（可用同一手法复现：见本文件末"复现配方"）；
3. `WindowFromPoint` 直接点名：盖住测试点的是**第三方悬浮窗 `HRNETFLOWTRAY`（pid 17828，23×60 at 2537,629）**。
   把 anchor 从 300 DIP 挪到 150 DIP（干净位置）后，铭牌四边轮廓都完整（`widget-tag-porcelain-edge2-diff.png`
   的轮廓是 0..92 全高）。

结论：不是渲染缺陷，是**测试点被无关置顶窗口压住**——这类观察今后先做"隐藏后差值"再下判断。

### 3.3 门禁

| 项 | 结果 |
|---|---|
| `-Gate Regression`（23 套件） | **21 通过**、1 SKIP（`dpi`，真实指针环境）、2 失败：`drag`（恢复高度，见下）与 `settings`（**过期断言**，已修） |
| `settings` 断言 | 该断言钉的是"未知样式有确定归宿"，期望值仍写着已删除的 `beacon`。按 DESIGN v1.10 ① 改成 `capsule`，并在测试里写明改期望值的理由（不是为了让测试变绿而放宽判据） |
| `settings / widget / geometry / motion / shell / tray` 单跑 | 6/6 全绿 |
| 几何契约未动 | `WidgetSize`（铭牌 14×74、胶囊 30×136）与 `WidgetMetricsFor`、命中区域（`HitStripShape`）一行未改 → geometry/widget/h-pages/h-controls 全绿 |
| `drag` 的"恢复高度"失败 | 归环境：同一时刻外部自动化正在顶光标（`SetCursorPos` 三个点 250 ms 后全被顶回 (1313,59)，无裁剪、无按键、无 capture），拖拽跟随真实光标所以恢复高度差 >2 px；随后 9 次尝试全部因 `MoveTo` 失败直接 SKIP，0 次复现。登记在 `BLOCKERS.md` BLOCKER-005 |

## 4. 真机

打包后替换真机实例（pid 60664 起），日志正常（`wsl ping exit 0` / `providers=18 fallbacks=3`），
`live-capsule-top.png` 是新构建在真机上的样子（用户当前用胶囊：名字在左、三灯居中、logo 在右——与
DESIGN 的水平边契约一致）。

**用户要看的铭牌**：设置页 →（挂件样式）切到「铭牌」，或把 `settings.json` 的 `widgetStyle` 改成 `tag`。
我没有替用户动这个偏好设置。

## 5. 复现配方（诊断工具都在 `tmp/hermes-panel-native/diag/`）

```
# 1) 起隔离实例（假助手 + 独立数据目录；Debug 才有控制面）
powershell -File diag/launch-shot-instance.ps1 -Instance shot-tag
# 2) 切换样式/贴边/主题并抓图（4× 放大）
powershell -File diag/widget-shot.ps1 -Instance shot-tag -Style tag -Edge 2 -Theme porcelain -Out <png>
# 3) 轮廓/结构/像素差
python diag/widget-diff.py 2 tag porcelain      # 与"隐藏后"的差值 + 每行占用图
python diag/widget-structure.py <png> <png>     # rail/accent/lamp/text 的像素数与包围盒
python diag/png-diff.py <a.png> <b.png>         # 变化像素数与最大通道差
# 4) 测试点被谁压住
powershell -File diag/who-covers.ps1 -X 2545 -Y 665
# 5) 光标是否被外力顶住（BLOCKER-005 的判据）
powershell -File diag/cursor-hold.ps1
```

## 6. 自查：我的改动有没有引入新 bug（2026-10-21，用户要求）

方法：① 逐 hunk 读完所有未提交改动（23 个文件、约 800 行新增）；② 针对本轮改动的风险类别做定向
排查（worker 线程共享状态、残留诊断代码、死代码、位图缓存键）；③ 对两处**有状态缓存**（玻璃快照、
抓取次数）做运行时实验；④ 修完复跑门禁 + 重拍像素对照。

### 6.1 找到并修掉一个真 bug：玻璃快照会过期（P3，视觉）

`Surface::EnsureBackdrop` 原来只判断"快照是不是空的"，于是**任何改动挂件几何但没显式
`InvalidateBackdrop()` 的路径都会继续用旧快照**（快照被拉伸着画进新形状）。可复现路径：设置页换挂件
样式 → `SetSetting("widgetStyle")` → `geometry_.Place`（窗口 30→14 DIP）+ `RenderStrip`，全程没有失效。

实测（`diag/backdrop-stale.py`，隔离实例）：

```
A: place(胶囊) -> configure(tag) -> 抓图        与 B 比：10146/26784 px 不同（37.9%，最大通道差 83）
B: place(tag)                    -> 抓图（参考）
```

修法（根因，不是补一处调用）：`EnsureBackdrop` 改成**按窗口当前矩形判定**——位置或尺寸与快照不符就
重抓；快照的矩形在 `CaptureBackdrop` 里落账。这样"谁动了窗口"不再需要每个调用点记得失效。

修后复测：**A vs B = 0 px（最大通道差 10）**；并且**抓取次数与旧逻辑逐项相同**（`diag/backdrop-captures.py`：
`hide+show` +2、切样式 +3/+3、换边 +1 —— 旧逻辑跑同一序列数字一模一样），也就是说这次修复
**没有引入任何多余的抓屏**（抓屏要短暂隐藏窗口，多余抓屏就是用户之前报的"闪一下"）。

### 6.2 顺手清掉的死代码（不是 bug，但会误导后人）

- 挂件动画定时器的残留：`kWidgetTimer` 常量、WM_TIMER 里那段永远走不到的分支、
  `widgetPhase_` 成员、`view.widgetPhase` 字段，以及已成空操作的 `UpdateWidgetTimer()`（3 处调用）。
  信标样式被删后它就不再被武装，留着只会让后来者以为挂件还有动画。
- `panel_view.h` 里 `widgetStyle` 的默认值仍写着 `"beacon"` → 改成 `"capsule"`（设置层会覆盖它，
  但默认值应当自洽）。`geometry/settings` 两处的未知值回落规则不受影响。

### 6.3 查过、确认没问题的（逐条）

| 风险点 | 结论 | 依据 |
|---|---|---|
| 刷新搬进 worker 后的数据竞争 | 无 | `uncCache*` 五个成员只在 worker 里读写（全仓 grep 只出现在 `application_data.cpp`）；worker 只用捕获的副本 + `log_` + `panel_` HWND；每条退出路径都清 `workerBusy_`（含 catch-all），单飞守卫不会死锁 |
| 临时诊断残留 | 无 | `git diff src/render/surface.*` 只剩行尾噪声；全仓无 `HERMES_DIAG_WIDGET`/`layeredBits_` |
| 删掉的三个 lambda / 通用旋转文字路径 | 无影响 | lambda 是 `Widget()` 内的局部量（编译器能证明没别的引用）；两条样式都在屏幕空间画完内容并把 `tag` 清掉，那段路径不可达 |
| `kSyncWidth` 92→60 是否挪动别的东西 | 无 | `layout.busy`（唯一用到 `syncLeft` 的盒子）全仓没有任何消费者；面板忙碌条画的是 `layout.content` 区间 |
| 同步时间戳两条读路径会不会不一致 | 不会 | 助手（WSL 内）与 UNC 快照都用**本地时间**格式化；实测 WSL 本地时间与 Windows 同为 CST(+08) |
| 渲染是否被这次改动碰过 | 没碰 | 胶囊结构报告前后完全一致（rail 7 px @13..19/14..25、text 125 px、170/170 行）；两次同构建抓图 0 px 差；与 before 基线那 ~150 px 差是**玻璃后面的桌面变了**（拿同构建的两张互比也有 ~160 px 差，量级相同） |
| 新加的 layer 是否配对 | 是 | 每个 `PushLayer` 在同一条路径上都有 `PopLayer`；只剩一个新增 layer（铭牌装饰），遮罩用的就是同一个变换后几何；几何创建失败时装饰整体跳过，板子与玻璃照常画（优雅降级） |

### 6.4 修完的重新验证

- `diag/backdrop-stale.py` A vs B = 0 px；`backdrop-captures.py` 抓取次数与旧逻辑逐项相同。
- 快速门禁 10/10（settings/widget/geometry/icons/motion/shell/tray/log/state/lifecycle）。
- `-Gate All` 30 套件 **100% 通过（exit 0）**，10 项真实指针/IME 按环境 SKIP（外部自动化仍顶着光标）。
- 重新打包并替换真机实例：SHA256 `3579780db36a5417d79b6fc5bfe58ff7dcf9920b222272a318f485daf4218469`。

## 7. 贴边/收起时"抖动闪烁几下"（2026-10-21，用户真机报告）

**症状**：贴条吸附到边缘的一瞬间、以及面板收起成贴条的过程里，贴条会抖/闪几下；铭牌与胶囊都闪。

**根因（就是我上一节的修复没落账）**：`EnsureBackdrop` 改成"按窗口矩形判断是否要重抓"之后，
`CaptureBackdrop` 里**只记了快照的尺寸（`backdropWidth_/Height_`），没记位置（`backdropLeft_/Top_`）** ——
那两行赋值当时用文本替换写进去时缩进不匹配、静默没落盘，而我的验证只测了"快照新鲜不新鲜"
（一直重抓 → 永远是新鲜的，测试反而通过了）。结果：**每次贴条重绘都被判成"几何变了"→ 每次都抓屏**，
而抓屏要先把窗口藏起来再 BitBlt 再显示 —— 每次抓屏就是一次肉眼可见的闪烁。

**真机证据**（用户自己那份日志，13:27–13:33）：

```
13:27:56 drop 2->4 → strip backdrop gen=147 / settings saved / gen=148        ← 一次落边闪 2 次
13:28:03 strip-drop → gen=153 / gen=154 / gen=155                            ← 一次拖动落边闪 3 次
13:28:04 strip-drop → gen=158 / gen=159 / gen=160
13:29:02 / 13:30:02 / 13:31:02 / 13:32:02 / 13:33:02 各 gen=161..165         ← 空闲时每 60s 轮询闪一次
```

**修法**：把这次快照对应的窗口位置真实落账（`backdropLeft_=rect.left; backdropTop_=rect.top;`），
让矩形判定只在几何真的变了时才成立。

**修后实测**（隔离实例，`diag/flicker-transition.py` + 应用自身日志）：

| 动作 | 抓屏次数 | 贴条消失帧 | 无玻璃帧 |
|---|---|---|---|
| 空闲（对照） | 0 | 0 | 0 |
| 轮询一次 | **0**（修前 1） | 0 | 0 |
| 展开（3↔4 收起） | 0 | 0 | 0 |
| **真实收起路径**（hide→show，即指针离开面板） | **0**（修前 ≥1） | 0 | 0 |
| 收起（控制面 place，含一次强制失效 = 最坏情况） | 1 | 0 | 0 |
| 换挂件样式 / 换边（几何真变了） | 1 | 0 | 0 |

另：`diag/widget-diff.py` 那套"隐藏后差值"证明每次抓到的都是真桌面（mean/noise 与桌面实测一致：
0xFCFCFC、noise 3.6→0.4），不是把自己拍进去。

**新增永久守卫**：`RenderStrip` 里若某帧没有玻璃快照（面板会画成不透明实色 = 真机表现为"闪一下"），
日志记一行 `strip frame without a glass snapshot`（拖动中的帧除外）。这类问题以后不再静默。

**残留（已知、未改）**：换挂件样式/换 DPI 时若贴条正在屏幕上，那一次必要的抓屏仍是
`hide→DwmFlush→BitBlt→show`（合成器很快，实测没有丢帧；真要彻底消除可把窗口临时挪出屏幕再抓，
但那要改共享的抓屏机制，风险大于收益，暂不动）。

**验证**：`-Gate All` 30/30（exit 0）；单跑复核 `a-hover`（A7 空闲 CPU 0.156%，
与改动前基线一致）与 `h-fallback`/`h-controls` 全绿 —— 第一轮 All 里的三项失败是机器被外部自动化
压着时的负载抖动（同一构建单跑即过）。

## 8. 顺手抓到并修掉的第二个问题：worker 线程起不来时面板会直接退出（2026-10-21 真机）

装完 §7 的修复后盯真机日志，14:42:56 看到面板**自己退出了**（不是崩溃转储，是走了"致命错误"出口）：

```
14:40:55 app: starting instance
14:41:56 data: refresh from UNC snapshot (config mtime=…)
14:42:56 [error] message: resource unavailable try again: resource unavailable try again
14:42:56 [error] message: fatal, closing
14:42:56 app: exiting with 0
```

定性：`resource unavailable try again` 是 **MSVC `std::thread` 构造失败**（`_Thrd_error`）时 `std::system_error` 的
`what()` 形状（"msg: category"两段同文）。链路：轮询定时器 → `StartRefresh` → `worker_=std::thread(...)`
抛异常 → 冒到消息循环 → `HandleWindowFailure` 认定"非绘制类失败不可恢复"→ 打印 `fatal, closing` 并退出。
当时机器正被另一个自动化会话压满（同一时段 Explorer 在批量开关窗口、CPU 被占），属于**瞬时资源失败**，
不是状态损坏——而面板的反应是让用户的小挂件凭空消失。全日志里只出现过 1 次（另一份轮转日志 0 次），
复现不了，但后果不小。

修法（结构性，不靠复现）：新增 `Application::SpawnWorker(slot,busy,extra,flag,body)`，六个 worker 起线程点
（`StartRefresh` / `StartHelperAction` / 探测 / 只读读取 / `StartLatencyProbe` / `StartProfileApply`）
统一走它：失败时回滚忙碌标志（`workerBusy_`/`latencyBusy_`/`profileBusy_`/`readBusy_`）、记一行
`worker thread could not start: …`、本轮跳过，**异常不再可能到达消息循环**。六个起线程点都是所在函数的
最后一条语句（逐个核对过），所以"失败即 return"不会跳过任何收尾逻辑。控制面线程（`control_pipe`）仍在
启动期失败即退出——那是 UI 起来之前的事，快速失败是对的。

验证：编译零警告；`-Gate All` 30/30（exit 0，见 `gate-all-spawnfix.log`）；数据通路门禁
（helper/hermes-state/a-c1/h-models/h-fallback/h-add/h-backup/h-settings/h-offline/h-migrate/h-controls）
全绿。真机重启后盯了 ~4 分钟：启动抓一次玻璃、随后两次轮询**零抓屏**、无自退出。

**残留**：这一类"瞬时资源失败"没有可复现的注入手段（线程无法按需创建失败），所以没有写门禁；
修法是结构性的（异常路径被消除），并以真机日志历史（只出现 1 次）作为发生率依据。

## 9. 门禁拆分：真实指针套件单独成 `-Gate Pointer`（2026-10-21，用户要求）

用户："能不能把这个点来点去和拖动的测试删掉，不需要每次都跑这个"。
采取的方案是**挪出默认跑法、不删覆盖**（删掉它们等于把"真机点过一遍"这条审计要求作废）：

- `tools/e2e.ps1` 新增 `-Gate Pointer` = `^(hover|drag|dpi|h-ime|a-snap|a-pullout|a-hover|a-edge|a-crossdpi|a-topology)$`
  （合成鼠标点来点去、拖窗口、跨屏、悬停，以及需要桌面的 IME 套件）。
- 从 `All` / `Regression` / `Shell` / `Pages` 里移除这 10 项；`a-c1`（重启恢复，不点不拖）留在默认集合。
  `Shell` 现在是 `^(shell|tray|motion)$`。
- 需要时单跑：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate Pointer`。

**效果**：默认 `-Gate All` 从 30 项 / 408 s（含若干 SKIP）降到 **20 项 / 110 s，0 失败 0 SKIP**；
`-Gate Pointer` 单独跑一次约 4.7 分钟。

**顺带把欠着的证据补齐了**：拆分后单跑 `-Gate Pointer`（当时那台机器上的另一个自动化刚好收工）——
**10 项 9 过 1 SKIP**（`a-snap` 那一下又被外部指针碰了一次），随后 `a-snap` 重试 **1 次即 PASS**：
`hover / drag / a-snap / a-pullout / a-hover / a-edge / a-crossdpi / a-topology / h-ime / dpi` 在**最终产物**上
全部有通过记录（`gate-pointer.log`）。这是之前一直缺的那一块：真机交互路径（含贴条拖动落边 = 本轮
闪烁问题所在的路径）在最终代码上验证过。

## 10. 第二轮自查：还带来别的 bug 吗（2026-10-21，用户要求）

方法同上（逐 hunk 复查本轮改动 + 定向排查 + 跑完四条完成门禁）。本轮改动清单：快照按矩形判定、
`SpawnWorker` 六处改写、挂件定时器残留删除、玻璃缺失守卫、门禁拆分。**查出 3 个我引入/遗留的问题，全部修掉**：

### 10.1 `-Gate Audit` 挂了：审计工具还在查已删除的两套挂件（工具问题，P2）

`tools/full_audit.py` 仍按四套样式循环：`STYLES=[beacon,tag,flag,capsule]`、`STYLE_DIP` 四种尺寸、
以及 CPU 段里 `configure widgetStyle=beacon/flag` 的腿 —— 旗标/信标删除后必然 16 项失败
（4 样式 × 4 边 × 2 DPI）。修：样式表收敛为 `tag/capsule`，CPU 段改成两套各测一次，
并**新增 6 条 legacy-style 检查**（`beacon/pulse/flag/magnet → capsule`、`halo/badge → tag`，
读 `dump.stored.widgetStyle` 对比）——删掉的样式仍有覆盖，查的是"归宿"而不是它们各自的尺寸。
`-Gate Audit`：**133 项 0 失败，exit 0**（`gate-Audit-bugcheck4.log`）。

### 10.2 上面那条新检查顺手抓到一个真产品 bug（P3）：设置里的样式名没归一

`SetSetting("widgetStyle", value)` 原来把**原始字符串**直接存进 `stored_.widgetStyle`，
只把 `model_.widget` 归一 —— 于是 "beacon" 这种已删除的名字会留在设置里、被写回文件，
而设置页的分段控件按名字选档（`stored_.widgetStyle=="capsule"?1:0`）会显示成**另一套样式**
（渲染的是胶囊、页面上亮的是铭牌）。`SettingsStore::Load` 是归一的，所以重启后会自愈，
但运行期一直不一致。修：在**唯一入口**按同一条规则归一
（`model_.widget=WidgetStyleFromName(value)`、`stored_.widgetStyle=WidgetStyleName(model_.widget)`）。
修后 6 条 legacy 检查全绿。

### 10.3 `-Gate Release` 挂了：真机实例占着 dist，打包删不掉（流程问题，P2）

真机实例是直接从 `dist\HermesModelPanelNative\` 起的，而 Release 门禁要 `package.ps1` 先删再写该目录
→ `Remove-Item ... Access to the path ... is denied`。修：**部署副本分开** —— 真机跑
`dist-live\HermesModelPanelNative.exe`，`dist\` 只当构建产物，两者 SHA 一致（已核对）。
`-Gate Release` 随即 **exit 0**。

### 10.4 复核确认没问题的（逐条）

| 风险点 | 结论 | 依据 |
|---|---|---|
| `SpawnWorker` 改写的线程语义 | 正确 | 六个起线程点前面都仍有 joinable→join（196/400/672/741/799/883 逐个核对），赋值给 joinable 的 thread 不会 terminate；失败路径回滚的忙碌标志逐点核对 |
| 失败路径的界面状态 | 已补 | 起线程失败时 `StartRefresh` 停掉旋转定时器、**不画"刷新成功"对勾**（否则是假状态）；`SpawnWorker` 失败给一条 toast「工作线程无法启动，稍后自动重试」（纯 ASCII 十六进制转义，本仓库约定） |
| 挂件定时器残留 | 清干净 | 全仓无 `UpdateWidgetTimer`/`kWidgetTimer`/`widgetPhase` 引用；定时器 ID 无重复（1-8/11/12/14，9 已空出） |
| 快照矩形会不会每次都判成"变了" | 无 | 真机 18 次轮询只抓 1 次；每次真实几何变更恰好 1 次抓屏（换边最多 2 次，两步落位） |
| `h-controls` 的 tooltip 断言 | 改成有界轮询 | 固定 `Sleep(900)` 在机器满载时不够（真机抖过两次：refresh / lampWsl）。断言不变（"悬停必须出提示"），改为最多等 2s、每 100ms 轮询；机器再忙也不会误判，出不来照样失败 |
| 门禁并发 | 是个坑 | 我边跑 `-Gate Pointer` 边跑 `-Gate Regression` 时 `drag` 报 "system move loop did not put the app into S2"（两条门禁同时在同桌面合成鼠标）；单跑 `drag` 3/3 通过。**门禁一次只跑一个**，记下来免得再踩 |

### 10.5 最终产物上的完成门禁

| 门禁 | 结果 | 证据 |
|---|---|---|
| `-Gate Regression` | **20/20，exit 0** | `gate-Regression-final2.log` |
| `-Gate Audit` | **133 项 0 失败，exit 0** | `gate-Audit-bugcheck4.log` |
| `-Gate Release` | **exit 0**（打包成功） | `gate-Release-bugcheck2.log` |
| `-Gate Pointer`（真实指针，单跑） | **10/10，exit 0** | `gate-Pointer-final2.log` |
| `-Gate All`（默认，不含指针） | **20/20，exit 0**（110 s） | `gate-all-nopointer.log` |

产物：`dist/` 与真机在跑的 `dist-live/` 同为 SHA256
`940c9c2d14a557ed75bf9800f3a7fa804ef583564a34a7cfb6c1a23b6584aed6`。

## 11. 面板弹出时"内容位移/抖动"的定性 + 缓和（2026-10-21，用户报告，方案 A）

**症状**：点贴条弹出面板的过程中，内容有小小的位移和抖动，然后恢复；铭牌/胶囊都会。

**定性（不是 bug，是入场动效本身）**：`ApplyState` 在 Collapsed→Expanded 时调 `StartPanelMotion(true)`，
`CurrentView` 把它变成两条动画：

```cpp
input.motion.panelAlpha    = progress(panelMotionStart_, kPanelMotionMs);      // 140ms 淡入
input.motion.panelOffsetDip = (1.0f - panel) * 8.0f;                           // 8 DIP 从下往上滑
```

渲染层把**整块内容**（标题栏 + 正文）按这个偏移平移后再画（`render/panel.cpp` 的 `MotionGuard`）。
问题出在共用缓动是 **ease-out cubic（前快后慢）**：8 DIP 在 125% 缩放下是 10 px，而这张曲线
**第一帧（≈17ms）就走掉 32%（3.2 px）、前 50ms 走掉 73%** —— 观感就是"窜一下再慢慢贴回去"，
用户描述的"位移 + 抖动 + 恢复"正是它。

**证据**：
- `evidence/filmstrip-before.png`：真实点击 + 逐帧胶片，t=918ms 内容偏下、t=964ms 上移、t=1011ms 归位，
  之后帧间完全一致（`0 px` 差）；
- 逐帧互相关测量（`diag/panel-enter-real.py`，真机与夹具两种节拍）：**除这段入场位移外没有任何第二段移动**——
  数据落地不改布局（`geometry: panel` 写入 +0、状态转换只有那一次展开）；
- 代码出处：`git diff src/render/panel.cpp` 里 `panelOffsetDip` 出现 **0** 次，
  `git log -S panelOffsetDip` 指向初始提交 v1.0.0（"移植自余额面板"）→ 先于本会话所有改动。

**改法（用户选 A：换缓和曲线，保留 8 DIP / 140 ms）**：
- 只给**位移**换成对称缓动 smoothstep `t*t*(3-2t)`（与胶囊形状过渡同一条曲线、同一理由），
  淡入仍用 ease-out（它要的是"尽快可见"，换对称会先闪一个近乎全透明的空壳），
  其余动画（卡片/贴条淡入/tooltip/悬停/主题交叉淡入）一律不动；
- 实现：`src/app/application.cpp` 里加 `progressOf(start,duration,curve)` + `progressSmoothed`，
  `panelOffsetDip` 改用它，一行生效。

**前后对照**（`evidence/enter-curve.md`，8 DIP = 10 px @125%）：

| 已过时间 | 改动前 ease-out 剩余位移 | 改动后 smoothstep 剩余位移 |
|---|---|---|
| 17 ms | 68% (6.8 px) | **96% (9.6 px)** |
| 50 ms | 27% (2.7 px) | **71% (7.1 px)** |
| 84 ms | 6% (0.6 px) | 35% (3.5 px) |
| 140 ms | 0% | 0% |

即：第一帧的移动量从 3.2 px 降到 0.4 px，起步与收尾都软，中段承担主要位移。

**验证**：`motion` 门禁 PASS（入场仍会启动、动画结束定时器停掉、animations=false 仍确定）；
21 项回归集（含 `unc-parity`/`a-c1`）全绿；`evidence/filmstrip-after2.png` 确认入场动效仍在
且没有第二段移动。逐帧采样只有约 35–50 ms 节奏（`ImageGrab` 本身 ~25ms），分辨不出 140 ms 内的曲线差异——
所以曲线本身的数值表是主证据，逐帧只用来确认"动效还在、没有多余移动"。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`96c1fee52c32f082898d4b1fdb99aa7ea4f8e7a077f4792a851d22a53889796c`（实例 PID 47096）。
若仍觉得晃，B 案（位移 8→4 DIP、140→180ms）与 C 案（只留淡入）随时可做。

## 12. 面板展开的动画与效果整体去掉（2026-10-21，用户要求"你去掉我看看"）

**改动（DESIGN v1.11，先改文档再改代码）**：展开方向（`Collapsed→Expanded` / `Hidden→可见`）不再触发任何
面板动效——既不滑也不淡入，展开即完整显示。删除展开专用的两处 `StartPanelMotion(true)` 触发点、
位移通道（`panelMotionEnter_` + `panelOffsetDip` 的写入 + 为它引入的对称缓动）。

**范围界定（重要）**：`StartPanelMotion(false)` 其实不是展开动画，而是**切页时的内容淡入**（5 个 tab
调用点各一处）——用户只说了"面板展开"，所以这条**保留**，函数随之改名去掉参数（`StartPanelMotion()`）。
其余同样原样不动：贴条淡入、悬浮卡进/退场、主题交叉淡入、悬停过渡、胶囊形状过渡。

**验证**：
- `motion` 门禁**按新契约重写**（旧断言"show/expand 必须启动入场"编码的是被删除的契约，理由写进了
  测试注释）：现在断言 show/展开**没有** motion start、**没有** 16ms 运动定时器，并**新增一条**
  "点 tab 仍会启动切页淡入并由 MotionTick 收尾"——把保留下来的东西也钉住。跑通：
  `PASS motion: no entrance on show/expand (removed on request), page-switch fade, strip fade,
  card enter/exit, theme crossfade, timer stops, animations=false is deterministic`
- 22 项回归集（含 `motion`/`unc-parity`/`a-c1`）全绿；构建 0 warning（Debug 与 Release 都 0）。
- 逐帧证据（控制面点击 + 行剖面互相关，`diag/panel-enter.py`）：此前"展开后第一帧偏移 +8~10 px、
  2-3 帧内归位"，现在是**展开后第一帧与最终帧完全相同（match score 0）**，没有任何中间帧。
- 肉眼对照：`evidence/enter-removed-comparison.png`——上行（改动前）三帧内容逐帧上移；
  下行（改动后）三帧完全一致。
- `evidence/filmstrip-noanim2.png`：完整胶片（含 t 标注）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`f4b581201808d6b0fc2a25b0c9ce939eaafa7b5c9b1abad04423efabb9bf1fe1`（实例 PID 31248）。
若哪天想要回动画：把两处 `StartPanelMotion()` 改成带方向的入场（渲染层的 motion 层叠加与
`panelAlpha`/`panelOffsetDip` 两个字段都还在，只是没人写偏移）。

## 13. 悬停提示被底栏盖住 → 置顶（2026-10-21，用户报告）

**症状**：底栏那几个按钮（模型/回退链/添加/备份/设置）悬浮时，提示气泡被压在界面底下看不见。

**根因（两条叠加，读代码即定位）**：
1. **绘制顺序**：`PanelRenderer::Draw` 的顺序是 … 内容 → 提示 → 下拉 → 焦点环 → 模态 → Toast →
   **底栏（`layout.nav` + `Nav()`）→ 窗口描边**。底栏是**最后一层实心条**，任何与它重叠的提示都被盖住。
2. **落位规则**：提示以**指针**为锚（"指针右下 14/18，放不下就翻到指针左上"，最后夹进**窗口**）。
   悬停底栏按钮时指针在底栏内部 → 翻到上方的气泡底边只比指针高 10 DIP → **仍在底栏范围内**
   （底栏高约 46 DIP）→ 于是整块落在底栏里 → 被第 1 条的底栏盖住。

**改法（DESIGN v1.12，先改文档）**：
- 提示块**移到 `Draw` 的最后**（底栏、模态、Toast、窗口描边之后）——它就是"置顶"的浮层；
- 指针落在底栏内时，纵向夹取的目标从"窗口底边"改成**内容区**（底栏之上 4 DIP），不再与底栏重叠；
- 模态打开期间**不显示提示**（否则"画在最后"会把提示压到模态之上；模态本来就该盖住一切）。其余
  落位规则（跟着指针、横向夹进窗口、多行按行画、120ms 淡入）一律不变。

**验证**：
- 像素证据：控制面复现（`diag/tooltip-nav-evidence.py`：`hover` 到 `tabFallback` 的 client 坐标 →
  `tooltip.visible=true`、文字「回退链：主模型不可用时按顺序接管」）→ 抓图
  `evidence/tooltip-nav-after.png`：气泡完整显示在底栏**上方**、盖在底栏之上，底栏按钮仍可见；
- `h-controls` 的悬停用例**扩到包含一个底栏按钮**（`tabFallback`，并断言提示文案含"回退链"），
  PASS 文案同步为 "the three lamps, the sync stamp and a bottom-nav tab explain themselves"；
- 21 项回归集全绿；构建 0 warning。

**已知覆盖缺口（登记，不当作 PASS）**：提示的**层级/矩形**没有门禁断言——提示矩形是渲染层按实测文字
宽度现算的（布局层没有 DWrite，量不了字），而 `h-*` 页门禁**没有 probe 通道**做像素断言（只有 `a-*`
带 `geometry-probe`）。本条的层级由上面那张像素证据覆盖；要做成断言需要给页门禁接 probe（约 40 行测试
脚手架 + CMake 一行），属于"下一批"。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`8a2b45a5044869e203273bf0c998d4079922443226560258338fe83d30632642`（实例 PID 36764）。

## 14. 悬停提示改到指针上方（2026-10-21，用户报告"容易被指针挡住"）

**症状**：提示气泡出现在指针右下角，光标本体（从热点往右下延伸的箭头）压住气泡左上角。

**根因**：v1.12 之前的落位是"指针右下 14/18，右边/下边放不下就翻到左上"——把气泡放在光标延伸的
方向上，挡住是必然的。

**改法（DESIGN v1.13，先改文档）**：纵向**优先放在指针上方**（指针上缘 −10 DIP − 气泡高），
只有上方顶到窗口顶（< 6 DIP）才翻到指针下方；横向、多行、淡入、以及"指针在底栏内时夹进内容区"
（v1.12）全部不变。

**验证（前/后差分 + 真光标，两个位置各一次）**：

| 悬停目标 | 气泡（面板坐标） | 指针行 | 结论 |
|---|---|---|---|
| 底栏「回退链」 | y 695..721 | 763 | **在指针上方 42 px**，且底边 721 < 底栏顶 ≈735（也在底栏之上） |
| 顶栏「同步章」 | y 20..34 | 60 | **在指针上方 26 px**（贴着窗口顶，未越界） |

- 手法：`diag/tooltip-above2.py` —— 先把指针移到别处拍一张（无提示），再用 SendInput **只移动不点击**
  把真光标放到控件上（真实 hover 路径，出提示），再拍一张做差分；排除控件自身的高亮矩形后，
  变化像素的包围盒就是气泡 ✓
- 肉眼证据：`evidence/tooltip-above-nav.png`、`evidence/tooltip-above-topsync.png`
- 22 项回归集全绿（含 `h-controls` 的悬停用例）；构建 0 warning

**已知覆盖缺口**（同 §13，未变）：落位方向没有门禁断言（页门禁无 probe 通道，提示矩形是渲染层
按实测文字宽度现算的），由上面的像素证据覆盖。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`0fb23636d8b2f4c2ba219b3b81ebc0a0253e3e460317919d4a2cbb01451ffee2`（实例 PID 55512）。

## 15. 悬停提示里的文字垂直居中（2026-10-21，用户报告）

**症状**：气泡里的文字看着偏下、不在高度中间。

**根因（读代码 + 实测）**：气泡内文字从 `box.top + 5 DIP` 起画，而每行排版盒是 16 DIP（10×1.6）、行距
10 DIP：n 行的文字块高 = 10n+6，气泡高 = 10n+11 → 文字块**底边正好贴到气泡底边**，上面却留 5 DIP。
实测（底栏「回退链」提示，面板坐标）：气泡 695..721，文字墨迹 706..717 → **上留白 11px、下留白 4px**。

**改法（DESIGN v1.14）**：首行起点改成 `box.top + 2.5 DIP`（= (10n+11 − (10n+6))/2），上下各 2.5 DIP
排版留白；横向 7 DIP、行距、淡入、落位全部不变。

**验证**：同一手法复测 → 气泡 695..721、墨迹 703..714 → **上 8px / 下 7px**（差 1px = 亚像素舍入）。
证据 `evidence/tooltip-after-center.png`（`diag/tooltip-above2.py` 前/后差分 + 真光标；脚本同时打印
墨迹行与上下留白）。附带修掉上一轮遗留的过期注释（横向规则说明里还写着"右下/左上"）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`3a53baf677afabfb3a752277389312fd0576083eb5e4e6f04db4f0552da85069`（实例 PID 25604）。

## 16. 顺带修掉 `h-controls` 悬停用例的取点 flake（既有问题，非本轮引入）

**现象**：`h-controls` 的悬停断言偶发失败（"hovering lampWsl/lampGateway shows no tooltip"）。
**机制（抓到失败现场的应用日志）**：失败时应用收到的是 `ui: hover control 0` —— 合成 hover 的落点
**没有命中任何控件**，所以提示没有理由出现。用例早先对整块 5 个控件只取**一份** `Controls()`/panel 矩形
快照，而紧挨着它跑过的"置顶 / 主题"用例会让几何在一瞬间不同 → 落点偏掉。
**证明它不是提示逻辑的问题**：同一条消息在我改提示之前就出现过（12:0x「hovering refresh」、19:0x 全量跑
「hovering lampWsl」）；本轮新增的"模态期间不显示提示"那道闸也没有参与（它前面的「回滚模态 Esc 取消」
用例是 PASS）。
**修法（测试侧，断言不动）**：每个控件**现读一次** `Controls()`/panel 快照，同一控件最多重试一次；
"提示必须出现且文案含关键词"两条断言原样保留。
**验证**：`h-controls` 连跑 3 次全过；随后 22 项回归集全绿。

## 17. 模型探测链路梳理 + 修掉"探测结果被丢弃"（2026-10-21，用户报告）

**探测在哪发生**：面板不自己抓模型。它调 WSL 助手的 `probe`（`hermes-panel-apply.py::cmd_probe`）：
按 `base_url` → `base_url+/models` → `+/v1/models`（base 以 `/v1` 结尾时用 `+/models`）依次 **GET**，
带 `Authorization: Bearer <key 或 key_env 读出的值>`，`extract_model_ids` 返回**全部** id（无截断，
已读源码确认）。面板侧有三个发起入口：模型页「供应商 ⋯ → 探测模型并追加」、添加页「加模型」的探测、
添加页「新建供应商」的探测。

**P1 bug（已修）**：探测结果的填充逻辑原来被 `activePage_==2` 门住 —— 从**模型页**发起时只写了
`addChips_`，而「加模型」子页显示的是**文本框**而不是药丸 → 结果等于丢弃，toast 却还说"去添加页拉"。
真机 2026-10-21 报"模型界面拉取完还要在添加那里再弄"即此。
**修法**：只要探测拿到了模型，就同时填「加模型」文本框、「新建供应商」文本框和药丸；从非添加页发起时
弹一条"N 个模型已填入「添加」页"。证据：12 项相关门禁全绿（h-add/h-fallback/h-models/geometry/widget/
settings/motion/shell/tray/h-controls/unc-parity/a-c1）。
**门禁缺口（登记）**：这条路径原本**零覆盖**（h-models 不碰供应商菜单、h-fallback 只测逐行探测），
所以 bug 一直没被发现；供应商菜单挂在列表**分组头的 ⋯** 上，控制面没有暴露该热点 → 下一批先把它暴露
出来再加断言。产品 SHA `ca107d3f…`。

**未修（需求类，需取舍/或需你配合）**：
- 回退链「追加一条」的模型是纯文本框（DESIGN §4.4 #17 原样），没有模型列表/探测 → 只能手打 id。
  建议加"探测该供应商的模型 → 可选列表"，属功能新增（先改 DESIGN）。
- ~~你的 `model:` 段没有 `key_env` …这很可能是"只能显示三个模型"的直接原因~~ —— **更正（2026-10-21 实测）**：
  这段推断错了。`custom_providers` 里的 `sheapi` 条目**有** `key_env: SHEAPI_API_KEY`，`.env` 里也有该变量；
  只读探测 `https://cf.sheapi.cc/v1`：不带鉴权 → `401 Invalid token`，带 `key_env=SHEAPI_API_KEY` → `200`、
  3 个模型（`deepseek-v4-pro` / `deepseek-v4-pro-0813` / `deepseek-v4.1-flash`），与配置里 `models:` 的 3 条一致。
  面板一键探测遍历的是 `custom_providers`（带各自 `key_env`），不走 `model:` 段 → "只能显示三个模型" 是
  **该供应商本来就只有 3 个模型**，不是截断；缺 `key_env` 的只是顶层 `model:` 段（网关自我解析用，不是探测
  失败的原因）。18 个供应商实探：13 个返回列表，5 个失败各有真实原因（xxs TLS 握手超时、local-cuda-35b
  从 WSL 内 127.0.0.1 拒连、sensenova 401、motomoto 超时、whiteapi 403 无订阅）。
  证据：`tmp/hermes-panel-native/diag/probe-all-live.py` 输出。

## 18. 一键探测全部供应商模型（只读）+ 添加页直接取用（2026-10-21，用户要求）

**用户要求**："加一个一键探测，就可以把所有供应商的现有模型列表从 wsl 那边读取到面板上，而不是我一个个去拉取"；
"只读就是只读"（探测不得写配置）。

**实现**（DESIGN v1.15）：
- 托盘菜单新增「探测全部供应商模型（只读）」（`MenuCommand::ProbeAllModels=1015`）→ `Application::ProbeAllModels()`：
  worker 上按 `base_url` 去重、最多 24 个、单个 `timeout_s=6`、串行 + 150 ms 间隔调用助手 `probe`（只 GET）；
  结果按供应商名进 `probedModels_`（`std::mutex` 保护），每完成一个发一次进度消息（`kProbeAllMessage`）。
- 添加页「加模型」：选中供应商时若缓存里有它，直接把模型填进「模型 id」框 + 药丸，
  `addInfo_` 注明"来自一键探测缓存"——不需要逐个供应商再点一次探测。
- 控制面 dump 新增 `probeAll{busy,done,total,ok,providers[{name,count}]}`。
- 同批修复：从**模型页**「供应商 ⋯ → 探测模型并追加」发起的结果不再被丢弃（见 §17）。

**验证**：`-Gate h-add` 新增断言并在夹具上通过：
`PASS probe-all: 11 of 12 providers returned a model list (read-only)`；
同场景原来那条"点一次探测 = 恰好一次 helper 调用"的断言因为一键探测已经产生过 probe，改为**增量**断言
（语义更准，未放宽）；22 项回归集全绿（Exit 0）。

**只读保证**：整条链路只调用助手 `probe`（GET `base_url` / `+/models` / `+/v1/models`），不产生任何配置写入——
`config.yaml` / `.env` 仍然只由 `hermes-panel-apply.py` 的写命令改，探测路径没有那条命令。

**未做（下一批）**：回退链「追加一条」表单里的**模型选择器**（用户同样要求）：需要在该表单加第二个下拉
（复用 `selectField` 机制）+ 布局/渲染/命中 + 门禁断言。

**给你的一个可操作项（已实测，结论与最初推断相反）**：你的 `sheapi` 供应商条目本身有
`key_env: SHEAPI_API_KEY`，一键探测对它是**带鉴权**的 —— 只读探测返回 `200`、3 个模型，和配置里
`models:` 的 3 条完全一致（"只能显示三个模型"是这个供应商的真实模型数，不是面板截断）。
实探 18 个供应商：13 个成功，5 个失败各有外部原因（xxs TLS 握手超时、local-cuda-35b 在 WSL 内
`127.0.0.1` 拒连、sensenova 401、motomoto 超时、whiteapi 403 无订阅）——没有一个和 `key_env` 有关。
顶层 `model:` 段确实没有 `key_env`，但它是网关自解析用的，不影响面板探测；想补齐也可以
（`model:` 下加 `key_env: SHEAPI_API_KEY`），属可选整洁项，面板目前没有这个字段的编辑入口。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`ad730ff7d40140570f96fe63b7bc1e089f9550fd20cbe719a74585b3d583e343`（实例 PID 67424）。

## 19. 回退链「追加一条」的模型选择器（2026-10-21，用户要求，DESIGN v1.16）

**用户要求**：回退链要能「探测该供应商的模型 → 可选列表」；"只读就是只读"（探测不得写配置）。

**契约**（DESIGN v1.16 / §4.4 #17 已更新）：
- 「模型 id」右侧加 ▾（`ControlId::FbModelSelect`）。命中区是字段右侧 28 DIP，且**插在控件表最前**——
  否则覆盖同一矩形的 `FormField` 先被 `ControlAt` 命中，点击永远进不了下拉。
- 候选顺序：①一键探测缓存 `probedModels_`（锁内拷贝）→ ②供应商条目自带的 `models` → ③都没有就
  **不开空列表**，改发一句"先探测"的 toast。手填模式（source==0）用「provider 名」框里的字去碰缓存。
- **只读**：选中只 `fbModel_.SetTextUtf8(id)`；这条路径上没有任何助手写命令，文本框仍可手打。
- 与来源供应商下拉互斥。第一版实现"点另一个下拉只关不开"，门禁立刻抓到（`selectOpen` 没打开）——
  改为在"关闭"分支里先做一次命中测试：点到的正是另一个下拉的框，就一次点击完成「关旧的、开新的」。
- 布局复用 `selectList / selectLabels / selectOptions` 与同一滚动量（可视 6 行 + 滚动条），
  列表贴在模型 id 字段下方；没有新增平行通道。
- 控制面：`controls` 暴露 `fbModelSelect`；`dump.fallback` 新增 `modelSelectOpen` / `modelOptions`。

**验证**：
- `h-fallback` 新增一条并在夹具上通过：空缓存时列表 = 配置里的 `["alpha-1","alpha-2"]` → 托盘 1015
  一键探测 → 列表变成缓存里的 `["probe-a","probe-b","probe-c"]`（缓存优先）→ 点第 2 项 →
  `modelValue=="probe-b"` 且 `probe` 调用次数不变（只读）→ 两下拉互斥断言。全量 `-Gate All` 20/20
  （证据 `tmp/hermes-panel-native/evidence/20261021-231939-701-All.txt`）。
- 真机（computer-use 之外的 SendInput 点击 + 物理像素截图）：来源供应商选 `deepseek-official` →
  ▾ 列出 `deepseek-v4-flash / deepseek-v4-pro / deepseek-v4-flash-vision-exp` → 点 `deepseek-v4-pro` →
  「模型 id」变成 `deepseek-v4-pro`、列表收起、未提交（无写）。
  证据：`tmp/hermes-panel-native/evidence/live-v116-picker-crop.png`、`live-v116-picked-crop.png`。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`411ae7f4685563c1fb92a98465a86f3e2f27930527f30fddf259c16b3cf47447`（实例 PID 62460）。

## 20. 写盘链路实证 + 统一网关重启交代（2026-10-21/22，用户质疑"供应商添加是不是摆设"）

**问题**：用户问"面板里的供应商添加真的能写进 hermes 配置吗，还是摆设"，并要求任何改动若需重启网关必须提示。

**实证方法**（`tmp/hermes-panel-native/diag/verify-writes.py`）：真实 `~/.hermes/config.yaml` 只读复制到
`tmp/hermes-panel-native/verify-home/`，用**真实**助手 `hermes-panel-apply.py`（同一份、同一条 stdin JSON
协议）按面板实际 payload 逐条执行，每步用 PyYAML 读回断言：

| 步骤 | 面板入口 | 断言 |
|---|---|---|
| upsert-provider（新建） | 添加 → 新建供应商 → 保存 | `custom_providers` 出现条目，base_url/api_mode/key_env/models 正确；`.env` 收到 key |
| add-models | 添加 → 加模型 | 供应商 `models` 追加 |
| set-main | 模型页点模型 | `model.provider/default/base_url` 切换 |
| fallback-add | 回退链 → 追加一条（选供应商 + 模型 ▾） | 链尾出现该行（from_custom 继承 base_url/key_env） |
| fallback-move | 阶梯行 ↑ | 行位置改变 |
| fallback-remove | 阶梯行 × → 确认 | 行消失 |
| delete-provider | 添加 → 删除供应商 → 确认 | 条目消失、`.env` 的 key 被 purge |
| 还原 | — | 副本逐字段回到基线 |

过程中 `delete-provider` 在"该供应商正是当前模型"时被助手**拒绝**——保护逻辑真实存在（面板
`ProviderDeletable` 用同一语义把删除键置灰）。真实 `~/.hermes` 全程未被写入。
面板侧 payload 由门禁逐字断言（见 §5 审计 F-002、`h-add`/`h-fallback`），两段合起来即"点击 → 真实写盘"。

**修的缺口**（DESIGN v1.17）：此前只有「切主模型」「套用配置档案」进入待重启/自动重启分支，供应商与
回退链的写入只弹"网关热读生效"——对机器人（微信/QQ 回合走 agent）不成立。现在 `ApplyDataResult` 对所有
`IsConfigWrite` 动词（set-main / upsert-provider / add-models / delete-provider / fallback-add /
fallback-remove / fallback-move / restore）在成功落盘后统一交代：
- 「自动重启网关」开 → 清待重启、排队 `reload {targets:[gateway],mode:auto}`、toast 追加"网关重启中，机器人稍后生效"；
- 关 → `pendingGatewayRestart_=true`（寄存器行出现可点的「重启网关」胶囊、贴条信息卡提示）、
  toast 追加"需重启网关后机器人才用上（点网关灯后的「重启网关」）"。
7 处写动作文案里的"（网关热读生效）"一并删除；`dry_run` 预演不进入该分支。

**门禁**：`h-fallback` 两条新断言通过——(a) 写入成功后 `data.pendingGatewayRestart==true` 且 toast 含"重启"；
(b) 打开自动重启再写一次 → 发出 `reload` 且待重启胶囊收起。全量 `-Gate All` 20/20
（`tmp/hermes-panel-native/evidence/20261022-000839-243-All.txt`）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`2107ce82d31052d4e7fa772d5bffde6a6c02ec39d6c1c60e03e70491332da853`（实例 PID 32660）。

## 21. 供应商 ⋯ 菜单的「探测模型并追加」补门禁（2026-10-22）

**先更正记录**：§17/G40 写的是"控制面没有暴露该热点"——不准确。`controls._list.items[]` 里
`kind==0`（分组头）的条目一直带 `menu` 矩形（`GroupMenuBox`：20 DIP，头行右端内缩 11 DIP），
`h-controls` 也已经在点它并断言"菜单能开、Esc 能关"。真正没有断言的是菜单里的**确定动作**
（`RunPendingAction` 的 `Pending::ProviderMenu` 分支：对该供应商发一次只读 `probe`，结果进添加页）。

**补的断言**（`h-controls`，紧跟"菜单能开"之后）：点 `modalConfirm` 后
1. `requests.log` 里恰好多一次 `cmd=probe`（不许连发、不许重复）；
2. 该请求的 `base_url` 与 `key_env` 等于**这个供应商**在状态里的值（不是别的供应商，也不是空）；
3. 切到「添加」页后药丸非空——即 v1.15 修的"探测结果被丢弃"在真实点击路径上仍然成立。

**控制面补强**：`dump.data.providerList` 从"只有 `providers` 个数"扩展为逐条
`{name, baseUrl, keyEnv, models, isActive}`，否则断言"发的是这个供应商的地址"无从取证。

**验证**：`-Gate Regression` 20/20（`tmp/hermes-panel-native/evidence/20261022-005021-051-Regression.txt`），
其中包含 `PASS click: the provider menu's probe fills the add page (read-only)`。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`bd5e533d65dd7dcdd397db74bcfff907480a300d901994fa4f1199c3484ecd34`（实例 PID 35504）。

## 22. 添加页收敛 + 供应商 ⋯「扫描并追加」（2026-10-22，用户要求，DESIGN v1.18）

**用户要求**："新建供应商能不能不加模型…也不要那个 key 变量"；"保存前探测模型列表是干嘛的"；
"加模型能不能删掉，我只需要在模型列表里点三个点后，扫描并追加"。

**改了什么**：
- 子分段：新建供应商 / **删除供应商**（「加模型」子页删除）。原「加模型」的活由 ⋯ 承担。
- 新建供应商：名称 / Base URL / API Key（掩码+眼睛）/ 模型（**可空**）。**Key 变量名不再由用户填**，
  面板只显示助手将推导出的名字（`DeriveKeyEnv`，与助手 `derive_key_env` 同一规则）；模型留空合法。
- 删掉「保存前探测模型列表」开关：`addProbeBefore_` 只被 UI 翻转，`SubmitAdd` 从未读取——死控件。
  保留「保存后设为当前模型」，勾选但没模型时在界面直接拒绝（附原因），不再依赖助手报错。
- ⋯ 菜单主操作 = **扫描并追加**：只读 `probe` → 与配置里已有 `models` 求差 → `add-models` 只写**新增**
  → 刷新后供应商分组直接显示最新模型；没有新增 → toast「已是最新（N 个）」且不写盘。
  菜单正文增加一行"确定后：只读探测该供应商，只把新增的模型写回配置（自动备份）"。
- 顺带修的真 bug：模型药丸的布局循环用错了 `width`（外层=面板宽），`chipX+width>right` 恒真 →
  **药丸从未进入命中表**，真机上点掉药丸是空操作。`h-add` 新增的"清空药丸"用例把它抓了出来。

**验证**：`-Gate All` 20/20（`tmp/hermes-panel-native/evidence/20261022-012347-092-All.txt`），
其中 `PASS h-add: a provider can be saved without models`（payload 里没有 `models`）、
`PASS h-add: upsert-provider carried the form and the key never hit the log`（且**不带 key_env**）、
`PASS click: the provider menu's 扫描并追加 writes only new models`（1 probe + 1 add-models 只带 3 个新增，
第二次扫描不再写盘）。
真机：添加页已是两档子页、四个字段（模型提示"可选（可留空）"）、只剩一个开关
（`evidence/live-v118-addpage2-crop.png`）；供应商 ⋯ 弹卡显示"扫描并追加"
（`evidence/live-v118-menu-crop.png`，未点确定，未写任何配置）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`5cd2ce5168b8c085db9baea54596705cb315f1480e4e2d7a2888d499b9388594`（实例 PID 68480）。

## 23. 「扫描并同步」：同步时删掉已下线模型（2026-10-22，用户反馈，DESIGN v1.19）

**用户实测**：s2api 配置 6 个模型，供应商实际只给 4 个；点 ⋯ 后列表没变成 4 个，却提示"已是最新
（4 个模型）"。

**诊断**：v1.18 的动作是**追加**语义（助手 `add-models` 只会 `insert_model_ids`），配置里的
`glm-5.3-flash` / `glm-5.3` 不会被删；提示只报了探测数，没说还有 2 个在配置里。

**改动**（用户授权修改共享助手）：
- `hermes-panel-apply.py`：`add-models` 新增可选 `prune: true` —— 用传入列表**替换**该供应商的
  `models:` 子区块，返回 `{added, removed}`；不传参数时原行为**逐字节不变**；空列表仍然拒绝
  （防止把列表清空）；备份名 `sync-models-<name>`。
- 面板：⋯ → **「扫描并同步」**；探测为空 → 不动配置并说明；有差异 → `add-models{prune:true,
  models: 探测结果}`，toast 报"新增 N（删除 M）"；无差异 → "已是最新（N 个）"。
- 确认卡新增一行："确定后：只读探测该供应商，把模型列表同步成最新（新增的写入、已下线的删除，
  写前自动备份）"。

**实证**（真实助手 + 用户真实 config.yaml 的副本，`tmp/hermes-panel-native/diag/verify-prune.py`）：
1. 默认模式（不带 prune）仍然只追加：`brand-new-model` 加入、其余不变；
2. `prune=true`：列表变成探测结果，`removed = ['glm-5.3-flash','glm-5.3','brand-new-model']`；
3. 再同步一次：`added=[] removed=[]`，不写盘；
4. `dry_run` 返回差异但不落盘；
5. 空列表被拒。
真实 `~/.hermes` 全程只读。

**同批修的两个状态问题**（门禁长期偶发红的根因）：
- 逐条探测的**迟到结果**会覆盖新一轮：`ProbeRow` 带上 `generation`，`HandleProbeRow` 丢弃旧轮结果
  （实测：probe_missing 切琥珀后界面自己变回绿色）。
- 测试实例名含 pid，pid 复用会把上一轮改过的 `previewDiff` / `autoReloadGateway` 带进下一轮；
  h-fallback / h-controls 启动前清空自己的设置目录（h-migrate 依赖共享目录，未改路径规则）。
- 悬停用例先把真实指针挪到虚拟桌面角落：指针停在测试窗口上时，系统补发的 WM_MOUSEMOVE 会把
  悬停目标改成「追加一条」（提示文案因此对不上）。

**验证**：`-Gate All` 20/20（`evidence/20261022-021614-907-All.txt`），其中
`PASS click: the provider menu's 扫描并追加 writes only new models` 已更新为断言 `prune=true`
与"同步后模型数 = 探测结果"。真机：s2api 的 ⋯ 显示「扫描并同步」
（`evidence/live-v119-dialog2-crop.png`，未确认、未写盘）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`61fb97382e12ec841a1a087558027229eddf1bfb27c58de9f500c843838c8b5b`（实例 PID 58056）。

## 24. 健康复查：列表形态 models 的缩进 bug + 探测缓存（2026-10-22）

**触发**：用户要求"看看还有没有问题"。做了三层检查：静态残留、助手边界用例、三套门禁 + 真机。

**静态**：`addModels_ / addKeyEnv_ / addProbeBefore_ / SubTabModels / modelsText / kSubModels`
在 `src/ tests/ tools/` 里 **0 处**残留（v1.18 的删除是彻底的）。

**助手边界**（`tmp/hermes-panel-native/diag/verify-prune-edge.py`，真实助手 + 自建配置）：
发现**既有 bug**——`models:` 为列表形态、元素缩进（6）比字段缩进（4）更深时，`insert_model_ids`
仍按 4 写 `- id`，而插入点是子区块末尾（后面还有下一条供应商），于是产生一个挂在
`custom_providers` 层级上的孤立 `- id`，整份 YAML 解析失败。助手的写后校验把它挡在落盘之前
（文件没有损坏），但老面板的「加模型」会直接报错。
修：新增 `model_item_indent(lines, sub, findent)`，沿用已有第一个列表元素的缩进（没有元素才退回
字段缩进）；`insert_model_ids`（追加）与 `replace_model_ids`（同步）共用。老调用行为在没有
列表元素、或元素与字段同缩进时逐字节不变。

**面板**：供应商 ⋯ 的探测缓存只在**成功且有结果**时写入。此前探测失败会用空列表覆盖缓存，
导致回退链的模型选择器短暂退回"配置里的几个模型"。

**验证**：
- 边界用例 6 组全过：列表缩进 6 / 列表缩进 4 / 无 `models:` 子区块 / 含斜杠点冒号的 id
  （`deepseek-ai/DeepSeek-V4.1`、`Qwen/Qwen3.5-32B`、`vendor:model:v2`）/ 重复 id / 老调用只追加。
- `-Gate Regression` 20/20（`evidence/20261022-083737-780-Regression.txt`）；
  `-Gate Audit` 133 项全过（`…-083952-019-Audit.txt`）；
  `-Gate Release` 通过（`…-084319-827-Release.txt`）。
- 真机：新包启动、18 供应商 2 回退链读取正常，面板截图 `evidence/live-v120-crop.png`。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`358142ae5fba56604b3213bdb5485a42706f990ee4400aba959810e32dee9625`（实例 PID 59728）。

## 25. 输入框插入符 + 可滚动模型列表 + 删除按钮文案（2026-10-22，用户要求，DESIGN v1.21）

**用户要求**："所有的输入框应该需要有光标"；"新建供应商的探测按钮点击后应该是显示一个下拉列表，
可以看到所有的模型，而不是一个模型编辑框和下边的一行模型（一个只能显示3个，一个不能滑动），
追加的话，所有的模型都要进列表"；"删除供应商的删除按钮上应该是删除，而不是追加"。

**改动**：
1. **插入符**：`Field()` 此前只画高亮圈，全产品只有搜索框有插入符。新增 `FieldWithCaret()`
   （`Field()` 变成 caretIndex=-1 的包装）：按 `caret` 量出前缀宽度定位，掩码字段按"每字符一个点"、
   多行字段按"最后一个换行之后"的行/列定位；accent 色、跟随 530ms 闪烁；设置页右对齐字段从右边界
   回推。`dump.focus` 新增 `caret` / `multiline`。
2. **模型区重做**：删掉"3 行文本框 + 一行不能滚动的药丸"，改成**一个可滚动列表**——
   探测结果填入列表并**默认全选**（"所有模型都要进列表"）；表头「全选/全不选（M/N）」；
   点一行切换"是否写入"；可视 5 行，超出用滚轮（`WM_MOUSEWHEEL` 优先滚它）+ 右侧滑块；
   手动添加改成单行输入框，回车（单行字段的 Enter = 失焦）即并入列表并选中。
   保存时只写勾选的行；`dry_run` 预览与 v1.17 的重启交代不变。
3. **删除页文案**：子页只剩两档后，`sub==1` 就是删除页，但主按钮仍走旧分支显示「追加」。
   改为 `page.danger ? 删除供应商 : 保存`。

**过程中修掉的两个门禁/测试问题**（都不是产品缺陷）：
- `const Json` 上用 `operator[]` 访问不存在的键或越界下标会触发 nlohmann 断言（`json.hpp:22188`），
  表现为"测试进程神秘中止"；改成先断言 size / 用 `value()`。
- 上一条用例把列表"全不选"后，下一条用例默认它还选着：手动添加用例开头补一次全选。

**验证**：`-Gate All` 20/20（`evidence/20261022-125210-416-All.txt`），其中
`h-add`：探测列出全部 3 个且全选 / 回车把 manual-a 并入列表 / 点行取消勾选后只写 3 个 /
表头全不选后可空模型保存；`h-controls`：聚焦名称框 + 打 abc → `dump.focus.caret==3`。
渲染证据：夹具实例截图 `evidence/live-v121-real.png`（可见表头 + 4 行带勾选框）。

**环境限制**：真机点击验证被桌面上的第三方置顶窗口（交易软件）与自动移动的指针挡住
（`BLOCKER-005`），本轮截图来自夹具实例（同一份渲染代码）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`9cbbc2f91d27ff2a25f792af9bf9b44b859fad03bb399c7dc7bc3f2c78bee72a`（实例 PID 35524）。

## 26. 输入框换成原生 EDIT（2026-10-22，用户要求「我要原生输入框」，DESIGN v1.23）

**为什么可以**：面板主体不是分层窗口（`Surface` 用 `CreateHwndRenderTarget`；只有贴条/悬浮卡走
`UpdateLayeredWindow`），所以能挂真正的子控件。此前是自绘输入框（`TextField` + 自己转发
`WM_CHAR/WM_KEYDOWN/IME`），所以没有"点哪落哪、拖选、双击选词、I 形指针、右键菜单"。

**实现**（`src/ui/native_field.*` + `application.cpp` 的 `CreateNativeFields/SyncNativeFields/
HandleNativeFieldEvent`）：
- 用系统 `EDIT` 类创建（`WS_CHILD|ES_AUTOHSCROLL`，API Key `ES_PASSWORD`，设置页 `ES_RIGHT`），
  再 `SetWindowLongPtr(GWLP_WNDPROC)` **子类化**；只截获 Enter/Esc/Tab/↑↓ 与焦点通知，
  其余一律 `CallWindowProc` 交回 EDIT（编辑、光标、选区、拖选、双击、右键菜单、IME、横向滚动）。
- `EN_CHANGE` 把控件文本实时镜像回 `TextField`，既有业务代码（保存/探测/失焦提交）零改动。
- 视觉：自绘层继续画外框/标签/聚焦描边，EDIT 内缩（左右 9 DIP、上下 3 DIP）叠在框内；
  底色/字色由 `WM_CTLCOLOREDIT` 按当前主题给（换主题立即生效）。
- 摆放：`SyncNativeFields` 每帧算"哪些字段可见、放哪"，`Place()` 对未变化的矩形直接跳过。

**途中修掉的两个真问题**：
1. **重绘自激**（自己引入）：每帧都 `SetWindowPos` 子控件 → 子控件变化触发父窗口重绘 → 又摆一次。
   实测空转 125 fps、`WaitForInputIdle` 永不返回、CPU 730ms/s。加位置缓存后：就绪 366ms、
   空闲 CPU 8ms/s。
2. **自定义窗口类没有编辑行为**（第一版写错）：用 `HermesModelPanelNative.Field` 自定义类创建的
   子窗口不插入字符。改为系统 `EDIT` + 子类化转发，打字立即生效（`field1=https://probe.example/v1`、
   `caret=24`）。

**配套改动**：
- 控制面 `key`/`vk` 在焦点位于原生字段时投递给该控件（否则门禁打字全丢）；
  `hit` 增加 `isPanelChild`（自己的子控件不算"被别的窗口遮住"）；
  `dump.focus` 增加 `native`；`ClearSearch()` 同步清控件。
- 搜索框：↑↓ 转给列表光标、Enter 切换选中行、Esc 清空（与自绘时代同语义）。
- `motion` 首帧加就绪等待：原生控件让启动变快，`WaitForInputIdle` 提前返回，
  第一帧 dump 可能早于状态初始化（批量跑偶发 "expected floating start"）。

**验证**：`-Gate All` 20/20（`evidence/20261022-171631-605-All.txt`）。
`h-add` 26.19s 通过（含探测、手动回车并入列表、只写勾选行、空模型保存）；
`h-models` 通过（↓/Esc/✕ 三条键盘路径）；`h-fallback`/`h-settings`/`h-backup`/`h-offline`/`h-migrate`
均通过。真机实例已换成新包（PID 24508，读到 17 个供应商、2 条回退链）。

**过程偏差（记录）**：DESIGN v1.23 条目是在代码改完后补写的，违反"先改设计文档再改代码"的约定；
原因是这次改动由用户当场追加，且牵涉窗口架构。已在 PROGRESS 与本节同时标注。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`c983e89172d17a64cf46ff6bdcf41b8acc5122228ddd41d838686d54e6d44c80`（实例 PID 24508）。

## 27. 原生输入框收尾：占位提示 / 光标 / 命中语义（2026-10-22）

第 26 节之后真机截图暴露了三处收尾问题，逐个修掉：
1. **占位提示消失**：自绘的提示文字被 EDIT 盖住（搜索框还从左侧露出半格）。改用系统 cue banner
   （`EM_SETCUEBANNER`）；但它在**控件隐藏时设置会失效**，改成首次显示时重设。仍不显示 →
   进程没加载 comctl32 v6（清单里只有 dpiAware）。把 v6 依赖并入既有 `src/app/app.manifest`
   （不动 CMake 的清单链路，避免 CVT1100 重复资源），随后搜索框正常显示「搜索模型或供应商」。
   同时删掉自绘的搜索占位，避免双份/露缝。
2. **两个光标**：原生 EDIT 自己画光标，自绘层又画了一个。`PanelViewModel` 增加
   `focusedFormNative`，由 `NativeFor(focusedField_)` 决定；原生字段不再画自绘插入符。
3. **点击门禁把子控件当遮挡**：`hit` 之前只认 `window==panel_`，而鼠标落在 EDIT 上时命中的是子窗口。
   新增 `isPanelChild`（`GetAncestor(window,GA_ROOT)==panel_`），页面门禁判定改为
   「isPanel 或 isPanelChild」，并保留 `isPanel` 的原有语义。
顺带：搜索框给放大镜（左 30 DIP）与 `/` 提示（右 24 DIP）留白；`SetCue` 保存文案，
显示时重放；API Key 仍按 `ES_PASSWORD` 掩码。

**坐标读数**：枚举子控件时 PowerShell 不是 DPI-aware，`GetWindowRect` 返回的是 125% 缩放后的
虚拟坐标（实测 224/1.25=179、287/1.25=230 与控件真实位置吻合）——先判断为"子控件放错位置"，
核对后确认是测量口径问题，不是缺陷。

**验证**：`-Gate All` 20/20（`evidence/20261022-200814-137-All.txt`）、
`-Gate Release` 通过（`…-201027-159-Release.txt`）；真机截图
`evidence/cue4-crop.png`（原生占位文字 + 放大镜 + `/` 提示）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`2b045c59379432ccd66ed452d085dca1219251561352bc0f67db36935b49d959`（实例 PID 46764）。

## 28. 原生输入框：文字居中 + 消除闪烁（2026-10-22，真机反馈）

**反馈**："输入框里的文字不在居中高度，而且一直在闪烁"。

**文字偏上**：单行 `EDIT` 的文字是**顶对齐**的，而我们给它的是整个字段内缩框的高度（≈29 DIP）。
改为用 `GetTextMetrics` 量出字体行高，摆放时把控件高度收成「行高 + 2」并放在字段框竖直中线；
外框/标签/聚焦描边仍是自绘，视觉不变。

**持续闪烁**：面板窗口没有 `WS_CLIPCHILDREN`。父窗口每次重绘（轮询刷新、光标定时器、悬停）
都会把子控件覆盖的区域一起刷掉一帧，再由子控件重画 —— 这就是"一直在闪"。修法两条：
①窗口样式加 `WS_CLIPCHILDREN`（父窗口绘制时让出子控件矩形）；
②焦点在原生字段时，`kCaretTimer` 既不切换也不重绘（原生控件自己画光标，不需要父窗口闪）。

**证据**：连拍三帧、对搜索框区域按 2px 采样累加 ARGB，三帧完全一致（`distinct=1`；
`evidence/flick-1.png`/`flick-2.png`/`flick-3.png`）；居中后的截图 `evidence/center2-crop.png`；
`-Gate All` 20/20（`evidence/20261022-203636-047-All.txt`）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`6f40a3b6df19aaacd0dc6fd8e4ded45d8a9d39939fd211f330a3f04a8d37550f`（实例 PID 53584）。

## 29. 下拉遮挡 / 新建页模型区 / 忙碌条独立渲染（2026-10-22，真机反馈，DESIGN v1.24）

**反馈**："回退链的下拉列表有 bug，供应商列表可以看到输入框，另外，选不到模型。新建供应商探测不到
模型列表，而且该界面的探测后不要有输入框，只需要探测后得到模型列表。上边的忙碌条还是很卡顿。"

**① 下拉遮挡（一条根因）**：原生 `EDIT` 是**子窗口**，z 序永远在父窗口的自绘内容之上。于是回退链的
「来源供应商」下拉和「模型 ▾」一展开，表单里的原生输入框就压在列表上——既"看得到输入框"，也
"选不到模型"（点击落到输入框上）。修：`SyncNativeFields` 里把「任一下拉展开」并入 blocked 判定
（与模态同一条路径），展开期间隐藏全部原生字段。

**② 新建供应商的模型区**：删掉「模型 id」输入框（表单字段 4→3：名称 / Base URL / API Key），
模型只来自「探测」→ 列表；`AddFieldAt(3)`、原生绑定、失焦并入列表、`addNewModels_` 全部移除。
`h-add` 的字段数断言改为 3，删除"手动回车并入列表"那一段（需求变更，不是放宽断言）。

**③ 忙碌条**：量化后确认根因不是动画逻辑，而是**主面板一帧 D2D 重绘 200–230ms**
（日志 `busy paint 216ms`，768×1326 / 125%；自适应节流 → `busy repaint interval 400ms` = 2.5fps）。
改为独立子窗口（`src/ui/busy_bar.*`）：每帧只填一块 900×6 的 32bpp 位图 + `StretchDIBits`，
实测 **0.05ms/帧**；相位按 `GetTickCount64` 算，16ms 一拍（60fps）；主面板忙碌期间不再重绘。
计时器由三种 worker（刷新 / 逐条探测 / 一键探测）统一驱动，全部结束才停表。

**验证**：`h-add` 新增断言通过——`PASS busy bar: repaints while busy and each frame is cheap`
（`dump.busyBar.paints` 在 400ms 内增长 >3，`lastPaintMs` <8ms）；
`-Gate All` 20/20（`evidence/20261022-223844-213-All.txt`）。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`7c8a443a7c723ef2eaa3ffcffc12140b6d37359b663ac2887146095e4d2be662`（实例 PID 64584）。

## 30. 一键刷新同步 / 回退链只选不填 / 点击穿透（2026-10-23，真机反馈，DESIGN v1.25）

**反馈**：模型页要一键刷新同步；⋯ 菜单最后一行说明删掉；回退链不要手填、模型不要手打；
"收起追加列表的按钮有时会穿透到下边的按钮"，"很多按钮都会穿透到下边的部分，设置页也是这样，
窗口缩小时点设置会点到下边的按钮"。

**一键刷新同步**：`SyncAllModels()` 串行遍历供应商：只读 `probe` → 与配置里的 models 求差 →
有差异才 `add-models{prune:true}`（写前备份）。确认卡说明范围；`dump.syncAll` 暴露
`busy/done/total/changed`。入口是搜索框右端的「刷新同步」。

**回退链只选不填**：`sources` 去掉"手填 provider + base_url"首项；模型字段整块作为下拉命中区，
选定供应商后自动展开模型列表；`SubmitFallback` 强制 `from_custom`；`fbProvider_/fbUrl_/
fbKeyEnv_/fbKey_` 与原生绑定全部删除。

**点击穿透（一条根因 + 三个具体场景）**：命中测试按"控件注册顺序"取第一个命中，而注册顺序
≠ **绘制层级**。
1. 底栏/标题栏/模态是最后画的，却注册得晚 → 页面里滚出视口的控件先被命中（"点设置点到下边按钮"）。
   修：`ControlAt` 先在外壳控件里找（底栏/标题栏/状态/模态/下拉/搜索框），页面控件必须落在内容区。
2. 回退链折叠卡画在阶梯之上，但控件注册在阶梯行之后 → 卡片的 × 命中阶梯的 ↑↓×（"收起按钮穿透"）。
   修：折叠块控件整体 `std::rotate` 到控件表最前（模型整块命中区插在折叠块首位，避免被旋转带走）。
3. 搜索框是整行大矩形，注册在右端两个按钮之前 → ✕/同步按钮的点击被搜索框吃掉。
   修：命中顺序改为 ✕ → 同步 → 搜索框，并把三者几何分开（✕ 右-114、按钮右-92、文字区更左）；
   原生 EDIT 右边界让到 140 DIP，避免盖住按钮（真机截图里按钮曾只剩一条缝）。
4. 页内滚动条在控件之前接管 → 右缘按钮点了变成"拖滚动条"。修：先看有没有控件命中。

**顺带修的崩溃**：供应商列表为空时 `std::clamp(input.source,0,size()-1)` 变成 `clamp(0,0,-1)`
（违反 `lo<=hi` 前置条件），Debug 版直接断言中止（0xc000041d，崩溃转储为 ucrtbased）。
改为空列表时取 0。这个崩溃只在"实时 WSL、无夹具"的场景（motion 门禁）复现。

**验证**：`-Gate All` 20/20（`evidence/20261023-021439-110-All.txt`）；控制面脚本
`verify-clickthrough.ps1`：折叠卡 × 关闭后链长度不变（0→0）、同步按钮弹出
「刷新并同步全部供应商的模型列表？」且取消后不写；真机截图 `evidence/final-search.png`。

**产物**：`dist/` 与真机 `dist-live/` 同为 SHA256
`42f2354b5b5277bec4651607139ec6cd507ff90d7de72b19fd623cda09cb223a`（实例 PID 16828）。
