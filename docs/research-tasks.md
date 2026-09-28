# 研究任务：第一条回测执行链

当前已实现独立 `asterion-task-service` 与 `asterion-backtest` 的第一条执行链。Task Service 持久化任务与执行尝试；Backtest 通过 Protobuf 认领已提交任务并执行。已接入本机 Node Agent 自动调度与 Terminal 研究页面。桌面打包已加入两个本机程序及 Linux x86_64 对应程序；macOS DMG 与 Linux 容器内程序链路已验证，Windows 原生验收仍未完成。

## 已实现边界

- `core/include/asterion/domain/strategy_port.hpp`：可信进程内策略接口，接收有序 TradeTick，返回目标持仓意图；不授予交易权限。
- `plugins/strategy/cta/moving_average.*`：长仓/空仓 SMA 策略，明确快慢窗口和手数，预热后产生目标。目前只有这个明确模型，尚非完整策略库。
- `apps/backtest/engine.*`：回测编排，复用 PaperExecution/FuturesAccount/Decimal。先撮合前一事件产生的委托，再向策略交付当前事件；最新成交价为限价，下一笔成交量共享，未成交部分在下次策略决策前撤销。每个时段末取消剩余委托；每日以显式结算价结算，持仓续接，不强行平仓。
- `protocol/proto/asterion/v1/research.proto`：类型化 BacktestInput/Result 与 TaskRequest/Response。JSON 仅用于 UI/文件和独立版本的存储格式，不在进程间 Protobuf 中塞 JSON 字符串。
- `apps/task-service/task_store.*`：不可变输入、幂等提交、队列、执行尝试、进度、取消、明确重试、结果摘要校验。持久化使用现有 filesystem-journal 插件，单写者锁、每个任务独立日志、每次尝试独立结果目录。

数据版本目前是嵌入任务的固定历史快照摘要：对规范化后的合约、成交序列和格式版本计算 SHA-256，与因子共用 TradeDataset 契约；交易日、账户本金、手续费和策略参数不属于数据版本。BacktestInput 版本 5 使用此摘要并要求逐日显式时段和结算证据；版本 1/2/3/4 明确拒绝，不静默迁移既有任务。任务自身持久化完整研究输入。同一个任务 ID 的完全相同提交返回现状，不同输入拒绝。Data Pipeline 发布任务与来源摘要已接入 Terminal；跨服务统一目录仍未实现，不把此摘要视为完整数据治理系统。

## 模型限制

当前支持一个中国期货实际月份合约、1–64 个有序交易日，每日 1–16 个显式 UTC 时段，可覆盖夜盘和日盘，最多 10000 笔成交，且数据量至少覆盖 SMA 慢窗口。每笔成交必须落在配置时段内；交易日归属由输入明确指定，不从自然日期推断，也不冒充交易所日历校验。价格必须为正，数量与价格按合约步长校验，时间非递减；策略只做多/平仓，使用固定每手保证金与手续费。

已实现按输入日程逐日结算和持仓续接，不支持交易所日历自动补全、主力换月、多合约组合、涨跌停/队列位置模型、真实保证金率或不可信策略隔离。出现不支持输入或资金不足等异常，任务失败，不静默忽略。统计先提供逐事件权益和最大回撤金额，不用不足一天的数据假装给出可靠年化指标。

## 生命周期与恢复

`queued → running → succeeded / failed`；排队取消直接进入 `cancelled`，运行取消先进入 `cancel_requested`，工作进程确认后才进入 `cancelled`。`failed / cancelled / interrupted` 可显式重试，并产生新执行尝试。

认领时分配随机 token，进度/完成/失败必须匹配当前尝试。工作进程默认每 100ms 或结束时报告进度；服务端默认 30 秒未收到有效报告将任务标记 `interrupted`（`--worker-timeout` 可配置 1–300 秒）。服务重启把仍在执行或取消中的任务标记中断，不自动重跑；旧 token 永远不能提交到新尝试。

结果先写入独立日志并同步，再记录完成状态与 SHA-256。只有确认完成的结果可被查询；中途写出的未确认结果不冒充成功。恢复时检查输入版本、状态转换、进度、结果摘要、输入/输出身份及权益统计。损坏和提交到一半的目录保留并明确报错，不静默删除或迁移。当前验证覆盖进程终止与重启，不宣称完成三平台断电故障验收。

Task Service 不直接启动业务子进程。Node Agent 每秒查询队列，每个研究服务最多同时运行两个按任务类型选择的 Backtest / Factor / Data Pipeline 子进程；Agent 是工作进程的真正父进程，停止/重启研究服务时清理其工作进程。Task Service 分配业务任务和尝试，使用独立的本机工作进程 IPC 入口，业务客户端可使用本机 IPC 或 TCP/mTLS。两者不合并为通用远程命令执行服务。

Terminal 初始化自动连接本机研究服务。研究插件提交当前已校验 CSV 的不可变快照，展示任务状态、取消、显式重试、权益、手续费、回撤金额和结果详情；插件向宿主任务中心贡献任务。刷新或关闭 Terminal 不停止服务，任务和结果保存在 Agent 管理的服务目录。费用与本金必须明确填写，不预填虚构交易成本。

## 程序入口

```sh
# 先创建专用的空任务目录；本机 Unix Socket 或 Windows Named Pipe。
asterion-task-service --session research --directory /absolute/task-store --endpoint LOCAL_ENDPOINT

# 从已提交的队列认领任务。输入、进度和结果均通过 Protobuf 传递。
asterion-backtest --session research --endpoint LOCAL_ENDPOINT --task TASK_ID

# 独立文件执行；input.pb 为 research.v1.BacktestInput，结果目录必须为空。
asterion-backtest --input /absolute/input.pb --directory /absolute/result-directory
```

Task Service 也支持 `--bind / --port / --tls-ca / --tls-cert / --tls-key`，工作进程以 `--host / --port` 和完整 TLS 身份连接，禁止明文或自动降级。当前本机进程和客户端都必须可信；没有多用户角色授权，不能作为公网服务开放。TCP/mTLS 使用既有通道机制，本轮尚未进行独立研究服务跨机验收。

## 本轮验证

- macOS 本机 GoogleTest：研究输入摘要、无未来数据成交、限价不满足不成交、费用与回撤、确定性、未声明时段拒绝、取消、插件生命周期。
- 任务存储：重复提交、冲突、旧尝试隔离、取消、重启恢复、中断、损坏结果拒绝。
- 原生子进程：客户端断开后继续执行、确认结果跨服务重启保留、工作进程超时、重复 IPC 连接。
- Windows 命名管道监听器补建后续实例，保持首实例所有权，相关重复连接测试已纳入跨平台测试代码；Windows 实机尚未运行。

前一阶段本机验证：`cmake --build build/Debug -j 4` 完成；`ctest --test-dir build/Debug --output-on-failure` 共 75 项，74 项通过，Linux 专用 `node_deployment` 1 项跳过，0 失败。包含 14 项新增研究单元/进程测试。证据日志为本机 `build/research-full-build.log` 和 `build/research-full-ctest.log`。尚未执行 Windows/Linux 原生研究测试、研究服务跨机 mTLS 验收或新安装包构建。

Agent/Terminal 集成阶段：本机 CTest 76 项，75 项通过，Linux 专用 node_deployment 跳过，0 失败；新增 research_agent_recovery 验证强制结束 Terminal、删除 CSV 后自动完成，以及研究服务重启后结果一致。日志 build/research-agent-full-ctest.log。界面测试使用真实 C++ 后端，验证提交、刷新恢复与结果；全量 UI 回归共 19 项，18 项通过，Linux 专用远程部署 1 项跳过；日志 build/research-full-ui.log。TypeScript/Vite 构建通过。

## 远程部署资源

Linux x86_64 服务包必须同时包含 `asterion-task-service`、`asterion-backtest`、`asterion-factor` 与 `asterion-data-pipeline`，分别校验平台和摘要。Terminal 的“设置 → 连接 → 远程 Linux → 部署服务”选择“研究服务”；Agent 持有研究服务与工作进程，Terminal 通过 TCP/mTLS 附着。任务输入上传为不可变快照，文件路径不作为远端数据依赖。研究页面显示当前主机，避免把远端任务误认为本机任务。远端服务端口仍使用既有防火墙检查与显式确认流程。

部署集成验证：Linux x86_64（Ubuntu 24.04，macOS 上的 amd64 仿真容器）已通过 Agent 原生部署测试，包含真实 ELF 上传、TCP/mTLS 回测、自动派发及研究服务重启后结果一致。这是容器内回环通信，不是独立物理机器网络或系统服务注册验收。新 macOS ARM64 DMG 已生成，镜像校验、签名、内置 Linux 资源摘要和包内研究程序的关闭 Terminal / 服务重启恢复通过；证据 build/research-dmg-verify.log。签名为 ad-hoc，未公证；没有替换用户已安装应用或升级现有系统 Agent。

Linux 容器完整 CTest：76/76 通过，0 跳过，日志 build/research-linux-tests.log。macOS 本轮定向原生研究/资源/CLI 17/17 通过，日志 build/research-targeted-ctest.log；桌面初始化与部署相关 UI 5/5 通过，日志 build/remote-research-ui-tests.log。

因子任务共用持久化任务生命周期与类型化工作进程传输，具体输入、评价边界、UI 和本轮验证见 [因子研究](factor-research.md)。


## TLS 握手与监听轮询期限

Task Service 的 TCP 空闲监听仍每 200 ms 返回检查租约，该阶段将 TLS 双向认证独立设为 3 秒期限，不再使用监听轮询间隔。原先把 200 ms 轮询间隔用于握手，会提前断开有短暂调度或网络延迟的合法客户端。新增真实 TLS 测试在建立 TCP 后延迟 600 ms 再认证并请求关联心跳；修改前失败，修改后通过，证据为 `build/research-admission-before.log` 和 `build/research-admission-tests.log`。没有重试或重放请求，没有放宽证书验证。该阶段外部请求仍串行接收；后续有界并发接收与工作通道隔离见下节。

此前 Linux 全量测试 137/138 通过，node_deployment 在读取数据发布结果时遭遇 TLS 重置（`build/agent-concurrency-linux.log`）。该日志不足以唯一确定其原因；延迟握手测试独立证实期限缺陷，最终复验结果另外记录。

最终复验：macOS 138 通过、1 Linux 专属跳过，Linux x86_64 仿真容器 139/139。分别见 `build/agent-concurrency-final-full-tests.log`、`build/agent-concurrency-linux-final.log`；保留初次失败日志。


## 持久化提交顺序与时间

每个 Task Service 在首次提交时分配从 1 开始的 submission_sequence，并与不可变输入在同一提交记录中持久化。服务容量仍为 1000 个任务；当前没有任务删除/归档。List 按提交序号升序返回，Agent 优先派发较早的排队任务，Terminal 研究页和数据发布页按相反顺序展示最新提交。序号只在同一个任务服务内有意义，不是跨机器全局顺序；最多两个工作进程并行，派发顺序不代表完成顺序。重复提交同一 ID/输入不分配新序号，重试保留原提交位置。

submitted_at_ms 为服务端首次提交时观察到的 UTC 毫秒，updated_at_ms 为最后一次持久化状态变更时观察到的 UTC 毫秒；重复进度和无状态变化的取消不刷新时间。运行中任务恢复为 interrupted 时也记录该变更的观察时间。时钟校准可能让更新时间早于提交时间，因此这些时刻仅用于展示，不计算耗时、不决定顺序；租约继续使用单调时钟。Terminal 按当前界面语言及本机时区展示提交时间，最近更新放入详情。

任务提交记录与状态记录当前存储版本均为 2，缺失元数据、版本 1、非法时间、重复或不连续序号会明确拒绝加载，保留原始文件，不推测/补写历史时间，不迁移或清空旧目录。回测/因子输入版本、数据内容摘要和结果格式不因任务元数据改变。此变更未操作用户当前安装和任务目录。

新增测试覆盖同毫秒提交、反字典 ID、服务时钟回拨、重启后继续分配序号、重复提交/进度、重试返回值、持久化时间恢复，以及非法元数据不被改写。浏览器通过真实 C++ 桥接验证研究任务和数据发布都按提交顺序展示，并在页面重载后保持。

本轮研究/因子/数据持久化专项 25/25、浏览器 8/8 通过（`build/task-chronology-final-tests.log`、`build/task-chronology-browser.log`），前端构建通过（`build/task-chronology-ui-build.log`）。已检查研究列表和数据发布截图。浏览器验证使用真实 C++ 开发桥与隔离 Agent，不等同于原生 Tauri 窗口验收。

最终全量：macOS 141 通过、1 Linux 专属跳过（`build/task-chronology-full-tests.log`），Linux x86_64 仿真容器 142/142（`build/task-chronology-linux.log`）。包含本机恢复和远程 TLS 部署链路。未执行 Windows 原生、物理跨机或断电验收；本轮没有重建桌面安装包。


## 研究服务连接容量与工作通道隔离

Task Service 的外部本机 IPC / TCP 接收循环只负责接收连接，不在监听线程执行握手或读取请求。外部连接使用 8 个工作线程、8 个排队项；Agent 的私有工作 IPC 使用独立的 4 个线程、8 个排队项。两者容量分开，外部慢连接不会占用工作进程报告进度、完成或取消所需的全部接收槽位。健康检查仍使用独立的仅心跳通道。超出容量直接关闭未派发连接，不启动无限线程、不重发操作；这不构成对不可信本机账户的隔离或公网拒绝服务防护。

自应用 accept 起，排队、TLS 握手与读取完整请求共享 10 秒入站期限，其中单次 TLS 握手仍最多 3 秒；回复写入最多 3 秒。业务操作获取任务互斥锁后、租约过期处理后均重新检查入站期限和停止标记，过期请求不能继续执行。任务状态、序号、持久化与租约继续串行操作；执行中的结果校验和日志写入仍可能占用业务锁，不承诺任意业务处理都能强制在入站期限内中断。

监听循环空闲时每 200 ms 检查租约，并在每次业务操作前再次检查过期租约；迟到的工作进程报告不能因维护检查被延后而重新续活已经过期的任务。过期任务进入 interrupted，令牌失效，仍需显式重试。连接池先于 Store、互斥锁和租约销毁，不允许回调访问已经释放的任务状态。

修复前两个独立本机用例（外部静默连接阻塞取消、工作通道静默连接阻塞领取）均失败（`build/research-concurrency-before.log`）；修复后研究进程与 TLS 专项 11/11 通过（`build/research-concurrency-final-tests.log`）。新增 TLS 过载测试要求在 3 秒握手期限到达前关闭超容量连接，同时保留证书拒绝与恢复响应检查；外部饱和测试验证私有工作通道和租约过期不受阻断。

最终全量：macOS 147 通过、1 Linux 专属跳过（`build/research-concurrency-full-tests.log`）；Linux x86_64 仿真容器 148/148（`build/research-concurrency-linux.log`）。包含实际远程 TLS 部署/结果恢复及并发过载检查。未执行 Windows 原生、物理跨机或原生 Terminal 窗口验收；本轮未重建安装包，未改变用户运行服务和历史数据。


## 结果的实验配置证据

TaskResponse 的完成结果必须携带 result_task：Task Service 在同一个任务锁内读取已验证的结果及其持久化 Task，包含完整的不可变输入、提交时间、执行次数和结果文件摘要。Terminal 通过 protocol 的 decode_task_result 验证请求 ID、完成状态、结果类别、输入存在及输入/结果的数据版本一致，不以当前表单、当前 CSV 预览或旧缓存补齐缺失证据。没有证据的旧响应明确拒绝，不提供降级。任务存储格式和各输入版本在本增量中未改变。

Terminal JSON 在回测与因子结果旁增加 experiment 摘要，保留原输入版本及参数，完整成交数组不重复发送给页面；以笔数和原始起止时间表示输入范围。回测包括 SMA 快/慢窗口、目标手数、交易日、初始资金、保证金/手续费和风险限额；因子包括全部候选窗口、未来收益笔数和原始留出分界。两者都显示原合约规格、数据版本与任务结果摘要。数据发布结果也携带任务元数据并核对来源身份。

界面默认仍突出计算结果，“实验参数”折叠展示提交时配置；查看历史结果不修改当前编辑表单。Browser 测试先把表单改成不同参数，再确认历史结果显示原值；独立进程恢复测试在删掉源 CSV、关闭 Terminal 和重启研究服务后比较整个结果证据。结果摘要是服务端已验证的持久化结果文件身份，不宣称是外部签名或第三方认证。

本轮新增原生验证：持久化回测恢复后返回精确参数和摘要，拒绝缺失任务、错误 ID、非完成状态、错误类别/定义及不匹配数据版本；因子结果保留全部候选及留出配置。初次专项因新增测试混用了 GoogleTest TEST/TEST_F 夹具而失败（`build/research-evidence-tests.log`），已修正声明；最终 macOS 全量 149 通过、1 Linux 专属跳过（`build/research-evidence-full-tests.log`）。浏览器 8/8，细节截图补验 4/4（`build/research-evidence-browser.log`、`build/research-evidence-browser-details.log`），已检查回测与因子参数截图；前端构建通过（`build/research-evidence-final-ui-build.log`）。

Linux 本次全量 149/150 通过，node_deployment 在已停止的研究服务更新阶段出现 `TCP/TLS connection failed: stream truncated`（`build/research-evidence-linux.log`）。不能把任务结果已经读出视作整条部署恢复成功。该错误尚无已证实根因；已补充 NodeClient 操作/阶段诊断，保留原失败证据，再做独立复验。

补充操作/阶段诊断后，Linux 维护与远程部署 4 个用例各连续运行 3 次均通过（`build/research-evidence-linux-diagnostics.log`），其中 node_deployment 三次分别 87.76 / 87.65 / 87.89 秒。未再次触发截断，不能据此声称已修复根因，也不能把第一次全量结果改写为 150/150。macOS 确认后读失败等诊断专项 3/3（`build/research-evidence-final-diagnostics-tests.log`）。本轮暂不更新安装包，保留该间歇传输问题继续调查。

## 显式交易日与逐日结算

BacktestInput 版本 5 要求 `days=[{trading_day,sessions,schedule_source,settlement_price,settlement_source},...]`，1–64 日，每日 1–16 个时段。单日也使用相同契约，旧顶层 trading_day/sessions/schedule_source 字段已保留为废弃编号，明确拒绝旧版本，不提供字段别名、默认补全或任务迁移。BacktestResult 为版本 3，引擎标识 `asterion.backtest.sma-long-flat.v4`（v4：按名义金额比例的费用与保证金、交易所平仓规则、排队撮合；v3 结果明确拒绝，不重算）。

交易日为严格递增的 YYYY-MM-DD；每个日期的时段为有序、不重叠的 UTC 纳秒半开区间 `[begin_ns,end_ns)`，日与日的区间不得交错。允许相邻时段，但仍视为两个撮合区间。每个声明的交易日至少有一笔成交，所有输入成交必须归属某一时段；不会跳过空交易日或截掉区间外成交。日期归属由输入提供，Core 的 TradingDaySchedule 只验证通用区间，不推断交易所节假日、夜盘日期或“下一个工作日”。Terminal 以北京时间输入并转成 UTC 纳秒。

每日结算价、时段来源和结算价来源均必填；价格用规范十进制字符串（Protobuf Decimal），必须正且符合当前模拟器的合约价格步长。来源是用户说明，不是系统认证。时段、来源、结算价和交易日进入不可变任务身份，但不改变纯成交内容摘要。手续费与每手保证金仍为整个实验固定值，不模拟逐日费率调整。

结算发生在每日末时段的 end_ns：先处理该日最后一笔成交、撤销未完成委托，再按给定价格结算；中间日通过 PaperExecution 的事件间结算继续回放，最后一日也明确结算。持仓浮盈亏进入资金，成本重置、今仓转昨仓；SMA 历史不清空。未成交委托不跨时段，目标减仓先平昨后平今，拆出的子委托逐笔风控、整体原子接受。价格来源不足时拒绝，不以末笔成交价冒充结算价，不强平未平持仓。

结果包含逐笔估值和逐日结算两类权益事件（`trade` / `settlement`），总点数为成交笔数加交易日数，时间非递减；最大回撤也考虑结算点。`settlements` 保存各日结算时间、价格、结算后资金/权益、累计已实现盈亏、累计手续费和未平手数。最终账户是最后一日结算后的状态，不能当作末笔成交估值。Terminal 折叠展示“逐日结算”，历史实验参数按日列出原配置，编辑当前表单不改变历史。

Task Service 提交与恢复时校验输入，完成及读取结果时按同一输入重算完整结果，比较所有曲线、逐日结算和最终账户；只有最终权益相同但中间结算不同的结果也拒绝。进度仍按处理成交笔数计，日终结算在报告该笔进度前完成。取消、明确重试、进程中断和日志恢复沿用同一持久任务机制。

仍未实现交易所日历提供者、结算价自动导入、换月、多合约组合、比例保证金、隔夜追加保证金和交易所队列模型；手工模拟会话仍只有单段回放及末尾手动结算，不能把多日回测等同于手工会话跨日操作或实盘能力。

版本 5 的 calendar_publication 为完整结算表发布证据；手工输入为 null。协议校验发布摘要、合约及所有 days 一致性。Terminal 通过完成任务读取发布物并注入，不接收同时存在的发布选择和手工日程。实验结果保留此发布物，支持离开源文件后的审计；详见 [结算表](settlement-calendar.md)。
