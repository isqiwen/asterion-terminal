# 验收索引

各记录只证明对应日期、构建和范围内的行为；不代表当前工作区重新跑过。当前能力见 [ROADMAP](ROADMAP.md)，逐次测试数字和日志位置保存在 [历次执行记录](validation/run-history.md)。

## 专题证据

| 范围 | 记录 |
|---|---|
| 机制与连接行情迁移 | [数据库/任务/归档/连接/行情回归检查点](validation/run-history.md#2026-09-22-rust-机制与连接行情迁移) |
| 分层替换 | [Rust、独立 UI 包与安装后全链验证](validation/run-history.md#2026-09-22-rust-分层替换) |
| 机制与插件 | [核心插件](validation/core-plugins.md)、[当前契约拒绝](validation/current-contract-cleanup.md)、[通信与运行诊断](internal-communication.md) |
| 身份与领域审查 | [F1 身份链](validation/f1-identity.md)、[安装与领域审查](validation/install-and-domain-review.md) |
| 数据工作流 | [导入与来源](validation/data-workflow.md)、[研究准备](validation/research-preparation.md)、[补齐恢复](validation/research-refill-recovery.md)、[独立同步身份](validation/standalone-sync-identity.md) |
| 数据规模 | [月分区增量](validation/history-merge.md)、[固定版本扫描](validation/history-scan.md)、[多合约历史下载](validation/history-downloads.md)、[分钟下载](validation/minute-downloads.md) |
| 规则与结算 | [规则插件](validation/contract-rules.md)、[Tushare 资料映射](validation/tushare-rules.md)、[Tushare 结算](validation/tushare-settlement.md) |
| 真实来源 | [Tushare 在线链路](validation/tushare-live.md)、[可重复验收工具](validation/repeatable-tushare.md)、[角色发布链](validation/role-publication-sequence.md) |
| 研究与恢复 | [有限回测模型](validation/local-backtesting.md)、[备份恢复](validation/backup-and-restore.md) |
| 桌面与交互 | [Fincept 观察与边界](fincept-interaction-review.md)、[桌面运行范围](desktop-runtime.md)、[历次执行记录](validation/run-history.md) |

历史日志中已经改变的页面、路径和实现不作为开发规范；当前协议见 [协议入口](protocols.md)，目标职责见 [架构](architecture.md)。测试日志里的失败和未验证范围应与通过结果一起读取。

## 如何记录新验证

- 专题记录写清日期、代码/构建、环境、真实/合成来源、执行项、结果和未验证范围。
- 有已有专题时更新其当前结论；独有的逐次执行事实放入 run-history，不继续扩充本索引为流水账。
- 单元测试、浏览器模拟、原生安装包、真实数据源、模拟交易、实盘分别报告，不能互相代替。
- 不把文档修改、类型存在或旧测试数量当作新架构完成证据。
