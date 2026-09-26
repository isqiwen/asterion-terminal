# 当前接口与任务协议

本文保留跨功能共用的执行约束，并索引各领域当前契约。目标设计见 [架构](architecture.md)；精确请求/响应以当前路由模型和生成的 OpenAPI 为准，不在此另建一份完整接口清单。

## 认证与通信

本机 API 使用 `/api/v1` 前缀，服务监听 127.0.0.1，所有业务请求携带 Bearer 身份：

- 可信工作台持有运行身份，经入口 `POST /access/scopes` 申请 5 分钟有效的功能作用域凭证；凭证绑定账户会话及发行版批准的方法/路径，不能提升权限，作用域凭证不能再申请作用域。
- 受保护用户操作另外校验账号会话与 PIN 解锁，由入口判定后随请求转发；锁定返回 423。身份策略见 [身份与终端锁](identity.md)。
- Worker 使用独立派生身份，仅允许领取、续租、失败和处理器声明的任务相对接口。它不使用工作台账号会话。
- 前端业务组件使用无凭据端口；原始输入校验错误不回显敏感 input/context。当前不是远程多租户部署方案。

HTTP、同进程端口与插件进程共享的请求上下文、截止时间、错误及事件分页语义见 [内部通信](internal-communication.md)。普通接口类型来自 OpenAPI；持久事件已支持按游标读取，行情尚不是通用高频事件流。

## 任务与租约

状态为 QUEUED → RUNNING → SUCCEEDED / FAILED / CANCELLED。相同 command_id 和输入返回原任务，复用命令 ID 但改变输入返回 409。内容质量失败不能发布成功结果。

RUNNING 租约过期可重新领取，attempt 增加并更换随机 fencing token；PostgreSQL 行锁与 SKIP LOCKED 协调领取。续租、失败、进度及发布都检查当前 token、运行状态和期限。失联不立即标为失败；等待计算期间，worker 按 lease_seconds/3 续租，间隔限制在 1–60 秒。

取消先将任务标为 CANCELLED，失效租约立即禁止发布；worker 在后续续租遭拒或请求失败时关闭计算进程，不等待业务函数自行返回。任务重领使用新 token，旧计算结果不能提交。HTTP 超时本身不表示取消或失败已提交，最终状态以服务端任务记录为准。

`GET /jobs` 最多返回最近 100 项，`GET /jobs/{id}` 可读取单项；均不返回 payload 或租约 token。列表、单项、领取、续租、失败和取消由 [Rust 入口](../services/server/src/tasks.rs) 以内核任务仓储实现，租约时长取运行设置 `ASTERION_LEASE_SECONDS`（默认 60 秒）；进度、观察记录与结果发布仍由声明它们的处理器在 Python 进程中提交。每种任务只有一个执行者：领取按任务类型限定范围（内核 `ClaimKinds`），入口自行执行文件导入任务，`/jobs/claim` 只向内部 worker 发放其余类型。Rust 校验任务命名空间、登记唯一性、发布路径与精确 POST 请求声明；处理器不能贡献领取、续租、失败或取消等宿主控制操作。[处理器绑定](../bindings/python/asterion_bindings/task_handlers.py) 保存 Python 回调，[发行版](../src/asterion/distribution.py) 选择业务实现。每次执行的请求域只允许该处理器声明的路径，不能借用其他处理器的授权；完成、异常或资源装配失败均关闭该请求域。

## 计算进程与预算

当前单 worker 一次执行一个任务，每任务启动独立计算进程。[固定 Rust TaskProcess](../kernel/mechanisms/src/tasks/process.rs) 持有进程组、非阻塞管道、响应校验和匿名临时工件；Python 仅负责 HTTP 控制及[业务回调引导](../src/asterion/runtime/task_child.py)。请求经私有 stdin 传入，二进制结果与控制头分开传输，不使用 pickle 或 base64，也不受普通插件 RPC 的 8 MB 帧上限约束。工件按 64 KiB 块发布，正文、租约和凭据不放入 argv、环境变量或运行诊断。

[产品装配](../src/asterion/runtime/worker.py) 明确传入以下单次预算；超额、截断、额外响应帧、关联不匹配或计算进程异常退出均拒绝发布：

| 内容 | 上限 |
|---|---|
| 计算请求 | 256 MiB |
| 二进制工件 | 1 GiB |
| 结果元数据 | 1 MiB |
| stderr 累计字节 | 1 MiB，不展示其正文 |

领取时建立独立的 24 小时执行 Context，保留任务事件的关联和因果信息。Rust 同时检查绝对期限及单调时钟预算，拒绝迟到结果；等待计算时仍须续租。关闭、超时、协议错误及正常析构会终止所属进程组并等待计算子进程；监督停止 worker 的 SIGTERM/SIGINT 也通过此清理路径回收同组子孙。突发 SIGKILL、崩溃或断电无法保证执行析构，当前不承诺这些情况下的完整子孙清理。

这些是执行期限和传输/工件预算，尚未实现通用 CPU/RSS 调度；业务回调构造对象的内存也不等于传输预算。各领域更严格的输入和结果限额继续生效，不能由通用工件上限扩大。

## 发布与不可变输入

Worker 向处理器声明的 job-relative 发布入口提交结果，携带 X-Lease-Token。领域所有者复核固定输入与内容，核心执行租约检查和事务完成；成功结果与任务状态原子可见。

文件先完整写入并校验 hash，目录事务后发布；失败可能留下未引用文件，文件存在不代表发布成功。客户端使用目录中的逻辑 ID/URI，不能提交任意文件路径。相同已完成任务的相同结果可幂等返回，内容冲突拒绝。

当前 CSV 发布仍在 API 侧重新校验最多 2 MB 输入；研究结果最多 8 MB，API 会复算校验。这些是当前有界执行路径，不等于大规模分钟研究的产物通道已经实现。

数据/规则/代码及配置按任务固定。配置损坏、工件缺失或不符合当前契约明确失败，不读取最新配置补齐历史输入。历史事实保留，重跑不自动改用新模型。

## 领域接口入口

下表路径省略 `/api/v1`；字段、业务语义和验证范围由链接文档维护。

| 领域 | 当前入口 | 契约说明 |
|---|---|---|
| 文件导入、规范身份 | /imports、/reference | [导入与数据身份](data-import-and-connections.md) |
| 数据来源和连接实例 | /data/providers、/data/connections、/data/providers/{id}/configuration、/configuration/check、/verify | [来源配置](data-providers.md)、[连接实例](data-import-and-connections.md) |
| 同步、准备与历史计划 | /data/sync、/data/preparations、/data/history | [来源与采集](data-providers.md)、[历史下载验证](validation/history-downloads.md) |
| 目录、版本、扫描与归档 | /data/catalog、/data/versions/{id}、/lifecycle、/archive | [数据生命周期](data-lifecycle-design.md) |
| 覆盖与补齐 | /data/versions/{id}/coverage、/data/coverage/{id}、/refill-status、/refill | [覆盖说明](data-providers.md)、[研究缺口流程](local-backtesting.md) |
| 研究运行、草稿和复现包 | /research/runs、/workspace、/templates、/packages | [本地回测](local-backtesting.md) |
| 研究实验与验证 | /research/experiments | [实验](research-experiments.md)、[验证](research-validation.md) |
| 规则、时间及角色 | /contract-rules、/trading-time、/contract-roles | [规则](contract-rules.md)、[交易时间](trading-time.md)、[角色](contract-roles.md) |
| 实时连接、行情与账户 | /connections、/market、/trading | [连接契约](broker-connections.md)、[CTP 使用范围](simnow.md) |
| 身份、插件及通信 | /account、/extensions、/communication/events | [身份](identity.md)、[插件](plugin-system.md)、[通信](internal-communication.md) |

接口族内的简称不是额外路由；具体路径见生成的 `docs/openapi.json`：入口实现的操作声明在 [services/server/openapi.json](../services/server/openapi.json)，其余见 [数据路由](../src/asterion/data/routes.py)、[目录路由](../src/asterion/data/catalog_routes.py)、[研究路由](../src/asterion/research/routes.py)。

## 共同一致性约束

- 配置/草稿/布局等可变对象使用 expected_revision；冲突拒绝覆盖。秘密省略表示保留、明确值表示替换或清除，接口不返回明文。
- 配置齐备不表示外部接口已验证。任务冻结配置修订和内容绑定；同输入命令重放仍返回原配置，失败重试创建的任务须遵守各领域固定输入规则。
- 覆盖报告固定输入与范围；COVERED 只表示按这些依据有预期记录，不证明来源权威或历史可知性。补齐不改写原报告，完成后需重新核对。
- 文件输入使用 explicit_external 依据时必须显式指定日历/合约版本及原因，不支持自动补齐；不能借此绕过同步连接隔离。
- 研究草稿、模板和包的所有者来自认证会话；expected_account 仅检查页面预期身份，不能指定其他所有者。模板删除使用墓碑，旧请求不能复活。
- 复现包保留 canonical JSON 的原始数值表示，只有 READY 可提交；hash 用于完整性和复算，不是来源签名。
- 已发布版本当前全部保留；归档只影响目录显示，不改写固定内容。引用统计包含全部账户但不泄露其他账户内容，零引用不授予物理删除权。

当前验收范围见 [验证索引](VALIDATION.md)；功能测试、离线数据和真实来源验证分别解释，不将接口存在等同于领域机制完成。
