# 文档导航

`requirements/` 从用户目标出发定义产品需求、业务规则、案例和验收条件；需求确认状态不代表实施进度。其他主题文档描述当前实现和使用限制；`reviews/` 保存按日期和源码基线划分的设计、审计及验收证据。历史文档中的“下一步”和测试数字不自动代表当前状态。

| 问题                                      | 当前文档                                                                                                                                          |
| ----------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| 为谁解决什么问题、具体需求与预期案例      | [产品需求总纲](requirements/README.md)、[图文阅读版](requirements/requirements.html)                                                              |
| 策略构建、模型训练、组合仓位与独立回测    | [分析](requirements/modules/analysis/README.md)、[策略](requirements/modules/strategy/README.md)、[回测](requirements/modules/backtest/README.md) |
| 数据模块职责、采集加工、存储查询与案例    | [数据模块详细设计](requirements/modules/data/README.md)                                                                                           |
| 公告与资讯结构化、LLM处理、核验及事件时点 | [情报](requirements/modules/intelligence/README.md)                                                                                               |
| 配置、定时调度、日志诊断与通知分工        | [系统管理](requirements/modules/system/README.md)、[公共通知规范](requirements/common/interaction.md#req-com-ux-02)                               |
| 目标界面、字段按钮、交互行为与操作流程    | [目标界面浏览](requirements/prototype.html)、[交互规格](requirements/common/screens.md)、[操作旅程](requirements/workflows/README.md)             |
| 项目范围、首次启动                        | [项目首页](../README.md)、[开发指南](development.md)                                                                                              |
| 层次、进程、状态归属、插件方式            | [架构](architecture.md)                                                                                                                           |
| 按需求重组模块、进程、配置与工程目录 | [目标架构设计](reviews/architecture-design-review.md)、[逐功能责任映射](reviews/requirements-architecture-map.md) |
| 已实现能力、限制与下一步                  | [现状](status.md)                                                                                                                                 |
| 界面、UI 贡献与命令                       | [Terminal](terminal.md)                                                                                                                           |
| 原生 SDK、安装、版本选择与任务绑定        | [原生插件](native-plugins.md)                                                                                                                     |
| 实时观测、历史版本、表与来源证据          | [行情与数据](market-data.md)                                                                                                                      |
| 账户授权、风控、订单与核对                | [交易](trading.md)                                                                                                                                |
| 任务、固定输入、结果、导出和恢复          | [任务](tasks.md)                                                                                                                                  |
| 回测、策略与因子语义                      | [回测](backtest.md)、[策略](strategies.md)、[因子](factors.md)                                                                                    |
| 节点部署、资源与运行管理                  | [服务](services.md)                                                                                                                               |
| 真实柜台验收条件                          | [CTP 仿真验收](acceptance/ctp-simulation.md)                                                                                                      |
| 设计取舍与有基线的验收记录                | [评审索引](reviews/README.md)                                                                                                                     |

模块 README 只解释该模块职责，并链接这里的完整契约；不复制状态页或阶段日志。
更新实现时同步生产者、消费者、测试和对应主题文档。历史失败及复验记录保留，不把新结果覆盖到旧基线。

