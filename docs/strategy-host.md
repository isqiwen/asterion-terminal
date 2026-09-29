# 独立策略宿主

`apps/services/strategy/` 装配可信策略插件；程序为 `asterion-strategy`。第一条路径复用 `asterion.strategy.cta.sma-long-flat`，接收有序成交事件，输出目标持仓意图。账本、订单授权、风控、提交与成交仍由交易服务负责。交易进程已提供策略授权和目标持仓交接命令；两个独立进程的测试链路已贯通，自动回放驱动已集成进策略宿主；Terminal 已接入本机配置、授权运行、进度观察与撤销。Agent 已管理策略进程，Terminal 的服务列表可观察、启停和重启已部署实例。

## 已实现的契约

协议位于 `protocol/proto/asterion/v1/strategy.proto`，版本为 1。

- 一个专用目录绑定一个 session、一个明确来源 stream、一个实际期货合约和一份不可变插件参数。`create` 完全相同可重试，任何配置变化拒绝覆盖。
- 事件 sequence 从 1 连续递增，与 timestamp 分开；相同时间戳和内容的两笔事件有不同 sequence 时仍分别保留。行情源必须提供可恢复序号，不能在断线后重新编号来掩盖缺失。
- 已处理 sequence 的完全相同输入返回原 receipt，不重算或写入；冲突输入、序号跳跃、倒退时间、错误来源、未知字段和无效合约/价格/数量被拒绝。
- receipt 在预热期间没有 intent；预热后产生精确 Decimal 目标持仓。意图身份绑定配置、原事件和目标。它不是订单，也不表示已通过账户授权或被成交。
- 写入使用已有文件日志插件：候选策略状态与 receipt 算好后，先持久化，再更新内存和返回。写入异常使当前实例进入 recovery_required；不得继续处理或自动删除故障证据。
- 重启逐条重放插件，核对原 receipt 与新计算结果。日志间断、配置不符、输出不符或 pending.tmp 均拒绝恢复，不静默改写。
- 每会话当前最多 10000 个事件，达到容量后拒绝新事件。没有轮转、检查点或持续高频吞吐保证；没有把短会话实现描述为可无限运行的实时策略服务。

## 运行与生命周期

构建：`cmake --build build/Debug --target asterion-strategy`。

本机启动参数为 `--session <id> --directory <专用绝对目录> --endpoint <本机端点>`；Unix socket 应放在目录外，Windows 使用本机命名管道。日志目录必须预先存在，不包含其他文件。

远端用 `--bind <地址> --port <端口> --tls-ca <CA> --tls-cert <证书> --tls-key <私钥>` 替换 `--endpoint`，强制双向 TLS。没有明文 TCP 模式。每连接一个有界请求；断开不会重置会话或停止程序。

`--health-endpoint` 提供独立本机健康通道。`--owner-pid` 绑定管理进程，管理进程退出时宿主退出。进程退出后的本机 socket 由掌握子进程身份与退出状态的管理者回收；宿主不会删除不明占用端点。Agent 的 STRATEGY 服务类型使用同一组参数管理策略进程；策略日志目录由 Agent 创建并独占管理，不接收用户传入的目录。

## 当前验证和下一步

macOS 原生会话、独立进程和 TCP/mTLS 测试见 `build/strategy-tests.log`：7/7 通过（5 个会话/进程测试、CLI 和 TLS 集成）。覆盖预热恢复、幂等、冲突/间断、日志损坏、持久化失败、客户端断开、进程重启以及缺失客户端证书拒绝。Linux x86_64（macOS 上的 Ubuntu amd64 容器）同一组 7/7 通过，见 `build/strategy-linux.log`；尚无 Windows 原生运行证据或物理跨机器验收。

全量 macOS C++ 回归为 97 项通过、1 项 Linux 专属用例跳过（共 98 项），见 `build/strategy-full-tests.log`。新增的丢失写入响应后重试测试也在 macOS/Linux 定向测试通过。未修改 UI 或部署包，本阶段没有重打 DMG。

下一步：实现已安装 Agent 自身的显式升级（托管服务已支持停止后更新），再扩展实时行情游标、检查点/日志轮转。程序已纳入分发，实例仅在显式授权后创建；不宣称实时/实盘策略交易、任意插件装载、Python/Rust SDK 或不可信策略沙箱。

## Agent 管理接入

`node.proto` 增加显式 STRATEGY 服务类型。部署必须上传并验证匹配平台的可执行文件，拒绝携带行情 provider 或研究 worker 附件。Agent 持久化服务类型、程序摘要和 desired 状态，独立健康通道校验 session/correlation/instance，区分等待初始化、就绪、恢复异常与无响应；复用现有最多 3 次有限重启机制。

策略进程崩溃、显式服务重启或 Agent 重启后，从同一份日志恢复，Terminal 断开不拥有其生命周期。显式停止的实例在 Agent 重启后保持停止。Terminal 的服务类型已更新，不会把策略错误路由到交易连接按钮；已增加策略连接、配置与授权控制，程序已加入桌面服务包。

集成测试使用临时 Agent 根目录，通过真实上传/部署协议安装测试程序，并验证附件拒绝、未初始化健康、业务初始化、崩溃自动重启、显式重启、Agent 重启、停止状态恢复及原 receipt 不变。没有替换本机已安装 Agent 或用户数据。

本次 Agent 接入验证：macOS 定向 8/8（`build/strategy-agent-tests.log`）；Linux x86_64 Ubuntu 容器定向 10/10，另覆盖研究恢复和现有服务心跳（`build/strategy-agent-linux.log`）。TypeScript/Vite 构建通过（`build/strategy-agent-ui.log`）。Windows 原生、策略经远程 Agent 部署和物理跨机器链路尚未验收。

本次全量 macOS CTest：98 项通过、1 项 Linux 专属用例跳过（共 99 项），见 `build/strategy-agent-full-tests.log`。没有重打安装包或变更已安装服务。

## 历史模拟账户的授权交接

交易协议新增 `strategy_grant`、`strategy_revoke`、`strategy_target` 命令，以及账户快照中的授权状态。授权只接受没有推进行情、没有持仓或委托的新账户，绑定策略实例、来源流、标准 TradeDataset 内容摘要和非负整手目标持仓上限。授权 ID 一经使用不可复用；命令 request_id 保留原有持久化幂等与冲突拒绝语义。

这些是受信任控制端管理的业务归属与限制；grant_id 不是密码或能力令牌，不提供客户端角色认证。当前本机同用户 IPC、远端受信任 mTLS 客户端拥有控制权限，不能把这些身份隔离声明用于第三方不可信策略。实盘仍拒绝启动。

授权生效后禁止手工下单、撤单或结算与策略混用；历史时钟 advance 仍由受信任控制端推进。意图必须携带同一授权、策略、来源和数据摘要，序号等于账户当前 cursor，时间等于该笔历史事件，且严格新于已接受意图。订单 ID 使用意图的 request_id；已持久化的相同命令在撤销或重启后重试只确认既有操作，不再次执行，不同内容复用 ID 拒绝。

目标到限价单的转换归 Paper 执行插件，回测复用同一方法：支持多头/空仓与今昨仓分桶平仓，撤销旧的未成交委托，以交易进程当前已知成交价作限价，调整到授权范围内的目标，仍经过统一账户资金、冻结与数量检查。失败保留原订单、资金和授权游标。提交发生在行情推进后，最早在后续事件按既有成交量模型撮合，不在信号自身事件成交；回放结束拒绝新委托，末笔信号不自动平仓。

撤销原子地取消未成交委托并停用授权，保留现有持仓和已成交记录，随后允许人工管理。授权变更、意图和执行仍使用同一交易日志持久化；策略宿主不复制账本。

新增测试覆盖授权范围、过期/超前事件、手工操作排斥、风险失败无副作用、部分成交后撤销、授权不复用、重启幂等。独立 Strategy 与 Trading 进程测试使用明确的测试行情，生成四笔真实模拟成交，并在处理中重启两个进程，确认旧意图不会重复下单。该测试首先验证了交接命令；随后增加的宿主自动回放驱动见下节。Terminal 已接入创建与撤销授权；策略专用网络身份尚未提供。

本次交易交接验证：macOS 全量回归 102 项通过、1 项 Linux 专属用例跳过（103 项，`build/strategy-execution-full-tests.log`）；随后补充的委托替换失败和未完成日志保护，连同账户恢复/双进程链路定向 11/11 通过（`build/strategy-target-tests.log`）。TypeScript/Vite 构建通过（`build/strategy-execution-ui-build.log`）。仍未修改安装包或已安装服务。

Linux x86_64 Ubuntu 容器本次全量 103/103 通过（`build/strategy-execution-linux.log`），最后的日志保护/委托原子性与账户链路定向 11/11 通过（`build/strategy-target-linux.log`）。Windows 原生与物理跨机器策略执行仍未验收。

## 宿主内自动历史回放

Config 可显式携带不可变 ReplayPlan：完整版本化成交数据、目标模拟交易会话、已存在的 grant_id，以及本机 Agent 地址或远端 TCP/mTLS 地址与身份文件路径。计划不携带私钥内容。驱动不会创建账户或自行授权，必须先由受信任控制端准备匹配数据版本的账户及授权。自动会话拒绝外部手工注入策略事件。

`apps/services/strategy/replay.cpp` 按账户和策略的两个持久化游标恢复。账户与策略相同进度时，先用原意图身份核对/补交，再提交下一笔确定身份的 advance；账户仅领先一笔时，消费已经推进的那笔数据；领先更多或策略反超则拒绝运行。序号、时间、合约、数据版本、授权实例和限额均校验。所有新订单仍由 Trading 进程处理；算法只收到逐笔历史输入，不接收未来标签。

意图 request_id 来自已持久化 receipt，advance/finish 身份由不可变计划确定。不会对不明手工命令进行猜测或重发。运行期间的连接或业务错误进入 blocked 并停止推进；修复后通过 Agent 重启实例，从已知命令身份和两份日志重新核对。启动阶段最多 20 秒仅重试读取依赖状态，不发送试探性变更。

到达数据末尾后，驱动取消未成交委托并撤销自己的授权，保留持仓，不强制平仓；完成后重启只幂等确认同一撤销命令。快照中 replay.phase 为 waiting/running/completed/blocked，诊断放在 replay.error；进程健康与回放阶段须分别呈现。当前以宿主短轮询推进历史事件，不按原始时间间隔重演，也不是实时行情订阅。

本机连接每次经 Agent 查询目标 PAPER_TRADING 服务的当前端点，避免绑定旧 socket；远端强制 TCP/mTLS，无明文降级。三平台共用同一驱动。原有不带 ReplayPlan 的事件接收会话继续按其明确模式运行，不自动转为交易模式。

新增恢复测试在 advance 发送前/提交后、target 发送前/提交后、最终 revoke 提交后模拟断线，再从两份日志重建，验证相同余额、费用、四笔订单和四笔成交。真实进程测试不再用测试代码推进行情，而是通过 Protobuf 创建自动计划，由实际 Strategy 程序通过 mTLS 驱动独立 Trading，验证自动完成和重启幂等。本机 Agent 地址解析与 Terminal 全链路已在后续增量中贯通。

自动驱动初版验证：macOS 最终全量 108 项通过、1 项 Linux 专属测试跳过（共 109 项，`build/strategy-replay-final-tests.log`）；Linux x86_64 Ubuntu 容器定向 30/30（`build/strategy-replay-linux.log`）。旧恢复测试曾期待忽略 pending.tmp，已改为验证明确拒绝与现场保留，并在已知测试注入文件经显式清理后再恢复；Terminal 也提前显示该原因。相关恢复与自动驱动定向在 macOS/Linux 均 5/5（`build/strategy-replay-recovery-tests.log`、`build/strategy-replay-recovery-linux.log`）。该阶段尚未验收 Windows 原生、本机 Agent 自动计划全链路及物理跨机器；随后 Terminal 增量完成本机链路验证，见下节。

## Terminal 本机策略控制

交易工作区的“策略运行”使用当前选中的历史数据和全新本机模拟账户，填写短/长窗口与目标手数后显式“授权并运行”。Terminal 将不可变输入和参数传给 Agent 托管的策略进程，不在浏览器中推进回放。首条产品路径仍是单合约、单日、可信 C++ SMA 历史模拟，不是实时策略或实盘。

`strategy.run` 要求稳定的运行 ID。创建策略服务、授予账户权限、提交策略计划是三个可独立失败的操作；出错不自动重发或回滚。界面保留当前尝试的 ID，允许未初始化实例显式重试相同配置。已存在授权时配置锁定；也可撤销授权后创建新账户。交易服务按数据版本验证授权，不能以同名合约替换原始输入。

`strategy.revoke` 显式携带 grant_id，直接由交易账户核对并持久化撤销，策略服务停止或失联不阻止撤销。它取消剩余委托、保留持仓，并阻止后续新意图；已提交请求的幂等重放不产生新订单。撤销不等于停止进程，服务启停仍在设置的节点列表中进行。手动下单、撤单、结算和 UI 手动推进在授权激活时禁用，撤销后恢复。

节点列表通过 `strategy.attach` 连接已有策略。页面显示进度、运行阶段和连接状态，配置 ID 与诊断收进详情；C++ 客户端只向页面返回公开摘要，不返回完整数据集、Agent socket 或远端 TLS 身份路径。原生计划支持 TCP/mTLS；Terminal 创建自动计划目前限定本机账户，远程配置尚未开放。

为允许 Terminal 观察与策略驱动同时连接，交易进程采用 8 个接入线程、8 个等待任务的有界线程池；账本操作在单个互斥区内串行执行，收包/发包不持有账本锁。本机与远程空闲连接均限 30 秒，Terminal 每 5 秒心跳。每条 TLS 连接独立拥有 I/O 上下文，不能与监听器或其他连接共享同步操作的事件循环。这个并发接入机制不改变可信客户端边界。

`strategy_terminal` 验证 Terminal 在线时策略推进、关闭 Terminal/删除原 CSV 后独立完成、重启幂等、运行中撤销及进程离线时撤销；`remote_trading` 增加两个长连接同时提交同一请求仍只记账一次。浏览器用实际 C++ 桥验证授权、进度和手动操作互斥。

Terminal 接入与并发修复验收：macOS 全量 109 项通过、1 项 Linux 专属测试跳过（共 110，`build/strategy-terminal-full-tests.log`）；浏览器策略/原工作台定向 3/3，策略/中英文定向 4/4（`build/strategy-ui-e2e.log`、`build/strategy-ui-final.log`），`pnpm build` 通过。Windows 原生、物理跨机器以及新安装包验收仍未完成；本轮未替换用户已安装应用或 Agent。

Linux x86_64 Ubuntu 容器全量 110/110 通过（`build/strategy-terminal-linux.log`，251.65 秒），包括本机策略/Terminal 并发、离线撤销、双 mTLS 客户端幂等和 Agent 远程部署。容器验收不替代 Windows 原生或物理跨机器验收。


## 策略分发验证

桌面包提供当前平台的 `asterion-strategy`，同时嵌入 Linux x86_64 对应程序。Linux 清单包含 8 个应用程序、CTP 数据插件和初始化脚本，共 10 项资源；Python staging 与 C++ 运行时均校验策略文件、架构和摘要。CI 的 Linux 构建目标和三平台 Electron 资源列表已同步。Node-API 模块通过 CMake 显式链接策略参数校验所需的 SMA 静态库。

macOS 资源校验/SSH 回归 2/2、Linux 策略/资源/部署回归 3/3。`pnpm desktop:build` 成功，挂载 DMG 后验证应用与各程序签名、内置 Linux 资源，并以包内 Agent/Trading/Strategy 运行 `strategy_terminal`，确认独立运行、重启幂等及离线撤销；包内研究恢复和 CTP SDK 回环也通过。证据为 `build/strategy-bundle-tests.log`、`build/strategy-bundle-linux.log` 和 `build/strategy-desktop-build.log`。

本轮未替换已安装应用或服务。DMG 为本机 ad-hoc 签名，未公证；Windows NSIS 与 Linux DEB 本轮未原生构建。已有 Agent/托管服务的显式升级仍待实现，不能把新包隔离验收等同于已有部署升级验收。

## 历史回放的交易日约束

绑定日程的历史模拟账户与策略回放使用同一份 CalendarPublication。交易进程拒绝越过未完成的日终边界，策略通过确定命令身份提交结算，Terminal 提供配置与观察。具体交易与结算规则归 Paper 执行插件，策略进程不复制账户账本；无日程计划不推断日期或自动结算。

第一步已落地：PaperReplaySchedule 位于 plugins/execution/paper/，负责显式日程与历史成交的映射、时段/日终边界及模拟器价格约束；多日回测已调用同一实现，不再保留应用内单独 locate 算法。空时段允许保留，但每个声明交易日必须有成交；重复时间戳按事件位置保留。时段末指该时段最后一笔已知历史事件，不代表实时市场闭市通知。随后已接入 PaperSession 和 ReplayPlan，当前状态见下节。

## ReplayPlan 版本 2 的逐日恢复

计划版本 2 增加可空的完整 CalendarPublication；未绑定日程的普通历史计划为 null，旧计划版本 1 拒绝，不迁移。已绑定计划与交易账户必须同时具有相同发布物，逐项校验合约、内容、来源和身份；账户结算计数必须与事件游标一致。驱动使用同一个 PaperReplaySchedule 识别日终，结算仍由 Trading 日志与执行插件完成。

日终命令身份由不可变计划摘要、策略实例及日序号确定。已处理末事件后提交结算，再推进下一日；最后一日结算完成才撤销授权。回包丢失后以账户结算计数确认提交，重复命令沿用既有请求身份。授权已经撤销而结算尚未完成时明确拒绝继续。Terminal 已支持选择完成的结算表任务，先绑定账户，再授权并创建版本 2 计划。日终时段约束由交易进程执行；新增独立进程验收在首日结算提交后暂扣回复、强制终止 Strategy/Trading，再重启完成后续日程并验证无重复入账。

Terminal 显示结算日程来源、已结算日数及发布详情。绑定账户的手动接管使用“按日程结算”，待结算时禁用下一笔推进，时段末禁止提交新单，不再显示自由价格结算输入。后台派生 settlement_due/session_end 状态，界面不根据本机时间猜测边界；最终拒绝规则仍在交易进程。
