# asterion-task-service

持久化回测任务、队列、执行尝试、进度、取消和确认结果，支持同一输入幂等提交、显式重试、服务重启恢复及旧尝试隔离。

使用现有 filesystem-journal 插件持久化，采用顶层 `protocol/proto/asterion/v1/research.proto`。支持本机 IPC 和 TCP/mTLS。任务服务负责业务任务状态，不直接创建工作子进程；Node Agent 每秒通过私有 IPC 上报存活的工作进程，Task Service 按提交序号选择排队任务并限制每个服务最多两个并发工作进程，返回类型化程序角色。Agent 只启动已部署且摘要通过校验的对应程序。

启动参数与精确边界见 [研究任务说明](../../../docs/research-tasks.md)。Terminal 页面与本机 Agent 调度已接入，Linux x86_64 部署与 macOS 打包已验证，独立机器网络与 Windows 原生仍待验收。

已接入 `MINUTE_DOWNLOAD`：Task Service 持久化不含凭据的查询定义，Token 独立保存于任务目录受当前账户保护的文件，Worker claim 通过本机 IPC 获取运行所需凭据；普通列表与结果不返回 Token。Agent 启动 data-pipeline 的 `--minute-download` 模式，工作进程心跳独立于网络请求；恢复和数据边界见 [Tushare 分钟下载](../../../docs/tushare-futures.md)。

已完成分钟任务支持 MinutePageQuery：按任务 ID、时间区间和行偏移读取最多 200 根 Bar，不开放路径读取。取得任务和结果快照后释放状态锁再校验和读取数据；分页结果保留精确 Decimal。非完成任务、无效分页和被修改的清单／当前分段均拒绝。

分钟查询 `include_macd` 为真时，在共享任务锁外校验并扫描从数据集起点至目标页的源记录，调用 `plugins/tools/chart_indicators` 的展示算法，返回可选 `MinuteBar.macd`。预热期缺值，损坏的前序段拒绝整个请求。每次扫描释放已消费的源段，不缓存整份历史；冷查询延迟随前序记录数量增长，尚无持久指标缓存。任务列表的 `minute_interval_minutes` 是从原任务定义投影的周期摘要，不改变已有任务存档。

分钟数据读取使用非阻塞共享文件锁：检查清单、研究读取与多个图表分页可以同时读取同一数据集；下载／断点续传仍使用独占锁。写入持锁时查询明确失败，不等待或读取未完成的修改；读者退出后写锁可以重新获取。摘要、完整性、分页和 MACD 起点校验保持不变。Core 只提供通用共享／独占锁机制，由数据流水线选择读写模式。
