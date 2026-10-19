# 目标（引导文字）：审查 → 报告 → 修复 → 二次验收

> **怎么用**：把下面 `====` 之间的整段文字粘到 ZCode 新会话的目标/指令框（或作为本工作区的
> 长期目标）。它自己会去读 `AUDIT-PLAN.md` 并连续执行，中途不需要我回复。
> 更细的检查清单在 `AUDIT-PLAN.md`，这份只是"启动引导"。

====

对 `E:\AI\Zcode\projects\hermes-panel-native` 这个面板做一轮**穷尽式质量工程**：
审查（功能/界面/交互/流畅性/动画/性能/稳定性/数据/安全/文案/与旧版差异）→ 写审查报告 →
据报告写修复文档 → 实施修复 → 二次迭代验收 → 交最终结果报告。
**按仓库根目录的 `AUDIT-PLAN.md` 执行到底**，它是权威计划；本文只是引导。

## 第一步
读 `AUDIT-PLAN.md`（权威计划）、`GOAL.md`、`DESIGN.md`、`PLAN.md`、`PROGRESS.md`，
并加载技能 `win32-d2d-panel`（项目知识与门禁约定）；落盘规则看技能 `workspace-organizer`。

## 怎么推进（不许停）
- 六个阶段连续跑，**不请求确认、不中途汇报**；每完成一项先把结论写进
  `audit/REVIEW-NOTES.md`（审查期）或 `PROGRESS.md`（修复期），再继续下一项。
- 每 10 项 / 2 小时写一次检查点（进度表追加一行，不打断工作）。
- 审查按维度成批走：A 功能 → C 交互 → D 流畅与动画 → B 视觉 → E 性能 → F 稳定性 →
  G 数据 → H 安全 → I 文案可达性 → J 与旧版差异。一个维度做完更新进度表再进下一个。
- 修复一次只改一个根因：构建 → 该项门禁 → 受影响历史门禁，全绿才写记录并继续；
  不绿先查根因（用 `systematic-debugging`），同一问题 3 次失败就回退该项并登记 BLOCKER。
  每批不超过 5 条 findings，批末跑一次 `-Gate Regression`。
- 单条 finding 定位超过 30 分钟：登记 BLOCKER（已绕行）并跳过，别在一个坑里耗一天。

## 阻塞怎么处置（先绕过去，别停下）
- **环境类**（WSL 不可用、网络抖动、供应商 reset、桌面被占用导致合成鼠标失败）：
  换夹具 / 重试一次 / 降级为"可人工验证"，登记 `BLOCKER-xx` 后**继续**。
- **需求类**（需要审美取舍、文案口味）：按 `DESIGN.md` 既有约定或最保守方案办，
  登记 `ASSUMPTION-xx`（写清依据与影响）后**继续**。
- **真阻塞**（缺权限、工具链损坏、目标盘不可写、被硬规则限制）：试两种替代方案，
  仍不行就登记 `BLOCKER-xx（状态：卡住）`，**跳过该项继续做别的**。

## 只有这三种情况停下来问我
1. 需要破坏性/不可逆操作（删数据、对外发布、改 WSL 助手）；
2. 剩余**所有**项都被同一阻塞卡死（给出卡点、已试方案、需要我做什么）；
3. 最终报告写完了（`audit/FINAL-REPORT.md`），我做验收。

## 硬规则（违反即停该项、登记、绕行）
1. 不改 WSL 里的 `hermes-panel-apply.py`（`~/.hermes/config.yaml` 与 `.env` 的唯一写入者）。
2. 不改旧 Electron 面板与其数据（`%APPDATA%\HermesModelPanel`），保持可回退。
3. 开发与验收一律用**隔离数据目录 + 假助手夹具**；真实配置**只读**，真实供应商只允许 GET `/models`。
4. 日志 / 报告 / 截图里**绝不出现** API Key、密码、token。
5. 只写 `E:\AI\Zcode\projects\hermes-panel-native\` 与 `E:\AI\Zcode\tmp\hermes-panel-native\`；路径用 `E:/` 或 `E:\`。
6. 任何"完成/通过"必须附**命令 + 输出摘要 + 证据路径**；禁止无证据断言，`SKIP` 不许当 `PASS`。
7. 一次只改一个根因；不许为了让测试通过去改测试断言（断言本身错要说明理由）。
8. 每个切片结束工程必须可构建；不绿就回退到上一个绿状态。
9. 违反 `DESIGN.md` 不变量的修复**先改设计文档**再改代码。
10. 关键路径必须真机点一遍（`computer-use`），不许只靠控制面合成点击就宣称"交互已验证"。

## 交付物（按 `AUDIT-PLAN.md` §1 落位）
`audit/BASELINE.md`、`audit/REVIEW-NOTES.md`、`audit/REVIEW-REPORT.md`、`audit/FIX-PLAN.md`、
`audit/ACCEPTANCE-R2.md`、`audit/BLOCKERS.md`、`audit/FINAL-REPORT.md`、`audit/evidence/*`；
门禁证据仍在 `tmp/hermes-panel-native/evidence/`。

## 完成定义
- 四份主文档齐全；每条 finding 有复现步骤与证据，每条修复有门禁结果。
- 二次验收**重跑 A–J 全部维度**并给出"审查前 vs 现在"两列（不是只验修复项）。
- `-Gate Regression`、`-Gate Audit`、`-Gate Release` 在最终产物上一遍全绿。
- 无 P0/P1 遗留；P2/P3 遗留都有理由与建议。
- 用最新 Release 重启真机实例，用户点一遍能看出区别。

====

## 备注

- 这份引导与 `AUDIT-PLAN.md` 冲突时**以 `AUDIT-PLAN.md` 为准**（检查清单、模板、门禁命令都在那里）。
- 想缩减范围时，只需在粘贴时补一句，例如"本轮只做 A–D 四个维度，E–J 留到下一轮"——
  其余规则不变，仍然是连续执行、遇阻绕行、不中途汇报。
