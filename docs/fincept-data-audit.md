# Fincept 数据管理源码核对

核对日期：2026-09-17。对象：Fincept-Corporation/FinceptTerminal 公开仓库，固定提交 `09b70f3bc5c751d0e9507cb877de0270445034fb`。以下是静态代码核对，不是对用户本机安装版的操作验收；不覆盖闭源 Enterprise，也不把 README 功能宣传当作实现证据。

## 结论

Fincept 已有连接器、字段标准化及原始响应留存、持久化历史行情、文件索引、工作流、缓存与清理。之前只谈连接器不够全面。

在本次检查的存储表、标准化路径、历史行情路径和工作流路径中，没有确认一套贯穿这些组件的“不可变数据版本 → 固定输入的加工任务 → 发布快照 → 研究引用保护”的闭环。这里是限定范围的结论，不声称整个项目或私有版本完全没有数据治理能力。

因此可以参考它的组件分工，但不能将它直接当作 Asterion 可复现量化数据仓库的完成模板。

## 已核对的实际行为

下列链接均固定到本次提交，可复核同一份代码。

| 范围 | 代码中的行为 | 对 Asterion 的意义 |
|---|---|---|
| 数据接入 | ConnectorRegistry 注册连接器；FileSources 声明 CSV、Excel、JSON、Parquet 等文件来源及参数 | 文件与远端数据采用统一接入入口；声明支持格式不等于已经完成我们所需的批量发布协议 |
| 标准化 | 映射配置经 JSONPath 提取、转换、默认值及 schema 校验，运行结果存 `normalized_data` | 应提供明确的字段映射、单位、schema 和质量报告 |
| 原始留存 | 同一标准化记录保存 raw_json、normalized_json、validation_errors、SHA-256、mapping_id、source_id 和采集时间 | 原始证据与处理结果关联值得借鉴；不是每次 API 调用都自动成为完整研究数据版本 |
| 历史行情 | SQLite `market_data` 按 symbol/exchange/interval/timestamp_ms 存 OHLCV+OI；支持区间读取、时间聚合、目录汇总 | 历史数据应按数据系列组织，而非按“下载了第几个文件”组织 |
| 自动采集 | `refresh_watchlist()` 通过已连接 broker 获取行情；主程序每 15 分钟调用。有自选系列且 broker 连通才执行 | 不是单纯 TODO；但固定回看区间并不能替代精确缺口规划 |
| 文件管理 | FileManagerService 将文件复制进受管目录，metadata.json 记录 ID、原文件名、大小、类型、时间和来源界面 | 文件管理器解决查找文件；其元数据不足以表达 schema、数据覆盖、血缘和研究引用 |
| 工作流 | 保存节点与边定义，执行时传递上游结果；有运行审计、成功/失败事件以及 WorkflowCache | 有流程图/日志不代表已经固定输入数据版本、代码及环境 |
| 业务数据分发 | DataHub 为进程内按 topic 发布/订阅，支持 TTL、刷新调度、请求合并和空闲清理 | 避免多面板重复请求值得借鉴；这里的 topic 最新值不是历史数据快照 |
| 缓存及清理 | CacheManager 使用独立 cache.db；StorageManager 有分类统计、清理和定期保留策略 | 缓存必须与权威历史数据分开；不能用缓存清理规则删除研究输入 |

证据：

1. [ConnectorRegistry](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/screens/data_sources/ConnectorRegistry.cpp)、[FileSources](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/screens/data_sources/connectors/FileSources.cpp)。
2. [标准化处理及持久化](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/services/data_normalization/DataNormalizationService.cpp)、[标准化表结构](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/sqlite/migrations/v012_data_normalization.cpp)、[界面运行入口](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/screens/data_mapping/DataMappingScreen_Operations.cpp)。
3. [历史行情实现](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/HistoricalDataStore.cpp)、[行情表结构](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/sqlite/migrations/v033_historify.cpp)、[主程序定时接线](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/app/main.cpp)。部分历史注释仍说 TODO，以 `.cpp` 实际调用为准。
4. [文件管理](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/services/file_manager/FileManagerService.cpp)、[文件记录字段](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/services/file_manager/FileManagerService.h)。
5. [工作流定义存储](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/repositories/WorkflowRepository.cpp)、[执行器](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/services/workflow/WorkflowExecutor.cpp)、[工作流缓存](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/services/workflow/WorkflowCache.cpp)。
6. [DataHub 接口](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/datahub/DataHub.h)、[DataHub 实现](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/datahub/DataHub.cpp)。
7. [缓存](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/cache/CacheManager.h)、[存储与清理](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/StorageManager.cpp)。

## 不能直接当作研究可复现保证的地方

### 历史记录可以覆盖，表键不含提供方和版本

`store_candles()` 使用 INSERT OR REPLACE，复合键是合约/交易所/频率/时间。表中没有 provider、数据版本或 available_at。相同键再次写入会取代原值；这一条路径不能单独还原修订前的研究输入，也不能单凭主键区分多来源同一根 K 线。

这不是说 SQLite 天生不能版本化，而是该具体模型采用了最新值语义。Asterion 需要追加修订和固定 manifest，不静默覆盖历史快照。

### 原始响应加 hash 不等于完整血缘

normalized_data 有 mapping_id，但 [DataMappingRepository](https://github.com/Fincept-Corporation/FinceptTerminal/blob/09b70f3bc5c751d0e9507cb877de0270445034fb/fincept-qt/src/storage/repositories/DataMappingRepository.cpp) 对同一 ID 更新映射内容，没有在该表中保留映射修订历史。仅凭 mapping_id 不能保证重建当次处理规则。

此外，normalize_raw 使用 raw.object() 保存原始内容，同时允许以数组作为提取输入。因此该实现不能被我们直接描述为“任意响应形态均逐字节原样留存”。Asterion 应独立保留响应/导入证据，再引用固定的映射配置与代码版本。

### 文件删除与清理不是引用保护

FileManagerService::remove_file 直接删除文件和索引项；核对到的这条路径没有检查研究快照引用。标准化表对 mapping_id 配置了级联删除，StorageManager 也有删除标准化数据与映射的操作。不能将其等同于受研究引用保护的版本回收。

### 接口名称不代表真实存储格式

HistoricalDataStore::export_parquet 在此提交实际调用 export_csv 并告警。这只说明该历史行情导出函数的实现，不能推断所有 Fincept 模块都不能处理 Parquet。Asterion 已有真实 PyArrow Parquet 发布能力，应保留现有实现，不因参考界面而退回这种兼容方式。

## Asterion 采用与不采用

采用：来源能力声明；原始与标准化结果的关联；按数据系列组织历史行情；通用字段映射；业务订阅与缓存；统一存储统计。

补建：逻辑 Dataset、不可变 Version、分区 manifest、固定处理输入与代码、完整性/覆盖证据、修订历史、跨层血缘、研究引用保护与可验证备份。

不照搬：行情覆盖写作为唯一权威存储；将缓存等同数据仓库；靠可修改的 mapping_id 代替处理版本；依据文件名或按钮名判断产物格式。

落地设计见 [Asterion 数据生命周期设计](data-lifecycle-design.md)。本文只核对和设计，没有更改运行中的数据库与用户数据。
