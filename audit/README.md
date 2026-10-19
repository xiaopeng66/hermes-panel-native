# audit/ —— 审查与验收产物

本目录只放"审查 → 修复 → 二次验收"这条线的产物；门禁证据仍在 `tmp/hermes-panel-native/evidence/`。

| 文件 | 内容 | 谁写 |
|---|---|---|
| `BASELINE.md` | 审查对象（exe 哈希/大小/构建时间）+ 审查前门禁与性能基线 | 阶段 0 |
| `REVIEW-NOTES.md` | 逐项审查记录（边审边写） | 阶段 1 |
| `REVIEW-REPORT.md` | 审查报告（findings + 通过项 + 对比） | 阶段 2 |
| `FIX-PLAN.md` | 修复文档（每条 finding 一条 FIX） | 阶段 3 |
| `ACCEPTANCE-R2.md` | 二次验收（A–J 全部维度：审查前 vs 现在） | 阶段 5 |
| `FINAL-REPORT.md` | 最终报告（完成/未完成、差异、阻塞、待用户决定） | 阶段 6 |
| `BLOCKERS.md` | 阻塞与假设登记（含绕行方式） | 全程 |
| `evidence/` | 审查期截图/对照图/录屏帧 | 全程 |

规则见仓库根的 `AUDIT-PLAN.md`。
