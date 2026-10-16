# asterion-data-pipeline

Agent 按任务尝试启动的历史分钟下载与全市场数据更新工作程序。向 Task 领取任务和报告进度，向 Data 领取固定授权与供应商共享额度，通过动态数据插件获取真实数据。

分钟下载由 worker 准备候选，Task 持久裁决发布，Data 验证并公开版本；表更新则由 Data 逐分区校验和入库，取消或失败不撤回已发布分区。worker 不拥有仓库目录或任务终态，无独立用户 CLI。见[现行架构](../../../docs/architecture.md)、[行情与数据](../../../docs/market-data.md)和[任务](../../../docs/tasks.md)。
