# 当前连接与 CTP 接入契约

本文记录已实现的只读连接契约。产品和模块目标统一见 [架构设计](architecture.md)；用户操作见 [CTP 与 SimNow](simnow.md)。当前没有交易提交、撤单或结算确认能力，真实券商差异与实盘准入未验收。

## 当前所有者与登记

[Rust connections](../kernel/futures/connections/src/lib.rs) 是 L2 权威状态所有者，管理档案、秘密绑定、配置修订、单活动会话、通道代次及响应准入。[Python 控制器](../src/asterion/connections/plugin.py) 属 L3，负责身份保护、HTTP 与 SDK 对象适配；[connector_ctp](../src/asterion/connector_ctp/plugin.py) 实现供应商协议和规范化。[market](../src/asterion/market/plugin.py) 装配行情、自选与来源合约缓存；[trading](../src/asterion/trading/plugin.py) 消费只读账户能力。

当前是 L2 权威状态加 Python 适配；按新架构，连接状态属于 L2 内核，CTP 接入改为内置 Rust 适配（经 FFI），Python 控制器随服务迁移删除，见[进度](ROADMAP.md#工程替换顺序)。

当前 Capability 绑定唯一提供者。connections 通过 `connections.access` 导出访问能力，并观察 `connections.connectors` hook；接入插件声明依赖 connections 并贡献 ConnectorContribution 工厂。全部插件激活后，在连接路由的生命周期启动阶段校验、冻结目录；重复 ID、非法字段/能力或与发行版批准归属不符均启动失败。

工厂登记不创建账户连接。connections 使用注入工厂创建会话，不静态导入 CTP；关闭时取消/等待会话并释放 SDK，close 幂等。内置接入集合随发行版构建，不支持热安装或热替换。来源缺失时保留档案并标为不可用，不选择备用券商或环境。

## 公开类型与能力

纯连接状态与配置契约由 [Rust 类型](../kernel/futures/connections/src/models.rs) 唯一定义，生成 [Python DTO](../bindings/python/asterion_bindings/connections.py) 与 [JSON Schema](../contracts/connections.schema.json)。Python 构造、复制及反序列化均回到 Rust 校验，只生成 Rust 显式声明的默认值。[Python 端口](../src/asterion/connections/public.py) 保留 SDK 回调和来源观察适配。QuoteEvent 由独立 L2 market-feed 拥有；合约与持仓观察 DTO 的完整领域迁移仍在后续范围。

| 类型 | 当前表达 |
|---|---|
| ConnectorDescriptor / ConnectorContribution | ID、owner、版本 1、字段/能力、配置校验与会话工厂 |
| ConnectionProfile | connection_id、connector_id、名称、config_revision 和非秘密配置 |
| ConnectionSession | 行情启动/订阅/停止、合约查询、账户查询、释放 |
| ChannelState | market/account 通道的状态、generation 与安全说明 |
| SourceInstrument / InstrumentBatch | 来源交易所和代码、名称、品种、完整交割月、生命周期；带查询身份的完整批次 |
| QuoteEvent | 来源价格、昨结算、高低价、成交量/持仓量、来源日期/交易日/时间及接收时间 |
| AccountSummary / Position / AccountBatch | CNY 资金与双向今昨持仓、套保类别、批次和账户身份；金额为 Decimal 或明确未知 |
| ReadBatch | connection_id、generation、request_id、起止时间与 complete=true |
| ReadRequest | L2 发出的连接、代次、请求 ID 和开始时间；接入必须原样回传 |
| ConnectorError | 安全说明、类别和可重试性，不传播原生请求对象或凭据 |

能力集合为 market_quotes、instrument_catalog、account_snapshot、positions。静态支持、用户授权、当前可用分别检查；缺能力不得用零资金或空持仓表示。InstrumentBatch 和 AccountBatch 各最多 10,000 项，重复身份、今昨数量不一致或非法时间拒绝。

来源键由连接、交易所及代码区分；账户按连接与来源账户 ID 区分。来源缓存尚未接通正式领域身份/规则发布，不可作为交易准入依据，UI 简称不是身份键。

## 配置、凭据与持久化

当前档案为 `connections/profiles.json` version 2，原子保存配置列表、selected_id 与加密秘密，文件权限为 0600。active_id 只来自内存。密文使用用途隔离的 SecretPort/Fernet，绑定完整档案及配置修订；公开接口仅返回 secret_saved 布尔映射。

保存必须明确保留、替换或清除秘密；界面的固定掩码永不提交。断开后可以重命名、修改非身份参数和凭据；目标地址改变须重新确认凭据，账户身份/接入方式改变须另存命名档案。修订冲突拒绝覆盖；未知格式、密文或身份不匹配时保留原文件并报错，不自动迁移或猜测。

发布前失败保持原配置；L1 明确报告已经发布但目录持久化确认失败时，内存同步到已发布内容并返回存储错误，不能声称磁盘回滚。当前最多 256 份档案、64 个接入、每接入 128 个字段，单字段文本最多 8 KiB；配置文件和单次查询响应各最多 16 MiB。Rust 端口同时限制响应嵌套和节点数，超限拒绝。

自选与来源合约缓存位于 `market/connections/<connection_id>/watchlist.json`、`instruments.json`，后者绑定连接/配置摘要和校验和，不含秘密。加密不能防御已取得本机密钥的代码；插件当前为同进程受信实现。

## 变更接口与会话

路径均以 `/api/v1/connections` 开头，要求账号/PIN 及服务端作用域授权：

| 接口 | 行为 |
|---|---|
| GET / | 配置、选择、能力与公开通道状态 |
| POST / | 保存配置 |
| POST /{id}/select | 保存所选配置；在线切换先关闭旧会话 |
| POST /{id}/connect | 统一开启支持的行情和只读查询能力 |
| POST /{id}/disconnect | 取消查询并释放会话 |
| POST /{id}/delete | 校验修订及离线状态后删除档案与密文 |

允许多份档案，同一后台最多一个活动连接。旧会话释放失败阻止新连接；新会话失败保留选择供重试，不暗中恢复旧连接。重启恢复档案/选择但不自动联网。删除清空被删档案的选择，不自动连接其他账户；保留该 ID 下的自选、合约缓存和历史数据，缺失接入插件的离线档案也可删除。

切换撤销整个会话；行情重连推进行情代次，不影响独立账户查询代次。旧行情缓存不能在重新登录后自动成为当前报价。查询必须匹配 L2 发出的请求 ID、连接、代次和开始时间，取消及迟到响应不能恢复已断开的通道。

生命周期操作串行，状态锁不跨 SDK 回调；同步回调可读取一致快照，回调内发起生命周期变更明确拒绝。恢复、关闭和校验回调必须同步完成，返回 coroutine 不能算作成功关闭。行情订阅维护期望集合，重连后恢复；登录成功不表示每个订阅成功。账户刷新不依赖行情认证，也不随面板数量创建重复查询。

## CTP 只读与故障约束

行情使用 MdApi；查询使用 TraderApi，仅认证、登录、品种/合约、CNY 资金和持仓查询。账户通道表示按需认证与定时查询，不是交易回报长连接；每批查询后释放原生实例。

查询按连接串行限流，同实例请求至少间隔 1.1 秒。原生回调内复制数据，完整末包、账户身份、结构和时间均通过才替换缓存；超时、取消、断线、不完整或解析错误不能发布成功空集。资金与持仓为顺序查询，保留批次起止时间，不声称原子账户快照。

失败保留同一身份的上次完整结果并标陈旧，断线隐藏快照，切换不能显示前账户内容。后台维护独立于面板；自选到期仅依据已知生命周期，来源遗漏或查询失败不视为到期。

只读权限不授予未来发单权限。正式执行还需要账户单写者、授权/规则/风险检查、耐久意图及对账；发送结果未知不能套用只读重试直接重发订单。要求见 [期货领域基线](futures-domain.md)。

## 验证边界

[Rust 状态机测试](../kernel/futures/connections/tests/lifecycle.rs)、[连接测试](../tests/test_connections.py)、[API 测试](../tests/test_connections_api.py) 及 [界面流程](../apps/terminal/e2e/connection-lifecycle.spec.ts) 验证装配、加密、来源隔离、唯一活动会话、并发切换、迟到回调、完整性与故障。第二接入仅用于测试，不进入产品；界面流程与真实接入应单独报告是否在当前版本实际执行。

真实 SimNow 新调用链、不同券商认证差异、当前连接格式的完整备份恢复和实盘仍须独立验收。历史 SimNow 测试不能替代当前链路验证；记录见 [验证索引](VALIDATION.md)。
