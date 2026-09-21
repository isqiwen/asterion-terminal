# 当前接口与持久化协议 v1

此文件描述已实现的 CSV、数据同步、配置与覆盖协议；目标通用协议见 architecture.md。

## 任务

`POST /api/v1/imports` 接受 command_id/source/csv，成功返回 202 和 job_id。相同 command_id、相同输入返回原任务，不同输入返回 409。输入结构错误返回 422；内容质量由 worker 校验，失败进入 FAILED。空来源响应不能发布为空快照。

状态：QUEUED → RUNNING → SUCCEEDED / FAILED / CANCELLED。RUNNING 租约过期可以被重新领取，attempt 增加并更换随机 fencing token。PostgreSQL 行锁和 SKIP LOCKED 防止并发领取。续期、失败及发布都要求匹配当前 token、RUNNING 状态与未过期租约。取消是逻辑取消，当前有界子进程可以完成计算，但不能再发布。

失联不会立即失败任务，过期后重试。worker 每约 lease_seconds/3 续期。单 worker 一次运行一个任务；当前没有内核级 CPU/内存限制，只有限量输入与隔离子进程。

## 发布

任务执行与发布契约定义在 `platform/tasks/handlers.py`，由 `data/worker.py` 与 `research/worker.py` 声明，`runtime/handlers.py` 统一装配；当前登记 `data.import_csv`、`data.sync` 和 `research.backtest`。未知或重复类型拒绝运行，spawn 子进程重新导入同一组合入口；这不是运行时安装第三方代码的接口。

worker 上传至对应 job 的 publish 入口，携带 X-Lease-Token。API 锁定任务，验证租约及内容，对本次有界 CSV 重新校验并与标准化字节比较；写入唯一文件，fsync 文件及目录，确认 hash，再次检查租约后在同一 PostgreSQL 事务内登记快照并完成任务。

此版本在 API 线程池中重新校验最多 2 MB CSV，属于开发切片限制。通用版本应使用持久化验证证据和 worker 暂存/manifest 提交，避免在 serve 重做大量计算。

相同 token 与相同结果重复发布返回原快照；不同内容返回 409。失败事务可能留下无目录引用文件，但列表和查询只读取已登记快照。不使用文件存在作为发布成功依据。当前尚无孤儿回收任务。

数据文件使用服务端生成的 UUID 命名；客户端提交的 ID 必须先命中已发布目录，不能提供任意文件路径。快照返回逻辑 URI 和内容 SHA-256，跨机存储映射未实现。

## 安全与范围

仅监听 127.0.0.1；所有 API 需要 Bearer token。令牌来自服务端环境，前端仅保存在内存中。当前是本机单操作者开发认证，未区分 worker 与 UI 身份，不能作为远程多租户部署方案。Tauri 不授予外部页面原生权限，也没有 shell/文件/凭据操作权限。

任务列表最多返回最近 100 项，快照最多 100 项；行情目前为前 1,000 行。后续需加入完整分页与服务端范围查询。普通 API 类型由 OpenAPI 生成；WS 事件协议尚未实现。

## 数据源配置修订

以下接口位于 `/api/v1`，均要求本机运行凭据及启用时的账号/PIN 授权：

- `GET /data/providers`：Provider API v2，返回 `configuration.schema_version/fields`、`description/demo` 和能力 `defaults`。字段子集为 string/integer/boolean，含秘密标记、必需性、默认值和边界；不是完整 JSON Schema。
- `GET /data/providers/{provider}/configuration`：返回 provider、revision、schema_version、普通 values、已保存秘密的字段名、configured 和可选 error；不返回秘密。
- `POST /data/providers/{provider}/configuration/check`：请求 `{expected_revision, values, secrets}`，测试合并后的草稿，不持久化；检查前后均确认修订未改变。测试范围由提供方说明。
- `POST /data/providers/{provider}/configuration`：同样的请求结构，保存新修订；普通 values 为完整字段集，秘密省略表示保留、字符串表示替换、null 表示移除。可选普通字段 null 恢复默认或省略。旧修订返回 409，类型/范围/未声明字段返回 422。

文件先加密落盘，数据库以 provider 主键和 expected_revision 比较更新；并发首次插入或更新仅一个成功。配置快照的 HMAC 绑定提供方、schema、修订和字段值；失败事务可以留下加密孤儿快照，尚无自动回收。configured 只表示必需配置齐备，不表示外部接口已经验证。

新任务 payload 固定 `configuration={ref,revision,schema_version}`，执行不读取后来的默认设置；缺失、损坏或不匹配的引用直接失败，不降级到当前凭据。领取只更新任务租约，不修改任务输入；缺少固定配置的任务明确执行失败，不读取当前配置补齐历史。新的失败重试使用当前配置，只有固定引用相同的历史分段才可续传。

同一同步 command_id 的重放返回原任务及原配置，后续配置变化不会重复创建任务；请求内容改变返回 409。客户端在同一输入失败重试中保持命令 ID，明确修改表单或成功提交后才创建新 ID。

数据源配置仅使用 `/configuration`、`/configuration/check` 与 `/verify`；旧 `/credential`、`/check` 入口已删除。原始请求校验错误统一返回安全提示，不回显 Pydantic 的 input/context。坏配置读取返回 error/未就绪，只有显式重填全部普通值及秘密（或明确移除）才可建立新修订；旧任务不可读引用保持失败，不被新配置悄悄修复。

同步版本只记录配置引用，不包含字段值。历史演示来源与开发测试使用独立 `synthetic/SIM` 身份；正式产品不注册合成提供方，目录和行情 manifest 增加 `demo=true`；CSV 出现精确的 `SIM.DEMO001` 也会标记演示。不提供旧配置迁移。Worker 独立请求身份、工作台服务端作用域和本地受信插件进程已实现；原生凭据库和恶意代码 OS 隔离尚未实现。

## 日线/日历累积发布

同步发布仍使用 `/jobs/{job_id}/publish-data`，RAW 记录本次完整采集，STANDARD 的累积系列身份包含 `version_series=monthly-observed-v1`。符合当前 manifest 契约的历史采集范围记录仍可读取，不自动合并进累积系列；不提供旧字段或旧结构的转换读取器。合约资料和 CSV 不启用累积语义。

发布顺序为任务租约锁 → 数据集目录插入/行锁 → 读取父版本 → 月分区合并与文件落盘 → RAW/标准版本和任务结果登记 → 租约复核后提交。PostgreSQL READ COMMITTED 下，等待数据集锁的任务重新读取已提交父版本。标准版本时间单调递增，固定 ID 查询不会跟随 latest。

STANDARD manifest 的 `format=partition_manifest`、`version_semantics=CUMULATIVE`；新增 `parent_version_id`、`revision`、`changes`、`partitions`、`acquired_rows`、`logical_bytes` 和 `coverage_gaps`。`inputs` 包含本次 RAW 与父标准版本；分区按 checksum 引用不可变文件。`checksum/bytes` 对应清单本身，`logical_bytes` 是引用分区的逻辑总量，不是新增磁盘占用。

版本预览 `/data/versions/{id}` 保持行分页，并增加与返回行对应的 `row_sources`（`observed_at/raw_version_id`）；采集范围版本该字段为 null。读取先核验清单及所访问分区。`changes` 分别记录新增、修订、同值刷新、未变化和忽略较旧响应。冲突时间相同但内容不同拒绝发布；未返回的主键不会删除已有数据。任务结果新增 `dataset_id/version_id`，重发相同成功结果仍返回原发布版本。

## 固定输入的日线覆盖报告

- `POST /data/versions/{version_id}/coverage`：`start/end` 为目标日期；可指定 `calendar_version_id/contracts_version_id`，缺省解析同源同交易所最新标准版本；`use_latest_daily=true` 在同数据集选择最新行情。最大跨度 3660 天，不允许未来日期。仅接受累积标准日线，输入类型、来源、交易所与结构版本须匹配，读文件验证 hash。
- `GET /data/versions/{version_id}/coverage`：该固定日线版本最近创建的报告，尚无报告返回 null。
- `GET /data/coverage/{report_id}`：读取固定报告，不重新选择 latest。
- `GET /data/coverage/{report_id}/refill-status`：按报告及其提交时复核报告恢复已落盘批次与当前任务尝试，返回 submitted/statuses/jobs/total/truncated；只读，不返回任务 payload 或租约 token。最新 200 次尝试内隐藏已有后继的前序尝试，超限明确截断。
- `POST /data/coverage/{report_id}/refill`：`command_id` 幂等创建补齐批次，返回 `status/report_id/checked_report_id/jobs/skipped_gap_days`。这些接口均需账号解锁权限。

报告 ID 固定 checker、输入版本、目标范围与核对日期；内容包括逐日状态、统计和可补取区间。`COVERED` 只表示按固定依据的预期日期有记录；`NO_EXPECTED_ROWS` 表示目标区间没有预期交易日。`UNCONFIRMED` 保留缺少依据/当日未完成，`CONFLICT` 阻止补取；报告独立于版本清单，不将既有版本改成“完整”。

补齐在同一事务内登记命令、锁定数据集、使用固定依据重查最新行情、写入新报告及全部任务。重复命令且报告一致时返回原批次，不受后来数据变化影响；命令复用于另一报告返回 409。只补取原报告已列出的 GAP 日期，已补日期跳过，确认 CLOSED 日期可用作区间桥接。没有可补取日期返回 NO_GAPS，冲突返回 BLOCKED；不会把这些情况伪装成已创建任务。

补齐任务的 payload 固定 `coverage_report_id`，发布版本和失败重试继续保留该引用；批次结果的任务状态是提交时快照，运行状态从通用任务接口读取。完成后需显式重新核对最新行情，当前无自动调度和批次取消协议。报告与输入虽有固定引用，但生命周期删除保护尚未实现。

## 固定版本研究运行

- `POST /api/v1/research/runs`：提交 `BacktestRequest`，固定标准日线版本、范围、策略/引擎、费用/滑点/保证金参数及显式数据假设。返回普通 `Job`。
- `GET /api/v1/research/runs`：最近 100 次运行摘要；`GET /api/v1/research/runs/{id}`：固定参数、版本、警示、权益/成交/事件结果。
- `POST /api/v1/research/runs/{id}/rerun`：新 `command_id` 复制原冻结输入，无需查询最新版本。
- `POST /api/v1/jobs/{id}/publish-research`：worker 原始 canonical JSON 结果，`X-Lease-Token` 约束，8 MB 上限；服务端复算并原子完成任务。

研究用户接口继承账户/PIN 保护；任务领取、续租、取消协议不变。模型与数据可用性边界见 [本地回测](local-backtesting.md)。

研究提交新增 `coverage_report_id`、`coverage_policy`（默认 `require_complete`）和 `coverage_note`。严格模式要求对应版本及精确日期范围的 `COVERED` 报告；探索模式 `allow_incomplete` 要求非空原因；`CONFLICT` 与错版/错范围报告在两种模式都拒绝。详情返回冻结的 `coverage`；无报告的历史运行返回 null。原输入重跑不重新核对最新依据。

`GET /api/v1/jobs/{job_id}` 提供账户/PIN 保护的单任务状态，用于研究补齐跟踪，避免最近 100 项列表截断影响进度。返回 `Job`，不返回任务输入或租约 token；不存在返回 404。


## 研究草稿与模板

账户/PIN 保护的 `GET /api/v1/research/workspace` 返回当前账户草稿与未删除模板。`POST /api/v1/research/workspace/draft` 与 `POST /api/v1/research/templates/{uuid}` 接收 `DocumentUpdate`：`expected_revision`、模板 `name`、schema version 1 的 `content`。草稿允许未完成输入，不复用执行任务的严格参数校验。`POST /api/v1/research/templates/{uuid}/delete` 使用 `expected_revision` 删除所选模板。

所有者只从认证会话推导；可附带 `expected_account` query 校验页面预期账户，不允许借此指定另一账户。无账户开发模式所有者固定为 `local-development`。冲突返回 409，不覆盖服务器内容。同一保存的响应丢失重试仅在下一修订且内容相同时返回原记录。删除使用墓碑避免旧创建请求复活模板。


## 研究结果导出与复现

- `GET /api/v1/research/runs/{id}/export?include_data=false` 返回 schema version 1 的 `asterion.research` JSON；true 附带本次计算使用的 OHLC 行。
- `GET /api/v1/research/runs/{id}/results-archive` 返回固定文件名与 `content_base64` ZIP。
- `POST /api/v1/research/packages` 接收原始 UTF-8 JSON（8 MB 上限，拒绝重复键及非有限数字），按认证账户暂存并返回核验报告。
- `POST /api/v1/research/packages/{id}/check` 重新核验；`POST .../{id}/replay` 接收 `command_id`，复核后返回 202 Job。导入自身不创建任务。

用户接口继承账户/PIN 保护。包接口支持 `expected_account`，所有者仍由服务端认证推导。报告状态为 READY、MISSING_DATA、UNSUPPORTED、INVALID；只有 READY 可以提交。导入的研究详情提供 reproduction 原运行/输入及预期结果指纹，成功结果摘要包含 reproduction_matches。包和结果均使用平台 canonical JSON SHA-256；输入上传/下载需保留 JSON 原始数值表示。此指纹用于完整性和数值复现，不是来源签名。


本地文件覆盖请求使用 `reference_policy=explicit_external`，必须显式传 `calendar_version_id`、`contracts_version_id` 和非空 `reference_note`，不允许 `use_latest_daily=true`。同步数据继续默认 `same_source`，不允许借外部模式绕过连接隔离。报告必须包含 reference_policy、reference_note、references（固定版本/来源/连接/校验和）与 refill_supported；缺少当前字段时拒绝读取或执行，不补字段、不猜测同源语义。文件报告禁止自动补齐，缺口仍在 days/counts 中保留；严格回测仍要求精确版本/范围及 COVERED。


## 数据版本引用与归档

账户/PIN 保护的 `GET /api/v1/data/versions/{id}/lifecycle` 返回 archived、revision、is_latest、按类别的 references 直接引用数量、reference_count 及保护说明。引用汇总包含所有账户，不返回其他账户研究内容。`protected=true`、`can_delete=false` 表示当前保留所有已发布版本，未提供物理删除能力；零引用不能作为删除授权。

`POST /api/v1/data/versions/{id}/archive` 接收 archived 与 expected_revision；事务内更新独立状态，冲突返回 409，同一下一修订且目标状态相同的重试返回原状态。未知版本 404。`GET /data/catalog?include_archived=true` 包含归档的最新版本；默认隐藏，不回退旧版本。历史版本接口仍包括归档记录并返回 archived 标记，固定 ID 预览接口保留原内容，不把可变状态混入不可变版本清单。


## 同连接研究数据准备

- `POST /data/preparations`：输入现有 SyncRequest（所选实际合约日线），按 futures.daily/calendar/contracts 类型能力派生三项同连接请求。批次 ID 为 command_id，批次与三项任务原子落盘；重复相同命令返回当前状态，不同输入返回 409。
- `GET /data/preparations`：最近 10 批，返回请求范围、提交时连接名称及当前未被后继替代的任务公开摘要。
- `GET /data/preparations/{identifier}`：读取指定批次；未知记录 404。每批读取最多 200 次尝试，超限 truncated=true。

`data_preparations` 记录批次及原请求；任务 payload 的 preparation_id 绑定批次，重试同时保留 preparation_id/retry_of。仅首次创建批次冻结一次配置，所有初始任务引用相同配置版本。接口不返回凭据或任务 payload，均使用现有账户/PIN 权限。

覆盖核对复用版本 coverage 接口，显式提供三项任务的已发布 version_id、原日期区间和 use_latest_daily=false；准备批次成功不自动生成 COVERED 结论。保留旧版本，不覆盖已有研究输入。
