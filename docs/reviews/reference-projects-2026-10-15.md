# 本地量化项目功能与架构借鉴评审

日期：2026-10-15。对象：`/home/qiwen/Code/quant` 下全部 8 个项目，包括 Asterion 和 7 个参考项目。
本报告交付源码比较、功能规划建议和架构取舍；建议项尚未实施，也不改变现行产品契约。

**结论：保留 Asterion 现有服务划分与 C++ 主干，借鉴成熟项目的业务分解、实验流程和操作体验。**
当前最有价值的投入是扩大可靠历史事实的可用范围、让研究结果更可解释，以及补齐实际策略进入
CTP 仿真前的运行条件。重新建设通用交易引擎、研究调度平台或桌面技术栈，收益不足以支持其成本。

| 参考项目        | 最值得借鉴                                                    | 在 Asterion 的具体落点                                  |
| --------------- | ------------------------------------------------------------- | ------------------------------------------------------- |
| vn.py           | 国内期货操作语义、行情/委托/成交/持仓的分工、研究入口的连贯性 | 平今平昨与冻结量解释、账户操作反馈、数据到实验的交互    |
| WonderTrader    | 策略目标与执行分离、连续合约到真实月份的映射                  | 目标/实际仓位差异、换月过程解释、未来单一组合候选的执行 |
| NautilusTrader  | 类型化订单事件、确定性时间推进、核对职责和数据分批边界        | 账户核对测试、策略时间一致性、F11 容量设计              |
| LEAN            | 选池、信号、组合目标、风险调整、执行的清楚分解                | 动态研究成员与持仓的区别、执行差量、历史规则建模        |
| Qlib            | 数据处理与训练/推理分离、实验产物组织、滚动研究               | 因子解释、同口径比较、未来确有需求时的拟合变换          |
| RD-Agent        | 假设、实现、运行、评价、反馈的可追溯闭环                      | 先完善研究记录；AI 先做解释与实验草稿，后续另定执行边界 |
| FinceptTerminal | 面板实例、合约联动、订阅生命周期和数据新鲜度                  | 现有行情与研究工作区之间的上下文延续和来源提示          |

## 阅读范围与证据强度

本轮覆盖各项目的入口说明、目录组织、核心接口和代表性调用链，并抽读相应测试与规划文档。
“深入”指追到支持结论的实现边界，**不代表逐行审计所有文件、第三方 SDK 或全部适配器**。
没有启动这些项目、安装其依赖、访问行情供应商、执行策略或复现性能宣传。

下文区分三种证据：源码可见的行为；项目文档声明的功能或目标；结合 Asterion 约束作出的建议。
文档声明不自动提升为运行验证。对独立仓库、商业版本和未下载组件，不作源码已核验的结论。

| 项目            | 本地 HEAD      | 阅读时工作树     | 核对重点                                                      |
| --------------- | -------------- | ---------------- | ------------------------------------------------------------- |
| Asterion        | `87e2698bd557` | 有既存未提交修改 | 当前所有权、研究链、任务验收、SHFE 新接入                     |
| vn.py           | `c6e231caf32b` | 清洁             | EventEngine、MainEngine、Gateway、OffsetConverter、Alpha      |
| WonderTrader    | `08b230dd05fa` | 清洁             | CTA 目标汇总、执行器、主力映射、回放、风控                    |
| NautilusTrader  | `ce453eb99573` | 清洁             | Rust Kernel、Risk、OrderCore、Reconciliation、Backtest、Clock |
| LEAN            | `80e7843f6456` | 清洁             | Framework、AlgorithmManager、期货保证金与撮合、交易处理       |
| Qlib            | `543552324638` | 清洁             | Dataset/Handler/Processor、Trainer、Recorder、Task、PIT       |
| RD-Agent        | `484776c211e4` | 清洁             | RDLoop、Trace、Qlib 工作区、factor runner 与反馈              |
| FinceptTerminal | `3d8617cae3f1` | 清洁             | Qt DockScreenRouter、DataHub、屏幕状态、PythonRunner          |

完整提交身份和 Asterion 既存修改的文件摘要保存在
`build/reviews/reference-projects-2026-10-15/baseline.json`。中断后继续工作时，8 个 HEAD
以及基线记录的 72 个既存修改/未跟踪文件的内容均未变化。参考项目仅作读取。

已阅读[架构设计评估](architecture-design-review.md)、[实施记录](architecture-implementation.md)
和[审计进度](audit-progress.md)，并与当前源码比较。前两者包含早期阶段语境，不能直接拿来
生成待办。现行 [AGENTS.md](../../AGENTS.md) 的“只维护当前实现”规则优先；旧评估中的
递增引擎版本要求不再适用。保留内容摘要、插件 ABI、传输契约及数据/政策修订的当前用途。

## 各项目的功能组织和实现取舍

### vn.py：学习期货操作模型，区分核心仓库与扩展生态

功能组织以交易平台、gateway 和 app 为中心。核心提供事件分发、统一数据对象、接口路由、
OMS 查询和 Qt 界面；CTA、价差、组合、算法交易、数据管理等许多功能由独立 app 仓库提供。
本地还包含 Alpha 研究模块，将数据集、模型、信号和回测组织成一个研究流程。

关键调用链是 `BaseGateway.on_* → EventEngine → OmsEngine/界面消费者`；
`MainEngine.send_order` 则按 gateway 名称调用适配器。`OffsetConverter` 跟踪今仓、昨仓、
活动委托冻结量，把用户平仓意图转换为交易所要求的委托。这些领域细节比“支持多少接口”更值得借鉴。

源码依据：[事件队列](../../../vnpy/vnpy/event/engine.py)、
[主引擎与 OMS](../../../vnpy/vnpy/trader/engine.py)、
[Gateway 契约](../../../vnpy/vnpy/trader/gateway.py)、
[开平转换](../../../vnpy/vnpy/trader/converter.py)、
[转换测试](../../../vnpy/tests/test_converter.py)。

对 Asterion 的价值有两点。第一，用户应该能看到总持仓、可平量、冻结量、平今/平昨和实际
委托之间的关系；失败信息应解释是哪一项限制了操作。第二，数据选择、参数设置、结果分析应
保持同一研究上下文，减少在多个入口中重复选择合约和数据。

不能照搬的部分也很具体：

- `EventEngine` 使用无上限的 `Queue()`，处理器在事件线程顺序调用。Asterion 已有有界队列、
  慢 I/O 隔离和控制容量，不应为了引入通用事件总线而撤掉这些边界。
- 核心 `send_order` 的路由本身不提供 Asterion 所要求的持久意图、统一授权和风险准入。
  这不是对 vn.py 整个扩展生态的安全结论，而是两者核心边界不同。
- SHFE 转换示例先平今再平昨；Asterion 当前 `target_orders` 明确先平昨再平今。
  借鉴拆分与冻结量模型，不擅自改变已定义的费用和开平顺序。
- Alpha 回测代码有用上一价格补缺失 bar、按前收盘价乘 `1.1/0.9` 计算涨跌幅边界的路径。
  这些具体模型不适用于直接充当中国期货历史规则；价格和金额的 `float` 也不能进入 Asterion 权威账本。

研究依据：[AlphaLab](../../../vnpy/vnpy/alpha/lab.py)、
[AlphaDataset](../../../vnpy/vnpy/alpha/dataset/template.py)、
[Alpha 回测](../../../vnpy/vnpy/alpha/strategy/backtesting.py)。AlphaLab 的文件更新方式也不应
替代 Asterion 的不可变快照。README 链接的 `vnpy_ctp`、`vnpy_ctastrategy` 等独立实现不在
本地目录，Fusion 的文档不能证明商业服务的具体实现。

### WonderTrader：学习目标仓位与执行的分离，保留实际持仓的独立性

其功能组织最贴近期货策略系统：CTA/SEL/HFT/UFT、行情采集与存储、历史回放、交易通道、
执行单元和组合风控分开。README 的 M+1+N 表达多策略目标汇总后路由到多个执行账户。
这一点在 `WtCtaEngine`、`WtExecuterMgr` 和执行器接口中有实际实现依据。

`WtCtaEngine` 调度策略上下文、枚举理论仓位、解析连续合约对应的真实月份，再把目标放入
执行缓存；`commit_cached_targets` 应用缩放/过滤并交给执行器。`ICtaStraCtx` 同时由实盘
上下文和回测 `CtaMocker` 实现，使策略面向同一类业务操作。

源码依据：[策略上下文](../../../wondertrader/src/Includes/ICtaStraCtx.h)、
[CTA 引擎](../../../wondertrader/src/WtCore/WtCtaEngine.cpp)、
[执行管理](../../../wondertrader/src/WtCore/WtExecMgr.cpp)、
[执行单元契约](../../../wondertrader/src/Includes/ExecuteDefs.h)、
[回测上下文](../../../wondertrader/src/WtBtCore/CtaMocker.h)、
[主力映射](../../../wondertrader/src/Includes/IHotMgr.h)。

Asterion 已有类似的正确基础：`Strategy::on_bar` 只输出可选目标，host 拥有数据源，账户拥有
执行权；目标转订单还规定先完成反向平仓再开仓。因此不需要新建“信号服务”和“执行服务”。
值得增加的是目标生成原因、目标与实际仓位差额、未完成委托和换月两腿的解释，帮助用户判断
“策略想要什么”与“账户实际发生什么”。

必须保留三类区别：

- 无新信号、明确零目标、来源数据缺失是不同状态。WonderTrader 的完整目标汇总中有将
  缺席旧标的设零的逻辑；不能把该规则推广为 Asterion 的“数据缺失即自动平仓”。
- 理论部位、真实账户持仓和研究归因可以分别展示，但后两者的权威来源仍分别是柜台与固定结果。
  不能增加可独立调整风险额度的虚拟子账户。
- “离合器”说明停止信号执行与停止策略观察可以不同；Asterion 的停止策略、撤销授权、撤单、
  平仓仍须分别有明确语义，不通过一个总开关隐式触发全部动作。

[HisDataReplayer](../../../wondertrader/src/WtBtCore/HisDataReplayer.h) 还包含 CSV 加载和模拟 Tick
路径，不能当作 Asterion 的生产数据来源。
[简单风控](../../../wondertrader/src/WtRiskMonFact/WtSimpRiskMon.cpp) 可参考回撤监控的产品含义，
其数值与线程实现不直接移植；[decimal.h](../../../wondertrader/src/Share/decimal.h) 是 double
容差工具，并非本项目的精确 Decimal。`wtpy` 监控台和 README 延迟指标本轮均未验证。

### NautilusTrader：学习事件语义和时间边界，不把单节点 Kernel 当成分布式服务架构

本地版本已经以 Rust 原生运行时为主，不能沿用旧版 Cython 架构印象。其
[ROADMAP](../../../nautilus_trader/ROADMAP.md) 优先稳定核心、改善文档和 API；明确将完整 UI、
内置分布式调度、参数优化及 AI/ML 工具排除在核心开源范围之外。这种范围纪律值得采用。

`NautilusKernel` 组合 Clock、Cache、Portfolio、Trader 和 Data/Risk/Execution engines。
这些对象通过进程内消息机制协作，代码使用 `Rc<RefCell<_>>`，并非多个服务共享一份分布式缓存。
Backtest、Sandbox 和 Live 共用业务组件，环境主要改变时间、数据来源与执行实现。

源码依据：[架构说明](../../../nautilus_trader/docs/concepts/architecture.md)、
[Kernel 所有权](../../../nautilus_trader/crates/system/src/kernel.rs)、
[Clock](../../../nautilus_trader/crates/common/src/clock/mod.rs)、
[RiskEngine](../../../nautilus_trader/crates/risk/src/engine/mod.rs)。

最有价值的三个细节：

1. `OrderCore::apply` 在应用事件前核对订单和策略身份、重复成交以及合法状态转换。
   Asterion 应继续把这些检查放在账户事实进入处，而非让 UI 各自修正订单状态。
2. 核对由报告转换、状态判断、定时请求/期限等不同职责组成。可借鉴纯函数核对及重复、乱序
   报告的测试方法，避免把网络轮询、持仓算术和账户状态修改混在一个函数中。
3. 分批回测明确要求同一个末尾 `ts_init` 的数据不能切到下一批，否则计时器或场所模块可能
   过早完成该时点。Asterion 的 F11 也必须按完整观测截面/交易日语义切块，不能只按行数分页。

对应证据：[订单状态](../../../nautilus_trader/crates/model/src/orders/mod.rs)、
[核对模块](../../../nautilus_trader/crates/execution/src/reconciliation/mod.rs)、
[核对说明](../../../nautilus_trader/docs/concepts/execution/reconciliation.md)、
[核对性质测试](../../../nautilus_trader/crates/execution/src/reconciliation/proptests.rs)、
[BacktestEngine 分批契约](../../../nautilus_trader/crates/backtest/src/engine.rs)。

不能直接采用的行为包括风险 `bypass`、可配置的合成核对成交，以及其架构说明中的部分无界
异步通道。合成调整是该项目明确建模的功能，不等于真实柜台成交；Asterion 不能通过补造
成交消除未知账本差异。Clock 与订单事件模型可以参考，但不增加新的全局事件存储，也不把
现有每账户服务拆成独立 Risk/OMS/Portfolio 网络服务。该版本适配器列表也不能替代 CTP 的实际验收。

### LEAN：学习研究决策链的分解，避免机械复制五套插件接口

LEAN 将 Universe Selection、Alpha、Portfolio Construction、Risk Management 和 Execution
拆成明确职责。`QCAlgorithm.OnFrameworkData` 更新成员和信号；`ProcessInsights` 生成组合目标，
再以风险输出覆盖目标并交给执行。这是业务分工，不要求每一阶段都是一个进程。

`ImmediateExecutionModel` 保存待执行目标，按保证金影响排序，计算尚未下单的数量，再提交
委托。目标数量与本次新增委托数量分开，使已有持仓和在途委托都能进入执行判断。
`AlgorithmManager` 还明确处理时间推进、预热结束、合约映射事件与撮合的先后次序。

源码依据：[Framework](../../../Lean/Algorithm/QCAlgorithm.Framework.cs)、
[立即执行模型](../../../Lean/Algorithm/Execution/ImmediateExecutionModel.cs)、
[事件推进](../../../Lean/Engine/AlgorithmManager.cs)、
[回测交易处理](../../../Lean/Engine/TransactionHandlers/BacktestingTransactionHandler.cs)、
[按持仓量选期货](../../../Lean/Algorithm.Framework/Selection/OpenInterestFutureUniverseSelectionModel.cs)。

这对 Asterion 的直接启发是区分：

- 具有行情的标的集合、按历史可知事实合格的集合、可计算评价标签的集合；
- 因子分数、组合目标权重/手数、实际委托、实际成交；
- 标的退出研究集合与处置已有持仓。

Asterion 的 F04 已用于日线截面因子资格，不能再提一个同名筛选功能。未来若让动态成员进入
回测或实盘组合，需要先明确退出持仓、缺数据、未成交目标和换月的行为，再扩接口。只有出现
多个实际实现时才考虑额外端口，不为了对齐 LEAN 的五阶段名称建立五套空抽象。

期货历史规则也值得读，但不能原样采用：
[FutureMarginModel](../../../Lean/Common/Securities/Future/FutureMarginModel.cs) 区分初始/维持及
日内/隔夜保证金，并以历史条目选择适用值；其公开取值属性存在缺失时 `?? 0m` 的行为。
Asterion 必须继续缺配置拒绝，并沿已实现的日初/结算阶段合同处理中国期货保证金。
[FutureFillModel](../../../Lean/Common/Orders/Fills/FutureFillModel.cs) 的成交规则与
[测试](../../../Lean/Tests/Common/Orders/Fills/FutureFillModelTests.cs) 可用于梳理时段、滑点和跳空
假设，不能证明中国期货封板排队可成交。LEAN 的股票、期权、多币种、CLI 和 Notebook 范围
不进入本项目规划；其云产品能力也不由此仓库阅读得到验证。

### Qlib：学习研究产物和时间隔离，不引入第二个任务系统

Qlib 的组织单位是一次可配置研究流程：数据加载/处理/切段、模型拟合与预测、信号评价、
组合回测和实验记录。`DatasetH` 定义时间段，`DataHandlerLP` 区分 raw、infer、learn 数据，
Processor 负责特定处理，Trainer 组织训练，Recorder 保管配置、模型、预测和评价产物。

源码依据：[研究流程说明](../../../qlib/docs/component/workflow.rst)、
[DatasetH](../../../qlib/qlib/data/dataset/__init__.py)、
[DataHandlerLP](../../../qlib/qlib/data/dataset/handler.py)、
[Processor](../../../qlib/qlib/data/dataset/processor.py)、
[Trainer](../../../qlib/qlib/model/trainer.py)、
[评价记录](../../../qlib/qlib/workflow/record_temp.py)。

其中最值得转化为 Asterion 设计约束的是：

- 需要拟合的标准化等变换，必须固定拟合区间和参数，再应用于验证/推理数据。
  Qlib `MinMaxNorm`/`ZScoreNorm` 会按配置区间取数据，但正确设置区间仍是调用者责任；
  有 train/test 两个名字本身并不保证不存在泄漏。
- 预测信号评价与包含费用的组合评价是不同产物。Asterion 已明确 IC/分组收益不等于可交易
  净值，应继续通过现有回测入口取得账户结果。
- 滚动任务生成明确时间移动与标签截断。现有 Asterion 留出、滚动和跨分界标签剔除已实现，
  后续重点是比较口径和结果解释，不重建 rolling 框架。
- PIT 区分数据所属期间与观察时点。下载时刻、来源报表更新时间、首次可知时间仍必须分开；
  不可变快照解决复算身份，不能自行证明历史可交易性。

时间与调度依据：[RollingGen](../../../qlib/qlib/workflow/task/gen.py)、
[PIT 运算](../../../qlib/qlib/data/pit.py)、
[TaskManager](../../../qlib/qlib/workflow/task/manage.py)。TaskManager 使用 MongoDB 原子领取，
`safe_fetch_task` 异常时将任务返回原状态；这是其研究任务策略，不能替换 Asterion 的 attempt
身份、显式重试和持久完成裁决。MLflow/MongoDB、配置导入和序列化对象加载也不是当前必需依赖。

最小借鉴顺序是：逐观测解释和同口径比较 → 一种有实际研究需求的因子 → 必要时增加一种
训练段拟合变换 → 有明确模型需求再讨论训练任务。不要把 Alpha158 股票特征全集当作期货路线图。

### RD-Agent：学习证据驱动的研究循环，AI 不成为实验事实或交易权威

RD-Agent 把提出假设、生成实验、编写实现、运行、评价和记录拆开。`RDLoop` 通过场景配置
组合这些步骤，`Trace` 保存实验与反馈及其父节点关系；`QuantRDLoop` 进一步组织因子/模型
研究。`QlibFBWorkspace` 在所选 Docker 或 Conda 环境运行 `qrun`，读取输出产物。

源码依据：[RDLoop](../../../RD-Agent/rdagent/components/workflow/rd_loop.py)、
[Hypothesis/Trace](../../../RD-Agent/rdagent/core/proposal.py)、
[Experiment/Workspace](../../../RD-Agent/rdagent/core/experiment.py)、
[QuantRDLoop](../../../RD-Agent/rdagent/app/qlib_rd_loop/quant.py)、
[Qlib 工作区](../../../RD-Agent/rdagent/scenarios/qlib/experiment/workspace.py)。

值得借鉴的是把“为什么试这个参数”和“计算实际说明什么”保留下来。Asterion 已有来源任务、
名称备注和固定证据导出，先让这些入口足以表达研究假设、选择依据和否定结论即可。
只有已有元数据表达不了实际需求时再加字段，不先造知识图谱或通用研究 DAG。

代码能运行、结果格式正确、统计表现更好也是三种不同结论。
[因子实现评价](../../../RD-Agent/rdagent/components/coder/factor_coder/evaluators.py) 分开运行与值检查，
[factor runner](../../../RD-Agent/rdagent/scenarios/qlib/developer/factor_runner.py) 组织基准与新因子，
[反馈模块](../../../RD-Agent/rdagent/scenarios/qlib/developer/feedback.py) 将指标送给模型生成解释和
是否替换当前最佳结果的判断。这里可以参考流程，但 LLM 的肯定意见不能代替确定性业务验收。

若今后引入 AI，最小范围应是读取用户选定的实验、解释已有差异、生成待审阅的参数草稿。
自动实验执行需要单独限定数据范围、实验次数、时间/费用预算，以及不会反复用于选择的最终
评价区间。反复根据同一测试区间改假设，会使该区间事实上成为研发数据，不能继续称为独立样本外。

这是一项设计风险判断，不是本轮证明 RD-Agent 所有场景存在泄漏。README 的收益、成本和
竞赛表现没有复现，不能转化为期货收益承诺。其生成代码、工作区恢复和依赖环境也不应直接
塞入可信的 C++ 交易插件：当前插件并无沙箱，生成代码不能默认取得交易权限。

### FinceptTerminal：学习终端工作流，保留 Asterion 的进程生命周期

本地项目已是 C++20/Qt6 桌面与 Python 分析子进程，不能按旧网页技术栈描述。
[架构文档](../../../FinceptTerminal/docs/ARCHITECTURE.md) 明确采用桌面模块化单体，文中同时
包含 current 与 target。其引用的 `docs/REFACTOR_PLAN.md` 和 ADR 索引中的具体条目在本地
未找到，不能将这些缺失文档视为已读或已落实的架构证据。

源码上有三组可用参考：

- [DockScreenRouter](../../../FinceptTerminal/fincept-qt/src/app/DockScreenRouter.h) 按需创建界面，
  支持面板复制、布局与窗口操作；
  [ScreenStateManager](../../../FinceptTerminal/fincept-qt/src/core/session/ScreenStateManager.h)
  用面板实例身份区分同一类屏幕的多个状态。
- [SymbolGroup](../../../FinceptTerminal/fincept-qt/src/core/symbol/SymbolGroup.h) 表达可选择的合约联动组。
  联动应传递明确上下文，而非让全局“当前合约”覆盖全部面板。
- [DataHub](../../../FinceptTerminal/fincept-qt/src/datahub/DataHub.h) 及其
  [实现](../../../FinceptTerminal/fincept-qt/src/datahub/DataHub.cpp) 将订阅绑定 owner 生命周期，
  集中处理在途请求、最新值与错误；[TopicPolicy](../../../FinceptTerminal/fincept-qt/src/datahub/TopicPolicy.h)
  区分刷新、推送、显示合并和非活动窗口策略。

Asterion 不需要为此引入全局 DataHub。Native 已按服务身份发布投影，前端有行情增量合并和
每窗口 `WorkspaceDraft`。应先在现有机制中改善“从行情看历史、从历史建实验、从结果返回
同一合约/快照”的流程；确有同时看多个合约的需求时，再定义面板实例和显式联动。

借鉴新鲜度显示时，要保留来源差异：图表旧值可以带时间显示，但不能成为账户报单的有效报价。
同样，合并 UI 刷新不能合并原始行情或交易回报。DataHub 的 TTL 和最后值策略并不是完整的
交易数据质量模型，其 push-only 默认也不能直接用作本项目的新鲜度规则。

[PythonRunner](../../../FinceptTerminal/fincept-qt/src/python/PythonRunner.h) 的并发、超时、结构化
输出体现了分析子进程的生命周期需求，但 Asterion 已由 Agent/Task/worker 承担这些责任。
另建桌面脚本运行池会绕过任务保管与资源准入。其屏幕状态兼容路径、数据库迁移、凭据方案、
多资产模块和商业版本功能都不作为当前移植目标。UI 保持 [Terminal 设计](../terminal.md)
及同花顺期货布局参考，不复制其整套视觉系统。

## 与 Asterion 当前实现的差距核对

下面是当前代码与最新交付记录支持的状态。它纠正早期规划的过时描述，不把既有能力重新列为新功能。

| 能力               | 已有基础                                                      | 真实剩余工作                                                               |
| ------------------ | ------------------------------------------------------------- | -------------------------------------------------------------------------- |
| 数据与任务分离     | Data 拥有快照/来源；Task 拥有任务/尝试/结果；Agent 管进程     | 保持边界，无需新 Research 服务或调度系统                                   |
| 实盘与回测复用策略 | 单合约 Strategy 输出目标；host 提供完整 bar；账户统一执行     | 固定历史预热与实时衔接、多合约实盘按实际候选另定                           |
| 历史保证金 F02     | 工作树已接通 SHFE AU2512、2025-10-20 至 22 日的日初和结算来源 | 更广范围、券商加收、手续费单位/生效证据、历史首次可知时间                  |
| 历史成员资格 F04   | 日线因子有前期历史、成交量、持仓量资格及原因                  | 动态成员进入可交易组合的持仓退出与缺数语义                                 |
| 实验工作流 F05     | 固定/新快照复用、名称备注、派生来源、少量比较和证据导出       | 留出/滚动的同口径对比；按实际需求扩参数派生                                |
| 研究统计 F06/F10   | 净费用、按月贡献、固定结果资金核对、结算占用与首批滑点敏感性  | 换月成本分解、含未平仓的合约贡献、更多受控敏感性维度                       |
| 因子研究 F09       | 动量、期限结构、波动率；IC、分组收益、留出和滚动              | 逐合约解释及一种有真实需求的新因子，再考虑组合或拟合变换                   |
| 容量 F11           | 20 万条上限内已消除重复准备，并有离线测量                     | Data→Task→worker→验收全链路、稀疏/并发；有证据后才分块                     |
| 数据更新 F07/F08   | 分区显式重取、固定快照、部分来源原文与质量核对                | 来源覆盖扩展及可选定时更新；不能假定所有来源支持同值独立观测               |
| 图表 F13           | 固定历史选择、返回恢复、分钟档案/实时观测显式分段             | 有统一标签证据后才拼接；多面板联动需实际使用需求                           |
| 实盘 L01–L04       | 授权、订单限额、持久意图、未知结果核对、单策略运行            | 账户级损失/占用风控、预热、异常定位、真实 CTP 仿真验收；组合执行有条件推进 |

关键当前源码：[策略](../../plugins/strategy/cta/strategy.hpp)、
[目标转订单](../../core/include/asterion/domain/position_target.hpp)、
[实时策略 host](../../apps/services/trading/strategy_host.hpp)、
[订单限额](../../plugins/risk/order-limits/order_limits.hpp)、
[资格规则](../../plugins/tools/factor_analysis/universe.cpp)、
[因子引擎](../../apps/services/factor/engine.cpp)、
[历史保证金解析](../../apps/services/data-service/historical_margins.cpp)、
[窗口草稿](../../apps/clients/terminal/src/host/workspace/drafts.tsx)。

最新范围见[历史保证金交付](shfe-margin-integration-2026-10-15.md)、
[因子容量记录](factor-capacity-2026-10-15.md)、[回测契约](../backtest.md)、[因子契约](../factors.md)。
这些交付文档中的测试数字属于原实施轮次，本轮没有重跑，不能算作本次验收。

文档还存在摘要滞后：`docs/status.md` 开头部分仍写只有 Tushare、没有分区重取、没有历史费率
接入，后文已记录范围有限的新交付；`docs/factors.md` 早期限制与后文资格规则也需一起理解。
本报告按代码和最新限定范围判断。后续整理应重写“当前状态”摘要，不再靠追加段落使读者自行
合并相互矛盾的说明；本轮不改写既有交付历史。

## 架构取舍与责任落点

### 保留现有所有权

| 责任                                        | 拥有者                       | 借鉴内容的落点                                           |
| ------------------------------------------- | ---------------------------- | -------------------------------------------------------- |
| 供应商事实、单位/时间语义、原文、不可变数据 | Data 与数据插件              | 扩来源覆盖、质量与历史规则；研究不得临时在线取数补缺     |
| 固定定义、attempt、取消、完成与结果         | Task                         | 研究记录和结果引用沿现有任务；不运行模型或另建研究状态机 |
| 模拟、因子算法及将来明确批准的训练          | 一次尝试的 worker 与业务插件 | 业务步骤可分函数/产物，当前不必每步成为独立任务          |
| 授权、风险占用、命令发送、柜台核对          | 每账户 Trading               | 目标仓位只是意图，所有执行继续经过统一准入               |
| 程序、进程与资源准入                        | Node Agent                   | 新研究工作消耗现有预算，不由 UI 自建不受管进程池         |
| 选择、草稿、布局、只读解释                  | Terminal                     | 面板上下文、可见状态和比较；不成为第二个账本或数据仓库   |

### 比较可行方案

| 问题         | 方案一                                      | 方案二                                     | 本轮建议                                   |
| ------------ | ------------------------------------------- | ------------------------------------------ | ------------------------------------------ |
| 复用成熟引擎 | 嵌入 Nautilus/LEAN，保留第二套账户/数据模型 | 在现有边界内借鉴所需语义                   | 选二；当前没有足以承担双模型同步的需求     |
| 扩研究流程   | 引入 Qlib/RD-Agent 调度及新依赖             | 固定任务内增加具体算法/结果，复用 Task     | 选二；只有独立可复用生命周期出现时才拆任务 |
| 改善行情体验 | 全局字符串主题总线与页面缓存                | 在 Native 投影和窗口草稿上表达身份/时间    | 选二；订阅合并先证明重复请求问题再做       |
| 拓展组合     | 多策略虚拟账户与资金分配平台                | 一个实际策略的类型化多合约目标进入原账户   | 后者作为 L04 条件项，不现在预建            |
| 扩大因子容量 | 直接提高上限并加线程/缓存                   | 量测全链路，移除重复工作，必要时按语义切块 | 选二；已有优化说明准备开销不可忽略         |
| 引入 AI      | 自动写代码、连续实验、直接交易              | 选定证据的解释和实验草稿                   | 先选二；执行与生成代码需独立界定信任和预算 |

当前 IDE 中的 [VerificationSlots](../../apps/services/task-service/verification_slots.hpp) 正是
应保留的具体机制：两个完成核验名额和一个公开读取名额分别计数，RAII lease 负责释放，
过量或重复请求在加载大输入前拒绝。它不拥有业务任务状态，也不是线程池或调度器。
[TaskHost](../../apps/services/task-service/task_host.cpp) 在完成流程跨文件工作保持 lease，并在
私有 worker 通道核对执行操作。不能因为参考项目用了 TaskManager/工作流，就将这个小机制
扩成通用资源框架；未来仅在全链路量测表明此容量确实限制目标工作量时重新评估。

### 需要保持的失败语义

数据缺少来源证据时是未知或拒绝，不回退为零成本、当前规则或合成行情；没有新策略目标与
明确空仓分开。发送结果未知时保留占用并核对，不自动重发。过期尝试不能提交成功，结果读取
不自动重算。UI 旧投影可标明时间继续展示，不能替账户决定是否可交易。表刷新失败后已落库
分区按现行合同保留，不把全部操作假装成回滚事务。

这些分别是数据、策略、账户、任务和显示边界的职责，不应汇总成一个全局“自动恢复”机制。

## 建议功能规划与交付顺序

沿用现有 F/L 编号，避免产生第二套功能编号。以下是建议，不是已完成状态，也不是要求一次实现全部。

### 第一组：让一个实际期货研究问题完整成立

1. **F02/F12：扩大一个明确研究范围内的事实覆盖。** 以目标合约/日期为边界，复用现有表下载、
   来源证据、预览和固定输入。先确认日初/结算、单位、适用对象与缺值行为，再扩支持范围。
   首批 AU2512 三天只证明窄范围链路，不能解除全历史限制；券商费率和手续费独立安排。
   验收看每个日期的依据能否追溯、缺证据是否拒绝、刷新后既有任务是否保持，不看下载量。
2. **F09/F05：完善因子解释和同口径比较。** 先让一个截面说明成员资格、排序值、入选/排除
   原因和样本数；若结果需新增明细，明确其由固定输入派生还是随结果保存，避免两份权威值。
   再选择一种成交量或持仓量因子，固定公式、窗口、缺失与零分母含义。比较同一数据/分段/费用
   的实验，差异参数可见；不靠隐式更换快照或反复挑选留出成绩完成“优化”。
3. **F06/F10：解释成本对结论的影响。** 在已完成净统计、月贡献和滑点变体之上，按实际研究
   需要优先做换月两腿费用/滑点分解或一种费用敏感性。金额仍来自账户结果，输出应与余额、
   权益和总费用核对。展示差异不等于证明真实成交可实现。

这组不需要训练平台、表达式 DSL 或第二供应商框架。若第一项因外部历史事实受限，可使用
明确的用户成本假设继续第二项，并保持结果标签；不能通过编码把假设升级成来源事实。

### 第二组：针对实际数据量和使用频率改进

- **F11：先完成全链路容量画像。** 量测固定表读取、Task 固定输入、worker 重读、计算、结果
  核验和 UI 分页，包含稀疏截面与两个任务并行。不要重复把此前热缓存 Protobuf 探针当作全链路。
  需要分块时，同时固定完整截面边界、回看窗口、标签跨块、训练/验证边界、取消与部分产物语义。
  不保留一套旧生产实现供回退；可用变更前固定基准输出和小规模独立算例检查一致性。
- **F03/F13：分钟研究以历史时段和来源标签为前置。** 先确认无成交分钟、夜盘交易日与闭市
  规则，再定义完整性和图表拼接。档案与临时观测来源不同，重叠不等于相同观测；证据不足时
  保留当前显式分段，不补点。
- **F08：有日常更新需求再做窄范围计划。** 计划限定表、来源、交易所、日期推进方式、额度和
  错过时间后的行为。触发后复用现有 Task 下载，操作身份防止重复创建；计划状态只设一个
  拥有者，Agent 仍只负责进程。先数据更新，不加入“下载后自动选优并交易”的工作流。
- **Terminal：用实际操作路径决定面板功能。** 优先统一合约、来源、快照和返回位置的可见
  上下文；只有同时比较多个面板确有需要才加联动组。当前窗口草稿不落盘，跨重启保存须
  单独列出允许持久化的 UI 字段，不能顺带保存密码或将草稿当成已提交任务。

### 第三组：围绕一个候选策略完成实盘准备

**L01–L03 优先于 L04。** 先选定策略、合约、运行时段和人工干预流程，再补账户级损失与
资金占用风控、固定历史预热、异常定位/提醒。风控算法在插件，风险状态及拒绝执行归账户。
减仓、平仓新委托、撤单、撤权各有准入条件，不能用“紧急操作”绕过持久执行链。

预热只更新策略状态，不补发历史目标；衔接后只对最新合格观测产生新意图。服务重启、行情
代次变化、断档、部分成交、撤单途中成交、未知结果与跨交易日核对，应通过现有 CTP 测试
替身和独立仿真环境分别验证。应用内提醒与窗口关闭后的外部通知属于不同需求，后者未选
渠道前不预建通知平台。

只有单策略候选经过研究与仿真后，才将 L04 定为“一个组合的目标与换月进入现有账户”。
不直接推广到多策略资金分配、跨账户复制、自动接管或高频交易。F15 涨跌停撮合仍保持已有
条件性优先级：结论依赖封板日成交时必须先解决；不因参考项目提供撮合器就声称缺口已闭合。

### 明确暂缓

完整 ML/AutoML 平台、自动因子工厂、任意代码/DSL、通用 DAG、插件市场、Tick/L2 队列模拟、
多策略虚拟资金账户和多账户复制均缺少当前范围内的必要性。股票/期权/加密资产、Web/Mobile/
Notebook/独立 CLI/Windows 客户端与项目范围冲突，不纳入候选。Qt/Rust/.NET 技术栈迁移、
微服务进一步拆分和全局消息总线没有由本轮证据支持的必要性。

## 如何验证后续借鉴是否有效

| 变化               | 最低有效证据                                             | 不足以证明完成的现象           |
| ------------------ | -------------------------------------------------------- | ------------------------------ |
| 来源与历史规则     | C++ 来源解析、阶段/单位/范围、固定引用；独立真实来源验收 | 某次 HTTP 成功、几条行情可显示 |
| 因子/分段/组合语义 | 小规模可手算样本、时间边界、费用与账户恒等式             | 指标更高、输出没有 NaN         |
| 任务与生命周期     | 真实可达的取消/迟到完成/身份冲突及持久结果测试           | 心跳仍在、进程退出码为零       |
| 核对与执行         | 重复/乱序回报、部分成交、断线未知、不重发等重要不变量    | 同一引擎重复运行得到相同结果   |
| UI 上下文          | 相邻组件测试验证来源切换、旧回复和面板独立性             | 截图看起来正确                 |
| 分块/容量          | 固定输入输出核对、真实阶段 RSS/耗时、稀疏与并发          | 放大输入上限、增加线程数       |
| 产品闭环           | 通过仓库隔离入口运行的必要 E2E，明确平台/源码/模块路径   | 另一 checkout 的成功日志       |

参考项目的测试代码用于学习要保护的业务性质，本轮没有执行其测试，也不按测试数量给项目排名。
后续 C++/Vitest/E2E 仍按 [开发指南](../development.md) 选择最低足够层次；真实供应商、真实
柜台、macOS 安装及长期运行需要各自证据，不能由 Linux 单测替代。

## 本轮交付与未验证范围

本轮只新增本评审并在审计进度登记结论；未改变生产代码、协议、依赖或功能开关。
相对路径链接指向本机兄弟仓库，供此工作区直接追踪；完整 HEAD 固定其阅读基线，
这些链接不保证脱离 `/home/qiwen/Code/quant` 布局后仍可访问。

文档链接/基线保留检查及报告引用文件的摘要清单位于
`build/reviews/reference-projects-2026-10-15/verification.json` 与 `source-manifest.json`。
本轮未运行 C++、Vitest、E2E、参考项目程序或性能实验；未联网下载、操作用户数据、提交、
推送或部署。源码静态阅读支持设计取舍，不支持收益、延迟、稳定性或实盘安全已经通过验收的结论。
