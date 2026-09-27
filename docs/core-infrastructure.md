# Core 基础设施

本实现对应架构图的 Foundations 与 Kernel Mechanisms。交易日历、账户账本、结算、风险算法等领域能力不混入通用内核。所有模块位于 `core/`，不设顶层 runtime，平台使用同一套 C++20 实现。

## 构建目标与依赖

`core/` 是整体核心，包含三个职责模块。CMake 目标与模块对应：

```text
asterion_domain → asterion_kernel → asterion_foundation
Asterion::Domain → Asterion::Kernel → Asterion::Foundation
```

领域模块可以使用内核提供的插件生命周期、消息和运行机制。内核不反向依赖交易领域。简单领域值对象只包含所需的基础头文件，不要求它们主动使用内核。`MarketDataPort` 当前仍继承 `Plugin`，此次命名调整不改变这个接口。

## 模块和实际能力

| 图中模块 | 实现 | 当前能力 |
| --- | --- | --- |
| ID | `foundation/id.hpp` | 严格标识格式、并发安全的作用域内递增身份、耗尽拒绝 |
| Time | `foundation/time.hpp` | UTC 与单调时间分离、系统时钟、可注入手动时钟、原子推进与溢出检查 |
| Decimal | `foundation/decimal.hpp` | 八位定点、加减乘、除法、步长量化；显式 exact / toward_zero / floor / ceiling / half_even |
| Error | `foundation/error.hpp` | 带稳定错误码的异常；C ABI 将错误码序列化，不让异常跨语言 |
| Serialization | `foundation/serialization.hpp` | 严格 JSON、重复字段/大小/深度限制、版本化事件封装、纳秒字符串无损编码 |
| Concurrency | `foundation/bounded_queue.hpp`、`kernel/thread_pool.hpp` | 有界多生产者多消费者队列、停止令牌、关闭唤醒；可配置工作线程池、任务 future、异常隔离、待执行任务取消 |
| Plugin Manager | `kernel/plugin.hpp`、`kernel/plugin_manager.cpp` | 可信静态插件、依赖排序、启动前验证、失败回滚、逆序停止 |
| Message Bus | `kernel/event_bus.hpp`、`kernel/message_bus.hpp` | 同步类型化分发、跨线程有界投递、显式背压、批量派发、回调失败后继续交付 |
| Resource Registry | `kernel/resource_registry.hpp` | 类型化资源、所有者作用域、可撤销句柄、容量限制、析构清理、锁外资源析构 |
| Scheduler | `kernel/scheduler.hpp` | 单调时钟、同到期时间按注册顺序执行、取消、容量/每轮预算、递归拒绝、下一轮处理新任务 |
| Configuration | `kernel/configuration.hpp` | 宿主声明字段及验证器、未知字段拒绝、补丁全量校验后原子应用、启动后封存 |
| Runtime | `kernel/runtime.hpp`、`kernel/runtime.cpp` | 统一生命周期、命令注册/分派、权限入口、工作线程、事件/调度泵、失败清理与关闭 |
| Observability | `kernel/observability.hpp` | 有界结构化调用记录、trace ID、单调耗时、成功/失败计数与丢弃计数 |
| Security & Isolation | `kernel/security.hpp` | 默认拒绝、精确能力授权、封存授权表、运行时撤销、命令执行前强制检查；仅可信进程内组件 |

## 生命周期与线程所有权

Runtime 是单次生命周期对象：created → starting → running → stopping → stopped；启动失败进入 failed，重试需创建新 Runtime。宿主串行调用配置、注册、启动、命令、调度和关闭；不要求固定 OS 线程，适用于 Tauri 的互斥调用桥。

启动时封存配置与授权，创建执行器并按依赖启动插件。关闭时先请求工作线程停止并 join，取消待执行任务，再关闭调度与消息队列、逆序停止插件、撤销资源。启动失败同样清理。业务回调和 `Runtime::poll` 期间拒绝递归分派、递归泵及关闭，防止在使用资源时拆除运行环境。

工作线程只能访问线程安全资源及 `MessageBus::post`。`Runtime::poll` 由宿主事件循环显式驱动，没有隐藏的后台定时轮询；手动时钟用于确定性回放和测试。一个调度回调失败不会阻止该轮其他到期任务和消息处理，最终向调用方报告首个异常。

## 资源、取消与交付语义

- 资源句柄每次 `lock()` 检查作用域状态。关闭后旧句柄不能再获取资源，同名新作用域也不会复活旧句柄；已经获取的 shared_ptr 保留对象生命周期，不承诺强制收回调用方手中的引用。
- 资源作用域默认最多 256 个资源，注册表最多 128 个活动作用域；整个注册表关闭后不可重新注册。资源析构发生在锁外，允许清理函数查询其他服务。
- 默认队列、执行器待办和调度容量分别为 256。队列满时返回失败或抛出资源耗尽错误，不静默丢掉已接受的任务。Runtime 默认 2 个工作线程，可在启动前配置 `runtime.workers`（1–256）和 `runtime.queue_capacity`（1–65536）。多线程不保证任务完成顺序；单线程用于需要串行执行的任务。
- 执行中的任务合作响应 stop_token；未开始任务在关闭时获得 cancelled 异常。任务不响应取消时，join 仍会等待它，不承诺硬超时或强制终止。
- 原始队列 close 后可排空；MessageBus 在 Runtime 关闭时丢弃未派发事件，不在插件停止后继续派发。正在交付的一个事件完成当前订阅快照。回调失败不自动重发，避免重复处理。
- Scheduler 的同批未执行任务可以被较早回调取消；回调中新建的即刻任务留到下一轮。当前没有定时线程、周期任务、磁盘任务恢复或分布式调度。

## 配置、安全与诊断边界

Configuration 只保存宿主明确声明的非敏感启动配置，不负责加载环境变量、存储凭据或在线迁移。验证器由应用/插件提供，具体供应商规则不写入内核。调用方不得将凭据作为配置字段；当前没有密钥保管库。

AccessPolicy 由可信宿主创建并封存，调用者身份由宿主赋予，不能从请求 JSON 中任意声明。授权为精确能力匹配，无隐式通配符。它用于协作组件的权限边界，不是操作系统隔离；同进程原生代码仍可访问进程内存与系统资源。不支持加载不可信策略，也未实现进程沙箱、网络认证或实盘授权执行链。

调用记录只包含受控操作名、trace ID、时间、耗时及结果，不记录请求参数、文件路径、异常文本或凭据；未知请求统一记为 `runtime.unknown`。默认保留最近 256 条记录，其余只计丢弃数，累计成功/失败计数保留。trace ID 只保证当前生成器作用域内唯一，跨进程/重启唯一身份需由宿主提供不同作用域；日志模块另提供 spdlog 滚动文件输出；当前没有 OpenTelemetry 导出。

事件协议固定 version=1，大小上限 64 KiB、嵌套深度上限 64，重复 JSON 键拒绝。纳秒使用规范整数字符串，Decimal 使用明确的十进制字符串。消息队列本身按条数限流，业务载荷仍需由边界校验，内存事件没有持久化或 exactly-once 保证。

## spdlog 日志

`kernel/logger.hpp` / `kernel/logger.cpp` 提供 spdlog 实现，支持线程安全 stderr 输出、UTF-8/Windows 宽字符文件路径、按大小滚动和保留数量、日志级别以及显式 flush。默认只写 stderr，不污染开发桥接的 stdout JSON 协议；文件输出由应用通过 LoggerOptions 指定路径，当前 Terminal 未默认持久化日志。

日志为 JSON Lines，记录事件名、级别、logger 名称、UTC 纳秒及结构化字段。password、token、secret、credential、authorization、api_key 等常见字段名会递归脱敏，拒绝过深或超过 16 KiB 的记录。脱敏不是任意文本的秘密识别器，调用方仍不得记录原始请求或凭据。Runtime 仅写生命周期与受控调用元数据。写入失败计数且不改变业务返回结果，初始文件创建失败会明确抛错。

spdlog 和 fmt 使用 Conan 的 header-only 选项，代码编入内核静态库，Tauri 不需要再手工枚举第三方日志归档库。GoogleTest 通过 Conan test_requires 引入，仅用于测试；`-o with_tests=False` 可以关闭其依赖及 CMake 测试构建。

## Terminal 集成

Terminal 在 `apps/terminal/native/terminal_application.cpp` 组装 Runtime，各业务域命令分别在 `commands_{paper,node,research,strategy,market}.cpp` 注册，共享状态在 `application_impl.hpp`；注册 `runtime.snapshot` 与 `futures.inspect_csv`，为本机调用者授予对应能力。文件和行数限额由应用声明为启动配置；CSV 源仍通过数据插件提供。预览状态保存在 Terminal 资源作用域内，完整文件通过后才替换。状态响应提供累计调用诊断，不改变现有界面设计。

C ABI 使用统一严格 JSON 解析和核心错误码（`classify` 统一映射异常，跨进程错误经 `throw_remote_error` 还原）。

### 持久文件写入

`kernel/durable_file.hpp` 提供 `write_file_durably`、`sync_directory` 与 `replace_file_durably`：写完后强制落盘（macOS 使用 F_FULLFSYNC，其他 POSIX 使用 fsync，Windows 使用 FlushFileBuffers），`owner_only` 从创建起即为 0600，密钥不会先以默认权限存在。交易日志、Agent 服务与防火墙记录、Agent 升级事务、节点身份证书与私钥、SSH 注册状态均使用该机制。CSV 解析错误、无权限、未知命令、重复 JSON 字段都不会越过 ABI 抛出异常。此集成没有引入实盘交易入口。

## 验收与剩余能力

所有 C++ 单元测试使用 GoogleTest 的 TEST / TEST_F、EXPECT_* 断言，由 CMake gtest_discover_tests 接入 CTest，不保留自定义 main 或断言框架。CLI 进程验收仍作为 CTest 集成测试。

`foundation_test.cpp` 覆盖并发身份、定点舍入、时间推进、溢出、序列化和队列关闭。`kernel_test.cpp` 覆盖配置原子性、资源撤销/重入析构、调度顺序与失败、总线背压、线程关闭、授权撤销、Runtime 启停与启动回滚。Terminal API 测试包含重复方法字段拒绝和失败预览保留。

这些是可信进程内基础设施的实现，不表示整个平台完成。动态插件安装/卸载、进程隔离、持久事件/任务恢复、分布式身份与追踪、密钥保管仍需对应实现与验收。三平台 CI 运行相同测试；本轮本机结果只代表 macOS。

本轮本机验收（macOS arm64）：Debug/Release CTest 各 8/8，ASan/UBSan 8/8，TSan 基础/内核 2/2，Playwright 5/5，前端构建及 Tauri 原生链接通过。最终 Release DMG 重新构建并完成校验和与挂载签名验证。Linux/Windows 仍等待远端 CI 和原生验收；本轮未做 DMG 内的原生界面交互复测。

spdlog / ThreadPool / GoogleTest 更新：Conan 固定 spdlog 1.17.0、GoogleTest 1.18.0；Debug、ASan/UBSan、TSan 均为 27/27，Playwright 5/5，Tauri 原生链接通过。`plugin.hpp` 已归入 kernel。此前 8 项测试的记录属于迁移前结果。

最新日志/线程池/GoogleTest 验收：macOS Debug、Release、ASan/UBSan、TSan 各 27/27（25 个 GoogleTest 用例和 2 个 CLI 集成测试），Playwright 5/5，Tauri 原生链接及 Release DMG 校验和/挂载签名通过。生产依赖图在 with_tests=False 时排除 GoogleTest。Linux/Windows 尚待远端 CI，本轮未做原生界面交互复测。

## 本机进程与 IPC 机制

`kernel/ipc/local_channel.hpp` 提供有界二进制帧、期限与断线处理，Unix Socket / Windows Named Pipe 在对应源码内处理平台差异，不解释交易语义。`kernel/ipc/tls_channel.hpp` 提供跨平台 TCP、TLS 1.3 双向认证、DNS/IP 校验及同样的有界分帧；不包含服务类型、账户或业务授权。`kernel/process/child.hpp` 提供无 shell 子进程启动、等待和回收。实际 Protobuf schema 位于 protocol，交易路由在 apps/trading，客户端在 apps/terminal/native。

当前可隔离桌面与模拟交易进程的崩溃，但不提供不可信代码沙箱或系统服务安装管理；本机模拟客户端和 Node Agent 应用层实现有限重启策略。详情见 [进程架构](process-architecture.md)。测试包含双进程独立账本、交易进程强杀恢复与 TCP/mTLS 远程连接；最新结果见 terminal-validation，三平台源码不替代 Linux/Windows 实机验收。

本机 Agent 统一管理后，`kernel/process/file_lock.hpp` 提供通用跨进程目录锁，应用分别用于 Agent 所有权和本机引导互斥。ChildProcess 的独立启动/释放用于显式开发测试 Agent；正式用户会话由操作系统托管 Agent。交易进程始终由 Agent 持有，业务健康判断留在 apps，未下沉为 Core 的交易分派逻辑。


### IPC 连接容量与期限

Channel::connect 现在将建立连接与发送业务帧明确分开。Unix Socket 在队列暂满（Linux EAGAIN、macOS ECONNREFUSED）时关闭失败套接字，在同一个调用期限内以最多 10 ms 间隔重新建立连接；不重用状态未指定的失败套接字。ECONNREFUSED 也可能是残留端点，因此只能在期限内等待，不推断服务已经启动。缺失/非法路径、权限失败及对端身份拒绝不进行业务降级，不改写或删除端点。

Windows 在 ERROR_PIPE_BUSY 时使用 WaitNamedPipeW 等待剩余期限，再尝试取得实例；等待成功后实例仍可能被别的客户端取得，因此所有尝试共享原期限。缺失端点明确失败。负超时继续表示显式无限等待，产品调用方均应选择有界期限。

这些尝试只发生在未发送应用字节之前。send/receive 失败仍关闭通道，不重新连接、不重放交易或管理命令；读取超时后的命令结果依旧可能未知。监听队列仍有界，没有通过无限队列隐藏过载。依据：[Linux connect 文档](https://man7.org/linux/man-pages/man2/connect.2.html)、[Apple AF_UNIX 实现](https://raw.githubusercontent.com/apple-oss-distributions/xnu/main/bsd/kern/uipc_usrreq.c)、[Windows WaitNamedPipe](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-waitnamedpipea)。

新增测试使用真实监听端点填满队列，验证等待空位、单一期限退出、12 个并发客户端逐条交付一次，以及非法/缺失端点拒绝。macOS 修复前两个容量用例均失败（`build/ipc-capacity-before-tests.log`），修复后 IPC 与维护专项 8/8 通过（`build/ipc-capacity-tests.log`）。源码同时包含 Windows 路径，但尚未 Windows 原生验收。

本轮 macOS 全量 133 通过、1 Linux 专属跳过（`build/ipc-capacity-full-tests.log`）。未变更业务协议、未自动重发命令、未删除残留端点或用户数据；未重建桌面安装包。

Linux x86_64 仿真容器全量 134/134 通过（`build/ipc-capacity-linux.log`），包含上述四项真实 IPC 连接测试、Agent 维护竞态及部署/恢复/心跳链路。此结果不代表 Windows 原生或物理跨机器验收。


### 可分派的 TLS 握手

TlsListener::accept_pending 只等待 TCP，返回不具备应用帧收发接口的 TlsPendingConnection。调用者可把该对象移动到有界执行器，在独立线程中以正超时调用 handshake；只有双向认证成功才返回 TlsChannel。原有 accept 仍提供完整同步接收用例，两者均不提供明文或跳过证书验证的模式。独立连接事件循环与共享证书配置上下文的所有权保持明确，监听器本身仅供单个接收线程使用。应用的线程数、排队容量、入站期限与业务串行化由应用装配，未下沉业务规则到 Core。Agent、Task Service、交易、策略与行情服务均在接收线程只接收 TCP，在有界工作池中完成双向认证；`trading_tls_admission` 验证静默的未认证连接不会阻塞合法客户端。

### TCP 接入的空闲期限与完成竞争

监听器的 accept 空闲期限只用于让接收循环定期检查停止状态。取消 accept 与已经完成的系统接入可能竞争；若完成回调明确成功，不能仅因定时器也已触发而销毁该连接。`TlsListener::accept_pending` 使用内部 `preserve_accepted` 期限策略，取消后仍收到成功回调时返回待认证连接；取消成功且回调返回错误时仍报超时。该策略不会越过 TLS 认证，也不会重发任何请求。

DNS/连接、握手和读写操作继续使用严格期限，超时取消后不得因后续成功回调而恢复已关闭的通道。内部定时执行助手位于 `core/src/kernel/ipc/timed_operation.hpp`，不是公共接口。确定性测试安排“定时器取消 → 已成功操作的完成回调”，修复前接入用例抛出超时，另外三个期限用例通过（`build/transport-deadline-red-tests.log`）。这证明了完成处理缺陷，并不证明此前 Linux 偶发截断仅由此造成。
