# 开发计划

这是唯一的开发计划：做什么、按什么顺序、做到什么程度算完成。人和 AI 都按下面的执行顺序表从上往下做。
产品目标见[产品说明](product.md)，交易必须遵守的规则见[交易规则](safety.md)，代码怎么写见仓库根目录的 `AGENTS.md`。

## 执行规则

- 取执行顺序表中第一个未完成、依赖已完成、且不需要使用者操作的工作包。一次只做一个。
- 只做工作包写明的内容。路上发现的其他问题追加到文末“待定问题”，不在当前工作包里修。
- 提交标题以工作包编号开头，例如 `M3.7: size weight targets with explicit rounding`。完成后把表中状态改为“完成”，并写一句结果。
- “完成标准”里的命令必须跑通；跑不了的写明原因。
- 需要真实柜台、真实凭据、签名或正式部署的步骤由使用者做，表中标为“使用者”。
- Lab 的 Python 测试要先运行 `pnpm prepare:python`。本机报 pybind11 缺失时，先按 `.github/workflows/ci.yml` 中的 `conan install` 命令重装依赖。

## 已做的决定（2026-11-01）

- 策略实例：断线和执行超时不再结束实例；实例可以用它自己记录过的 K 线热重启，但不读其他来源的历史数据（M3.9）。
- 实盘换月：两周仿真浸泡先用固定月份合约，换月在第 4 阶段之前完成（M4.0）。
- 交易规则只保留影响资金安全和使用者操作的 9 条（[交易规则](safety.md)），不再把每个边界情况写成规则；其余文档在 M3.14 精简。

## 执行顺序

| 顺序 | 编号 | 内容 | 状态 | 依赖 |
| --- | --- | --- | --- | --- |
| 1 | M3.7 | 权重下单修复 | 完成：`权益 × 权重` 向零取整，带分的权益配 8 位小数权重不再报错 | |
| 2 | M3.8 | CI 恢复 | 待做 | |
| 3 | M3.9 | 策略实例存活与热重启 | 待做 | |
| 4 | M3.10 | Market 版本按批提交 | 待做 | |
| 5 | M3.11 | 研究源码固定 | 待做 | |
| 6 | M3.12 | 录制写入不阻塞行情 | 待做 | |
| 7 | M3.13 | 研究与界面小修 | 待做 | |
| 8 | M3.14 | 文档与代码瘦身 | 待做 | |
| 9 | M3.3 | 实例操作与监控（剩余部分） | 进行中 | M3.9 |
| 10 | M3.4 | 客户端收敛 | 待做 | |
| 11 | M3.6 | 两周仿真浸泡 | 待做（使用者） | M3.3、M3.9、M3.10 |
| 12 | M4.0 | 实盘换月 | 待做 | M3.6 |
| 13 | M4.1–M4.4 | 多因子组合 | 未开始 | M4.0 |
| 14 | M5 | 小资金实盘 | 未开始 | M4.4 |

## 第 3 阶段：让策略在仿真柜台上稳定运行

### M3.7 权重下单修复

问题：`core/src/futures/portfolio_targets.cpp` 用精确乘法计算 `权益 × 权重`。权益带分、权重有 8 位小数（如 1/3、波动率倒数加权）时，乘积超过 8 位小数，抛出 `decimal result would require rounding`。回测会整次失败，实盘会让该批次失败并结束实例。

改动：
- `requested` 改为 `multiply(equity, member.weight, Rounding::toward_zero)`；`unallocated_nominal_budget` 里的 `equity * gross_weight_limit` 同样处理。
- `tests/core/portfolio_targets_test.cpp` 加一例：权益 `999996.75`、权重 `0.33333333`，得到向零取整的手数。
- `tests/lab/backtest_test.py` 加一例：`momentum_rank.MomentumRank` 设 `volatility: 3` 的回测能跑完。

完成标准：`ctest --preset conan-debug -R "PortfolioTargets|lab_backtest"` 通过。

### M3.8 CI 恢复

问题：最近 40 次推送有 39 次 CI 失败，CI 不能提供回归信号。
- 三个进程测试在 CI 机器上超时，本机连续通过：Linux 的 `CtpPublication.CatalogRevisionIsOmittedOnlyWithinItsWatchConnection`，macOS 的 `CtpEvents.ProcessExposesBoundedProtobufReadsAndRejectsStaleIdentity` 和 `market_recording_process`。
- 两个 sanitizer 作业在 `python scripts/desktop/python_runtime.py` 这一步崩溃：插桩过的 `asterion_engine` 被加载进未插桩的 Python。它们从未运行测试。
- 桌面作业依赖其他作业全部通过，所以一直被跳过。

改动：
- 三个测试里的固定等待改为等待具体条件；确实需要时间上限的，放宽到慢机器也够用。
- sanitizer 作业二选一，取改动更少的一种：Python 进程加载 `asterion_engine` 前预加载 sanitizer 运行库（Linux 用 `LD_PRELOAD` 指向 libasan，macOS 用 `DYLD_INSERT_LIBRARIES` 指向 TSan 运行库）；或者在 sanitizer 作业里不构建 `asterion_engine`，跳过依赖它的 Lab 测试。

完成标准：
- 本机在单核限制下（`taskset -c 0 ctest --preset conan-debug -R "<三个测试>" --repeat until-fail:5`）连续通过。
- 使用者推送后，同一提交的 CI 全部作业通过，包括桌面作业。

### M3.9 策略实例存活与热重启

问题：实例在以下情况都会结束，之后只能用新实例从空窗口重新预热（20 日窗口的日线策略约需 21 个交易日）：
- 行情连接断开（`apps/services/trading/strategy_host.cpp` 中的 `strategy market connection interrupted`），行情时间落后超过 10 秒，时间倒退；
- 一个目标批次 10 秒内没有全部成交；
- 第一个可执行目标到达时账户还没授权；
- Trading 重启（实例标为 `interrupted`）。

柜台前置常在非交易时段维护或周末重启，所以日线策略几乎等不到就绪。

要做到：
1. 行情断开后实例不结束，等 CTP 自动重连。重连后，早于已处理行情或落后超过 10 秒的报价直接跳过，不用于 K 线和下单价格。断开和恢复的时间写入实例记录 `runs/<实例>.sqlite`。跨越断开的 K 线照常交付。
2. 批次在时限内没完成时：撤掉本批未成交的委托，放弃本批剩余目标，实例继续运行；下一根 K 线的目标照常执行。每个分支下单时用最新行情定价，不再沿用批次开始时的报价。
3. 账户未授权时，就绪目标只记录、不执行，实例继续运行。
4. 热重启：启动实例时可以指定一个已结束的旧实例，要求包名、版本、合约、周期和参数都相同。新实例先把旧实例记录里的 K 线按原顺序交给策略（只更新状态，不执行目标），然后接实时行情。新实例用新的 ID，它的记录也包含这些重放的 K 线，所以可以被再次热重启。授权规则不变：Trading 重启后要重新授权。
5. 以下情况仍然结束实例：策略代码异常、超时或返回无效结果；策略子进程退出；柜台拒绝策略委托；交易记录写入失败；使用者停止。

改动范围：`apps/services/trading/` 下的 `strategy_host.cpp`、`strategy_bars.cpp`、`strategy_execution.cpp`、`python/strategy_runner.py`；`protocol/proto/asterion/strategy.proto`（启动参数加旧实例 ID）；交易页 `features/trading/StrategyInstance.tsx`（“从上次记录恢复”）；`docs/trading.md`、`docs/architecture.md`、`docs/product.md` 中关于现场预热的描述。

完成标准：
- 用测试 SDK 的隔离测试覆盖：断线重连后实例继续，K 线不重复、不倒退；批次超时后实例继续，下一批正常下单；未授权时目标不执行，实例继续；Trading 重启后热重启，之后同样的 K 线得到与不重启时相同的目标。
- `ctest --preset conan-debug -R "StrategyBars|StrategyExecution|StrategyTarget|strategy_process|strategy_stream_process"` 通过。

### M3.10 Market 版本按批提交

问题：采集分钟线、结算参数和交易所日线时，每写一个交易日分区就提交一个新版本（`apps/services/market/acquisition/manager.cpp` 的 `commit()`），每个版本的清单列出该来源的全部分区，旧版本不删除。清单总量随分区数二次方增长：20 个品种、3 年的分钟线约 22 GiB。单个清单 32 MiB 的上限在约 15 万个分区时达到，之后再也发布不了新版本。

改动：每累计 200 个分区、距上次提交超过 5 分钟，或任务结束、取消、失败时，才提交一次。取消和失败时仍提交已写好的分区，保持“已取得的数据保留”。

完成标准：
- 新测试：一个任务写 1000 个日分区，只产生约 5 个版本；取消后已写分区可以读到。
- `ctest --preset conan-debug -R "Acquisition|ExchangeSources|RecordedSource|Warehouse"` 通过。

### M3.11 研究源码固定

问题：回测、扫描候选和滚动验证的子进程启动时，才从策略仓库导入当时的文件。运行记录只有提交号和改动摘要，代码无法恢复。滚动验证不核对候选的代码，会用旧结果选参数、再用新代码运行。

改动：
- 研究请求被接纳时，把策略仓库里 `git ls-files -co --exclude-standard '*.py'` 列出的文件复制到运行目录。扫描只复制一次，候选和滚动验证共用这一份。总量超过 16 MiB 时拒绝。
- 子进程从这份副本导入，不读策略仓库。运行记录照旧写提交号和改动摘要。

改动范围：`apps/services/lab/asterion_lab/` 下的 `service.py`、`backtest.py`、`rolling.py`、`repository.py`、`runs.py`。

完成标准：
- 新测试：接纳之后修改仓库文件，回测、扫描候选和滚动验证仍执行接纳时的代码。
- `ctest --preset conan-debug -R "^lab_"` 通过。

### M3.12 录制写入不阻塞行情

问题：录制写入积压 4 个文件时，行情状态线程在 `future.get()` 上等待（`apps/services/market/recording/recorder.cpp` 的 `flush`）。磁盘持续变慢时，容量 65,536 的 SDK 入口队列可能写满，按现有设计行情流随后永久失效，要人工重启服务。

改动：
- 积压达到上限时不等待：丢弃这批待写数据，结束当前录制段（结束原因 `write_backlog`），丢失行数计入录制状态。
- 入口队列写满后，写完缓冲并以非零状态退出，由 systemd 重启。

完成标准：
- 测试：写入线程被阻塞时，状态线程继续推进、报价继续更新，录制状态显示丢失行数。入口队列写满时进程以非零状态退出。
- `ctest --preset conan-debug -R "Recorder|market_recording_process|market_concurrency"` 通过。

### M3.13 研究与界面小修

每项一个测试：
1. `cancel_sweep` 同时终止该扫描已经启动的滚动验证（`<扫描>-rolling`），并取消尚未启动的滚动验证（`asterion_lab/service.py`）。
2. `apps/clients/terminal/electron/lab-channel.cjs`：连接在回复完整之前关闭时返回错误，不再一直等待；接收时不再每次拼接全部已收数据。用本地假服务端写一个 Node 测试。
3. 参数扫描先算组合数，超过 10000 就拒绝，不先展开全部组合（`service.py`）。
4. 回测子进程的标准输出读后丢弃，标准错误只保留最后 64 KiB（`service.py` 的 `watch`）。
5. 因子：分组曲线固定使用下一根 K 线的收益，不随第一个持有期变化；持有期 h > 1 的 IR 和 t 值改用每隔 h 天取一个 IC 的样本计算（`asterion_lab/factor.py`）。
6. 最小变动价位只用截至当天的 20 个交易日推断，删掉向后看的窗口（`asterion_lab/backtest.py` 的 `read_increments`）。
7. CTP：委托回报晚于成交到达时，给已有的成交补上委托编号（`apps/services/trading/ctp/ctp_reconciliation.cpp`）。

完成标准：`ctest --preset conan-debug -R "^lab_|ReportReconciliation|CtpTrader"` 和新的 Node 测试通过。

### M3.14 文档与代码瘦身

目标：文档只写使用者能做什么、看到什么，以及关键设计取舍（附一句理由）。删掉实现细节（线程数、队列容量、代次、许可、额度）、“不代表 / 不能证明 / 仍待验收”之类的重复声明和过时内容。

改动：
- 行数目标：`trading.md` ≤ 150，`development.md` ≤ 150，`lab.md` ≤ 120，`architecture.md` ≤ 100，`market.md` ≤ 100，`recording.md` ≤ 80，`services.md` ≤ 80，`terminal.md` ≤ 80，`status.md` ≤ 40。
- 修正已知过时内容：`services.md` 的“当前目标不产生订单”；`terminal.md` 的 Tushare Token 设置项；`development.md` 说 Secret Service 用来记住行情与数据源凭据；`dominant.py`、`backtest.py` 注释中引用的已删除 Data 服务。
- 删掉没有调用方的旧代码。先用 `git grep` 确认，例如为已删除的 Data 服务保留的 `DataCredentials`。

完成标准：
- 行数达标，文档中的链接都指向存在的文件。
- `git grep -n "Data service\|Data instance"` 没有结果。
- 改了代码的部分，相关 `ctest` 和 `pnpm lint` 通过。

### M3.3 实例操作与监控（剩余部分）

已完成：发布策略包、配置实例、确认后启停、状态读取。

剩余：
- 暂停新增：实例继续接收 K 线、计算目标，但只执行减仓；可以恢复。
- 平仓：对实例所选合约下平仓目标，走同一条执行路径，需要单独确认。
- 执行监控：交易页显示最近的批次、委托、成交、失败原因和断线记录。
- 告警：实例结束、委托被拒、未确认委托、行情断线写入 Trading 日志；Terminal 在线时弹桌面通知。外部推送渠道以后再定。

完成标准：隔离测试覆盖“暂停后不开仓”和“平仓后持仓为零”；交易页有组件测试；中英文文案同时更新。

### M3.4 客户端收敛

行情和交易改用生成的 TS 客户端，方式与 Lab 和 Market 数据页相同；删除 Node-API 原生编排和手写 JSON 映射。

完成标准：`apps/clients/terminal/native/` 中行情、交易客户端的编排代码已删除；`pnpm run test:e2e:journeys` 通过。

### M3.6 两周仿真浸泡（使用者）

- 在 SimNow 用固定月份合约的策略连续运行两周，每天收盘后核对成交、持仓和资金。
- 期间演练：关闭 Terminal；停止 Market 和 Lab；行情断线（实例应继续运行）；Trading 重启后热重启并重新授权；柜台断线。
- 每天记录对账结果，以及每处不一致和原因。

第 3 阶段完成条件：浸泡期间每天对账一致；上面的演练都按预期运行。

## 第 4 阶段：多品种组合

### M4.0 实盘换月

目标：实盘实例可以交易主力连续（例如 `SHFE/rb`），合约到期前自动移仓；策略看到的连续历史与研究一致。

- 研究和实盘统一改为“按当时复权”：每次换月，把已有历史按新旧合约前一交易日结算价之比整体调整，当前合约价格保持原价。研究侧以此替换 `dominant.py` 的整段后复权，这样追加未来数据不会改变过去的价格。因子评价可以继续用整段后复权，在 `lab.md` 写明。
- 换月判断与研究相同：前一交易日持仓量最大的合约，只向后换。实盘从 Trading 自己的行情读取持仓量。换月日先平旧合约、再开新合约，都走同一条执行路径。
- 下一个月份必须在账户白名单内；不在时，换月前结束实例并提示。

完成标准：研究侧测试证明追加未来数据不改变过去的 K 线；实盘隔离测试跨过一次换月后，持仓在新合约上，策略历史连续。

### M4.1–M4.4 多因子组合

| 编号 | 内容 |
| --- | --- |
| M4.1 截面研究 | 多品种日线面板、因子合成、样本外检验 |
| M4.2 组合构建 | 在权益、价格、乘数和保证金约束下配置权重，确定调仓阈值和时点 |
| M4.3 一个账户多个策略 | 账户层合并目标，确定成交和费用的归属，以及与手动委托共存的规则 |
| M4.4 组合仿真 | 多品种组合在 SimNow 按日调仓两周，每天对账一致 |

注意：目前一个批次的分支逐个发送，每个分支都要等一次持仓查询（间隔 1.1 秒）。合约多时一个批次需要十几秒以上，M4.2 之前要允许互不影响的分支同时发送。

## 第 5 阶段：小资金实盘（M5）

真实资金账户第一次下单前，以下各项都要完成并留下记录：
- 仿真浸泡（M3.6、M4.4）每天对账一致。
- 通过期货公司的穿透式认证，取得 AppID 和授权码。
- 同一个资金账户，无论有几份配置、在几台主机上，都只有一个执行者。
- 日内亏损和回撤限额：正确处理出入金、换日、重启和重新授权。
- 告警覆盖实例结束、委托被拒、未确认委托、连接中断、风控触发和对账不一致；“停止全部策略并撤单”演练通过。

然后用小资金连续运行一个月，每天对账，没有解释不了的差异。

## 外部事项（使用者）

- Market：在主机配置 Tushare token；处理大商所、郑商所部分归档返回的 HTTP 412；正式部署（systemd、加密凭据、开机启动），录满一个完整的日盘和夜盘，记录数据量和合并耗时。
- Trading：正式部署；准备 SimNow 账户；确认 SimNow 和期货公司柜台前置的维护时间，供 M3.9 测试和 M3.6 安排参考。
- 桌面：macOS 签名和公证。

## 待定问题

- 风控是否增加资金/保证金占用、日内亏损和撤单次数限制（M5 之前决定）。
- 同一资金账户跨配置、跨主机只有一个执行者的实现方式（M5 之前决定）。
- Tushare 分钟线是否把集合竞价和收盘 tick 并入相邻 K 线，与实盘和录制的分钟 K 线是否一致（M4.0 之前核实）。
- 按批提交之后，单个版本清单仍有 32 MiB 上限（约 15 万个分区）；需要时再改清单格式。
