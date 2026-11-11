# 当前能力与限制

Terminal、Market、Warehouse、Lab 和 Trading 已实现行情、数据管理、研究与账户执行流程。真实数据源、正式部署和连续仿真运行的验收按[开发计划](plan.md)推进。

## 当前能力

| 应用 | 使用者可以做什么 |
| --- | --- |
| [Terminal](terminal/README.md) | 在 macOS Apple Silicon 或 Linux x86_64 上查看行情、编辑策略与因子、运行研究、管理数据和交易账户；支持中文和英文 |
| [Market](market/README.md) | 接收 CTP 行情，向 Terminal 分发 |
| [Warehouse](warehouse/README.md) | 以 Tushare 为准采集历史数据，公开来源补缺；查看覆盖；发布可固定读取的数据版本 |
| [Lab](lab/README.md) | 对固定的数据与源码运行日线或分钟回测、参数扫描、滚动验证和日线因子评价；查看、比较和复现研究记录 |
| [Trading](trading/README.md) | 发布策略包、启动和恢复实例、现场预热、授权交易、手动委托、暂停新增及平仓；独立于 Terminal、Lab、Market 和 Warehouse 运行 |

## 当前限制

- Market、Warehouse 和 Trading 仅部署在 Linux x86_64。当前没有 Web、移动端、Windows 或独立 CLI 客户端。
- 数据仅来自真实来源。客户端只看到一份数据，来源选择在 Warehouse 内部。
- 回测使用日线或分钟线，因子评价只使用日线。保证金和手续费取固定数据集里交易所每个交易日的结算参数，滑点、委托限额和权重预算由运行配置给出；回测尚未实现完整的涨跌停成交限制或交易所微观结构，组合按单币种处理。
- 历史最小变动价位由 Warehouse 随行情采集[自动补齐](warehouse/README.md#历史最小变动价位)，六家交易所均已接入历史来源。参数缺失或冲突的区间不能回测。
- Trading 仅发送限价单，策略使用固定月份合约；自动换月和多策略目标合并尚未实现。
- 回测排除含不完整分钟的 K 线；Trading 在行情重连后继续聚合，跨越中断的 K 线仍会交付策略。这一差异列入[待定问题](plan.md#待定问题)。
- Trading 重启后必须重新授权；未知委托由使用者核实，不自动重发。操作要求见[交易规则](safety.md)。

下一步及真实验收条件统一见[开发计划](plan.md)，备份操作见[服务与部署](services.md#备份与恢复)。
