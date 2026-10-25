# 文档导航

| 问题 | 文档 |
| --- | --- |
| 产品要解决什么、需求与验收 | [产品需求](requirements/README.md)（运行 `pnpm run docs:requirements` 生成图文阅读版与界面浏览） |
| 现在能做什么、有哪些限制 | [现状](status.md) |
| 系统怎样组成、状态归谁 | [架构](architecture.md) · [目标架构](architecture/target.md) · [数据身份与所有权](architecture/data-paths.md) · [Node Manager](architecture/node-manager.md) |
| 怎样构建、运行与测试 | [开发指南](development.md) |
| 界面与交互 | [Terminal](terminal.md) |
| 行情与历史数据 | [行情与数据](market-data.md) |
| 研究对象与流程 | [研究](research.md) · [回测](backtest.md) · [策略](strategies.md) · [因子](factors.md) · [任务](tasks.md) |
| 情报 | [情报](intelligence.md) |
| 交易与风控 | [交易](trading.md) · [CTP 仿真验收](acceptance/ctp-simulation.md) |
| 节点、部署与日志 | [服务](services.md) · [原生插件](native-plugins.md) |
| 正在进行的整改 | [评估与重构计划](reviews/README.md) |

约定：需求正文定义目标行为；主题文档描述当前实现；`reviews/` 只保留仍在使用的评估与计划。
文档与代码同步修改，过程记录不写入文档（Git 提交说明即可）。
