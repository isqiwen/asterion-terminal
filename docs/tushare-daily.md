# Tushare 期货日线接入

## 当前状态

正在接入期货日线，目标是通过现有数据源工作台、Task Service 与独立数据流水线下载月份合约的官方日线，并在 Terminal 共享图表中查看。

已实现 Core 的 `HistoricalDailyBar`／`HistoricalDailyRange`／`HistoricalDailyPort` 和 Tushare `Daily` 数据插件。模型保留供应商交易日期、OHLC、成交量、成交额、持仓量，以及可缺失的昨收、昨结算和当日结算。插件执行真实 HTTPS 请求时复用既有固定域名、证书校验、响应大小限制与取消机制；离线测试通过注入明确供应商响应验证解析。

已实现独立的日线分段存储与下载恢复库，接口返回类型化数据集和元数据；日期严格以 `YYYY-MM-DD` 持久化，金额保留八位定点字符串，可缺失价格保留 null。分段先持久化再推进清单，恢复验证已有分段与请求身份，多个读者共享锁、下载独占锁。

已接入独立 `DAILY_DOWNLOAD` Protobuf 任务、Task Service 持久化状态与结果、Agent 的数据流水线调度，以及 `--daily-download` 托管进程模式。日线按分段计进度，凭据通过受保护文件与私有工作通道传递，不进入任务日志。取消、重试和恢复复用现有尝试围栏；结果必须属于任务指定目录且完整验证。

已实现日线分页服务与协议边界校验，支持交易日期筛选、最多 200 条分页，以及从数据集起点计算的显示用 MACD。查询在任务状态锁外读取数据，完整验证来源分段；小于二十年的日线数据有界读取。

已接入 Terminal C++ 原生桥的 `research.daily.submit`／`research.daily.page`，范围来自本次已加载目录的合约生命周期；旧目录截止标识拒绝提交。日线查询与分钟查询共用并发读取机制，普通快照的 `daily_page` 固定为空，响应只属于调用方；查询期间切换研究服务时拒绝旧服务结果。

已接入数据源工作台的「Tushare · 日 K 线」、下载任务、精确数据查看器及行情日 K。只有所选合约存在已完成日线数据时启用日 K；其余未接入周期仍禁用。独立交易日期筛选、可缺失昨收／昨结／结算字段、分页及 MACD 均已执行真实 macOS Electron 验收。没有访问真实账户，也没有伪造分钟数据为日线。

## 供应商与领域语义

权威依据为 [Tushare fut_daily 文档](https://tushare.pro/document/2?doc_id=138)。查询使用实际月份合约 `ts_code` 和包含两端的 `start_date`／`end_date`；不接收连续或主力别名。单次请求限制为最多 366 个日期，上游返回达到 2000 行时明确拒绝，避免接收可能截断的结果。完整合约范围仍须由真实合约目录确定。

- `trade_date` 是供应商交易日期，保留为类型化公历日期，不转换为真实成交时刻，不推断夜盘归属或交易时段。
- OHLC 必须完整且符合高低关系；成交量和持仓量必须是非负整数手。缺失记录不填充、无数据不等同于休市。
- `pre_close`、`pre_settle`、`settle` 可以明确缺失；缺失保持 optional，不使用 0 代替。结算价与收盘价分别保留。
- 上游 `amount` 单位为万元，领域模型使用报价币种基本单位。先对原始十进制文本移动四位，再按八位定点检查精度与范围；不先转浮点，也不在换算前错误丢弃可精确表示的小数。
- 日期、合约身份、字段集合、重复日期、数量与十进制范围均严格校验。供应商错误原文可能回显凭据，不传入日志或界面。
- 日线仅作为历史数据；没有新增 Bar 撮合假设，不自动成为现有逐笔回测输入。

## 剩余验收

1. 使用用户本机有效供应商权限时验证真实账户下载；目前外部响应均为明确测试夹具，没有伪报实测账户成功。
2. 新安装包未验收；本阶段范围是 macOS 开发构建和 Electron 运行链路。

## 存储验收

`apps/services/data-pipeline/daily.*` 按最多 366 个日期分段，总范围有界；清单记录 `tushare.fut_daily`、`provider_trade_date`、`quote_currency` 及各分段摘要。空日期不补造 K 线，读取完整数据集前验证全部分段，不修改旧分钟格式。下载凭据不进入文件。已有不同请求／未知格式／摘要错误明确拒绝，没有迁移或静默覆盖。

macOS Release 日线存储、日线插件和既有分钟插件相关 CTest 23/23 通过（`build/daily-storage-tests.log`）。覆盖取消续传不重复请求、已落盘未登记分段恢复、Decimal 与可缺失结算字段往返、凭据排除、读写互斥和外部文件保留。数据为明确供应商响应夹具。Task Service 后续验收见下文；Electron 日线验收仍待接入。

## 托管任务验收

macOS Release 插件、存储和日线任务 CTest 26/26 通过（`build/daily-task-tests.log`），包括实际 Task Service 与 `asterion-data-pipeline --daily-download` 进程通过私有 IPC 领取任务、恢复已保存来源分段并发布结果。注入供应商响应只用于准备测试存档；托管进程完整恢复已下载文件，不请求真实账户。另验证真实服务提交／取消／列表与结果读取、凭据排除、目录归属、结果摘要和行数拒绝、取消期间已验证结果不可发布、重试旧尝试失效，以及重启后恢复。

既有回测、因子、流水线、研究任务与结算表回归通过，日志为 `build/daily-task-regression-tests.log`、`build/daily-task-state-tests.log` 和 `build/daily-task-calendar-tests.log`。Node Agent 已构建并接入受限日线启动标记，但本阶段未执行 Agent 自动派发的日线全链路、日线 Electron UI 或真实供应商账户验收。

## 分页验收

独立 `DailyPageQuery`／`DailyPage` 保留供应商交易日期和精确 Decimal，昨收／昨结／结算价在协议中通过字段存在性表达，在界面边界明确输出 null。范围筛选使用交易日期而非盘中纳秒；空数据集不虚构覆盖日期，空筛选结果仍保留数据集实际覆盖。MACD 使用统一工具插件，从数据集起点预热，分页和筛选不重置。

macOS Release 构建及相关 CTest 28/28 通过（`build/daily-page-tests.log`）。覆盖真实 Task Service 分页请求、分页／筛选指标一致、缺失参考价格、精确成交额、空数据、错误日期／越界／摘要／响应拒绝、并发读锁及写锁排他。尚未接入原生桥与 UI，不能将服务查询验收当作 Electron 日 K 可用。

## 原生桥验收

macOS Release C++ 原生桥新增四项测试，包含真实 IPC 的日线／分钟并发查询、快照与命令不被阻塞、服务切换拒绝迟到响应、返回身份拒绝、输入日期严格校验、非法参数在请求发送前拒绝，以及合约目录范围与提交。两项原有分钟查询测试一同通过（`build/daily-native-recheck-tests.log`）。先前完整 34 项运行仅有一项异常类型断言失败：非法 ID 按已有契约返回带错误码的 `Error`；修正断言并确认不触发服务读取后，六项桥测试全部通过。其余 28 项日线／分钟测试在 `build/daily-native-tests.log` 通过。

前端桥类型新增日线页与命令，TypeScript 检查通过。尚未授权数据源／行情插件使用日线命令，也未开放日 K；Electron 日线端到端验收待 UI 集成后执行。

## 数据源工作台与 Electron 验收

日线来源适配器使用独立 `tushare.fut_daily` 身份和提交命令，复用合约目录；不传分钟周期，不提供手工日期下载范围。查看器使用交易日期筛选，显示精确 OHLC、成交额、持仓量及可缺失参考价格。市场和自选图表支持日 K、MA 与 MACD，周期选择保留于面板；无日线数据时明确不可用。绘图所需日期坐标只在 Terminal UI 转换为 UTC 日期坐标，保留原始交易日期，不进入服务查询和交易逻辑。

浏览器回归通过：日线查看／筛选／分页／错误响应保留、数据源切换、独立日线提交、不发送分钟字段、Token 清空，以及既有分钟查看器／周期状态／下载测试。日志 `build/daily-ui-recheck.log`、`build/daily-submit-ui-tests.log`、`build/daily-market-ui-tests.log`；未修改的两项查看器／面板回归在 `build/daily-ui-e2e.log` 通过，该初次运行的一项单数据源旧断言已更新后重跑通过。TypeScript、ESLint、格式与 diff 检查通过。

`pnpm desktop:check` 与隔离 `pnpm test:desktop` 通过（`build/daily-desktop-verified-check.log`、`build/daily-native-ui-verified.log`）。测试通过真实数据插件与 Task Store 生成隔离分钟／日线存档，再由实际 Node-API、Task Service 与 Electron 查询；验收日 K 切换、分页、精确成交额、空结算字段、交易日期筛选及 MACD 图例与服务返回值一致。截图 `build/native-daily-check/native-daily-market.png`、`native-daily-viewer.png` 已检查，修复历史合约列表漏报日 K 的问题并重新执行完整原生验收。

供应商响应明确为测试夹具，含用于验证渲染的连续日期，不代表真实交易日历。没有实际账户网络下载或新安装包验收。Agent 自动日线派发的后续验收见下文。

## Agent 自动派发验收

隔离原生夹具现在只创建日线 QUEUED 任务和已持久化来源分段，不直接领取或完成任务。Electron 检查启动前只有一条提交记录；启动本机研究服务后，由实际 Node Agent 派发 `--daily-download` 工作进程，领取第 1 次尝试、恢复已有来源分段、校验并发布成功结果。验收同时检查结果摘要、进度完成和至少三条持久任务记录，随后执行已有日线分页／筛选／MACD 图表检查。

`ASTERION_TEST_NODE_ISOLATED=1 pnpm test:desktop` 全流程通过（`build/daily-agent-native-tests.log`）。该证据覆盖 Agent 自动派发与下载进程的恢复发布链路；来源分段仍由明确供应商响应夹具准备，工作进程复用已下载内容，不代表真实网络账户下载已验收。


## 周／月／季／年图表查询

`research.daily.page` 当前显式接收 `period: day | week | month | quarter | year`，原始日线查看器固定请求 day。任务服务只读取原有日线存储，不改写文件，也不重复下载供应商聚合周期数据。响应携带相同 period，Terminal 拒绝周期不一致的回复。

周线按交易日期所在的周一至周日分组，月线、季线、年线分别按自然月、自然季度和自然年分组。开盘与参考价取首条来源记录，最高/最低取期间极值，收盘、结算、持仓量取末条，成交量/成交额使用精确 Decimal 求和。结算价以末条的缺失状态为准。图表日期为该组最后一条来源日期；不补齐缺失日期，不据此宣称交易日覆盖完整或末期已结束。

先聚合完整已验证数据集，再按聚合记录的日期标签筛选与分页。total_rows/matched_rows 是聚合 K 线条数；first_day/last_day 仍描述来源日线覆盖。MACD 从首根聚合收盘价计算，前 33 根聚合记录预热，不在页码或筛选边界重置。选择不同周期重置当前页码，数据版本变化保留可用的聚合周期显示选择。

年线同样遵守 33 根聚合 K 线的 MACD 预热；当前日线存储最多二十年，因此年线不会输出 MACD。这是数据范围限制，不使用较短周期的指标替代。
