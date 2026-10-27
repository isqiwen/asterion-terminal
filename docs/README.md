# 文档导航

| 问题 | 文档 |
| --- | --- |
| 产品是什么、给谁用、不做什么 | [产品说明](product.md) |
| 交易必须遵守哪些规则 | [交易安全契约](safety.md) |
| 往哪走、先做什么、做到哪了 | [评估与路线](reviews/project-assessment-2026-10-28.md) |
| 现在能做什么、有哪些限制 | [现状](status.md) |
| 系统怎样组成、状态归谁 | [架构](architecture.md) · [Node Manager](architecture/node-manager.md) |
| 怎样构建、运行与测试 | [开发指南](development.md) |
| 界面与交互 | [Terminal](terminal.md) |
| 行情与历史数据 | [行情与数据](market-data.md) |
| 研究对象与流程 | [研究](research.md) · [回测](backtest.md) · [策略](strategies.md) · [因子](factors.md) · [任务](tasks.md) |
| 交易与风控 | [交易](trading.md) · [CTP 仿真验收](acceptance/ctp-simulation.md) |
| 节点、部署与日志 | [服务](services.md) · [原生插件](native-plugins.md) |

约定：

- 产品说明和交易安全契约定义目标与不变量；评估与路线定义顺序和状态；其余主题文档只描述当前代码。
- 主题文档跟代码走：替换或删除代码的同一提交里更新或删除对应文档，不保留旧版本或归档副本。
- 过程记录写在提交说明里，不写进文档。
