# 基线（阶段 0）

状态：**已建立**（2026-10-19 03:35–03:40，执行者按 `AUDIT-PLAN.md` §4）

## 审查对象（冻结）

| 项 | 值 |
|---|---|
| exe 路径 | `E:\AI\Zcode\projects\hermes-panel-native\dist\HermesModelPanelNative\HermesModelPanelNative.exe` |
| SHA256 | `b113b415d5cdbb3cb8ead1a2f0a307f94ccadb4e213be6b23a819db30ce14216` |
| 大小 | 943,616 bytes（0.90 MiB） |
| 构建时间 | 2026-10-19 02:57:45 +08:00 |
| manifest | `dist/HermesModelPanelNative/manifest.sha256`（exe 哈希与实测一致） |
| 对应 PROGRESS 条目 | G13 之后的当前树（G5–G12 全部门禁绿）；本次审查针对**这一版** |

审查期间若修复产生新包，基线对比仍以本哈希为准；二次验收会同时给"审查前 vs 现在"两列。

## 审查前门禁（全部 PASS）

命令：`powershell -NoProfile -ExecutionPolicy Bypass -File tools/e2e.ps1 -Gate <Gate>`
证据目录：`E:/AI/Zcode/tmp/hermes-panel-native/evidence/`

| 门禁 | 结果 | 关键输出 | 证据 |
|---|---|---|---|
| Unit | PASS | geometry 17,432 / state 32 / log 21 / settings 22 断言，0 failed | `20261019-032529-938-Unit.txt` |
| Widget | PASS | widget 570 / icons 60 断言，0 failed | `20261019-032538-356-Widget.txt` |
| Lifecycle | PASS | 3 passed（重复启动唤醒/有序退出/重启） | `20261019-032539-937-Lifecycle.txt` |
| Pages | PASS | 9/9（h-models/fallback/add/backup/settings/offline/migrate/controls/ime） | `20261019-032553-391-Pages.txt` |
| Shell | PASS | 6/6（shell/tray/hover/drag/motion/dpi） | `20261019-032740-852-Shell.txt` |
| Regression | PASS | 23/23 | `20261019-032830-398-Regression.txt` |
| ReadHelper | PASS | read-helper 27 passed | `20261019-033056-595-ReadHelper.txt` |
| Audit | PASS | full audit 160 passed, 0 failed | `20261019-033107-046-Audit.txt` |
| Release | PASS | 打包 exe 943,616 B，启动 3s 不退出，目录无 DLL | `20261019-033339-429-Release.txt` |

Audit 门禁内部基线（同一次运行）：CPU 展开 1.24% / beacon 1.56% / tag 1.24% / flag 1.56%；
150 轮泄漏循环 GDI +0 / USER −2 / 句柄 0 / 线程 −3 / 内存 +512 KB；2 显示器 × 4 挂件 × 4 边尺寸与贴边全 PASS。

## 审查前性能

| 指标 | 数值 | 方法/证据 |
|---|---|---|
| 冷启动（进程→控制面可应答） | 0.333 / 0.340 / 0.374 s（3 次） | `tmp/hermes-panel-native/audit/measure_populated.py` → `audit/coldstart-populated.json` |
| 冷启动（→ 列表数据到位，非 loading） | 0.360 / 0.363 / 0.397 s | 同上 |
| 窗口创建→可见 | 0.012 s，矩 384×634 DIP | `measure_coldstart.py` → `coldstart.json` |
| 空闲 CPU（展开） | 1.24%（阈值 3%） | Audit 门禁 |
| 空闲 CPU（收起 beacon/tag/flag） | 1.56% / 1.24% / 1.56% | Audit 门禁 |
| 工作集 / 私有字节 | 51,512 KB / 60,768 KB（冷启动后空闲） | `measure_baseline.py` → `baseline-perf.json` |
| GDI / USER / 句柄 / 线程 | 16 / 16 / 371 / 20 | 同上 |
| 打包体积 | 943,616 B（单 exe，无 DLL） | Release 门禁 + `baseline-perf.json` |
| Debug exe | 3,855,360 B | 同上 |

## 审查环境

- 真机面板实例（PID 37468）已在跑门禁前停止，避免置顶窗干扰截图/合成点击；最终阶段会用最新 Release 重启。
- 所有交互/数据检查使用隔离数据目录（`tmp/hermes-panel-native/audit-*`、`baseline-*`）+ 假助手
  `build/Debug/fake-hermes.exe`；真实 `~/.hermes` 仅只读。
- 桌面：2 台显示器（120 / 192 DPI），Windows 11 26300。
