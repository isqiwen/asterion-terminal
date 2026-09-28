# 交易前风险插件

当前新增公开 `RiskPort` 与官方 `asterion.risk.order-limits` 插件。契约位于 `core/include/asterion/domain/risk_port.hpp`；具体规则与配置位于 `plugins/risk/order-limits/`，构建目标 `asterion_order_limits`。风险插件不持有账本，不修改订单，也不直接发送执行指令。

## 当前能力

- 单笔委托数量上限。
- 单合约潜在总持仓量上限：现有多空总持仓 + 在途开仓单剩余数量 + 新开仓数量。不同方向不相互抵消，未成交平仓单不提前释放额度。
- 在途委托数上限，精确达到上限可接受，超出拒绝。
- 已超总持仓上限时允许满足其他限制的平仓委托，以便减仓。合法可平量、手续费和保证金仍由交易账本校验。
- Decimal 精确数量计算；通过剩余额度比较避免先累加导致溢出。
- 插件未启动/已停止时返回不可用；非法上下文拒绝，非法委托抛出异常。上层必须将拒绝或异常视作不允许执行，不得转为默认放行。

执行方必须提供同一串行事务中的一致上下文，包括现有总持仓量、在途开仓余量、在途委托数和新订单。Core 提供统一表示，插件拥有规则与参数；插件类型不是隔离机制。

## 执行与持久化接入

PaperExecution 现在必须由宿主注入已启动的 RiskPort；缺少插件拒绝构造，不可用或拒绝决策均阻止提交。手工委托与策略目标调整统一通过 submit 检查；目标替换在候选状态中撤销旧单、检查新单，拒绝不会取消原有订单或改变冻结资金。FuturesAccount 的保证金/可平仓量检查继续执行，风险许可不代替账本合法性校验。

模拟会话 manifest 必须包含 risk 三字段配置，PaperInput 与 Snapshot 通过 Protobuf RiskLimits 携带相同内容。风险配置随创建记录持久化，恢复时重新构造官方插件并重放相同限制；缺失配置的旧记录明确拒绝，不迁移、不覆盖。策略授权数量上限继续作为独立的授权边界，不替代账户风险限制。

Terminal 创建模拟账户及提交回测时显式填写三项限额，无隐藏默认值；账户详情显示恢复后的风险配置。Backtest 使用相同插件和提交路径，结果保留配置；Task Service 拒绝风险配置不同的结果。当前限额在会话内不可修改。实盘、跨品种组合风险和动态配置仍未实现。

## 验证证据

GoogleTest/CTest 的四项测试在 macOS 与 Linux x86_64 均通过，日志为 `build/order-limits-tests.log` 与 `build/order-limits-linux.log`。覆盖插件生命周期、边界等值、在途开仓占用、方向不抵消、超限时减仓、非法配置/上下文及大数比较。Windows 尚未原生执行；未声称通过生产订单链路验收。

## 账本上下文与配置契约

`assess_order` 从 FuturesAccount 的只读、类型化持仓和委托记录构造上下文，不从界面快照解析权威数量。已受理及部分成交的开仓单只计算剩余数量；已成交部分计入持仓，撤单部分不再占用额度。调用方必须将检查与订单提交置于同一串行事务内；此函数不会提交订单，也不能自行提供并发原子性。

插件提供 `encode_order_limits / decode_order_limits`，严格要求 `max_order_quantity`、`max_gross_quantity`、`max_working_orders` 三字段。数量为规范 Decimal 文本，在途数为正整数；字段缺失、未知字段、非整数或非规范数量拒绝，不补默认值、不重写输入。

新增测试使用真实 FuturesAccount 检查部分成交、撤单、多空总量以及平仓成交前后额度，确认评估不改变账本；另验证配置精确往返及非法配置拒绝。随后已完成模拟会话/Protobuf/Terminal 配置与统一提交接线，见上方执行与持久化接入。

本轮六项测试在 macOS 与 Linux x86_64 均通过（`build/risk-ledger-tests.log`、`build/risk-ledger-linux.log`）。没有修改旧会话数据，没有启用隐藏的默认风险配置；这两批早期测试当时未覆盖实际提交链路；最新接线证据见下文。


## 统一执行链验收

新增 PreTradeRisk 用例覆盖手工超限拒绝、已受理订单额度占用、策略目标超限、风险插件不可用、拒绝替换后原订单与冻结资金不变，以及从持久化日志恢复相同规则。缺失风险配置的创建记录/Protobuf 请求与非规范数字文本均拒绝。Task Service 另验证不能接收使用不同风险配置的结果。

macOS 全量回归实际 121 通过、1 Linux 专属跳过（`build/risk-integration-final-tests.log`）；规范文本修正及结果配置一致性定向测试 4/4（`build/risk-integration-canonical-tests.log`）。真实桥接交易、研究、策略、中英文浏览器测试 6/6（`build/risk-integration-final-ui-tests.log`）；单独通过浏览器验证两手委托被一手限额拒绝、订单/冻结资金不变以及恢复后限额一致（`build/risk-rejection-ui-tests.log`）。前端构建通过。安装包随后已重建并验收，见下文；Windows 原生仍未验收。

Linux x86_64 全量回归 120/120（`build/risk-integration-linux.log`），包含远程部署、研究和策略恢复；随后纳入最终新增测试与显式字符串修正，定向 4/4（`build/risk-integration-final-linux-checks.log`）。Linux 增量构建发现同步时间戳使新增研究测试未进入二进制，已强制重编受影响源文件并确认四项实际被发现和执行，未将遗漏测试计为通过。macOS 与 Linux 的日志数量差异来自最后两项测试加入时点，最终新增范围均单独验收。


## 分发验收

本轮重新构建 Linux x86_64 服务资源与 macOS ARM64 DMG，包含相同风险契约和 Agent 升级实现。Linux Release 的模拟交易恢复、研究恢复、分发资源检查 3/3 通过（`build/risk-release-linux-bundle.log`）；归档十项载荷逐项摘要检查通过。

`paper_recovery.py` 现在在交易进程重启前后都提交超过单笔上限的委托，确认被拒绝且账户快照不变，并核对恢复后的三项限额。此测试既在 Linux Release 程序上运行，也直接使用挂载 DMG 内的 Agent 与交易程序执行。

macOS DMG 构建成功，镜像校验、严格签名检查、内置 Linux x86_64 资源校验、包内交易 TCP/mTLS 与恢复、研究任务恢复、策略自动历史模拟和真实 CTP SDK 回环生命周期均通过（`build/risk-release-desktop.log`）。测试通过 Release 开发桥访问包内独立程序，未操作打包后的 Tauri 原生窗口，未替换用户安装或升级现有 Agent。镜像已卸载。签名仍为 ad-hoc、未公证；Windows EXE 和 Linux DEB 本轮未生成，三平台系统服务升级仍不能据此宣称全部验收。
