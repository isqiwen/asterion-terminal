# asterion-backtest

Agent 按任务尝试启动的回测工作程序，从 Task 领取固定定义，从绑定的 Data 读取并核验固定数据。运行四条单合约规则或三条排序规则，复用组合账本、历史撮合和交易前风控；结果交 Task 验收和保管，worker 不自行确认任务终态。无独立用户 CLI。

策略与实盘共享适用的规则，历史撮合保留自己的时间和成交模型。见[现行架构](../../../docs/architecture.md)、[回测](../../../docs/backtest.md)与[策略](../../../docs/strategies.md)。
