# 实施进度与验收

以下为顺序实施的切片，不将设计目标标为完成。

维护者最新优先级：**期货是第一种资产，Asterion Terminal 是第一种产品，保持原界面设计**。桌面接入已提前推进，其他资产与客户端暂不优先。

1. **工程与生命周期（基础完成）**：C++20、Conan 2、CMake、CLI、可信插件注册；验证依赖排序、非法注册、缺失/循环依赖、失败回滚、重复停止和重启。尚无动态加载、沙箱。
2. **基础与交易领域（进行中）**：作用域 ID、可注入时钟、严格序列化、有界并发、资源/配置/调度/权限/观测已由统一 Runtime 组合并接入 Terminal；Decimal 支持显式除法舍入和步长量化。标的规格、逐笔数据与本地限价单状态机已实现；单合约账户、持仓、冻结、每手费用和显式价格手动结算已实现；交易所真实规则与自动结算待实现。多资产先有真实用例再扩展规则。
3. **事件与存储（进行中）**：同步控制面事件分发与跨线程有界队列、背压已实现；历史模拟会话已有版本化文件日志与重放恢复；通用持久事件、数据库事务和订阅恢复待实现。最终验证崩溃与重放后的权威状态一致。
4. **C++ 与原界面（首条桌面链路已实现）**：Tauri 内嵌 C++，C ABI 请求边界，原主题/工作台/总览布局、设置与状态接入。期货数据校验和历史逐笔展示已连接真实 C++，模拟账户/交易面板已接入；持久化任务、回测、因子评价、数据发布和本机策略控制已接入。模拟交易现已拆为独立 C++ 子进程并通过 Protobuf 调用；无后台常驻实盘或不可信策略隔离。
5. **期货数据插件（进行中）**：CSV 历史逐笔、显式交割月份/实际合约/整手规格校验，CLI 与桌面预览已实现；版本化持久导入与 CTP 只读接口已实现，交易日历/交易所规格来源和真实账户时段行情仍待补齐或验收。无凭据时只报告离线验证。
6. **研究/模拟执行闭环（首版历史模拟完成）**：单合约历史回放、模拟下单/撤单、持仓与资金、文件持久化恢复已接入 Terminal。可信 SMA 的独立回测与策略宿主、可撤销模拟授权、自动回放、单因子计算与评价及恢复已实现。不可信策略隔离、独立风险插件、多日/多合约回测待实现；不能另建回测账本语义。详见 [精确范围](paper-trading.md)。
7. **交易接入与扩展 SDK**：真实券商回报、恢复、限额与授权；随后动态插件交付、Python/其他语言 SDK；三平台构建与打包 CI 已配置，Linux/Windows 实机验收待完成。实盘下单需要用户明确授权。

保留 `rust` 分支作为原界面与历史实现参考。工作区中旧文件的删除由用户发起，不执行 reset、不修改备份分支、不自动提交或推送。

全图模块对应的实现状态、边界与验收要求见 [完整实现矩阵](implementation.md)。唯一当前架构图是 [`assets/architecture.png`](assets/architecture.png)。

## 本次验证（2026-09-26）

- macOS arm64，Apple Clang 21，Conan 2.32.0，CMake 4.4.3：Conan install、Debug/Release configure/build 成功，当前 GoogleTest/CTest 用例已扩展至 27 项；Debug、ASan/UBSan、TSan 各 27/27 通过，Release 结果见下方最新验收记录。
- 生命周期测试覆盖正常依赖排序、逆序停止、析构清理、重启、运行中注册拒绝、重复启动拒绝、缺失/循环依赖无副作用、失败启动回滚、重复身份/非法依赖/不支持契约拒绝。
- 领域测试覆盖 Decimal 极值、精度损失拒绝与 40,401 组整数参照乘法，逐笔标的/步长校验，成交重复/冲突、超额/越价拒绝、部分成交、撤单与终态保护；事件测试覆盖有状态订阅、订阅变更与错误传播。
- CSV 测试覆盖生命周期、LF/CRLF、EOF、时间顺序、规格拒绝、输入错误后停止；CLI 集成验证有效文件完成摘要和坏行非零退出。
- 最新架构附件与 `docs/assets/architecture.png` 的 SHA-256 一致，仓库工作树只存在这一份架构图片：`7c3d4024359353f137ee277663f37d5e0996e719d266cc42152df63204b9ddae`。
- TypeScript/Vite 生产构建、Cargo check、Clippy（warnings denied）与 Rustfmt 检查通过。Playwright 5/5 通过，浏览器测试通过真实 C++ API 验证界面、期货 CSV、失败后保留预览和连接重试。
- Linux / Windows CI 已配置，但尚未在远端运行。动态插件、真实在线行情、券商与交易均未验收。桌面包验证记录见 [终端验收](terminal-validation.md)。
- 本机原默认 profile 为 Clang 20，PATH 编译器为 Clang 23，且当前 Conan 设置不接受 Clang 23。验证改用显式 Apple Clang 21 本地 profile，未改写全局 profile。链接器仍报告环境中的重复 LLVM rpath 警告，不影响本次构建与测试。

Core 基础设施实现与精确边界见 [Core 基础设施](core-infrastructure.md)，安全隔离、持久任务恢复仍不能标为完成。

日志已切换为 spdlog，单工作线程升级为可配置 ThreadPool，插件契约已归入 kernel。单元测试已迁移到 GoogleTest，CLI 保留进程集成测试。

最新日志/线程池/GoogleTest 验收：macOS Debug、Release、ASan/UBSan、TSan 各 27/27（25 个 GoogleTest 用例和 2 个 CLI 集成测试），Playwright 5/5，Tauri 原生链接及 Release DMG 校验和/挂载签名通过。生产依赖图在 with_tests=False 时排除 GoogleTest。Linux/Windows 尚待远端 CI，本轮未做原生界面交互复测。

## 历史模拟交易验收（2026-09-26）

- 单合约账户、持仓/冻结、固定每手费用、历史逐笔 Paper 撮合、撤单、手动结算已接入 Terminal 交易插件；文件日志记录完整输入和操作，可关闭/重启后选择原目录恢复。
- macOS Debug、Release、ASan/UBSan、TSan 各 38/38（35 个 GoogleTest、3 个进程集成测试）；新增直接终止 C++ 进程、锁释放、未完成写入、幂等重发与恢复后继续平仓验证。
- TypeScript/Vite 构建成功，Playwright 6/6；真实 C++ 后端完成导入、开仓、平仓、费用/资金核对和关闭恢复。测试初始 1000，开仓 99、平仓 110、乘数 10、费用合计 5，最终资金 1105。
- macOS Release Tauri 原生链接、DMG 校验和、挂载后签名验证通过。当前为本机 ad-hoc 签名，未做 Apple 公证。
- 本轮 UI 通过浏览器端到端和截图核对；没有执行原生窗口交互复测。Linux/Windows 原生 CI 未运行，不能以本机通过代替三平台验收。文件系统插件新增相应系统锁/同步实现，但 Windows 分支仍待实机验证。
- 首版不包含自动交易日/夜盘、跨日续接、真实保证金率、独立风控插件、强平、策略回测或在线交易。详情见 [期货模拟交易](paper-trading.md)。

## 独立交易进程、Protobuf 与 CLI11（2026-09-26）

- 当前三个 C++ 产品/开发入口 `asterion`、`asterion_terminal_dev_bridge`、`asterion-trading` 使用 CLI11；依赖经 Conan 锁定，Windows 参数显式转换 UTF-8。没有创建空的 market-data 或 worker 工程。
- PaperSession 移入 `apps/trading/`，Terminal 改为 Protobuf 客户端，不链接 Paper/文件日志实现。实际通信使用 Unix Socket / Windows Named Pipe，通用进程与 IPC 机制位于 Core kernel。实时行情宿主的目标名称为 market-data，历史模拟不依赖它。
- macOS Debug、Release、ASan/UBSan、TSan 各 44/44（40 个 GoogleTest、4 个进程集成测试）。新增精确数值、未知字段、帧大小、断线/期限、版本/模式/会话拒绝、两个独立账户进程以及 CLI11 帮助/版本/非法参数测试。
- Protobuf 会话从宿主启动独立交易进程；强杀交易子进程后宿主继续响应并返回明确恢复状态。重新启动并打开日志后，重复请求幂等，账户与回放状态恢复。测试使用中文临时路径。
- TypeScript/Vite 构建及 Playwright 6/6 通过，交易界面与总览均验证崩溃后的恢复提示；保留原界面风格。
- macOS Release DMG 重新构建；验证镜像校验和、挂载后的 app 与交易 sidecar 签名，并直接使用 DMG 内的交易程序完成强杀/恢复/继续平仓测试。本机 ad-hoc 签名，未公证。本轮未操作原生窗口。
- Linux/Windows CI 配置会执行同一组测试和原生打包，但本轮尚未运行。当前 live 模式退出码 3，未开放实盘；实时行情订阅、策略/研究工作进程、后台常驻会话、不可信策略沙箱尚未实现。
- 结构、协议、生命周期与边界见 [进程架构](process-architecture.md)。

## TCP 远程部署与 Terminal 连接配置（2026-09-26）

- Core 新增 Asio/OpenSSL TCP+mTLS 通道，仍使用有界长度前缀和 Protobuf；服务端独立持有模拟会话，Terminal 断开/退出不结束远程服务。显式重连获取服务器权威快照，命令不自动重发。
- 设置 → 连接支持具名配置、服务器地址/端口/会话及本机 PEM 路径，保存后手动连接；远程账户目录由服务端 CLI 参数决定。界面沿用原主题和工作台，未增加通用空服务工程。
- macOS Debug、Release、ASan/UBSan、TSan 各 **45/45**（40 个 GoogleTest + 5 个集成测试）；新增验证错误服务端 DNS/IP 身份、不可信 CA/客户端、过期或缺失客户端证书、会话/模式拒绝、分帧、部分帧超时、超限帧、远程 shutdown 拒绝、独立生命周期、服务重启恢复及持久命令去重。
- TypeScript/Vite 构建、Tauri Debug/Release 原生链接成功；Playwright **7/7**，真实后端覆盖配置保存/刷新、远程连接、初始化、回放、重连与断开。检查了连接页截图。
- Release DMG 重新生成并验证校验和、挂载后 app/sidecar 签名；直接使用 DMG 内的交易程序完成 TCP+mTLS 与本机强杀恢复测试。测试 PKI 仅生成在临时目录，不随安装包发布。当前为 ad-hoc 签名，未公证。
- 网络测试在本机 loopback 上运行，**没有两台物理机器联调**；Linux/Windows 原生 CI 与本轮原生窗口交互尚未执行。系统服务安装、自动重启、多客户端、实时行情推送、细粒度证书角色/吊销和实盘仍未实现。

## 默认本机、Node Agent 部署与后台保活（2026-09-26）

- 默认本机按需启动；远端安装流程现已改为 Terminal 的 SSH 引导，后续通过 Agent 管理。新增 `asterion-node-agent` C++/CLI11 入口与 `node.proto` 管理协议，支持多个节点监控、分块上传、SHA-256/实际 OS/CPU 校验、独立服务部署、启动与停止，不覆盖已有服务或账本。
- C++ 后台每 5 秒执行交易/节点专用心跳，不依赖前端轮询。节点、OS 进程和交易就绪状态分别展示；节点心跳超过 15 秒未确认显示失联、缓存服务状态未知。本机和 Agent 托管模拟进程异常最多重启 3 次，远程交易连接每次故障最多尝试 3 次只读重新附着；不重发交易命令。
- Agent 状态目录独占，服务名称与路径受限；管理程序或目录校验失败拒绝执行。受管进程检测 Agent 所有权，Agent 异常退出后结束，避免恢复时留下无主交易实例。主动停止的服务不自动拉起，关闭 Terminal 或移除监控不停止远程服务。
- macOS Debug、Release、ASan/UBSan、TSan 各 **47/47**（40 个 GoogleTest、7 个集成测试）。验证无前端请求超过 30 秒仍保活、本机重启恢复、远端部署、校验失败/路径越界拒绝、强杀服务后重启、主动停止、Agent 异常退出与恢复、端口冲突到 3 次后停止重试。补充的受管账户账本恢复及重复请求去重测试在 Debug/ASan/TSan 和解压后的 Release 服务包均通过。
- TypeScript/Vite 构建、Release Tauri 链接、Playwright **8/8** 通过；浏览器端使用真实 C++ Agent 完成保存节点、上传/部署、连接交易、启停和状态展示，已核对截图。服务表在设置页优先展示，证书与部署表单收进详情。
- macOS Release DMG 重新构建，校验和、挂载后的 app/交易程序签名以及包内本机/远程交易恢复通过；另生成 `asterion-services-macos-arm64.zip`，解压后核对 manifest 摘要、执行版本命令，并完成 Agent 部署/监督/账户恢复集成测试。CI 上传各原生平台服务 ZIP；它是部署载荷，不增加第二种桌面安装包格式。
- 网络验证均为本机 TCP loopback，多物理机器、Linux/Windows 原生流水线、本轮原生窗口交互未验证。Agent 已接入本机用户任务和远端 SSH 系统服务注册，远端特权安装尚待实机验证；在线升级/回滚、跨节点迁移、细粒度多租户授权、实时行情与实盘未实现。详情见 [服务管理](service-management.md)。

### 本机 Node Agent 统一管理

- 本机与远端使用同一 Node Protobuf 管理模型；Terminal 不拥有交易子进程，业务连接直接访问交易服务。
- Agent 独立业务健康探测、进程异常/连续无响应的有限恢复、显式启动/停止/重启和持久期望状态已接入。
- 本机按需引导用户级 OS 托管；macOS launchd 已隔离验证。Linux systemd 用户服务和 Windows 当前用户计划任务待原生验收。
- 原工作台保留，部署设置统一状态展示；Debug 46/46、Playwright 8/8、打包 Agent 与 DMG 挂载验证通过。详细证据见 terminal-validation。
- 待完成：SSH 三平台特权安装实机验收、显式版本升级/回滚、注销后机器级部署、Linux/Windows 原生服务管理验收。
- 本轮最终原生回归：macOS Debug/Release/ASan-UBSan/TSan 各 46/46，Playwright 8/8；未代替其他操作系统验收。

### SSH 首次安装入口

- Terminal 仅保留 SSH 引导远端机器的安装流程；旧手填 Agent 身份入口和旧 node.connect 参数已移除。
- 严格主机信任、粘贴 SSH 私钥认证、平台/权限核对、SFTP 上传、摘要校验、独立 mTLS 身份和三平台系统服务脚本已接入；日常管理继续走 Node 协议。
- 本机真实 OpenSSH 身份校验、带真实 Agent/mTLS 的安装编排替身测试、macOS 四种配置各 47/47、界面 8/8、DMG 挂载验证通过。
- 远端特权安装仍待真实目标机验证；Windows SCM 尚未原生编译运行。编排替身不是三平台实机安装证据。

### Linux 原生与用户服务验收补充

- Linux x86_64（Ubuntu 26.04.1 / GCC 15.2）独立原生构建、CTest 47/47 通过；macOS 本轮 Debug 47/47。
- Linux systemd 用户服务与 macOS launchd 的真实 Agent 故障恢复、Terminal 退出独立性、主动停止状态恢复均已验收。
- 剩余阻塞：两台测试机缺少免交互 sudo；LAN 管理端口 7442 直连超时；Windows 原生运行、系统级 SSH 安装与整机重启尚未验收。新增原生 CI 测试入口不等于已执行通过。

### 防火墙管理首版

- Terminal SSH 引导和 Agent Protobuf 检查/确认已接入，支持预览单来源/端口、受管规则放行与撤销、权限不足提示，以及变更后的 TLS 连通性检查。
- Linux UFW / Windows 持久规则适配已实现；macOS 与其他 Linux 防火墙返回手动配置状态。全部产品和进程继续保留三平台支持。
- Linux/macOS 原生回归 52/52、UI 9/9、macOS Tauri 原生链接通过；实际 Linux 检查确认无配置权限。真实特权规则操作、Windows 原生规则验收及 LAN 7442 直连仍待完成。

- Linux 独立账户初始化和受限服务注册已实现；下一步在一次性机器验证特权初始化全流程，完善 Linux 初始化与 Terminal 三字段入口；Windows/macOS 只保留本机部署。防火墙写权限仍不授予专用账户。

## 当前持续推进顺序（2026-09-27）

1. **已落地，正在扩大验证**：不可变回测输入摘要、可信 SMA 插件、单日回测、Task Service 持久化与独立工作进程。见 [研究任务](research-tasks.md)。
2. **已接入，正在验收**：本机 Agent 部署 Task Service 并按任务启动/监管 Backtest，Terminal 研究页提交、状态、取消、重试和结果，任务中心显示研究任务。已补齐远程 Linux 研究资源，Linux 容器部署及 macOS DMG 验证通过；因子计算已接入持久化任务、Agent 类型化派发与中英文界面。
3. **进行中**：Data Pipeline 版本化发布、来源摘要、持久化任务、Agent 和 Terminal 已接入，继续推进数据目录和分片能力；因子样本外评价与参数搜索；独立 Strategy 宿主与能力限制。
4. **继续完善**：期货日历/夜盘、多日结算、跨机器调度和资源配额，三平台原生验收。实盘必须等授权、风控、券商链与恢复能力具备后再开放。

任何阶段不以工程入口或接口定义冒充完整能力，不自动重跑未确认任务、不覆盖已有数据。

### 独立策略宿主进展

已实现持久化有序事件与目标持仓意图、IPC/TCP mTLS、独立健康通道和重放校验。Agent 管理、历史模拟账户授权/撤销及目标持仓交接已实现并通过双进程恢复测试；宿主自动历史回放与交接恢复已接入，接下来接入 Terminal 策略控制并完成本机 Agent 全链路与分发，再扩展实时行情游标。验收边界见 [策略宿主](strategy-host.md)。
