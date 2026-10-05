# 开发指南

## 环境

- macOS（Apple Silicon 或 Intel），Xcode Command Line Tools（Apple Clang）
- Node.js 22+、pnpm 10、Python 3、Conan 2、CMake 3.25+、Ninja
- 可选：OrbStack 或 Docker（重建远程 Linux 服务包）

首次准备：

```sh
conan profile detect          # 没有 Conan profile 时
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
pnpm exec install-electron   # 显式准备锁定版本的 Electron 二进制
```

`build/local-profile` 存在时，桌面脚本会使用它并强制系统 Apple Clang。

## 构建

| 命令 | 说明 |
| --- | --- |
| `pnpm desktop` | Conan → CMake Debug → Node-API → Vite，启动 Electron（Vite 端口 1422） |
| `pnpm desktop:check` | 同上但不打开窗口，验证原生模块与桥接 |
| `pnpm desktop:build` | Release 构建、Developer ID 签名、公证并验证发行 `.dmg` |
| `pnpm dev` | 仅界面开发：浏览器 + C++ 开发桥（端口 1423），需要已构建的 `build/Debug` |
| `cmake --build build/Debug` | 只重建 C++ |

三个 `desktop` 命令由 `scripts/desktop.mjs` → `scripts/desktop.py` 统一编排。
桌面入口只接受 macOS，安装包只生成 DMG；远程 Linux 服务包由下面的独立构建流程提供。
Conan 与 CTP SDK 只接受 macOS armv8/x86_64 和 Linux x86_64；CMake 拒绝其它系统及非 x86_64 Linux。SDK 准备入口不再提供 Windows 下载清单，显式指定 `--os linux --arch x86_64` 可在 macOS 上准备远程服务依赖；不支持的目标在下载前拒绝。Node-API 插件的 CMake 入口只接受 macOS。仓库已移除 Windows 专属的延迟加载、SCM 服务入口、Task Scheduler 验收分支和语法检查脚本；Core 中的可移植机制不表示支持其它桌面交付目标。

排查正在运行的桌面时，先核对 Electron 的工作目录、Vite 提供的源码和加载的 `build/electron-resources/native/asterion_terminal.node`。其它 worktree 中的修复不会自动进入当前桌面；单独重编译 C++ 也不会替换已加载的模块。应用原生修复后从对应目录通过 `pnpm desktop` 正常重启。当前构建与验证记录见 [架构实施记录](reviews/architecture-implementation.md)。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --test-dir build/Debug -j 8` | C++ 单元测试与进程集成测试 |
| `pnpm run test:e2e [spec...]` | Playwright 端到端测试 |
| `pnpm run lint` | ESLint |
| `pnpm run format:check` / `python3 scripts/format_cpp.py --check` | 格式检查 |
| `npx tsc --noEmit -p apps/clients/terminal/tsconfig.json` | 类型检查 |

端到端测试必须用 `pnpm run test:e2e`。它通过 `tests/ssh_e2e.py` 启动临时目录中的隔离 Agent，并使用测试 CTP SDK；直接运行 `playwright test` 会连接并修改你本机正在使用的服务。运行前确认没有其他进程占用 1423 端口。

`ctp_sdk_smoke` 要求真实行情和交易 SDK 同时存在，缺失明确失败。交易 SDK 检查版本与 ABI，通过本机临时回环监听器验证连接、断开、重新创建和释放；安装包验证显式使用包内的两套库。测试只使用临时状态，不提供柜台协议应答，不等于真实认证、成交、跨日或流文件验收。 Linux CTP 库的模块级状态随服务进程保留，避免 SDK 卸载时遗失分配；每个 API 连接仍单独 `Release`，SDK 更新通过停止并重启服务生效。

## 远程 Linux 服务包

桌面安装包内置 Linux x86_64 服务程序，用于远程部署。包的清单记录服务源码指纹（`scripts/service_fingerprint.py`，覆盖 `core`、`protocol`、`plugins`、`bindings`、`apps/services`、`conan`、`scripts/node` 和构建脚本）。

- `desktop:check` 和 `desktop:build` 要求 `build/linux-bundles/asterion-services-linux-x86_64.zip` 与当前源码指纹一致。
- `pnpm desktop` 在不一致时只警告，本次开发会话中远程 Linux 部署不可用。

重建（Docker 或 OrbStack，首次需编译全部依赖）：

```sh
scripts/build_linux_services.sh
```

脚本把当前源码打包进 Ubuntu 24.04 x86_64 容器，执行 `scripts/linux-services-container.sh`，输出到 `build/linux-bundles/`。可用 `ASTERION_LINUX_IMAGE` 指定已缓存 Conan 依赖的镜像以加速。

当前支持的构建/回归基线是 Ubuntu 24.04 x86_64；本次审计修复使用 glibc 2.39、GCC 13.3。服务动态依赖宿主的 glibc、`libstdc++.so.6`、`libgcc_s.so.1` 等运行库，部署包没有随包提供这些系统库。换用构建镜像可能改变 ELF 符号版本要求，源码指纹相同也不意味着运行库要求相同。其它发行版、旧 Ubuntu 或 musl 环境未通过兼容性验收。

部署目标还需已启动的 systemd、Python 3、OpenSSH 服务、sudo 和初始化脚本列出的管理工具。容器中的程序与部署回归验证了进程、上传及恢复流程；它不等同于真实主机上的系统服务注册、开机启动、防火墙和升级验收。发布验收须保留实际构建镜像、包摘要、ELF 依赖及目标主机环境，不能只记录 CPU 架构或源码指纹。

## 本机服务

`pnpm desktop` 和 `pnpm dev` 默认使用开发环境；安装的 App 使用日常环境。首次开发启动是独立空环境，需要单独配置数据源和账户，不复制或迁移日常数据。

| 内容 | 安装版 | 开发版 |
| --- | --- | --- |
| Agent、服务、账户、历史数据与任务 | `~/Library/Application Support/Asterion/node` | `~/Library/Application Support/Asterion Development/node` |
| Electron 界面配置 | `~/Library/Application Support/me.asterion.terminal` | `~/Library/Application Support/me.asterion.terminal.dev` |
| launchd 标识 | `me.asterion.node-agent` | `me.asterion.node-agent.dev` |
| SSH 身份与远程节点记录 | `~/.asterion/nodes` | 开发 `node/enrollments` |

开发窗口标题及标题栏显示“开发环境”。Agent 各自持有独立随机 IPC 地址，安装版关闭窗口后继续运行。开发版关闭主窗口或按 Ctrl+C 时，核验开发 Agent 的系统服务归属后请求停止，由 Agent 先停止 worker、再停止任务与其他业务服务、最后停止数据服务，然后退出界面和 Vite；停止成功后移除开发 Agent 的登录启动注册，数据与已确认的服务运行意愿保留。退出不通过逐服务管理命令改写配置，Agent 正在初始化或需恢复也可结束。下次启动重新注册 Agent，按保存的运行意愿恢复进程；柜台连接和交易授权不会自动恢复。升级与维护只作用于当前环境。钥匙串账户按数据源配置目录的摘要区分，两个环境相同连接名称不会读取或覆盖对方凭据。行情密码和授权码可由用户明确选择保存在本机钥匙串，账户身份及前置地址参与隔离；交易登录密码不持久化。

同一开发环境同时只能由一个开发入口管理，避免退出一个入口时影响另一个。开发中未完成任务按服务现有中断恢复规则保留记录，不声称完成；关闭进程不会自动撤销已经发送到柜台的委托。停止失败会明确报错，可处理后再次关闭重试。开发启动器在 Electron 崩溃时也尝试清理；强制杀死整个启动器或系统掉电无法保证执行退出流程。

浏览器开发入口为 `http://127.0.0.1:1423`，其界面偏好与 Electron 分开，业务服务与开发桌面使用同一开发目录，正常关闭 Vite 也会停止本机开发服务。不要复用旧浏览器入口的连接配置。环境隔离不是权限沙箱：显式选择同一远程服务仍会操作相同资源。生产交易服务按稳定账户 ID 取得当前操作系统用户范围内的记录独占权，开发与安装环境共用固定归属目录；同一 ID 只能恢复其原账本。不同 ID 不被自动判定为同一外部资金账户，保证不覆盖其他用户、软件或机器；测试使用专用账户与隔离节点。

`ASTERION_NODE_DIRECTORY` 仍只作为显式临时测试目录覆盖：不注册 launchd、不执行系统 Agent 升级。测试同时使用独立 `--user-data-dir`；不能只改界面目录就声称业务已隔离。自动化测试继续通过隔离包装器运行。

开发入口首次连接本机节点前，先停止遗留的开发 Agent，由新构建的 Agent 离线同步各服务的程序、CTP 库、worker 和已选用内置插件，再更新 Agent 并启动。原运行意愿、服务绑定、账户配置与业务数据保留；相同摘要不重写服务配置。同步失败则阻止启动，并将原因写入开发节点 `logs/development-programs-*.log`。不修改远程节点或安装版服务。安装版服务仍通过“设置 → 连接与部署”停止后更新；行情登录状态不保留，需要重新登录。

原生插件的 ABI、独立编译、安装目录和契约测试见 [原生插件 SDK](native-plugins.md)。修改插件后需要重新构建动态库；首次启动时选择数据服务的插件集合；之后在「设置 → 插件」停止服务、保存启用清单，再启动生效。安装版服务程序升级保留原插件集合。开发入口保留启用的插件身份，将其中的内置插件更新到当前构建；用户安装的其他插件保留原摘要。任何更新都发生在服务停止时，账本锁定的插件和引擎版本校验仍生效，不迁移或改写账本。


## macOS 发行与安装验收

`pnpm desktop:build` 是正式发行入口。构建前要求本机钥匙串已有唯一匹配的 Developer ID Application 签名身份，以及通过 `xcrun notarytool store-credentials` 在本机保存的公证 profile；证书私钥、密码和 Apple 凭据不进入源码或聊天。通过 `CSC_NAME` 指定证书摘要或不含 `Developer ID Application:` 前缀的名称，通过 `APPLE_KEYCHAIN_PROFILE` 指定 profile；可用 `APPLE_KEYCHAIN` 指定所在钥匙串。缺少身份或 profile 时立即失败。凭据格式对应锁定的 [electron-builder v26 签名配置](https://www.electron.build/v26/docs/features/code-signing/)。

应用、原生服务、Node-API 模块、CTP SDK 与原生插件全部签名，安装包自身也签名。应用和 DMG 分别完成公证及票据装订，验证 Gatekeeper、签名团队、当前机器架构和完整资源集；随后从 DMG 复制到临时目录运行隔离安装验收。只有全部通过，才生成 `build/desktop/distribution-acceptance.json`，其中的 SHA-256 必须与实际 DMG 一致。失败不生成通过记录。当前保留 JIT 和禁用 library validation 权限：前者供 Electron 使用，后者允许用户显式安装的可信原生插件；插件仍是进程内可信代码。

无发行证书的 CI 用 `python3 scripts/desktop.py package-test` 生成 ad-hoc 签名的验收包，名称含 `TEST`，保存在 `build/desktop-test/`；对应校验入口是 `python3 scripts/desktop.py verify-test`。这类包仅用于隔离安装测试，不产生发行通过记录。GitHub 工作流的桌面任务依赖 style、Linux/macOS core、sanitizer 和 remote-linux 全部成功，并分别在 Apple Silicon 与 Intel runner 上构建与安装测试；[runner 架构对应关系](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)由 GitHub 定义。本地单架构通过不能代替另一架构或同提交的 CI 结果。

原生发行资源清单唯一来源为 `scripts/native-resources.json`，只包含当前程序与自带插件。每次打包先验证来源，再用新目录替换生成的 native/remote-linux 目录；不从重复使用的构建目录搜集未知插件。Linux 资源也拒绝多余架构、文件或目录。上述操作只替换 `build/` 下的生成资源，不涉及 Agent、账本、任务或用户安装的插件。

## 运行诊断

POSIX 服务启动时显式忽略 `SIGPIPE`，使断管写入返回 `EPIPE`，避免供应商库持有的连接关闭时直接终止服务。该策略不依赖父进程的信号设置；`SIGINT` / `SIGTERM` 仍请求正常停止。回归同时检查默认信号环境中的断管和停止行为。

Terminal 原生日志默认位于 Electron 的 `app.getPath("logs")`，可在启动前用绝对路径 `ASTERION_LOG_DIRECTORY` 指定。Agent 及托管服务使用各自节点目录的 `logs/`。文件按本地日期轮换，保留 30 天；服务启动时未设置日志目录则不创建进程事件文件。日志是诊断证据，交易账本、任务库与柜台记录仍是各自的权威状态。

- `terminal_YYYY-MM-DD.log` 记录命令名称、`trace_id`、耗时和成功状态；常规状态读取仅在 debug 级别记录。每个 Terminal 实例的标识含本次进程生成的随机范围，跨重启不会复用。
- `requests_YYYY-MM-DD.log` 的 `rpc.started` 将 `trace_id` 连接到一次独立的 `correlation_id`。服务 `rpc.completed` 使用相同请求标识，记录操作名、成功状态、稳定错误码和允许的任务/交易标识；它表示处理完成，不保证回复已经到达客户端。
- 下载、回测和因子任务用 `task_id` 与数字 `attempt` 连接服务和 worker 的领取/完成记录。进度心跳不逐次写成功日志，凭据与尝试令牌不记录。
- 交易 `journal.committed` 只在 SQLite 提交成功后记录 `account_id`、`request_id`、`order_id` 和已分配的 `broker_key`。`broker.order_observed`、`broker.trade_observed` 在服务读取柜台快照时关联委托、柜台及交易所标识。采样可能合并两次读取之间的中间回报，不能替代柜台原始成交历史；SDK 回调不因此写磁盘。
- `transport_YYYY-MM-DD.log` 记录连接处理、握手、接入和健康通道故障。重复故障在累计次数 1、2、4、8 等位置报告，空闲监听超时不报告为故障。记录只含错误码和计数，不写请求体、凭据或对端异常原文。

日志写入、刷新或日志实例创建失败时，会增加进程累计失败计数，并通过独立的 stderr 输出 `logging.failed` 固定诊断，同样按上述次数限制。通知不包含原始日志内容；如果 stderr 本身不可写，仍保留进程计数。Terminal 服务详情显示本次运行发生过的日志丢失；该提示不宣称当前仍在故障。后台状态刷新失败显示最近成功视图和待确认状态，成功刷新后恢复，累计失败次数保留在 `diagnostics`。

原生 C ABI 的 `asterion_terminal_create(char** error)` 在失败时返回空句柄，并通过可选输出返回含 `code` / `message` 的 JSON 错误；成功时清空输出。错误字符串只能由 `asterion_terminal_free` 释放，分配失败时错误指针也可能为空。Node-API 保留错误码，开发桥把同一结构写入 stderr；不存在旧的无参数创建入口。

钥匙串访问由 Terminal 的 `native/keychain_store.cpp` 管理助手进程。输入、输出及退出等待共享 30 秒期限，管道使用非阻塞读写；超过 4,096 字节的响应明确拒绝。超时、断管或异常输出会终止并回收本次助手进程，后续显式操作仍可使用该接口。凭据通过匿名管道传递，不放进命令行、环境变量、临时文件或错误消息。设置或删除操作超时可能发生在钥匙串已经提交之后；本机配置不因此自动重写，也不会自动重试，应显式确认当前凭据状态再继续。
