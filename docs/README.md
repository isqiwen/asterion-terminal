# 文档导航

目标设计、当前契约和验证证据分别维护，避免用规划冒充已实现能力。

## 从这里开始

| 阅读目的 | 文档 |
|---|---|
| L0–L5 层级、语言、单层插件与稳定接口 | [架构](architecture.md) |
| 目标目录、包边界、当前代码归属 | [工程结构](engineering-structure.md) |
| 当前能力、缺口和下一步 | [进度](ROADMAP.md) |
| 期货概念和 F1–F6 完成条件 | [领域基线](futures-domain.md) |
| 构建、调试、测试、生成类型 | [构建与开发](development.md)、[贡献指南](../CONTRIBUTING.md) |
| 验证结果及实际覆盖范围 | [验收索引](VALIDATION.md) |

## 当前契约与使用

| 主题 | 文档 |
|---|---|
| 宿主、任务与通信 | [协议入口](protocols.md)、[内置贡献](builtin-contributions.md)、[内部通信](internal-communication.md) |
| 扩展 | [插件包与 SDK](plugin-system.md)、[策略插件](strategy-plugins.md) |
| 本机运行与身份 | [桌面运行](desktop-runtime.md)、[账户与锁定](identity.md)、[备份恢复](backup-and-restore.md) |
| 数据 | [生命周期](data-lifecycle-design.md)、[数据源](data-providers.md)、[导入与来源连接](data-import-and-connections.md) |
| 期货资料 | [合约目录](reference-data.md)、[交易时间](trading-time.md)、[交易规则](contract-rules.md)、[角色](contract-roles.md) |
| 研究 | [当前回测模型](local-backtesting.md)、[实验](research-experiments.md)、[后续区间验证](research-validation.md)、[逐日结算](research-settlement.md) |
| 行情与账户接入 | [连接契约](broker-connections.md)、[CTP/SimNow 使用](simnow.md) |
| 终端体验 | [交互规范](terminal-experience.md)、[UI 规范](ui-style.md)、[Fincept 观察](fincept-interaction-review.md)、[Fincept 数据参考](fincept-data-audit.md) |
| 开源交付 | [开源准备](open-source.md) |

当前 OpenAPI 与参考 schema 为生成/契约文件，不在说明文档中手工复制字段表。

## 维护规则

- 一项事实只保留一个权威定义；其他页面链接过去，不复制架构图、状态表或测试数字。
- 架构和工程页描述目标，专题说明当前可执行契约；未实现差距明确写出。
- 已被替代的设计和操作步骤直接删除；有价值的执行证据保留在 validation，不能再充当开发指引。
- ROADMAP 维护当前状态，VALIDATION 只做索引，详细执行事实按专题或历次记录保存。
- 更新和删除文档时同时修复入链、锚点及代码引用。文档更新不等于代码重构或功能验收完成。
