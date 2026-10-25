# 死代码排查与代码梳理（2026-10-21，用户要求）

> 目标：找出"软件里没用的死代码"并做一次梳理。方法、逐条证据、以及**故意保留**的东西都写在下面。
> 工具脚本在 `tmp/hermes-panel-native/diag/`（`deadcode-survey.py`、`unused-fields.py`、`duplication.py`、
> `content-diff.py`、`ab-capture.ps1`），脚本只做统计、从不改动源码。

## 1. 方法

工程已经用 `/W4 /WX` 构建，所以**编译器能看到的死代码早就是 0**：未使用的局部变量、未被引用的静态
函数、未使用的形参、部分不可达代码都会直接变成编译错误。剩下的是编译器看不见的语义死代码，用三个
自制调查 + 一次引用检查覆盖：

| 手段 | 查什么 |
|---|---|
| 声明普查（`deadcode-survey.py`） | 头文件里声明、全仓只在声明处出现的字段/常量/枚举值 |
| 成员访问普查（`unused-fields.py`） | 只统计 `[.>]名字` 这种**成员访问**，避免 JSON 键/注释假冒"用过" |
| "只在头文件出现"普查 | 类成员走裸名字访问，所以在类头文件上按"是否出现在别的文件"判定 |
| 重复块扫描（`duplication.py`） | 忽略空白与注释后 ≥6 行的重复块 |
| 目标引用检查 | CMake 目标 / tools 脚本是否被门禁、测试、脚本或文档引用 |
| 像素 A/B（`ab-capture.ps1` + `content-diff.py`） | 重构类改动的输出是否**逐像素一致**（同会话背靠背抓图，带噪声底） |

## 2. 删掉的死代码（8 项，全部"没有任何读取方"）

| 项 | 位置 | 证据 |
|---|---|---|
| `Application::helper_` | `src/app/application.h` | 全仓只有声明（各调用点都建局部 `HelperBridge`），一个完整未使用的成员对象 |
| `Application::hotLoadedMtime_` | 同上 | 只有声明；真实值在 `stored_.hotLoadedMtime`（注释也写着"leave-behind"） |
| `Application::uncCacheValid_` | 同上 | 只在 `application_data.cpp` 写 true、无人读；真正的判据是 `!uncCacheListJson_.empty()` |
| `HelperBridge::lastReadStart_` | `src/data/helper.h` | 只有声明 |
| `FallbackPageInput::cursorField` | `src/app/panel_view.h` | 只有声明；实际焦点是 `PanelViewModel::focusedFormField` |
| `ModalView::scrimCloses` | 同上 | 只有声明；遮罩语义是固定的（只有取消/Esc 关，点遮罩吞掉） |
| `WidgetMetrics::gap` | `src/core/geometry.h` + `geometry.cpp` | 两处赋值、**零读取**（灯的间距用自己的 `kLampGap`） |
| `widgets::kGroupHeadHeight` | `src/render/widgets.h` | 零引用（同组的 `kGroupHeadPadX/kGroupHeadGap/kCaretBox` 都在用） |

**验证**：`cmake --build build --config Debug` → 0 error / 0 warning；`-Gate Regression` 20/20、
外加 `unc-parity`/`a-c1` 的 22/22 全绿（`gate-Regression-tidy.log`）。

## 3. 梳理：`Widen` / `Narrow` 三份合成一份

产品侧原来有三份逻辑相同的实现：`app/application_internal.h`（两份都在这）、`data/settings.cpp`（Narrow）、
`render/panel.cpp`（Widen）。逐行核对过**三份等价、没有行为漂移**（都是 `CP_UTF8` + 空串特判），
但这正是编码类 bug 的温床——在其中一个里修，另外两个照旧错（本项目被 CJK 源码/`cp936`/"逐字节加宽"
坑过多次，DESIGN v0.8 的记录里就有）。

新增 `src/core/text.h`（`inline` 两份函数），三个 TU 改成 include 它；产品侧现在**只有一份定义**。
测试与工具各自的夹具副本（7 处）保持不动——它们是独立编译的夹具，合并价值低、改动面大。

**重构成不改变输出的证明**：把改动前的 exe 存下来，用同一隔离实例**背靠背**抓图（同一会话、同一桌面）：

```
噪声底（同一个 exe，相隔 0.9s 两次抓图）: 0/26784 px 变化（最大通道差 3）
改动前 vs 改动后（左贴边铭牌·白瓷）:      0/26784 px 变化（最大通道差 3 / 0）
```

证据：`evidence/ab-before-{a,b}.png`、`evidence/ab-after-{a,b}.png`。

## 4. 删掉两个零引用的调试目标（顺带每次构建少编两个程序）

| 目标 | 说明 |
|---|---|
| `environment-probe`（`tests/harness/environment.cpp`） | 打印显示器清单的一次性小工具；门禁/测试/脚本/文档**零引用**，而且同样的信息 `geometry-probe monitors` 就能拿到 |
| `drag-repro`（`tools/drag_repro.cpp`） | CMake 注释写着"一次性 drag 复现，留到 H2c 清掉"——H2c 早已完成，零引用 |

两个文件与 CMake 目标一起删除（git 历史仍在）。构建后 `build/Debug/*.exe` 36 个、0 警告。

## 5. 查到但**故意保留**的（附理由，不是漏网）

| 项 | 为什么留 |
|---|---|
| `layout.busy`（`src/render/layout.h`） | 产品侧确实没人画它了（真机要求把"正在读取…"文字换成旋转图标+顶部流动条），但 `tests/widget_tests.cpp` 有两条断言在读它，而且 **DESIGN §4.2 #5 仍把"忙碌条=细字"写成契约**。删它属于改设计契约 → 按仓库规则要先改 DESIGN、再删字段与那两条断言。**建议**：把 §4.2 #5 更新为"顶部不定进度条 + 同步章旋转"，然后一次清掉字段+断言 |
| 测试/工具里的 7 份 `Widen/Narrow` | 独立编译的夹具副本；合并要动 7 个文件且收益低 |
| `tools/` 里的一次性迁移脚本（`cut-balance-commands.py`、`patch-application-hermes.py`、`patch-shell-leftovers.py`、`rename-from-balance.py`、`strip-balance-content.py`、`coexist-check.ps1`） | 无任何门禁引用、本身已不可重跑（当年是"改一次就完"的脚本），但它们记录了 balance→hermes 的搬运过程。**建议**：想清目录就移到 `tools/one-off/`，不要直接删 |
| `tools/wsl-probe.cpp`、`tools/widget-gallery.cpp` | 手工诊断/出图工具：`widget-gallery` 是 DESIGN §4.10 证据表里的截图来源（`widget-gallery-*.png`），`wsl-probe` 在 PROGRESS 里有使用记录 → 属于"按需现场用"，保留 |

## 6. 还剩的重复值得下一步做（本次没动，各附理由与验证方式）

1. **`render/layout.cpp` 的下拉滚动条几何写了两遍**（`1003-1020` 与 `1348-1365`，约 20 行 ×2）。
   抽一个 `ScrollbarGeometry(...)` 能少 ~20 行、只有一个地方要改。
   验证：`geometry`/`widget`/`h-pages` 门禁 + 两个下拉的截图对照。
2. **`render/surface.cpp` 的 D2D/DWrite 工厂初始化写了两遍**（`PaintStrip` 与 `PaintHoverCard` 各 3 行）。
   抽 `EnsureFactories()`。验证：门禁 + 贴条/信息卡截图。
3. **挂件玻璃管线里的渐变笔刷样板**（`render/panel.cpp`，body/contact/两道 rim/sweep 共 5 段，
   每段都是 `CreateGradientStopCollection` + `CreateLinearGradientBrush` + `FillGeometry` 的样板，
   约 60 行可压到 ~20 行）。重复块扫描抓不到它（变量名不同，精确匹配看不见）。
   **本次没动**：玻璃刚做过逐像素验收，动它就得再跑一次 A/B（方法已就绪：§3 的背靠背抓图）。

## 7. 结构观察（供参考，不构成缺陷）

- 规模：`src/app` 8232 行、`src/render` 6443、`src/data` 1830、`src/core` 457（本次加了 `text.h`）、
  `tests` 8853、`tools` 1355；单文件最大：`application.cpp` 2646、`layout.cpp` 2403、`panel.cpp` 2173。
- `app` 层已经按职责拆过（`application_{data,pages,control,input}.cpp`），这个方向是对的；
  `application.cpp` 里还堆着窗口过程 + 托盘 + 拖拽 + 热键（约 600 行），若继续长，下一步就按这个缝拆。
- `src/core` 很薄（几何/状态/设置/文本），说明核心逻辑没被 UI 污染 ✓；`render` 的两个大文件主要是
  "一页一个绘制函数"的写法，属于可接受的取舍。

## 8. 本轮用到的命令（可复跑）

```powershell
cmake --build build --config Debug --parallel                       # 0 warning / 0 error
powershell -File tools/e2e.ps1 -Gate Regression -Configuration Debug # 20/20
ctest --test-dir build -C Debug -R "^(...|unc-parity|a-c1)$"         # 22/22
python tmp/hermes-panel-native/diag/deadcode-survey.py               # 声明普查
python tmp/hermes-panel-native/diag/duplication.py                   # 重复块扫描
powershell -File tmp/hermes-panel-native/diag/ab-capture.ps1 -Exe <exe> -Out <png 前缀>  # 像素 A/B
```

## 9. 梳理后的产物与门禁（2026-10-21 收尾）

- 打包：`dist/HermesModelPanelNative/HermesModelPanelNative.exe` SHA256
  `5dae7b43a3f3b18ffac0aee6a8d43c025c323f2990116a7be8513ff564822f58`；真机实例跑的是同一份字节的
  `dist-live/HermesModelPanelNative.exe`（已核对 hash 一致，实例 PID 63312 启动于 18:35，日志正常）。
- 门禁（都在梳理后的树上跑过）：
  - `-Gate Regression` 20/20，exit 0（`gate-Regression-tidy.log`）
  - 含 `unc-parity`/`a-c1` 的扩展集 22/22，exit 0
  - `-Gate Pointer` **10/10，exit 0**（`gate-Pointer-tidy.log`：hover/drag/a-snap/a-pullout/a-hover/
    a-edge/a-crossdpi/a-topology/h-ime/dpi）——本轮改动虽属重构，真机交互仍重新验过一遍
  - `cmake --build build --config Debug --parallel`：0 error / 0 warning
- 像素 A/B（§3）：噪声底 0 px，改动前后 0/26784 px。
