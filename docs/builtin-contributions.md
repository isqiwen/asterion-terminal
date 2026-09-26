# 内置插件宿主契约（待移除的现状）

> 2026-09-24 起的[架构](architecture.md#基本决定)不为内置功能提供运行时插件机制：内置功能改为静态组合的 Rust 模块，Python 只保留计算。本文记录的 Python 内置插件宿主、贡献登记与钩子是尚未迁移的现行实现，随各模块迁至 Rust 服务逐项删除；不要在其上新增内置插件或扩展点。

本文记录 Rust 宿主机制、Python 回调绑定、受信进程内插件及前端贡献契约。后端 L0–L2 Rust 替换尚未完成；前端已归入独立 L4 源包，唯一划分见[模块与层级](architecture.md#层级与模块)，替换范围见[当前代码归属](engineering-structure.md#当前代码归属)；外部 ZIP 插件见 [插件系统](plugin-system.md)。内置宿主不是恶意代码沙箱。

## 后端登记与生命周期

默认插件由 [distribution.py](../src/asterion/distribution.py) 选择，层级、依赖、授权及生命周期由 [Rust 内核](../kernel/src/plugins.rs) 执行，[Python 回调绑定](../bindings/python/asterion_bindings/plugin_host.py) 转换声明、对象与回调。当前包含身份、时间、规则、角色、数据、研究、策略、扩展管理、连接、CTP、行情和只读账户功能；交易执行未开放。

| 声明 | 当前约束 |
|---|---|
| `id / api_version / layer / type_requires / requires` | ID 唯一、契约版本明确；缺失依赖和依赖环在激活前拒绝 |
| `provides / consumes` | Capability 固定能力 ID、提供者和类型；消费者同时声明提供者依赖 |
| `resources` | 发行版逐项授权；完整集合、类型和重复声明在任何激活前检查 |
| `observes` | 只能读取已声明的命名 hook；上下文不公开整个宿主 |
| `handlers / publishes` | 贡献任务处理器及有类型、所有者和读取权限的事件主题 |
| `backup / restore` | 领域备份校验与恢复操作参加宿主强制执行流程 |

插件通过 `Context.require/resource/hooks` 获取端口，资源绑定按启动快照保存。`Activation.exports` 必须与声明完全一致且类型正确；异常处理器不能重复。全部必需插件激活成功后才公开路由，失败释放资源，关闭按逆序且幂等；关闭后不能重新解析能力或资源。

内置插件集合在启动时固定。接入贡献的当前登记与生命周期见 [连接契约](broker-connections.md)。`type_requires` 与 `requires` 分别形成源码类型依赖图和激活图，两者在激活前独立检查；当前 Python 模块的源码依赖声明和完整 L2 领域插件装配仍待收敛。

## 资源、事务与任务

[distribution_storage.py](../src/asterion/distribution_storage.py) 批准表所有权，发行版的 `bootstrap_resources`、`execution_resources` 绑定资源。普通插件不取得完整 Settings、主密钥、SQLAlchemy Engine 或全局任务服务。

- `Storage` 只允许所属表写入和明确授权读取；拒绝原始 SQL、越权表及只读事务写入。结构不符合当前契约时拒绝启动，不自动修补用户数据库。
- `borrow` 将跨领域只读查询绑定到同一事务；`join` 允许所有者以自身授权加入写事务。两者检查数据库归属，事务或存储关闭后失效。
- `TaskPort` 限定任务类型，提交前校验完整批次；进度、完成和领域发布共享事务，检查有效租约、任务类型和数据库归属。
- `SecretPort` 通过不暴露密钥的 Rust 句柄提供用途隔离的 Fernet 加解密和指纹操作，`DigestPort` 提供 HMAC 摘要。业务所有者校验字段语义、修订和内容绑定；认证可在内存使用批准的明文，不写入公开响应或任务载荷。

`TaskHandler.execute(context, payload)` 只接收输入与 ExecutionContext，资源须与声明完全匹配，执行后关闭。同步处理器使用目录、凭据和绑定当前任务的 SyncReporter；研究处理器使用策略目录资源。报告端口仅开放进度、证据提交和断点读取，不暴露租约或通用 HTTP 客户端。

Worker 负责领取、续租、发布和失败上报；Rust 内核拥有任务仓储、行锁、状态转换、租约栅栏、CAS 条件与同事务事件写入；等待行锁的时间计入租约检查。Python 仅传递当前接口数据和组装事务。发布校验器只收到状态码及错误详情，不能将失败响应改为成功。状态、幂等与取消边界见 [任务协议](protocols.md)。受信引导层仍持有运行配置；端口封装不阻止同用户恶意代码反射或读文件。

## 备份与恢复

`BackupCheck` 消费显式装配的只读领域证据。[Rust 恢复机制](../kernel/src/recovery.rs) 检查声明与输入集合、重复声明及统计；[Python 绑定](../bindings/python/asterion_bindings/recovery.py) 只检查对象类型并调用领域校验器。全部输入先通过检查，再执行校验；统计必须是非负 64 位整数且键不重复。文件端口只允许数据目录内读取、摘要和枚举，拒绝绝对路径、路径穿越及符号链接。损坏、解密失败或研究复算不一致阻止恢复，保留源备份和原状态。

`RestoreStep` 与未完成任务取消由[恢复装配](../src/asterion/runtime/restore.py) 放在同一写事务执行。Rust `RestoreScope` 为每个批准操作生成不可自行构造的调用句柄；必须在创建线程恰好完成一次，遗漏、失败、吞掉操作异常或重复调用均拒绝提交。内核不持锁执行业务回调，重入调用立即拒绝并使该操作失败；完成检查会封闭作用域，退出或作用域释放也使保留句柄失效。每次恢复最多 4096 个批准操作。身份插件使用受限会话清理操作清空入口所有的 `identity_sessions`（备份恢复迁入 Rust 前的剩余差距），已完成任务和业务事实不参与清理。

备份需要当前发行版具备对应领域校验器；未知格式不会自动转换。完整流程见 [桌面运行](desktop-runtime.md)。

## 前端与原生贡献

前端不是插件机制：每个面板包导出一个 `UiModule`，[distribution.ts](../products/terminal/src/distribution.ts) 在构建期以 [`UiComposition`](../presentation/workbench/src/extensions/modules.ts) 组合，只检查贡献 ID 唯一和工作区引用的面板存在；没有运行时依赖声明、启用顺序或加载。模块贡献工作区、面板、设置、任务视图及命名扩展点，Workbench 和 PanelHost 通用承载。

面板与设置声明 `scope`，宿主只传入该用途资源，缺失时失败。各面板包拥有自己的上下文；`terminal-workspace` 组合这些界面输入，产品装配注入选定贡献，不反向导入产品。当前窗口、目录与请求状态仍集中在该面板包，尚未拆成各面板自治状态。

每个前端包通过 `package.json` 声明唯一层级、依赖与公开导出；面板模块 ID 使用 `asterion.ui.*`，与后端身份分开。产品边界测试检查实际导入、包依赖 DAG、L4 → L5 反向引用，以及每个面板包恰好被组合一次且不声明运行时依赖。当前仍静态打包可信贡献，尚未实现按需加载或外部 UI 沙箱。

业务请求使用无凭据的 RequestClient。受信装配绑定地址、账户会话和服务端作用域凭证；路径/方法越界、重定向、旧账户响应被拒绝，撤销时取消在途请求。取消不保证已受理命令回滚，仍须幂等与服务端鉴权。身份、启动和维护引导保留可信工作台权限。

`tasks.views` 由领域提供名称、结果与操作，任务中心管理通用状态。`dashboard.widgets` 的当前类型见 [overview/public.ts](../presentation/panels/overview/src/public.ts)，默认组合由发行版决定。这是可信 React 贡献，不是可安装的任意第三方 UI；未接入能力使用明确空态。

L4 [desktop-bridge](../presentation/desktop-bridge/native/lib.rs) 承载窗口、修订、联动、拆分归并和 UI 会话；独立 [terminal-workspace](../presentation/panels/terminal-workspace/native/lib.rs) 定义布局/profile，由 L5 [产品装配](../products/terminal/native/lib.rs) 选择。外部插件不能注入 Rust。当前业务状态的前端与原生校验需同步，不能将目标单源状态契约标为已经完成。

## 验证范围

机制用例见 [宿主](../tests/test_plugin_host.py)、[存储](../tests/test_storage_ports.py)、[授权](../tests/test_authorization.py)、[恢复](../tests/test_restore_ports.py) 和 [边界检查](../tests/test_boundaries.py)。边界检查的手写模块名单仍有缺漏，测试通过不代表全部插件已覆盖。

冻结环境证据见 [核心与插件验收](validation/core-plugins.md)，当前范围见 [进度](ROADMAP.md)。这些不代替真实行情、实盘、不可信代码隔离或全部平台分发验收。
