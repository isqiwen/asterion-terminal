# asterion-data-pipeline

独立 C++20 / CLI11 程序，读取类型化 CsvImport，使用 CSV 数据插件解析明确的期货合约成交，发布不可覆盖的数据快照与来源记录。存储复用 filesystem-journal 插件。

```sh
cmake --build build/Debug --target asterion-data-pipeline
asterion-data-pipeline --input /absolute/import.pb --directory /absolute/empty-publication
asterion-data-pipeline --inspect --directory /absolute/publication
```

输入规格包含版本 1、源 CSV 的绝对路径、预期 SHA-256 和完整合约规格。发布目录必须预先存在；相同发布重试幂等，不同数据、损坏记录或中断临时文件均拒绝覆盖。源文件删除后，发布物仍可独立验证与读取。

当前最多 10000 笔、源文件 32 MiB，明确拒绝超限，不截断、不排序、不去重或填补。支持独立发布与检查；已接入 Task Service 类型化任务、Agent 派发、Terminal 发布和选择版本，并加入本机与远程 Linux 分发配置。上传任务限 4 MiB，不发送客户端路径。实时行情连接与订阅仍归 market-data。详见 [数据发布](../../../docs/data-publication.md)。

结算表现支持 `--settlement-calendar` 独立导入/检查，使用明确时区、共享交易日模型和独立发布身份；已接入持久化结算表任务、Agent 派发和 Terminal 导入/查看；回测已支持发布版本选择与来源证据绑定。完整格式与边界见 [结算表](../../../docs/settlement-calendar.md)。

新增 `tushare-minutes` 独立下载/检查入口，以及 Task Service 的 `MINUTE_DOWNLOAD` 任务。通过 Tushare 数据插件下载分段、可恢复的实际期货分钟 OHLCV 数据集，独立于逐笔 CSV，不受逐笔 10000 笔上限控制，也不冒充逐笔成交。界面入口、凭据与恢复语义见 [Tushare 期货分钟](../../../docs/tushare-futures.md)。


分钟下载以具体月份合约为单位，通过数据源合约目录确定上市至最后交易日（未到期则至查询截止点）的范围。CLI 不再接受 --start/--end。恢复沿用已保存的截止时间，不静默扩大原数据集。
