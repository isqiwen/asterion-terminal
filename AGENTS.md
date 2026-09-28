# Asterion 开发约束

本文件适用于整个仓库。开始工作前读取本文件。

## 当前架构（2026-09-26）

以维护者提供的多资产量化交易平台架构图为准：C++ Core、Conan 2、CMake，统一领域模型、事件驱动、插件扩展。此决定替代备份 `rust` 分支中“仅期货、Rust 固定内核、仅三类扩展”的规则。

权威架构图固定保存在 [`docs/assets/architecture.png`](docs/assets/architecture.png)，仓库工作树只保存最新版本，不建立带日期、编号或 final 后缀的副本；历史由 Git 管理。收到新版后替换同一文件，同步 `docs/architecture.md` 和实现验收矩阵。文字说明必须与图一致，不能自行收缩七类插件或多资产目标。

- Core 分为 foundation、kernel、domain。构建目标分别为 asterion_foundation、asterion_kernel、asterion_domain；领域模块可以依赖内核机制，内核依赖基础能力，不反向依赖领域模型、供应商、策略或页面。
- 交易标的、行情、订单/执行、持仓/组合、账户/资金、交易日历/结算、基础风险语义和多资产公共模型属于交易领域。
- 数据、执行、存储、策略、风控、工具、UI 为七类功能插件。官方能力与第三方能力使用相同公开契约；供应商、存储后端、具体策略和风险算法不进入内核。
- 策略插件按策略能力分类（CTA、因子、统计套利、做市、组合、机器学习、事件驱动和自定义）；Python / C++ / Rust 是 Strategy API 的 SDK 语言，不是插件类型。外部市场、云、数据商和数据库通过插件接入，不属于 Core。
- 接口按具体用例建立，不为图中每个方框生成空实现，不把接口存在等同于功能完成。
- C++ 使用 C++20、RAII、明确所有权；金额不得使用二进制浮点作为权威账本值。异常不得跨语言或动态库 ABI 边界。
- Conan 管理 C++ 第三方依赖，CMake 管理 target 和依赖关系，单元测试使用 GoogleTest，CTest 发现并执行测试；日志使用 spdlog，C++ 产品/开发命令行入口使用 CLI11，统一由 Conan 管理依赖。禁止隐式下载依赖和全局 include/link 路径。
- 原 React / TypeScript 界面样式与交互沿用，业务调用直接更新为新契约。保留 Tauri 桌面外壳；Rust 只负责窗口、本机启动和薄桥接，核心全部使用 C++。
- 插件类型不是安全隔离。进程内原生插件只接受可信代码；未可信策略需要单独进程与能力限制。没有实现隔离前，不宣称沙箱安全。
- 实盘执行必须经过授权、账户风控和统一执行链；缺少必要能力时拒绝执行。

## 工程归属

- 运行时属于 Core 内核机制，不设并列的顶层 runtime。
- 插件身份与复用范围是两个维度：跨应用插件位于 `plugins/`，Terminal 专属 UI 插件位于 `apps/terminal/plugins/`。不要因为是 UI 插件就默认共享。
- Terminal 内置宿主（窗口、布局、导航容器、设置、桥接）与产品装配位于 `apps/terminal/src/`；插件贡献导航、页面和总览卡片。
- `apps/terminal/src/ui/` 是 Terminal 内置主题与基础 UI 库，不强行插件化。只有出现明确跨应用复用需求后，才提取共享库，不设顶层 ui 目录。
- Terminal 插件接口定义位于 `apps/terminal/plugins/contract.ts`，注册校验属于 `apps/terminal/src/host/plugin-registry.ts`，产品装配位于 `apps/terminal/src/plugins.ts`。当前不设顶层 sdk；待有明确使用者与稳定边界后再提取。共享目录不得反向依赖 apps。
- 进程按生命周期和隔离边界组织：交易程序位于 `apps/trading/`，实盘/模拟是同一程序的不同会话实例，模式固定，账本/存储分离；缺少实盘能力时拒绝启动。实时行情宿主位于 `apps/market-data/`，通过 CTP 数据插件接入只读行情，历史回放不依赖它。
- 进程通信用 Protobuf 定义，schema 位于顶层 `protocol/proto/`，消息校验与表示转换也归 `protocol/`；`bindings/` 仅负责跨语言调用绑定；通用 IPC 和进程管理位于 `core/kernel` 对应头文件/源码目录，业务路由在应用中。本机 Unix Socket / Windows Named Pipe、跨机器 TCP + mTLS 的边界与当前限制见 `docs/process-architecture.md`。
- 本机和远程统一由 Node Agent 管理服务；Terminal 不拥有交易子进程，关闭窗口/会话只断开连接。默认本机初始化时自动启动 Agent 和行情服务，模拟交易会话创建后自动启动并托管；已部署且设为运行的服务随 Agent 恢复。随桌面包提供 Agent 并注册当前用户的 OS 托管任务。远端仅通过 Terminal 的 SSH 引导安装 `apps/node-agent/` Node Agent：严格校验 SSH 主机身份、检测平台与安装权限、校验上传摘要、配置 mTLS 身份并注册系统服务。SSH 认证默认由 Terminal 本机生成并保存 Ed25519 密钥，界面只返回可复制的公钥，管理员在目标 Linux 初始化时授权；也允许本机临时粘贴已有未加密私钥。不使用 SSH Agent、用户输入的私钥路径或密码登录；私钥不进入连接配置、页面响应或远端。生成的私钥由 C++ 独立保存在本机受当前账户权限保护的密钥目录，相同机器名称复用，不静默覆盖或轮换。SSH 只执行受控安装/维护脚本，不提供任意远程命令入口；后续通过 Protobuf/mTLS 部署和管理服务，不保留手动安装产品入口。Agent 独立探测进程和业务健康并执行有限重启，Terminal 保活只负责连接，界面显示节点、进程与业务状态的区别，机制和限制见 `docs/service-management.md`。
- Terminal 在设置中配置远程服务地址、会话与 TLS 身份文件；远程服务独立于 Terminal 存活，账本目录由服务端管理。本机与远程部署显式选择，不自动降级或重发交易命令。
- `bindings/c/` 封装 C ABI，Terminal 专属 C++ 应用编排和交易进程客户端位于 `apps/terminal/native/`。不以 services、presentation、products 重复划分同一职责。
- Terminal 浏览器开发与测试桥接入口位于 `apps/terminal/dev/core_bridge.cpp`，可执行程序名为 `asterion_terminal_dev_bridge`，不作为独立 API 应用；正式桌面使用 Tauri 薄桥调用 C++ 应用编排，再通过 Protobuf 访问独立交易进程，不保留进程内交易降级路径。
- 插件契约头文件位于 `core/include/asterion/kernel/plugin.hpp`；线程池位于 `kernel/thread_pool.hpp`，日志模块位于 `kernel/logger.hpp`。
- 交易前风险契约与官方订单限额插件见 docs/pre-trade-risk.md。风险配置是模拟会话与回测的必填持久化输入，手工与策略委托统一在执行插件提交路径检查；缺失配置、插件不可用或拒绝均不得默认放行。
- 历史模拟交易的账户语义、撮合假设、文件日志恢复和当前限制见 `docs/paper-trading.md`；模拟规则不能冒充交易所规则，实盘仍未开放。
- Core 基础设施的线程所有权、取消、资源撤销和安全边界见 `docs/core-infrastructure.md`；内核只能提供通用机制，业务配置与命令由应用/插件注册。
- 多语言属于 Terminal 宿主基础设施，位于 `apps/terminal/src/i18n/`；当前支持 zh-CN/en-US。宿主文案归宿主，各 UI 插件通过公开契约贡献独立命名空间的语言资源。Core 不依赖界面语言，错误码由 UI 本地化，原始诊断进入详情。
- 当前 UI 插件为构建时注册、面板按需加载，React 管理挂载与清理；没有动态安装、热卸载或隔离，不能宣称完整第三方插件平台。

- 防火墙属于应用部署管理：先检查并展示具体来源 IP / TCP 端口，显式确认后执行；只管理自身记录的规则，不开启全局防火墙、不接管已有规则。Linux UFW / Windows 的权限不足与 macOS 手动配置必须明确反馈，规则执行与网络连通性分别验收。Terminal 和所有服务继续支持三个平台。

## 当前交付优先级

维护者明确：第一种资产是期货，第一种产品是 Asterion Terminal。先贯通原 React 工作台、Tauri 与 C++ 的桌面链路，保持 `rust` 分支界面设计一致，再按期货用例补齐领域、数据、研究与模拟交易。多资产、Web、Notebook、Mobile 保留为整体目标，但不得先于期货终端扩展。

## 跨平台要求

Linux、Windows、macOS 为一等支持目标，共用 C++ 核心和 Terminal 界面。平台差异收敛在工具链、系统桥接和打包配置，不复制业务实现。`bindings/` 保留顶层作为跨语言绑定的统一位置。

安装包分别为 Linux `.deb`、Windows NSIS `.exe`、macOS `.dmg`。CI 在三个原生系统分别构建、测试和打包；本机通过不能代替其他系统验收。Windows 使用 MSVC，与 Rust 使用相同动态 CRT；Linux 使用 GCC/libstdc++，macOS 使用 Apple Clang/libc++。新增原生依赖需核对目标平台支持。

## 不兼容旧设计

只实现最新契约。不恢复旧后端、旧协议转发、别名、双实现、自动迁移或降级路径。`rust` 是历史参考，不作为运行时依赖。保护用户历史数据，不静默改写或删除。

## 验收与界面

保持简洁、干净、高效；业务结果、时效、异常和主要操作优先，配置、证据与技术标识进入详情。复用原工作台与共享控件。新增交互应参考实际产品体验，区分观察与设计推断。

每次交付报告实际实现、测试、集成与未验证范围。模拟数据仅用于明确的开发样例和测试，不能作为生产行情。凭据只在用户本机输入或生成，不进入聊天、源码或日志。禁止未经要求提交、推送或修改备份分支。

## 目标机器初始化

管理员在目标机器执行独立初始化脚本，只接收 SSH 公钥，不接收私钥。Linux 实现位于 `scripts/node/initialize-linux.py`，创建普通 `asterion` 账户、安装 root 持有的受限服务注册助手；Terminal 的 Linux 上传安装脚本不得以 root 执行。Agent 和业务服务使用普通账户；不授予免密任意 shell 或 NOPASSWD ALL。当前防火墙仅查询权限；远程仅 Linux；界面三字段简化尚未完成，详见 `docs/host-initialization.md`。


## 部署范围（2026-09-27 最新决定）

本机部署支持 Linux、Windows、macOS，使用当前用户的系统托管机制和本机 IPC，不需要 SSH、私钥或目标机器初始化。远程部署目标仅支持 Linux，使用专用 asterion 账户、独立初始化脚本及 SSH 引导，后续通过 mTLS 管理。不再提供远程 macOS/Windows 安装流程；不影响三平台 Terminal、Agent 与业务服务的本机运行。设置中的“本机部署”和“远程 Linux”分开显示，默认本机。


桌面分发内置同版本的本机服务、Linux x86_64 远程服务及 Linux 初始化脚本。Terminal 提供脚本导出，自动探测远端架构并选择内置程序；不要求用户下载部署材料或选择可执行文件。Linux x86_64 构建产物由 CI 汇集，校验版本、架构和摘要后打入每种桌面安装包。

Linux 当前仅支持 x86_64（本机 Terminal、Agent、业务服务及远程部署），暂停 Linux ARM64 的构建、测试和分发。此限制不影响 macOS Apple Silicon。

## 已批准的独立应用工程（2026-09-27）

维护者明确要求建立 `apps/strategy/`、`apps/backtest/`、`apps/factor/`、`apps/data-pipeline/`、`apps/task-service/`，程序分别为 `asterion-strategy`、`asterion-backtest`、`asterion-factor`、`asterion-data-pipeline`、`asterion-task-service`。策略宿主不使用 strategy-worker 名称；回测与因子分开，不设置合并的 research-worker。 各工程的实现进度与验收范围记录在 `docs/implementation.md`，不写入本文件。业务未实现时明确拒绝执行，不伪报健康、不自动启动、不加入部署包。应用装配 Core 与插件；共享算法不复制到应用。Task Service 管业务任务与执行尝试，Node Agent 管机器进程，两者不可混淆。

## Agent 升级体验（2026-09-27 最新决定）

本机组件升级由启动流程自动协调，正常情况下用户不需要理解 Agent、点击升级或手工停止服务。保留身份、配置与业务数据；本次开发机清理不作为产品升级方案。运行中任务/交易必须先建立可恢复维护边界，不能强杀后宣称无感升级。当前已接入空闲 Agent 的自动检查/更新/继续恢复；运行中排空和恢复的设计与验收要求见 `docs/agent-upgrades.md`。此决定替代此前要求启动页显式确认 Agent 升级的产品交互。

## 工程约定（2026-09-27 架构评审）

- C++ 按 `.clang-format`（clang-format 23.1.1）、前端按 Prettier 格式化；提交前运行 `pnpm format`，CI 检查。
- 领域对象通过类型化查询交互；JSON 快照只用于协议边界与展示，不作为内部接口。
- Core 与服务只输出英文诊断，跨进程错误必须携带 `ErrorCode`；面向用户的新诊断同时登记到 `apps/terminal/src/i18n/locales/diagnostics.*.json`。
- 改变撮合、费用/保证金、风控或交易命令语义时，必须提升 `apps/trading/paper_session.cpp` 中的日志引擎标识；恢复拒绝不同标识，不静默重算历史。
- 持久状态与密钥通过 `kernel/durable_file.hpp` 写入；TLS 服务在接收线程只接收 TCP，握手放入有界工作池。
- Terminal 状态读取不得排队在长操作之后；新增的长操作不得持有全局锁阻塞其他窗口。

