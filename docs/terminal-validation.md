# 期货优先的 Asterion Terminal

## 界面与代码来源

参考 `rust` 分支 `8d5bc418a79a84b0dfe9310035b8611ffc450ced`。初始移植时以下八个界面源文件与该分支逐字节一致（后续国际化等变更见本文末尾验收记录）：

- `apps/terminal/src/ui/theme/style.css`
- `apps/terminal/src/ui/theme/tokens.css`
- `apps/terminal/src/host/workspace/Workbench.tsx`
- `apps/terminal/src/host/components/WindowFrame.tsx`
- `apps/terminal/src/host/components/window-frame.css`
- `apps/terminal/plugins/overview/Dashboard.tsx`
- `apps/terminal/plugins/overview/layout.ts`
- `apps/terminal/plugins/overview/dashboard.css`

保留原深色橙色主题、64px/40px 导航、工作区标签、工具栏、状态栏、总览布局编辑与共享交互反馈。窗口框架、总览编辑器消费的上下文直接调整为新 C++ 契约；旧服务/API、身份后端、账户凭据、Python 运行时没有恢复。

本次实际打开已安装的旧终端，观察到启动流程和账号入口；没有登录旧账号或冒充已完成旧业务工作区实机对照。工作台设计一致性以源文件复用与新工作台实际渲染为依据。业务覆盖尚不等同于旧版全部功能。

## 当前可运行流程

1. 启动桌面应用，Tauri 创建 C++ 运行时，通过本机 C ABI 读取状态。
2. 在原工作台中切换总览、市场、数据、研究、交易；研究支持均线回测与成交动量因子任务，交易支持历史模拟账户。
3. 数据 → 选择本机 CSV → 填写交易所、品种、实际合约、交割月份、价格步长、每手乘数和手数步长 → 校验并预览。
4. C++ 校验完整文件后发布内存中的会话预览；错误保留此前有效预览。数据只读，不覆盖原文件。
5. 市场展示历史成交序列及最近 240 笔成交，明确标记“历史数据 · 非实时”，纳秒时间使用字符串传递。
6. 设置 → 外观可调整原密度和涨跌配色；设置 → 插件显示本次发行版可用的 CSV 插件。

期货首批身份校验限定国内六个期货交易所。交割月份与规格由用户提供，尚未关联交易所正式资料、交易日历、保证金、涨跌停或结算规则。价格图使用浮点坐标仅用于显示，权威价格、成交量、输入校验仍在 C++ Decimal 中。

## 自动验证（2026-09-26）

- Debug、Release、ASan/UBSan：各 6 项 CTest 通过，包括 C ABI 错误边界、期货身份与完整预览发布。
- TypeScript / Vite 生产构建通过；Cargo check、Clippy `-D warnings`、Rustfmt 检查通过。
- Playwright 2/2：使用真实 C++ 开发传输，验证启动、原导航折叠宽度、布局编辑、CSV → 历史行情、非法主力代码拒绝、旧预览保留、设置入口与断连重试。
- Playwright 行情输入是临时目录中的测试数据，不打包进终端，不登记为生产行情。
- UI 截图在 `apps/terminal/test-results/`；该目录不提交。

## macOS 原生验收（2026-09-26）

- Release DMG 已生成。初次发现只有链接器签名，补齐 `bundle.macOS.signingIdentity = "-"` 后重新打包。
- 最终 DMG 的 `hdiutil verify` 与只读挂载后 `codesign --verify --deep --strict` 均通过。此为本机 ad-hoc 签名，未做 Developer ID 公证。
- 从 DMG 内启动 `.app`，实际观察到原工作台及“本机核心已连接”；通过 Tauri 系统文件选择器选择明确标记的 `acceptance-test-only.csv` 临时验收文件。
- 填写 SHFE / rb2610 / 2026-10 / 步长 1 / 乘数 10，界面校验成功；历史市场页面显示 2 笔、总量 5 手、最后价格 3511，与测试输入一致。该记录仅是测试证据，不是真实行情验证。
- 验收结束已退出测试会话并卸载镜像，原 `/Applications/Asterion Terminal.app` 未覆盖，`rust` 分支未修改。
- 构建脚本已加入 DMG 与镜像内 app 的签名检查；可单独运行 `python3 scripts/desktop.py verify`。

## 当前边界

当前为本机单用户工作台和历史数据预览，不包含账号体系、数据库持久化、实时行情、研究引擎、账户账本或交易执行。运行时随桌面进程退出，重新启动不恢复预览。进程内 C++ 不提供崩溃隔离；动态第三方代码尚不能加载。

产品脚本 `pnpm desktop` 启动开发终端，`pnpm desktop:build` 生成当前平台安装包。正式 macOS 包只输出 DMG，构建过程中的 `.app` 会被 Tauri 清理。Windows/Linux 尚未完成本地实机打包验证。

## 工程归属调整验收

当前宿主与专属插件统一归属 `apps/terminal/`，Terminal 内置 UI 库位于 `apps/terminal/src/ui/`，插件接口定义位于 `apps/terminal/plugins/contract.ts`，注册校验位于 `apps/terminal/src/host/plugin-registry.ts`。五个工作区使用插件贡献生成导航，业务卡片由行情、交易、数据插件提供；设置列表来自实际注册记录。面板代码按需加载。

本次目录调整后重新执行 C++ 构建/CTest、前端构建和 Playwright（含冲突注册、共享代码依赖边界、插件设置列表和工作区切换）。上文的原生 DMG 交互记录属于调整前验证，不代表本次产物已重新做原生交互验收。

本次实际结果：C++ CTest 6/6、Playwright 5/5、TypeScript/Vite 构建、Tauri cargo check、git diff --check 通过；上述 8 个视觉与布局文件与 rust 分支再次逐字节核对一致。本次未重新打包或进行原生 DMG 交互验收。

Terminal UI 库归属调整：已移入 `apps/terminal/src/ui/`，移除顶层 ui 及旧 ui-kit 导入别名。此次前端构建与 Playwright 5/5 通过，主题文件与 rust 参考逐字节一致，未重新打包。

Terminal 插件接口归属调整：移除顶层 sdk，接口定义与宿主注册校验分别归入应用内；此次 TypeScript/Vite 构建与 Playwright 5/5 通过，未重新打包。

开发桥接归属调整：入口为 `apps/terminal/dev/core_bridge.cpp`，Vite 启动 `asterion_terminal_dev_bridge`。C++ 构建及 CTest 6/6、前端构建、Playwright 5/5 通过；本次未重新打包。

三平台基础设施调整：CI 扩展至 Linux/Windows/macOS 原生构建与打包；本机 macOS Debug 原生链接、Debug/Release CTest 各 6/6、Playwright 5/5、Release DMG 构建与挂载签名校验通过。Linux/Windows 流水线尚未执行，原生安装交互待验收。

Core 基础设施集成后：Debug/Release CTest 各 8/8、ASan/UBSan 8/8、TSan 基础/内核 2/2、Playwright 5/5、前端构建、Tauri 链接与重新生成 DMG 的挂载签名校验通过。没有改变原界面设计；本轮未进行安装包原生交互复测。

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

- 默认本机按需启动；远端采用维护者确认的“手动安装 Agent，后续 Terminal 管理”。新增 `asterion-node-agent` C++/CLI11 入口与 `node.proto` 管理协议，支持多个节点监控、分块上传、SHA-256/实际 OS/CPU 校验、独立服务部署、启动与停止，不覆盖已有服务或账本。
- C++ 后台每 5 秒执行交易/节点专用心跳，不依赖前端轮询。节点、OS 进程和交易就绪状态分别展示；节点心跳超过 15 秒未确认显示失联、缓存服务状态未知。本机和 Agent 托管模拟进程异常最多重启 3 次，远程交易连接每次故障最多尝试 3 次只读重新附着；不重发交易命令。
- Agent 状态目录独占，服务名称与路径受限；管理程序或目录校验失败拒绝执行。受管进程检测 Agent 所有权，Agent 异常退出后结束，避免恢复时留下无主交易实例。主动停止的服务不自动拉起，关闭 Terminal 或移除监控不停止远程服务。
- macOS Debug、Release、ASan/UBSan、TSan 各 **47/47**（40 个 GoogleTest、7 个集成测试）。验证无前端请求超过 30 秒仍保活、本机重启恢复、远端部署、校验失败/路径越界拒绝、强杀服务后重启、主动停止、Agent 异常退出与恢复、端口冲突到 3 次后停止重试。补充的受管账户账本恢复及重复请求去重测试在 Debug/ASan/TSan 和解压后的 Release 服务包均通过。
- TypeScript/Vite 构建、Release Tauri 链接、Playwright **8/8** 通过；浏览器端使用真实 C++ Agent 完成保存节点、上传/部署、连接交易、启停和状态展示，已核对截图。服务表在设置页优先展示，证书与部署表单收进详情。
- macOS Release DMG 重新构建，校验和、挂载后的 app/交易程序签名以及包内本机/远程交易恢复通过；另生成 `asterion-services-macos-arm64.zip`，解压后核对 manifest 摘要、执行版本命令，并完成 Agent 部署/监督/账户恢复集成测试。CI 上传各原生平台服务 ZIP；它是部署载荷，不增加第二种桌面安装包格式。
- 网络验证均为本机 TCP loopback，多物理机器、Linux/Windows 原生流水线、本轮原生窗口交互未验证。Agent 是手动安装运行的可执行程序，没有自动注册系统开机服务；在线升级/回滚、跨节点迁移、细粒度多租户授权、实时行情与实盘未实现。详情见 [服务管理](service-management.md)。

## 本机与远端统一 Node Agent（2026-09-26）

本机交易也改为 Agent 部署、持有和监督，Terminal 断开或退出不停止服务；设置页统一展示节点、进程、业务健康，支持启停/重启。健康探测经独立私有 Protobuf 通道运行，不占用交易控制连接。

本轮已实际验证 macOS Debug 46/46 CTest（Terminal 的两个 GoogleTest 用例合为一个隔离进程入口，实际仍运行两个用例），Playwright 8/8。增强的 service_heartbeat 检查无 Terminal 时业务心跳继续、SIGSTOP 卡死后的 Agent 强制恢复、持久账本一致性、显式重启和停止后不再拉起。隔离 launchd 测试检查自动注册、Terminal 退出后 Agent 存活、Agent 被杀后由 launchd 拉起，并在结束后撤销测试注册。

Release DMG 已重新生成：挂载后验证应用及交易程序/Node Agent 的签名，执行打包程序的 mTLS 测试和经打包 Agent 的账本恢复测试；镜像已卸载。仍为 ad-hoc 签名，未公证。Node Agent 自身的 Windows 用户计划任务和 Linux systemd 用户服务已有实现，尚未在本机之外运行验证。没有双物理机器测试、SSH 引导、在线升级或注销后机器级常驻保证。

最终回归：macOS Debug、Release、ASan/UBSan、TSan 均为 **46/46 CTest 通过**；Playwright **8/8**。记录分别为 `build/node-unify-tests-final.log`、`build/node-unify-release.log`、`build/node-unify-asan.log`、`build/node-unify-tsan.log`、`build/node-unify-e2e-final.log`，打包和 launchd 分别见 `build/node-unify-package.log`、`build/node-launchd.log`。这些日志为本机忽略产物，不替代 CI。测试 Agent、受管进程和隔离 launchd 注册均已结束；未提交或推送。

## SSH 引导替代手动安装（2026-09-26）

安装入口已统一为“通过 SSH 添加机器”，旧的手填 Agent 地址/证书表单移除；`node.connect` 只接受已引导节点的 ID，旧参数拒绝，不做兼容转发。认证仅接受本机粘贴 SSH 私钥正文，强制已有主机密钥信任；平台与权限检查后上传并校验摘要，生成独立 TLS 身份，注册目标系统服务，再确认 mTLS 心跳。

验证层级明确分开：

- `tests/ssh_host_identity.py` 在本机启动真实回环 sshd，验证未知/变更主机密钥拒绝，以及正确主机与密钥认证成功。没有执行特权安装；证据 `build/ssh-host-identity.log`。
- `ssh_bootstrap` 使用 SSH/SFTP 测试替身，检查严格选项、安装失败重试、身份不重置、旧连接参数与命令注入拒绝；随后运行真实 Node Agent，通过新生成身份执行真实 TCP/mTLS。远端系统服务脚本的执行是模拟的，不能据此宣称已实机安装成功。
- Playwright 8/8，通过 SSH 界面引导（使用上述替身）、真实 Agent/交易程序部署管理，以及使用已保存身份重新连接；证据 `build/ssh-e2e-current.log`。
- DMG 重新打包，挂载验证应用/两个程序的签名，打包程序的远程 TLS 与本机 Agent 恢复通过；证据 `build/ssh-package.log`。仍为 ad-hoc 签名。

没有提供外部机器或安装权限，因此未在远端真实执行 systemd/LaunchDaemon/Windows SCM 注册；Windows SCM 源码也未在本机编译执行。Linux/Windows 原生 CI及三平台特权安装、证书轮换、升级卸载、密码/交互 sudo 均不宣称完成。

SSH 变更最终回归：macOS Debug、Release、ASan/UBSan、TSan 均 **47/47 CTest 通过**，Playwright **8/8**。最终日志分别为 `build/ssh-debug-final.log`、`build/ssh-release.log`、`build/ssh-asan-final.log`、`build/ssh-tsan-final.log`、`build/ssh-e2e-current.log`。这些本机忽略日志不替代远端实机安装或跨平台 CI。未提交或推送。

### SSH 私钥正文输入

认证入口改为直接粘贴私钥；`node.bootstrap` 只接受 `private_key`，拒绝旧 `auth` / `key_file` 参数。具名机器配置不含私钥，提交后清空输入。OpenSSH 仅启用 publickey，禁用 Agent 和密码认证；临时身份文件限制当前用户访问，正常成功或异常退出调用时删除，强制终止进程的残留限制见服务管理说明。

本轮 macOS 验证：Debug 编译、SSH 引导集成测试、TypeScript/Vite 构建、8 项 Playwright 测试、真实 loopback OpenSSH 主机身份验证通过。覆盖缺失/错误密钥输入、旧字段拒绝、配置不保存私钥、成功/失败临时文件清理。远端安装编排使用测试替身，mTLS 握手使用真实 Node Agent；Windows/Linux 原生验证及远端系统服务实际安装尚未在本轮执行。

### Linux / macOS 原生部署验收补充

- macOS arm64 / Apple Clang：本轮 Debug CTest **47/47**；新增测试证书 SAN 参数后，远程交易与 SSH 引导聚焦回归 **2/2**。真实 loopback OpenSSH 主机信任检查通过。
- 独立 Linux 测试机：Ubuntu 26.04.1、x86_64、GCC 15.2、CMake 4.2.3，独立 Conan 2.32 缓存和锁定依赖，Debug 原生构建与 CTest **47/47** 通过。未构建 Linux 桌面 DEB。
- `tests/user_service_acceptance.py` 在 Linux systemd 用户服务和本机 macOS launchd 分别通过。使用唯一临时服务名，真实验证 mTLS、程序上传部署、交易连接、关闭 Terminal 客户端不停止交易进程、强杀 Agent 后 OS 拉起、恢复受管服务，以及主动停止状态跨 Agent 重启保留；测试服务已清理。这不是原生窗口交互、系统级 SSH 安装或 OS 重启验收。
- 修复本机服务注册只检查超时、不检查命令退出码的问题；macOS 先检查已加载状态，避免重复 bootstrap 掩盖注册失败。Windows 临时 SSH 身份 ACL 改为显式当前用户 SID；Windows 分支仍待原生运行。
- 新增 Linux/macOS 真 SSH + 特权系统服务验收脚本和 Windows SCM 验收脚本，已接入 Core CI。当前两台机器均无免交互 sudo，本轮特权 SSH 系统安装未执行；Windows 无测试机，不能标为通过。
- macOS 到 Linux 实际 LAN TCP 验收未通过：Linux 临时 Agent 由 systemd 正常启动、监听 `0.0.0.0:7442`，macOS TCP connect 超时；22 端口 SSH 密钥认证正常。随机测试端口亦超时。网络/防火墙原因尚未定位，未修改防火墙、未以 SSH 隧道代替直连结果。临时 Agent 和身份材料已清理。
- 本轮证据保存在忽略的 build 目录：`service-acceptance-tests.log`、`linux-native-tests.log`、`macos-user-supervisor.log`、`linux-user-supervisor.log`、`two-host-acceptance.log`。测试期间只使用临时身份；用户 SSH 私钥未写入报告或日志。

### 防火墙预览与确认（2026-09-27）

- 已接入 SSH 管理端口检查、Agent Protobuf/mTLS 交易端口检查、单主机来源预览、五分钟一次性确认、权限/状态重检、归属记录和明确撤销入口。验证规则执行与 TCP/mTLS 连通性分别显示。
- macOS Debug CTest **52/52**，Linux x86_64 Debug CTest **52/52**；规则解析最后补充重复标识拒绝后再做聚焦回归。TypeScript/Vite、macOS Tauri Debug 原生链接通过；Playwright **9/9**，覆盖检查无修改、确认执行、清空私钥及撤销。
- UFW 变更测试通过隔离命令替身执行实际生成的 shell 脚本，验证拒绝外部规则、被改动规则、重复归属标识和未启用防火墙；Windows 规则的生成范围有单元测试，Windows 原生规则操作仍未实机执行。测试替身不是实际放行证明。
- 真实 Linux SSH 检查：通过已核验主机身份和密钥连接，实际取得 Terminal 来源 IP、检测到 UFW；返回 `permission_required` / `can_apply=false`。没有修改服务器防火墙，也没有声称 LAN 管理端口已经打通。
- macOS 返回手动配置状态，不自动修改 PF；Linux 非 UFW 后端和云安全组/路由策略仍需管理员处理。Windows Agent 的默认 LocalService 权限不足时也明确提示，可通过管理员 SSH 入口维护。
- 证据：`build/firewall-all-tests.log`、远端隔离目录中的 `firewall-tests.log`、`build/firewall-final-rules.log`、`build/firewall-e2e.log`、`build/firewall-desktop-check.log`、`build/firewall-real-linux-inspection.log`。本轮未重新生成安装包，未执行原生窗口交互或特权防火墙写入。


## 2026-09-27：Linux 机器初始化与受限服务注册

- 独立脚本 `scripts/node/initialize-linux.py` 创建普通 asterion 账户、根持有公钥/SSH 配置及受限助手；不安装 Agent、不开放防火墙。
- Linux Terminal 引导改为普通账户处理上传文件，助手只注册固定 User=asterion 的服务。专用账户的防火墙预览只读，不显示可执行确认。
- macOS 和 Linux 原生 C++ 构建通过；两平台 `host_initialization`（5 项权限边界检查）与 `ssh_bootstrap` 通过。目标 Linux 的 sudo-rs 已解析新规则，未安装 sudoers 文件。
- Terminal Playwright 9/9 通过；TypeScript/Vite 构建通过。
- 真正创建账户、重新加载 SSH、安装助手及 systemd 启动尚未在实际机器执行；相关一次性主机验收已更新 CI，但尚未运行流水线。Windows/macOS 独立初始化及最终三字段表单未实现。


## 本机与远程部署范围调整

- 默认“本机部署”，不展示 SSH、私钥和远程安装字段；“远程 Linux”独立展示连接、初始化及部署流程。
- 删除远程 macOS/Windows SSH 安装生成逻辑；非 Linux Agent 在任何 SSH 调用前拒绝。本机三个平台的 Agent 托管实现保留，远程服务 ZIP 仅生成 Linux 版本。
- macOS / Linux C++ 构建及相关 CTest 2/2 通过。macOS 用例验证非 Linux 安装包拒绝，Linux 用例验证完整 SSH 替身编排与真实 mTLS 握手。
- UI 构建通过；Playwright 9 通过、1 跳过（macOS 不运行需要 Linux 原生 Agent 的远程安装用例）。新增本机/远程入口隔离及远程 OS 选项测试。
- 未执行目标机器特权初始化；未运行本轮 Windows 原生测试。


## Terminal 本机生成 SSH 登录密钥

- 新增 `node.key.prepare`：真实 OpenSSH Ed25519 生成、受当前用户权限保护的独立目录、重复查看及重启复用、公私钥匹配校验。已安装节点缺失密钥时拒绝生成替代项；不静默覆盖已有文件。
- SSH 命令必须显式选择 managed / provided；原生端处理私钥，页面响应只含公钥。界面默认本机生成，保留临时使用已有未加密私钥的入口；支持复制公钥和只读展示。
- Linux 初始化脚本默认提示粘贴公钥，保留 `--public-key` 自动化参数。真实系统安装验收脚本改用 Terminal 生成的密钥，但当前未提权执行该流程。
- macOS/Linux 原生构建与相关 CTest 各 5/5 通过（密钥生成/复用/权限/路径/匹配/认证/泄露检查、SSH 编排、CLI 和 Terminal API）。初始化输入提示另有 6 项脚本测试通过。
- 前端构建通过；完整 Playwright 10 通过、1 跳过（需要 Linux 原生 Agent 的远程安装用例）；新增公钥复制剪贴板检查的聚焦用例 2/2 通过。
- 私钥当前以无口令文件保存，Unix 0700/0600，Windows 创建受当前用户继承 DACL 保护的目录；不是 OS 凭据库或额外静态加密。Windows 保存权限未实机验收；实际远程公钥授权安装未执行。


## 桌面安装包统一入口验收

- `pnpm desktop:check` 编译链接，`pnpm desktop:build` 生成当前平台唯一安装格式：Windows NSIS EXE、Linux DEB、macOS DMG。Node 启动器在 Windows 使用 python，其余平台使用 python3。
- 修正构建环境中 ASTERION_TRADING_EXECUTABLE 错误指向 Agent 的问题；分别指定交易程序和 Agent 路径。
- CI 先构建 Linux x86_64 / arm64 服务 ZIP，作为中间构建材料嵌入所有桌面包；terminal 归档仅含对应安装包，终端用户无需获取独立服务 ZIP。
- 三平台构建编排和安装包数量拒绝检查通过模拟工具调用验证，不能替代各平台实际打包。
- 本机 `pnpm desktop:build` 成功，生成 `Asterion Terminal_0.1.0_aarch64.dmg`；hdiutil 校验、挂载内应用和 Agent/Trading 签名检查、包内程序版本检查、TCP/mTLS 及账本进程崩溃恢复全部通过。挂载已卸载。
- 当前 DMG 使用本地 ad-hoc 签名，未进行 Apple 公证。此轮没有执行 Windows EXE / Linux DEB 实机打包或远端 CI。


## 内置远程 Linux 服务与初始化脚本

- 每种桌面安装包必须内置 Linux x86_64 / arm64 Agent、交易程序及同版本初始化脚本；构建校验产品版本、ELF 架构、SHA-256、脚本内容与 ZIP 成员，任一架构缺失即失败。
- SSH 安装自动检测目标架构；交易部署依据已认证 Agent 的版本和架构选择内置程序。界面移除程序路径、平台和架构选择，新增初始化脚本导出；原生保存拒绝覆盖已有文件。
- macOS Debug CTest 54 项通过、1 项远程 Linux 部署测试按平台跳过；Linux 原生相关 CTest 5/5 通过，包含上传、部署、心跳、重启和账本恢复。Playwright 11 项通过、1 项 Linux 专属用例跳过；Tauri Debug 编译链接通过。
- 资源测试的假 ELF 仅用于测试夹具；发布构建必须使用 Linux 编译环境生成的真实可执行程序。当前没有执行目标机器特权初始化，也没有 Windows 实机验证。
- Linux ARM64 原生容器、x86_64 模拟容器均完成 Ubuntu 24.04 Release 构建；两种架构在干净 Ubuntu 容器中的 Agent / Trading 版本启动和运行库检查通过。
- 新 macOS ARM64 DMG（约 32 MB）已实际生成；挂载确认内置两个 Linux 架构和初始化脚本，资源摘要、应用/本机服务签名、TCP/mTLS 与包内服务账本恢复全部通过，挂载已卸载。Windows EXE / Linux DEB 的新资源打包配置已更新，但本轮未实机生成，也未运行远端 CI；macOS 仍是 ad-hoc 签名，未公证。


## 首次设置与启动流程

- Terminal 先显示独立欢迎页，沿用 rust 分支首次设置的布局和主题；用户点击“开始设置”后才调用本机服务初始化。
- 按实际响应依次验证 C++ 核心、通过 `node.local` 准备用户级 Agent、确认 Agent 在线及实例身份；失败保留数据并提供重试。未就绪时不挂载工作台。
- 首次完成后由用户点击“进入工作台”，浏览器本地存储仅记录引导完成标记；后续启动仍重新执行真实服务检查，标记不代表服务健康或账号身份。当前不提供账号登录。
- 安装包已提供本机运行组件，不再沿用旧 Python/PostgreSQL 下载和初始化链路。关闭 Terminal 不停止已启动的 Agent。
- 验收：TypeScript/Vite 构建通过；Playwright 12 项通过、1 项 Linux 专属测试跳过，覆盖未同意不启动、初始化失败/重试、完成进入、重启复查及故障拦截。macOS DMG 重新生成并通过包内资源、签名与服务恢复检查；已安装应用的原生 WebView 确认停在“开始设置”，未预先初始化用户级 Agent。Windows/Linux 原生首次启动界面未在本轮实机验证。


## Node Agent 工程与发布命名

- 管理进程工程位于 `apps/node-agent/`，CMake 和可执行文件统一为 `asterion-node-agent`，本机程序路径覆盖使用 `ASTERION_NODE_AGENT_EXECUTABLE`。节点模型、Node 协议和 `nodes` 数据目录保持原有含义。
- 同步 Tauri sidecar、Linux 双架构资源 manifest、SSH 安装、初始化助手及平台服务注册：macOS `me.asterion.node-agent`、Windows `AsterionNodeAgent`、Linux `asterion-node-agent.service`（远端带节点 ID）。不提供旧名称别名或自动迁移已有服务。
- 验收：macOS Debug CTest 54 项通过、1 项 Linux 专属测试跳过；Linux 原生相关集成测试 7/7 通过；Playwright 12 项通过、1 项跳过。Linux x86_64 / ARM64 Release 均已重编译，Agent 版本输出为 Asterion Node Agent。Windows 服务名称与打包配置已同步，未进行 Windows 实机验收。
- 新 DMG 已生成，挂载验证只包含新名称的 Linux 双架构资源；本机 sidecar 名称、签名、版本及服务恢复测试通过。当前已安装应用不会由构建命令自动替换，需使用新安装包更新。

## 中英文验收（2026-09-27）

宿主与五个 UI 插件采用独立的中英文资源；启动页和外观设置共用语言状态，偏好在重启后保留。新增测试覆盖资源命名空间/占位符校验、各工作区双语切换、外观偏好保留、首次启动语言传递、错误摘要与诊断详情。原有 CSV、模拟交易恢复、远程连接及 SSH 管理测试继续通过。截图检查发现的英文侧栏截断及设置标题溢出已针对英文布局修正。

此轮不再声称宿主和插件源码与 rust 分支逐字节一致：原主题与交互保留，组件文案已接入翻译。服务端日志与原始诊断保留原文；Windows/Linux 原生界面的语言切换仍需实机验收。

本轮验证：TypeScript/Vite 生产构建通过；Playwright 全套 15 项通过、1 项 Linux 专属测试在 macOS 跳过；英文布局修正后定向复测通过，截图已核对英文启动、工作台和设置。资源检查覆盖 470 条中英文消息的键与占位符一致性。

本轮重新执行 `pnpm desktop:build` 成功，生成 macOS ARM64 DMG；镜像校验、app/sidecar 签名、包内双架构 Linux 资源、本机/远程交易恢复验证全部通过。macOS 签名为 ad-hoc，未公证；未执行本轮 Windows/Linux 原生打包或桌面交互验收。

启动页简化（2026-09-27）：移除重复步骤说明、部署技术说明和常驻页脚，保留标题、当前状态、三个步骤与主要操作，失败诊断仍按需展开。语言选择框收窄为 88×28，左右对称内边距并居中显示中英文。生产前端构建通过，首次设置/英文启动两项既有流程测试通过，中文和英文截图已核对。

简化后的 macOS ARM64 DMG 已重新生成，镜像、签名、包内 Linux 资源与交易恢复验证通过。页面截图验证使用浏览器环境，本轮未做 Windows/Linux 原生界面验收。

## 2026-09-27：只读 CTP 行情链路

- macOS Debug：原 59 项 CTest 中 58 项通过，Linux 专用部署测试跳过；新增真实 SDK 回环启停、TCP/mTLS 行情客户端测试通过。订阅拒绝、登录失败、自动重连、乱序丢弃、密码脱敏、Agent 业务健康均有进程集成覆盖。
- 浏览器：16 项通过、1 项 Linux 专用测试跳过；新增行情测试走真实 C++/Agent/独立行情进程和明确的测试 SDK。修正远程已订阅列表编辑后，行情界面测试再次通过。
- Linux arm64/x86_64：Ubuntu 24.04 容器中 GCC 13 Release 编译和部署材料打包通过；不是目标机器安装或真实 CTP 登录验收。
- macOS 供应商 SDK：真实 6.7.7 动态库加载、版本、回环启动和 Release 通过；macOS 薄化后的库在 Conan 生成阶段重新签名。
- 未验证：真实 SimNow 账号与交易时段行情、Windows 原生运行、实际跨机器行情吞吐与长期断线恢复。Linux ARM64 没有当前选定版本的 CTP SDK。
- 最终 macOS DMG 验收通过：挂载后的整包/三个 sidecar/CTP 库签名、Linux 双架构资源摘要、模拟交易恢复，以及使用包内 Agent + 行情程序 + 真实供应商 SDK 的回环启动和关闭。已通过 entitlement 修复打包后 SDK 加载限制；安装包仍为本机 ad-hoc 构建，未做 Apple 公证。

## 当前 Linux 架构范围

维护者已决定暂停 Linux ARM64；上文双架构构建记录属于历史验收。当前 CI、远程部署与桌面内置资源只支持 Linux x86_64；macOS arm64 保留。

## 总览业务内容调整

总览默认以实时自选行情为主、模拟账户摘要为辅，沿用原工作台主题和布局编辑。行情表与市场页共用时效/断线判断；账户展示权益、可用资金、浮动盈亏、保证金和真实模拟持仓，资金明细折叠。未导入历史数据时收起数据卡片，无账户时不显示重复风险空卡；编辑模式仍可查看所有已保存组件，`hasContent` 只影响展示，不改写布局。

移除重复“工作概览”和卡片上方“总览”标签，服务摘要直接进入连接设置；实时行情入口显式打开实时页，CSV 预览入口打开历史页。没有提醒贡献时隐藏空提醒按钮。

本轮通过前端构建、空状态/保存布局/窄窗口、实时行情总览、模拟持仓与恢复提示、中英文切换共 4 项聚焦浏览器回归。测试行情来自明确的测试 SDK，空状态使用测试响应裁剪，不代表真实市场验收。

## 研究服务部署与分发（2026-09-27）

本机研究任务恢复及远程 Linux 资源完整性测试通过，部署/初始化/研究界面定向 5 项通过。Linux x86_64 容器中 GCC 严格警告修正后构建通过，Agent 原生上传及 TCP/mTLS 研究任务执行、服务重启结果恢复已通过。macOS Tauri 外壳链接通过，DMG 已重新生成；挂载后校验镜像、程序签名、Linux x86_64 资源以及包内 Agent/Task Service/Backtest 的真实任务恢复。安装包约 50 MB，ad-hoc 签名、未公证。该验证不包含替换用户当前安装或升级已运行系统 Agent，亦不能代替 Windows 原生或独立机器网络验收。

## 因子任务集成（2026-09-27）

研究页支持因子参数、Agent 执行、持久化任务、取消/重试与结果恢复；查看因子结果会切换到因子配置，任务中心区分因子与回测。中英文界面复用原主题，细节默认折叠。

本机 CTest 84 通过 / 1 Linux 专用跳过；Linux x86_64 仿真容器 85/85；全量 Playwright 19 通过 / 1 Linux 专用跳过。DMG 包内 Factor、Task Service、Backtest 的任务与结果恢复验证通过，内置 Linux 离线资源及初始化脚本。证据：build/factor-integrated-tests.log、build/factor-integrated-linux.log、build/factor-full-ui.log、build/factor-desktop-build.log。未验收 Windows 原生，未进行新的物理跨机网络测试，未替换用户安装或升级现有 Agent。详见 [因子研究](factor-research.md)。

## 数据发布任务（2026-09-27）

Terminal 数据插件支持 CSV 快照发布、任务状态、取消/重试、选择已发布版本和开始研究；载入后收起导入表单，保留结果与主要操作。删除源文件、终止 Terminal 与研究服务重启后，完整数据与来源仍可恢复并用于因子计算。来源文件名、合约和状态直接显示，技术标识在详情。

macOS 原生 CTest 91 通过 / 1 Linux 专用跳过；Linux x86_64 仿真容器 92/92；全量 UI 20 通过 / 1 平台专用跳过，最后列表细节复测通过。新 DMG 验证本机程序与内置 Linux 离线资源、签名和包内研究/数据恢复链路。证据 build/data-integrated-tests.log、build/data-integrated-linux.log、build/data-full-ui.log、build/data-label-ui.log、build/data-desktop-build.log。Windows 原生、两台物理机器网络、本轮打包后原生窗口交互未验收。范围见 [数据发布](data-publication.md)。


## 策略控制增量

交易工作区已通过真实 C++ 桥运行可信 SMA 历史模拟，授权激活时禁止手动操作，撤销后恢复。浏览器策略/原工作台 3/3、策略/中英文 4/4 通过，截图位于 `apps/terminal/test-results/strategy-running.png` 和 `strategy-revoked.png`。原生测试另外验证关闭 Terminal 后继续运行、原 CSV 删除后完成、重复交接不重复下单、策略进程离线时仍可撤销账户授权。macOS 109 通过/1 Linux 专属跳过，Linux x86_64 容器 110/110。策略程序随后已加入安装包并完成挂载 DMG 内集成验证，见 [策略分发验证](strategy-host.md#策略分发验证)；Windows 原生与物理跨机器待验收。


## 风险限额与 Agent 升级分发

macOS ARM64 安装包已重新构建，大小 78,449,058 字节，内含本机程序、Linux x86_64 服务与初始化脚本。挂载后的实际程序通过超限拒单及重启限额保持、账本恢复、TCP/mTLS、研究/数据任务恢复、自动历史策略及 CTP SDK 回环检查；镜像与签名通过并已卸载。证据 `build/risk-release-desktop.log`；Linux Release 分发前专项 3/3，证据 `build/risk-release-linux-bundle.log`。风险规则与范围见 [交易前风险](pre-trade-risk.md)。

此轮未替换已安装应用、未升级用户现有 Agent，未操作包内 Tauri 窗口。仍为 ad-hoc 签名、未公证；未生成 Windows EXE / Linux DEB，未新增物理跨机器验收。包内包含 Agent 升级实现不代表 Linux systemd 和 Windows 计划任务升级已经实机验收。


## 因子时间留出评价

研究页新增显式全样本/时间留出选择及前段成交笔数；结果分段展示有效样本数和相关性，技术详情展示区间与跨界标签剔除数量。FactorInput/FactorResult 采用版本 2，旧输入拒绝、不迁移。标签隔离、分段统计、任务恢复与篡改拒绝见 [因子研究](factor-research.md#时间顺序留出评价)。

macOS 全量 124 通过、1 Linux 专属跳过；Linux x86_64 仿真容器 125/125；因子与数据发布浏览器 3/3、中英文 3/3。证据分别为 `build/factor-holdout-full-tests.log`、`build/factor-holdout-linux.log`、`build/factor-holdout-ui-tests.log`、`build/factor-holdout-i18n-tests.log`。已检查留出结果截图。本轮没有重建安装包、操作原生 Tauri 窗口、执行 Windows 原生或物理跨机器测试。


## 动量参数比较与分发

研究页支持输入最多 32 个递增回看窗口，只在显式时间留出模式按前段成绩选择。候选使用共同样本范围，结果只暴露候选前段成绩及选中窗口的留出结果；算法、候选证据、取消和持久化边界见 [因子参数比较](factor-research.md#动量参数比较)。FactorInput / FactorResult 当前版本 3。

macOS 最终全量 129 通过、1 Linux 专属跳过；Linux 129 项全量之后，对最终 Agent 客户端修正补测 5/5。最终因子浏览器三模式 3/3，数据发布与中英文另 4 项通过，测试按提交 ID 定位本次结果。日志与最初失败原因保留在 factor-research.md，不用重跑成功覆盖原始证据。

新 macOS ARM64 DMG 已通过挂载包内程序、Linux x86_64 资源摘要、交易/研究/策略恢复、CTP SDK 回环与签名检查（`build/factor-search-desktop-build.log`）。没有原生窗口操作、Windows 原生、物理跨机验收；签名为 ad-hoc，未公证。未修改用户安装、后台服务或历史数据，未提交/推送。


## 本机 IPC 并发连接容量

已修正监听队列暂满时连接立即失败、未使用调用期限的问题。新增真实队列饱和、期限退出、12 客户端并发且帧只交付一次、非法/缺失端点测试。macOS 修复前容量两项均失败，修复后专项 8/8；全量 macOS 133 通过、1 Linux 专属跳过，Linux x86_64 仿真容器 134/134。证据：`build/ipc-capacity-before-tests.log`、`build/ipc-capacity-tests.log`、`build/ipc-capacity-full-tests.log`、`build/ipc-capacity-linux.log`。

连接建立等待不发送命令，不提供业务自动重发或降级。Windows 命名管道等待路径已实现，未原生验收；本轮未重建安装包或修改用户安装。该阶段尚未实现 Agent 慢请求隔离，后续增量见下节。


## Agent 慢连接隔离与研究 TLS 期限

Agent 已使用有界工作线程处理本机请求、TLS 握手和完整帧读取，维护与业务写入仍串行；见 [服务管理](service-management.md#有界并发接收与慢客户端)。研究服务分别设置监听轮询与握手期限，延迟认证测试修复前失败、修复后通过，见 [研究任务](research-tasks.md#tls-握手与监听轮询期限)。

最终 macOS 138 通过、1 Linux 专属跳过；Linux x86_64 仿真容器 139/139。日志 `build/agent-concurrency-final-full-tests.log`、`build/agent-concurrency-linux-final.log`；初次 Linux TLS 失败保留在 `build/agent-concurrency-linux.log`。没有 Windows 原生或物理跨机验收。

本轮 macOS ARM64 DMG 已重建，并通过挂载后的包内交易/研究/策略恢复、真实 CTP SDK 回环生命周期、签名及 Linux x86_64 服务/初始化脚本摘要检查（`build/agent-concurrency-desktop-build.log`）。镜像已卸载。包含本机 IPC 容量等待、Agent 并发接收和研究握手期限修正；未替换用户应用或 Agent，未操作原生 Tauri 窗口。仍为 ad-hoc 签名、未公证；未生成 Windows EXE / Linux DEB。


## 任务提交时间与稳定顺序

研究及数据发布列表按服务端持久化提交序号倒序展示，直接显示提交时间、在详情中显示最近更新；相同毫秒、ID 字典顺序、时钟回拨、重试或重新打开工作台均不改变提交先后。Task Service 列表自身按序号升序返回，供 Agent 优先派发较早任务。版本与恢复边界见 [研究任务](research-tasks.md#持久化提交顺序与时间)。

专项 25/25、浏览器 8/8；macOS 全量 141 通过、1 平台专属跳过，Linux x86_64 仿真容器 142/142。证据为 `build/task-chronology-final-tests.log`、`build/task-chronology-browser.log`、`build/task-chronology-full-tests.log`、`build/task-chronology-linux.log`。已检查研究/发布截图，中英文及 800 像素窗口检查通过。无原生 Tauri 窗口、Windows 或物理跨机验收；未重建安装包，上一轮 DMG 不含该增量。未修改用户安装、服务和历史数据，未提交/推送。


## 研究服务慢连接隔离

Task Service 使用独立的外部连接池与私有工作连接池，外部慢客户端不会消耗全部工作进程接收容量；请求获取状态锁后再次检查入站期限，过期工作租约先行中断，不允许迟到回报续活。接收并发不改变任务状态的串行持久化，见 [研究任务](research-tasks.md#研究服务连接容量与工作通道隔离)。

修复前两个本机阻塞测试失败，修复后研究进程/TLS 专项 11/11。最终 macOS 147 通过、1 Linux 专属跳过，Linux x86_64 仿真容器 148/148。证据：`build/research-concurrency-before.log`、`build/research-concurrency-final-tests.log`、`build/research-concurrency-full-tests.log`、`build/research-concurrency-linux.log`。没有 UI 变更，没有新的浏览器或原生窗口验收；未构建新安装包，未执行 Windows 原生或物理跨机测试。


## 历史实验配置追溯

回测和因子结果增加折叠的“实验参数”，展示 Task Service 同次查询返回的不可变输入证据，含数据范围/版本、合约规格、SMA 或因子候选/留出参数及回测资金、成本、风险限额。修改当前表单不改变历史配置，详见 [研究任务](research-tasks.md#结果的实验配置证据)。

macOS 全量 149 通过、1 Linux 专属跳过；浏览器 8/8，参数截图补验 4/4。已检查 research-evidence.png 和 factor-search-evidence.png，实际断言覆盖当前表单与原参数不同的情况；独立 Agent/研究服务恢复测试覆盖删除原 CSV 后查询完整结果证据。日志见 `build/research-evidence-full-tests.log`、`build/research-evidence-browser.log`、`build/research-evidence-browser-details.log`。

Linux x86_64 仿真容器首次全量 149/150，node_deployment 更新阶段出现 TLS stream truncated；新增操作/阶段诊断后，4 个维护/部署用例各连续 3 次通过，根因尚未确认。原始失败和复验分别保留在 `build/research-evidence-linux.log`、`build/research-evidence-linux-diagnostics.log`。未把该现象标为已修复，安装包更新暂缓。没有 Windows 原生、物理跨机或原生 Tauri 窗口验收，未改动用户安装、服务或数据，未提交/推送。

### Agent 传输阶段诊断验收

- macOS Debug 构建通过，维护与连接测试 6/6 通过（`build/agent-transport-targeted.log`）。新增断言验证截断正文记录 receive/failed、非法 Protobuf 记录 parse/rejected、正常状态回复记录 status/send/completed；日志不包含测试正文。
- Linux x86_64 隔离容器完整 CTest 150/150 通过，耗时 278.46 秒（`build/agent-transport-linux-full.log`），其中远程部署与更新 87.90 秒通过。测试失败时会附上轮转诊断文件末尾 100 条记录。
- 本轮未复现之前的间歇 TLS 截断，不能认定根因已消除；尚需验证监听超时与 TCP 接入完成相邻时的行为。未重新打包、未修改已安装应用或用户服务/数据；Windows 未在本轮执行。

### TCP 接入完成与空闲超时竞争修复

确定性测试安排空闲取消后到达的成功接入回调：原实现 1 项失败、3 项通过；修复仅对 accept 保留已成功连接，握手/读写仍严格超时。回调层测试证明此缺陷，尚不能把此前间歇 stream truncated 的所有来源归因于它。

- macOS Debug 完整 CTest：153 项通过，1 项 Linux 专属部署测试跳过，180.10 秒（`build/transport-deadline-macos-tests.log`）。
- Linux x86_64 仿真容器完整 CTest：154/154 通过，284.66 秒（`build/transport-deadline-linux-tests.log`），包含部署、上传、更新、研究恢复与心跳。
- macOS Release 构建通过（`build/transport-deadline-release-build.log`）。本轮未重新打包或安装，Windows 原生未执行。保留 Agent 阶段诊断，不以未复现证明所有间歇故障消失。

### 研究证据与传输修复的安装包验收

当前 macOS ARM64 DMG 已纳入任务提交顺序/时间、历史实验参数证据、Task Service 有界并发接收、Agent 阶段诊断和 TCP 接入空闲取消竞争修复。先用已通过 Linux 154/154 CTest 的构建重新生成 Linux x86_64 服务包，逐一校验 10 个文件与当前构建/初始化脚本摘要，再执行 `pnpm desktop:build`。

`build/research-evidence-desktop-build.log` 记录完整构建与只读挂载验收：镜像校验、包内签名、八个本机服务程序、Linux 内置资源与初始化脚本验证通过；挂载后的交易恢复/去重、研究任务及实验结果恢复、策略授权与撤销、CTP SDK 回环启动/释放通过。验证完成后镜像已卸载。

产物为 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`，大小 79030290 字节，SHA-256 `53d4941df4d46354a225651dbe00ab0d8a45862fdaf3511dcf5790ec7d28b3e9`。签名仍为 ad-hoc，未公证；本轮未操作打包后的原生窗口，未替换已安装应用或用户服务/数据，未构建 Windows EXE 与 Linux DEB。Linux 验证为 amd64 仿真容器，不能代替物理跨机器、Windows 原生或真实行情账号验收。此前偶发截断仍保留诊断追踪，不宣称排除所有来源。

### 因子滚动验证

已接入因子契约版本 4、固定训练窗口滚动选参、逐轮统计与证据、Task Service 完整结果校验、Agent 工作进程及中英文 Terminal。验收详见 [因子研究](factor-research.md#滚动验证验收)：macOS 157 通过/1 跳过、Linux 158/158、四种评价模式浏览器 4/4，英文窄窗口与返回工作台恢复 1/1；增强的 Linux TCP/mTLS 滚动任务、数据发布、源删除、程序更新与重启恢复 1/1。源码及 Release 构建已更新，本阶段没有重新打包；该阶段的 DMG 当时仍是契约版本 3；后续包验收见下文。

### 滚动验证 DMG 交付

`build/factor-rolling-desktop-build.log` 记录新版 DMG 的完整构建和只读挂载检查；新增包内版本 4 滚动任务、逐轮配置与结果恢复通过，其他交易/策略/行情回环检查通过。内置 Linux x86_64 服务与初始化脚本来自当前已验证构建。产物仍位于 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`，79096723 字节，SHA-256 `63c2076a064b9cda8f101f3be2a0b679260604ccfdc77267ec3a151c0a3eadc9`。验证后已卸载，未操作已安装应用及用户服务/数据。签名为 ad-hoc，未公证；本轮不含 Windows 原生、Linux DEB、物理跨机器或外部行情账号验收。


### 回测显式交易时段验收

BacktestInput 版本 3 支持一个明确交易日内的夜盘、日盘及休市间隔；时段与来源由用户输入并持久化，结果版本为 2。Task Service 重算完整结果，拒绝使用不同区间边界生成的结果。没有推断节假日，也没有实现跨交易日结算，详见 [研究任务](research-tasks.md#显式交易时段)。

- macOS 原生专项 25/25；完整 CTest 160 通过、1 项 Linux 专属跳过，177.13 秒。日志：`build/backtest-sessions-tests.log`、`build/backtest-sessions-full-tests.log`。
- Linux x86_64 仿真容器完整 CTest 161/161，281.80 秒，包含 TCP/mTLS 部署与更新恢复：`build/backtest-sessions-linux-tests.log`。夜盘专项是领域/研究用例；不宣称物理远端夜盘验收。
- 日盘与夜盘浏览器 2/2：`build/backtest-sessions-browser-identity.log`；英文详情与 800 像素窗口补验 1/1：`build/backtest-sessions-ui-layout-verified.log`。已检查时段表单与英文历史参数截图，修正窄侧栏日期时间输入裁切。
- 保留初次结果版本检查、日期输入及动态翻译失败日志；已修正当前契约校验和静态/动态文案边界。历史结果按任务 ID 定位，不按相同交易日选取。

本轮未执行 Windows 原生测试、外部行情账号或原生 Tauri 窗口验收；未改动已安装应用、用户服务或历史数据。安装包验证单独记录在后文。

### 显式时段 DMG 交付

`build/backtest-sessions-desktop-build.log` 记录本轮完整打包和只读挂载验收。包内回测使用输入版本 3 与结果版本 2；本机八个服务程序、Linux x86_64 内置服务与初始化脚本校验通过。挂载后的交易恢复与命令去重、研究/因子滚动/数据发布结果恢复、策略授权运行与撤销、CTP SDK 回环生命周期均通过，镜像已卸载。研究进程集成使用显式日盘时段；夜盘另由上述原生与浏览器测试验证。

产物 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`，79142292 字节，SHA-256 `598d15b97b35ee870a981107099fdb796109e3f49773915236e9575c4ea6e682`。签名仍为 ad-hoc、未公证；没有 Windows 原生或 Linux DEB 构建，没有原生 Tauri 窗口或物理远程验收。未安装到用户应用目录、未更新用户 Agent、未修改用户历史数据。


### 跨日执行前提：事件间结算与混合持仓目标

执行插件新增显式边界结算；目标减仓支持先平昨、再平今的两个独立开平委托，统一风险检查并整体提交候选状态。新增测试覆盖结算价成本续接、不同平仓费用、共享逐笔流动性、第二笔风险拒绝、子委托 ID 冲突、空 ID、重复/倒退/越界结算、活动委托与非法价格拒绝，见 [模拟交易](paper-trading.md#计划回放的结算与持仓续接机制)。

macOS 初次专项 22/22、全量 163 通过/1 Linux 专属跳过（164 项，173.99 秒）：`build/settlement-execution-tests.log`、`build/settlement-execution-full-tests.log`。随后增加空 ID 校验与子 ID 冲突用例，最终专项 23/23（`build/settlement-execution-identity-tests.log`）；没有把此前全量数量改写为最终 165 项全量验收。

本轮尚未改变单交易日回测契约、Terminal 表单或多日结果，不宣称完整跨日回测。没有重建 DMG、操作用户服务或用户历史数据；Windows 原生未执行。

Linux x86_64 仿真容器初次完整 CTest 164/164，275.74 秒（`build/settlement-execution-linux-tests.log`）；同步最终 ID 校验变更后重新构建并运行相关专项 23/23（`build/settlement-execution-linux-identity-tests.log`）。物理跨机器与 Windows 原生不在本轮验收范围。


### 多交易日回测与结算证据

BacktestInput 升级为版本 4，每日显式时段、结算价和两种来源均必填；BacktestResult 版本 3 返回逐日结算与包含结算事件的权益曲线。今昨仓和 SMA 历史续接，结算不强平，Task Service 重算完整结果。模型和未实现范围见 [研究任务](research-tasks.md#显式交易日与逐日结算)。

- 初次原生专项 29/29（`build/multiday-tests.log`），包括两日结算、平昨手续费、即使末值相同也拒绝错误中间结算、逐日证据恢复，以及无日历/结算证据时拒绝。
- macOS 完整 CTest 167 通过/1 Linux 专属跳过，168 项，182.78 秒（`build/multiday-full-tests.log`）。增强 research_agent_recovery 在关闭 Terminal、删除 CSV、停止/更新/重启研究服务后比较完整多日结果，同时验证因子滚动和数据发布恢复。
- TypeScript 与前端构建通过（`build/multiday-final-ui-build.log`）；日盘、夜盘、两日回测与四种因子模式浏览器 7/7（`build/multiday-final-browser.log`）。英文参数、800 像素窗口与任务 ID 恢复通过，已检查逐日参数及完整日期时间输入截图。

本轮未执行原生 Tauri 窗口、Windows 原生、物理远端或真实行情账号验收。本轮打包前，已有 DMG 仍是前一阶段单交易日契约，当时尚未包含此增量；不覆盖用户应用、服务或历史任务，旧契约明确拒绝且保留文件。

补充末日未平仓用例：最后一笔价格 101、显式结算价 90，账户按 90 重置持仓成本，未实现盈亏归零，结算造成的 112 回撤进入统计。macOS 更新构建后的研究/时段专项 30/30（`build/multiday-final-settlement-tests.log`），Release 构建通过（`build/multiday-release-build.log`）。此补测发生在上述 168 项全量之后，不把新增用例计入此前全量数量。

Linux x86_64 仿真容器完整 CTest 168/168（281.65 秒，`build/multiday-linux-tests.log`）；加入末日未平仓用例后更新构建，研究/时段专项 30/30（`build/multiday-linux-final-settlement.log`）。node_deployment 已用两日数据通过远程 TCP/mTLS 提交、结算证据读取、更新及重启恢复。这里的“远程”是隔离容器中的协议验证，不是物理跨机器验收。


### 多交易日 DMG 交付

`build/multiday-desktop-build.log` 记录完整构建及只读挂载检查。包内研究任务已使用输入版本 4 / 结果版本 3，关闭 Terminal、删除原 CSV、更新并重启服务后，两个交易日的结算和原输入证据完整恢复。包内交易与策略恢复、撤销、CTP SDK 回环生命周期通过；八个本机服务签名及 Linux x86_64 的 10 个资源文件摘要校验通过。镜像已卸载，未替换用户安装或更新用户 Agent。

产物 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`，79244463 字节，SHA-256 `941867c8c16053721bd27a42ed08fbafc6946af317fb8f97d9ad477e8c8b3f20`。签名为 ad-hoc，未公证；未构建 Windows EXE / Linux DEB，未操作原生 Tauri 窗口，未进行物理跨机或外部行情账号验收。


### 结算 CSV 与不可变发布基础

已实现结算数据插件、共享 SettlementDay 契约、明确时区/报价来源校验、内容版本和原始文件身份、独立 Data Pipeline CLI 发布与检查。发布内容可直接驱动已有多日回测；尚未接入结算表任务、Agent 派发和 Terminal 选择入口，见 [结算表](settlement-calendar.md)。没有 UI 变更或新的浏览器验收，也未更新安装包。

macOS 初次专项 40/40（`build/calendar-import-tests.log`）；加入发布物驱动回测的测试后，全量 174 通过/1 Linux 专属跳过，175 项，180.87 秒（`build/calendar-import-full-tests.log`）。随后将发布层的价格约束与模拟回测分离，新增负值/非整步长数据保留及回测负结算价拒绝检查，最终相关专项 42/42（`build/calendar-price-boundary-tests.log`）。此前全量不计入这个新增用例。

原始构建失败日志保留：首次包含不存在的 instrument.hpp，随后同一行多个 GoogleTest EXPECT_THROW 导致重复宏标签；已修正头文件和格式。最终构建见 `build/calendar-price-boundary-build.log`。没有修改用户文件、服务和安装；未提交或推送。Windows 原生及物理跨机不在本轮验收范围。

Linux x86_64 仿真容器完整 CTest 175/175（282.71 秒，`build/calendar-import-linux-tests.log`）；同步最终价格边界调整后重新构建，相关专项 42/42（`build/calendar-price-boundary-linux-tests.log`）。独立程序实际发布/检查、源文件删除、不可覆盖、回测复用均被覆盖。未构建新的桌面安装包；当前 DMG 仍为上一阶段多日回测版本，不包含结算表 CLI 增量。

## 结算表持久化任务与 Terminal 导入

Task Service 保存 CALENDAR_IMPORT 原始快照并重算结果；Agent 派发 Data Pipeline，支持取消、显式重试、执行令牌隔离和重启中断。Terminal 数据工作区提供导入与发布内容查看，研究任务列表只包含回测/因子。

macOS Debug 构建成功，针对性 CTest 42/42（build/calendar-tasks-tests.log）；包括真实 Agent 派发、源文件删除、Terminal 退出和服务重启后的发布结果恢复。pnpm build 成功；浏览器结算表与成交数据发布测试 2/2（build/calendar-ui-tests.log），结算表窄屏截图已检查。尚未验证本增量的 Linux/Windows 原生构建或重新生成安装包；回测选择发布结算表仍未实现。

## 回测绑定结算表发布版本

BacktestInput 版本 5 保存可空的完整 CalendarPublication，严格核对发布摘要、合约及逐日时段/价格/来源。Terminal 的日程来源选择已完成发布任务；手工模式明确解除关联，后端拒绝同时提交发布选择和手工日程。结果展示发布标识与内容版本。旧输入版本不兼容、不迁移。

macOS 构建及 pnpm build 通过。针对性 CTest 首轮 39/40：负例更换合约代码未同步交割月份，在测试构造数据版本时提前抛错；修正测试为有效的另一合约后 40/40（build/calendar-binding-recheck-tests.log）。已覆盖修改日程、不同合约、篡改来源、旧版本拒绝，以及持久化结果重开后完整发布物保持。浏览器 4/4（build/calendar-binding-ui-tests.log）：发布源文件删除、刷新后选择结算表执行回测并恢复证据，及手工日盘/夜盘/多日回测。旧失败日志保留。全量原生测试另行运行；本增量尚未经过 Linux/Windows 或安装包验证。

### 结算表绑定的全量与打包前恢复检查

macOS Debug 全量 CTest 177 通过、1 Linux 专属 node_deployment 跳过（共 178，181.37 秒；build/calendar-binding-full-tests.log）。补充真实 Agent 场景：删除原始成交/结算 CSV 后，从两种持久化发布物提交回测，更新并重启研究服务，完整回测结果及 calendar_publication 与此前一致；1/1 通过，18.95 秒（build/calendar-binding-package-test.log）。该场景也被 DMG 验证脚本复用。Linux 本轮构建/全量测试仍在运行，尚未更新安装包。

### Linux x86_64 全量验证

结算表任务与回测版本 5 已在 Ubuntu 24 x86_64 容器完成 Release 构建，全量 CTest 178/178，通过耗时 303.04 秒（build/calendar-binding-linux-tests.log）。包括 Linux node_deployment 的 TCP/mTLS 部署与恢复；仍属于容器/回环测试，不是物理跨机器或 Windows 原生验收。生成服务 ZIP 后仍需摘要核对、最新恢复脚本补测及桌面包验证。

## 结算表任务与回测绑定安装包

Linux 补充 research_agent_recovery 场景通过（build/calendar-binding-linux-recovery.log），服务 ZIP 的 10 个文件摘要与构建程序及仓库初始化脚本逐项一致。macOS Release 构建及 pnpm desktop:build 成功（build/calendar-binding-desktop-build.log）。

当前 DMG：apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg；79,503,705 字节；SHA-256 `8b40e5d5c0ceebd61c7107e3afeb2892b085a9640353b1f94eff48704eb53ec5`。包含 BacktestInput 5、结算表任务/导入/查看/回测版本绑定与 Linux x86_64 服务包、初始化脚本。

只读挂载后验证镜像校验和、应用及本机进程签名、Linux 服务与初始化资源；包内 Trading、Task Service、Backtest、Factor、Data Pipeline、Strategy 经真实独立进程恢复场景通过。结算表发布绑定回测在源文件删除与研究服务重启后完整证据一致；策略自动回放与撤销通过。CTP 厂商 SDK 工厂/回环启动/释放通过，未连接外部行情服务器。挂载已卸载，未修改已安装应用或用户数据。

当前仍为本机 ad-hoc 签名、未公证；未做真实 Tauri 窗口交互验收、Windows NSIS/Linux DEB 本轮构建或物理跨机验收。

## 共享历史回放日程规则

PaperReplaySchedule 已由多日回测实际使用，归属 Paper 执行插件。macOS Debug 构建成功，日程、研究、结算发布及 Agent 恢复定向 43/43（build/replay-schedule-tests.log，32.42 秒）。新增 3 项测试覆盖空时段、重复时刻、半开区间、乱序/越界事件、缺失交易日成交、合约不匹配、负值及非步长对齐结算价；边界负例改为单事件输入以避免先触发乱序后，复测 3/3（build/replay-schedule-edge-tests.log）。该源代码增量尚未验证 Linux/Windows，也不在刚生成的结算表版本绑定 DMG 中；策略进程的授权及日终恢复仍待接入。

## 持久化交易会话的日程边界

已实现 replay_calendar/replay_settle 类型化命令及快照，交易进程绑定完整结算表并执行 PaperReplaySchedule 边界。覆盖时段末剩余委托取消、禁止跨时段下单、日终未结算拒绝推进、绑定价格结算、相同请求恢复去重与不同请求重复日序号拒绝。

macOS Debug 构建成功；首轮相关测试 38/38（build/trading-calendar-tests.log），随后策略授权及协议/进程定向 25/25（build/trading-calendar-final-tests.log）。持仓跨日测试余额从 1000 经第一日 1058 到第二日 1208，今仓转昨仓、结算后成本 120，重试与重新打开会话无重复入账。策略时段末意图确认不创建委托，不能跳过结算。最终诊断文案修改后构建通过（build/trading-calendar-guard-build.log）。尚未接入新版 ReplayPlan/Terminal，现有策略驱动明确拒绝绑定日程账户；本源代码增量未重新打包或在 Linux/Windows 验证。

## 策略回放的结算计划与故障注入

ReplayPlan 版本 2、账户结算表一致性、结算计数和游标核对已实现；策略驱动提交确定身份的日终结算，在最后一日结算后撤销授权。macOS Debug 构建通过，针对性 12/12（build/strategy-calendar-tests.log，23.81 秒）。新增测试使用真实持久化 PaperSession/Strategy Session，在两个交易日各注入提交前失败及提交后回包丢失，共四种场景，重开两个日志后结果与无故障运行完全一致，余额 1024、费用 6，两个结算日完成、授权撤销；再次恢复无重复入账。另覆盖缺少日程、不同来源发布身份及旧计划版本拒绝。已有无日程独立进程与 Terminal 策略流程保持通过。新增日程的物理双进程/Terminal 选择入口、Linux/Windows 和打包尚未验证。

## Terminal 策略日程与手动接管

strategy.run 显式接收 calendar_task，从已完成研究任务读取发布物，校验计划后绑定账户并授权。已有相同绑定可继续设置，不同绑定拒绝。界面提供日程选择、结算进度与发布详情；交易快照给出日终待结算/时段末标志，手动接管使用绑定价格。

macOS 原生构建与 pnpm build 通过。首次控件类型检查发现 act 只接受字符串参数，修正为支持数字 day_index 后通过，原日志保留。真实 Agent/C++ 浏览器测试 2/2（build/strategy-calendar-controls-tests.log，17.6 秒）：删除结算 CSV 后由独立策略/交易程序完成两日回放，余额 1024、费用 6、结算计数 2；重启策略服务、重开账户结果完全一致；撤销另一个绑定账户的授权后，界面禁止跨日推进，按日程结算后允许推进。日程截图已检查。原生恢复与策略定向 12/12（build/strategy-calendar-terminal-tests.log，22.62 秒）。尚未验证 Linux/Windows 或重打安装包，实际进程日终提交点的故障注入仍待扩展。

## 策略日终提交后的双进程中断

macOS Debug 全量 CTest 184 通过、1 Linux 专属 node_deployment 跳过（共 185，179.75 秒；build/strategy-calendar-full-tests.log）。随后新增 strategy_calendar_process，复用生产 Strategy/Trading 程序，通过测试专用双向 TLS 转发器在首日 replay_settle 已成功提交后暂扣回复，强制终止两个进程；交易恢复快照确认 cursor=3/settled_days=1，策略重启后完成第二日，总余额 1024、费用 6、两个成交、结算计数 2；再次重启不改变结果。转发器与证书仅在隔离测试目录使用，生产代码没有故障注入开关。

首次手动测试用了不存在的 Homebrew protoc 路径，导致启动探测超时；改用 CMake/Conan 工具路径并补充工具存在性检查后通过（build/strategy-calendar-process-recheck.log）。CTest 注册后的普通/日程双进程用例 2/2（build/strategy-calendar-process-ctest.log）。旧失败日志保留。最新 Linux 全量与 macOS Release 构建另行运行，安装包尚未更新。

### 研究与策略回放一致性

新增同输入对照测试 StrategyReplay.ScheduledExecutionMatchesBacktestLedgerAndFillEconomics：用相同 TradeDataset、CalendarPublication、SMA 参数、费用和风控输入，分别运行 Backtest 与持久化 Strategy/Paper 会话，比较余额、权益、可用资金、保证金、冻结、手续费、已实现/未实现盈亏、持仓、游标及逐笔成交价格/数量/方向/开平。只忽略不同宿主各自生成的订单标识；两日结算计数一致。macOS Debug 1/1 通过（build/strategy-backtest-parity-tests.log）；已有 Linux 全量运行尚不含此新增测试，需在该运行终止后同步补验。macOS Release 初轮构建已成功，安装包尚未更新。

## 策略日程 Linux 全量验证

Ubuntu 24 x86_64 容器 Release 全量 CTest 186/186，通过耗时 287.17 秒（build/strategy-calendar-linux-tests.log）。包括日终结算回包丢失后的双进程强制终止/恢复、节点部署、心跳和行情 SDK 回环。macOS Release 的同输入回测/策略账本一致性测试也通过（build/strategy-calendar-release-parity.log）。后加入的 Linux 一致性测试正在单独补验，安装包尚未更新；仍不代表 Windows 原生、实盘或物理跨机器验收。

## 策略日程安装包交付验证

Linux 一致性与日终双进程测试补验 2/2（build/strategy-calendar-linux-parity.log），服务 ZIP 的 10 个文件摘要与当前 Linux 构建及初始化脚本逐项一致。pnpm desktop:build 成功（build/strategy-calendar-desktop-build.log）。

当前 DMG 为 apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg，79,992,749 字节，SHA-256 `85912584d9b5e87d3f4601525d2a2044a862c43a5f486ea3c815a66947c33aec`。包含交易日程绑定/持久化结算、ReplayPlan 2、策略日程选择与手动接管、最新本机与 Linux x86_64 程序及初始化脚本。

只读挂载校验镜像、签名和 Linux 资源通过，包内交易/研究/策略恢复通过。新增 CTest strategy_calendar_process 使用挂载包内的 Strategy/Trading（通过既有测试程序环境覆盖），完成日终提交后暂扣回复、强制终止双进程及后续恢复；1/1 通过，1.63 秒。CTP SDK 工厂/回环生命周期通过，未接外部行情。挂载已卸载，未修改已安装程序或用户数据。当前仍是 ad-hoc 签名、未公证，Windows NSIS/Linux DEB 和物理跨机本轮未验收。


### 合并前实时行情事件入口（2026-09-27）

CTP 插件在界面合并前保留报价与连接/订阅状态，乱序报价仅不覆盖界面最新值，事件中仍保留并标记。默认 4096 条有界保留，非破坏分页、独立流身份、溢出缺口与失败标记已通过行情宿主的 Protobuf 暴露。此入口未提供持久录制，也不把一档报价累计成交量转换成回测的逐笔成交事件。

macOS Debug 和 Linux x86_64 Release：相关 8/8 测试通过，覆盖真实行情子进程和 TCP/mTLS；供应商由仅测试 SDK 替身驱动。构建与测试日志：`build/market-events-all-build.log`、`build/market-events-integration-tests.log`、`build/market-events-linux-tests.log`。未修改安装中的应用、Agent 或用户数据；未重新构建安装包，上一节 DMG 的验收边界保持不变。

### 启动页 Agent 版本预检

本机安装中的 Terminal 启动失败，原生 UI 错误详情为 `invalid_request: unknown Protobuf field`。只读检查确认常驻 Agent 与 `/Applications/Asterion Terminal.app` 内置 Agent 摘要不同；启动页原先未执行 `node.agent.inspect`，先发送了业务初始化请求。

启动页现在在部署任何行情/研究服务前检查 Agent 程序，对 `update_available` / `recovery_required` 展示明确提示和显式升级/恢复入口。升级成功后重新执行完整启动检查；不自动覆盖程序或清理数据。旧 Agent 不认识在线升级协议时，提示需要维护更新，拒绝绕过维护协议。该修改不等于已经更新用户安装中的系统服务。

启动修复安装包已于 2026-09-27 17:00（Asia/Shanghai）生成：`apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`，80,033,625 字节，SHA256 `08871970fd089fa4bc202fd66194fa24bc93eba0fbd81d90dd28c1738fa267e6`。包含启动 Agent 预检/升级入口和合并前行情事件读取；Linux x86_64 内置服务包也已重新生成，10 个材料摘要校验通过。启动页浏览器测试 2/2，通过日志 `build/startup-agent-preflight-retest.log`；完整打包验证见 `build/startup-fix-desktop-build.log`，DMG 校验、签名、包内交易/研究/策略恢复与 CTP 厂商 SDK 回环通过，挂载已弹出。签名为本机 ad-hoc，未公证；未替换 `/Applications` 的程序或旧常驻 Agent，未完成旧 Agent 的维护更新。


### 自动准备与本机清理（最新决定）

用户明确要求删除旧本机服务及配置，已关闭 Terminal、注销并停止 `me.asterion.node-agent`，删除旧 Asterion 状态/程序目录及 Terminal 偏好、WebKit 和缓存；服务目录为空，没有业务账本。复查系统注册不存在，旧进程退出，未重建用户配置。

启动页已将空闲 Agent 更新/继续恢复纳入自动启动流程，完成后重新检查程序一致性，移除必经的升级按钮。浏览器启动测试 3/3，日志 `build/automatic-agent-startup-tests.log`。运行中业务的自动排空和恢复仍待实现，不能把本测试当作该能力验收；设计见 `docs/agent-upgrades.md`。

新 DMG 于 2026-09-27T17:10:07（Asia/Shanghai）生成，80,033,565 字节，SHA256 `b6a1d15d85c6f0d0f05b21bd38b38095833f1d1402eab7b71cc729d6fe81c080`，路径 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`。打包、签名、包内交易/研究/策略恢复和厂商 SDK 回环验证成功，日志 `build/automatic-agent-desktop-build.log`。仍为 macOS ARM64 本机 ad-hoc 签名且未公证；未替换已安装应用。

### 底栏与左下角交互

参考 `rust:presentation/panels/terminal-workspace/src/Workspace.tsx` 的底栏信息顺序，以及 `rust:presentation/workbench/src/components/ServiceStatus.tsx` 的向上展开服务详情。新组件归 Terminal 宿主 `apps/terminal/src/host/components/ServiceStatus.tsx`，使用当前 Snapshot，不引入旧 HTTP API、登录或安全状态。

底栏为服务详情入口、运行/排队任务数量、当前语言的状态读取时间与交易工作区入口。服务详情可重新读取状态或打开连接设置，支持外部点击及 Escape 关闭并恢复焦点；失联节点的缓存服务统一显示待确认。左下角提供任务中心和设置快捷操作。尚未实现的锁定、账户退出与独立窗口未添加虚假按钮。

构建通过；底栏/总览/模拟交易/终端故障恢复 5/5，策略回归在独立环境中 2/2。证据 `build/status-bar-final-tests.log`、`build/status-bar-strategy-tests.log`。首次联合测试发现并修复 host 命名空间缺少模拟交易翻译；策略套件与已有模拟会话状态冲突，因此其验收单独使用隔离 Agent。截图 `apps/terminal/test-results/status-bar-services.png` 已人工检查，为明确的测试环境数据。

该 UI 已打入 2026-09-27T17:26:33（Asia/Shanghai）生成的 macOS ARM64 DMG，80,034,427 字节，SHA256 `48f838b84550d9376b621b0ac5a5592b493d6b9d1dfc6d07faec85ee7e6305f4`，路径 `apps/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`。打包校验与包内服务验收通过（`build/status-bar-desktop-build.log`），未替换已安装应用。

### 独立设置窗口（2026-09-27）

已按 [设置窗口契约](terminal-settings.md) 实现独立窗口、重复聚焦、工作台草稿保留、分类整理及跨窗口偏好同步。macOS Tauri 编译通过，浏览器交互回归 8/8，部署与窗口回归 5/5（窗口用例重复一次）。日志 `build/settings-window-tests.log`、`build/settings-window-deployment-tests.log`。

新 macOS ARM64 DMG 于 2026-09-27T21:58:01（Asia/Shanghai）生成，80,046,963 字节，SHA256 `3d9181c53e5576ef94af386ea8098631fe161eb57d915a6edd7e6a9ce78349d8`。打包校验、内置 Linux 材料校验、交易/研究/策略恢复和厂商 CTP SDK 回环通过，日志 `build/settings-window-package.log`。本机 ad-hoc 签名，未公证，未替换已安装应用。原生多窗口交互仍待实机验证，不以浏览器测试代替三平台验收。
