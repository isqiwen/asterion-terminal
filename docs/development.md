# 开发指南

## 环境

- macOS（Apple Silicon 或 Intel），Xcode 26 及以上的 Command Line Tools（Apple Clang；Xcode 16 的标准库没有 `std::stop_token` 和 `std::jthread`，编译不过）；或 Linux x86_64，GCC 13 及以上
- Node.js 22+、pnpm 10、Python 3、Conan 2、CMake 3.25+、Ninja
- 可选：OrbStack 或 Docker（重建远程 Linux 服务包）

Linux 桌面还需要：

- 可用的 systemd 用户服务管理器（`systemctl --user`）和统一 cgroup v2。本机 Agent 注册为当前用户的 systemd 单元；没有用户会话总线的纯 SSH 登录不能启动本机节点。
- 图形会话（X11 或 Wayland）。Electron 的沙箱需要非特权用户命名空间；Ubuntu 24.04 默认用 AppArmor 限制它，开发机需要按发行版说明放开（例如 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`），不要用 `--no-sandbox` 运行。
- 可选：Secret Service（GNOME Keyring、KWallet 等）及 `libsecret-1.so.0`，用于记住行情与数据源凭据。没有时 Terminal 正常运行，凭据只在本次运行期间有效。

首次准备：

```sh
conan profile detect          # 没有 Conan profile 时
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
pnpm exec install-electron   # 显式准备锁定版本的 Electron 二进制
```

`build/local-profile` 存在时，桌面脚本会使用它。macOS 强制系统 Apple Clang；Linux 使用 Conan profile 中的编译器，依赖按 Release 构建（带调试信息的内嵌 DuckDB 会超过 Agent 的程序大小上限），项目自身在开发模式下按 Debug 构建。

## 构建

| 命令 | 说明 |
| --- | --- |
| `pnpm desktop` | Conan → CMake Debug → Node-API → Vite，启动 Electron（Vite 端口 1422） |
| `pnpm desktop:check` | 同上但不打开窗口，验证原生模块与桥接 |
| `pnpm desktop:build` | Release 构建并验证发行安装包：macOS 为签名、公证的 `.dmg`，Linux 为 `.deb` |
| `pnpm dev` | 仅界面开发：浏览器 + C++ 开发桥（端口 1423），需要已构建的 `build/Debug` |
| `cmake --build build/Debug` | 只重建 C++ |

三个 `desktop` 命令由 `scripts/desktop/desktop.mjs` → `scripts/desktop/desktop.py` 统一编排。`scripts/` 下按用途分组：`desktop/` 是桌面的构建、开发入口和打包校验，`services/` 是 Linux 服务包的构建与源码指纹，`node/` 是随包分发到远程主机的初始化脚本；`prepare_ctp.py` 和 `format_cpp.py` 留在顶层。
桌面入口接受 macOS 和 Linux，三个命令在两个系统上都可用；安装包在 macOS 是 DMG，在 Linux 是 Debian 包，只能在各自的系统上生成。远程 Linux 服务包由下面的独立构建流程提供，在 Linux 开发机上同样通过容器构建，以保持 Ubuntu 24.04 的运行库基线。
Conan 与 CTP SDK 只接受 macOS armv8/x86_64 和 Linux x86_64；CMake 拒绝其它系统及非 x86_64 Linux。SDK 准备入口不提供 Windows 下载清单，显式指定 `--os linux --arch x86_64` 可在 macOS 上准备远程服务依赖；不支持的目标在下载前拒绝。原生资源清单 `scripts/desktop/native-resources.json` 列出程序名和不带后缀的动态库名，macOS 取 `.dylib`，Linux 取 `.so`。仓库不含 Windows 专属的延迟加载、SCM 服务入口、Task Scheduler 验收分支和语法检查脚本。

排查正在运行的桌面时，先核对 Electron 的工作目录、Vite 提供的源码和加载的 `build/electron-resources/native/asterion_terminal.node`。其它 worktree 中的修复不会自动进入当前桌面；单独重编译 C++ 也不会替换已加载的模块。应用原生修复后从对应目录通过 `pnpm desktop` 正常重启。当前构建与验证记录见 [架构实施记录](reviews/architecture-implementation.md)。

## 编辑器（VS Code）

仓库的 `.vscode/` 在 macOS 和 Linux 上通用：

- **扩展**：打开仓库时会推荐 clangd、C/C++（只用作调试器）、CMake Tools、Prettier、ESLint 和 EditorConfig。Playwright 扩展被列为不推荐：它直接调用 `playwright test`，会操作本机正在使用的服务。
- **C++**：clangd 读取 `build/Debug/compile_commands.json`，这个文件由 `pnpm desktop` 或 `pnpm desktop:check` 的配置步骤生成，所以要先跑一次其中之一。C++ 不在保存时自动格式化（编辑器自带的 clang-format 版本可能与 CI 固定的版本不同），以 `pnpm run format` 为准；TypeScript、JavaScript 和 CSS 在保存时用仓库锁定的 Prettier 格式化。
- **任务**（“运行任务”）：准备依赖、运行或检查桌面、打安装包、构建 C++（默认构建任务）、C++ 测试（默认测试任务）、界面单元测试、E2E（关键链路或全部）、Electron 宿主测试、界面开发服务、类型/Lint/格式检查、格式化、重建 Linux 服务包。命令在两个系统上相同。
- **调试**：C++ 调试在 Linux 用 gdb、在 macOS 用 lldb。在 CMake Tools 状态栏选择启动目标后，“C++: debug test” 会询问 GoogleTest 过滤条件并带上测试节点容量；还可以附加到运行中的服务，或附加到 `pnpm desktop` 的 Electron 主进程调试原生模块。Linux 上附加到不是调试器启动的进程需要 `kernel.yama.ptrace_scope=0`。“UI: debug in Chrome” 用于在浏览器入口调试界面，先启动“UI: dev server”任务。

CMake Tools 使用 Conan 生成的 preset，同样要先跑过一次 `pnpm desktop:check`。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --test-dir build/Debug -j 8` | C++ 单元测试与进程集成测试 |
| `pnpm test:unit` | 界面的单元测试与组件测试（Vitest），几秒钟，不需要构建和服务 |
| `pnpm run test:e2e:journeys` | 端到端的关键链路，日常改动后跑这一档 |
| `pnpm run test:e2e [spec...]` | 全部端到端测试，发布前和 CI 跑 |
| `pnpm run lint` | ESLint |
| `pnpm run format:check` / `python3 scripts/format_cpp.py --check` | 格式检查 |
| `npx tsc --noEmit -p apps/clients/terminal/tsconfig.json` | 类型检查 |

`tests/` 按被测模块分目录，C++ 测试、Python 进程测试和它们的夹具放在一起，由 `tests/CMakeLists.txt` 统一登记：

| 目录 | 内容 |
| --- | --- |
| `core/` | 基础层、内核、领域和协议的单元测试 |
| `market/` | CTP 行情与合约目录、行情服务，行情 SDK 替身 |
| `trading/` | 实盘会话、CTP 交易、风控限额、交易服务，交易 SDK 替身 |
| `data/` | 历史数据源、存储、数据服务，数据集种子 |
| `tasks/` | 任务服务、回测与因子引擎、工作进程 |
| `node/` | Agent：监督、部署、升级、SSH 主机、TLS 角色，以及各服务共同遵守的约定 |
| `terminal/` | 终端原生层：命令、凭据、插件管理 |
| `desktop/` | Electron 宿主、安装包及其内容校验 |
| `support/` | 不属于某个模块的公用部分：隔离节点运行器、E2E 包装器、计时与监听辅助、示例 C 插件 |
| `acceptance/` | 需要手动运行、接触真实系统（系统服务、sshd、CTP 柜台、在线数据源）的验收探针，CTest 不运行 |

夹具跟着它所替代的东西所在的模块放，其它模块按路径引用（C++ 写 `#include "data/bar_fixture.hpp"`）。测试程序和产品程序一样生成在构建目录根下。服务包不带测试构建，`tests/` 下的改动不影响服务源码指纹。

测试节点自己声明容量：CTest 和 `tests/support/isolated_node.py` 给每个用例设置 `ASTERION_TEST_HOST_CAPACITY=10,16384`（逻辑 CPU 数、内存 MiB），Agent 据此计算[资源准入](services.md#node-agent)而不读取宿主机，所以 3 核的 CI 机器和本机得到相同的准入结果。绕过 CTest 直接运行测试程序时需要自己设置这个变量，否则在小机器上服务会停在“等待节点容量”。它只用于测试，生产节点不设置。

测试分三层，新用例放在能证明它的最低一层：

- **C++ 测试**是主体：定价、撮合、费用、保证金、风控、账本和服务间协议都在这里验证。
- **界面单元与组件测试**放在源码旁（`*.test.ts(x)`），验证“界面拿到某个回复后怎么做”：步骤、提示、重试、草稿、语言。组件测试用 `src/testing/core.ts` 顶替界面访问 C++ 核心的唯一通道，每个回复由用例自己给出；`readySnapshot()` 是一份带类型的“全部就绪”快照，契约变化时由类型检查指出。它不模拟核心的行为，核心做了什么仍由 C++ 测试和端到端测试证明。
- **端到端测试**只保留必须穿过真实核心、Agent 和服务才能证明的行为。其中标记 `@journey` 的是每项能力一条的完整链路（行情、下单与授权、交易恢复、下载到回测、回测、因子、数据集持久化、远程部署、C++ 与界面的类型契约）；`e2e/snapshot-contract.spec.ts` 保证真实核心的输出符合组件测试所依赖的类型。新增端到端用例前先确认它不能写成组件测试；每个用例文件都要重新部署一套隔离服务，在 Linux 的 Debug 构建上约 6 秒。

端到端测试必须用 `pnpm run test:e2e`（或它的 `test:e2e:journeys`）。首次运行或升级 Playwright 后先执行 `pnpm exec playwright install chromium`，准备与锁定版本对应的测试浏览器；缺少时全部用例在启动浏览器时失败。其中两个用例和 `pnpm test:desktop` 会启动真实的 Electron 窗口，Linux 上需要图形会话（无显示器的机器用 `xvfb-run -a`）。Linux 的 Debug 程序内嵌调试信息，每个用例文件的隔离节点约占 650 MB；换到下一个用例文件时上一个节点目录即被删除，整套运行同一时间只占一个节点目录的空间。`/tmp` 空间不足时，用 `TMPDIR` 指向磁盘上的目录。从没有图形会话的终端（例如 SSH）运行时，`DISPLAY` 为空，那两个 Electron 用例和 `pnpm test:desktop` 会因“Missing X server”失败，需要指向一个可用的显示。它通过 `tests/support/ssh_e2e.py` 启动临时目录中的隔离 Agent，并使用测试 CTP SDK；直接运行 `playwright test` 会连接并修改你本机正在使用的服务。运行前确认没有其他进程占用 1423 端口。

`ctp_sdk_smoke` 要求真实行情和交易 SDK 同时存在，缺失明确失败。交易 SDK 检查版本与 ABI，通过本机临时回环监听器验证连接、断开、重新创建和释放；安装包验证显式使用包内的两套库。测试只使用临时状态，不提供柜台协议应答，不等于真实认证、成交、跨日或流文件验收。 Linux CTP 库的模块级状态随服务进程保留，避免 SDK 卸载时遗失分配；每个 API 连接仍单独 `Release`，SDK 更新通过停止并重启服务生效。

## 远程 Linux 服务包

桌面安装包内置 Linux x86_64 服务程序，用于远程部署。包的清单记录服务源码指纹（`scripts/services/service_fingerprint.py`，覆盖 `core`、`protocol`、`plugins`、`bindings`、`apps/services`、`conan`、`scripts/node` 和构建脚本）。

- `desktop:check` 和 `desktop:build` 要求 `build/linux-bundles/asterion-services-linux-x86_64.zip` 与当前源码指纹一致。
- `pnpm desktop` 在不一致时只警告，本次开发会话中远程 Linux 部署不可用。

重建（Docker 或 OrbStack，首次需编译全部依赖）：

```sh
scripts/services/build_linux_services.sh
```

脚本把当前源码打包进 Ubuntu 24.04 x86_64 容器，执行 `scripts/services/linux-services-container.sh`，输出到 `build/linux-bundles/`。可用 `ASTERION_LINUX_IMAGE` 指定已缓存 Conan 依赖的镜像以加速。

当前支持的构建/回归基线是 Ubuntu 24.04 x86_64；本次审计修复使用 glibc 2.39、GCC 13.3。服务动态依赖宿主的 glibc、`libstdc++.so.6`、`libgcc_s.so.1` 等运行库，部署包没有随包提供这些系统库。换用构建镜像可能改变 ELF 符号版本要求，源码指纹相同也不意味着运行库要求相同。其它发行版、旧 Ubuntu 或 musl 环境未通过兼容性验收。2026-10-09 的服务包（SHA-256 `4b1d6acbff6816d0cace0708adc40f38a5c29d042c27f4a1df954f476b1fa7a6`，最高要求 glibc 2.38、GLIBCXX 3.4.32）在 `ubuntu:26.04` 容器（Ubuntu 26.04.1，glibc 2.43）中完成了运行库层面的检查：全部程序与动态库依赖可解析、8 个程序可启动、Agent 可加载两个随包插件，并用包内程序通过任务恢复与厂商 CTP SDK 回环两项进程回归。这证明在更新的 Ubuntu 上可运行，不等同于下一段所说的真实主机部署验收。

部署目标还需已启动的 systemd、Python 3、OpenSSH 服务、sudo 和初始化脚本列出的管理工具。容器中的程序与部署回归验证了进程、上传及恢复流程；它不等同于真实主机上的系统服务注册、开机启动、防火墙和升级验收。发布验收须保留实际构建镜像、包摘要、ELF 依赖及目标主机环境，不能只记录 CPU 架构或源码指纹。

## 本机服务

`pnpm desktop` 和 `pnpm dev` 默认使用开发环境；安装的 App 使用日常环境。首次开发启动是独立空环境，需要单独配置数据源和账户，不复制或迁移日常数据。

| 内容 | 安装版 | 开发版 |
| --- | --- | --- |
| Agent、服务、账户、历史数据与任务（macOS） | `~/Library/Application Support/Asterion/node` | `~/Library/Application Support/Asterion Development/node` |
| Agent、服务、账户、历史数据与任务（Linux） | `~/.local/share/asterion/node` | `~/.local/share/asterion-development/node` |
| Electron 界面配置（macOS） | `~/Library/Application Support/me.asterion.terminal` | `~/Library/Application Support/me.asterion.terminal.dev` |
| Electron 界面配置（Linux） | `~/.config/me.asterion.terminal` | `~/.config/me.asterion.terminal.dev` |
| 用户服务标识 | `me.asterion.node-agent` | `me.asterion.node-agent.dev` |
| SSH 身份与远程节点记录 | `~/.asterion/nodes` | 开发 `node/enrollments` |

用户服务在 macOS 是 `~/Library/LaunchAgents/<标识>.plist` 的 launchd 任务，在 Linux 是 `~/.config/systemd/user/<标识>.service` 的 systemd 用户单元。Linux 设置了 `XDG_DATA_HOME` 时节点目录位于其下。Agent 的本机套接字目录在 `/tmp` 下，重启后不存在，Linux 单元在启动 Agent 前重新创建它。单元不设启动频率限制（systemd 会把 Terminal 主动发起的启动也计入并拒绝后续启动），Agent 异常退出后每 10 秒重试一次，与 launchd 的行为一致。

开发窗口标题及标题栏显示“开发环境”。Agent 各自持有独立随机 IPC 地址，安装版关闭窗口后继续运行。开发版关闭主窗口或按 Ctrl+C 时，核验开发 Agent 的系统服务归属后请求停止，由 Agent 先停止 worker、再停止任务与其他业务服务、最后停止数据服务，然后退出界面和 Vite；停止成功后移除开发 Agent 的登录启动注册，数据与已确认的服务运行意愿保留。退出不通过逐服务管理命令改写配置，Agent 正在初始化或需恢复也可结束。下次启动重新注册 Agent，按保存的运行意愿恢复进程；柜台连接和交易授权不会自动恢复。升级与维护只作用于当前环境。钥匙串账户按数据源配置目录的摘要区分，两个环境相同连接名称不会读取或覆盖对方凭据。行情密码和授权码可由用户明确选择保存在本机钥匙串，账户身份及前置地址参与隔离；交易登录密码不持久化。

同一开发环境同时只能由一个开发入口管理，避免退出一个入口时影响另一个。开发中未完成任务按服务现有中断恢复规则保留记录，不声称完成；关闭进程不会自动撤销已经发送到柜台的委托。停止失败会明确报错，可处理后再次关闭重试。开发启动器在 Electron 崩溃时也尝试清理；强制杀死整个启动器或系统掉电无法保证执行退出流程。

浏览器开发入口为 `http://127.0.0.1:1423`，其界面偏好与 Electron 分开，业务服务与开发桌面使用同一开发目录，正常关闭 Vite 也会停止本机开发服务。不要复用旧浏览器入口的连接配置。环境隔离不是权限沙箱：显式选择同一远程服务仍会操作相同资源。生产交易服务按稳定账户 ID 取得当前操作系统用户范围内的记录独占权，开发与安装环境共用固定归属目录；同一 ID 只能恢复其原账本。不同 ID 不被自动判定为同一外部资金账户，保证不覆盖其他用户、软件或机器；测试使用专用账户与隔离节点。

`ASTERION_NODE_DIRECTORY` 仍只作为显式临时测试目录覆盖：不注册用户服务、不执行系统 Agent 升级。测试同时使用独立 `--user-data-dir`；不能只改界面目录就声称业务已隔离。自动化测试继续通过隔离包装器运行。

开发入口首次连接本机节点前，先停止遗留的开发 Agent，由新构建的 Agent 离线同步各服务的程序、CTP 库、worker 和已选用内置插件，再更新 Agent 并启动。原运行意愿、服务绑定、账户配置与业务数据保留；相同摘要不重写服务配置。同步失败则阻止启动，并将原因写入开发节点 `logs/development-programs-*.log`。不修改远程节点或安装版服务。安装版服务仍通过“设置 → 连接与部署”停止后更新；行情登录状态不保留，需要重新登录。

原生插件的 ABI、独立编译、安装目录和契约测试见 [原生插件 SDK](native-plugins.md)。修改插件后需要重新构建动态库；首次启动时选择数据服务的插件集合；之后在「设置 → 插件」停止服务、保存启用清单，再启动生效。安装版服务程序升级保留原插件集合。开发入口保留启用的插件身份，将其中的内置插件更新到当前构建；用户安装的其他插件保留原摘要。任何更新都发生在服务停止时，账本锁定的插件和引擎版本校验仍生效，不迁移或改写账本。


## macOS 发行与安装验收

`pnpm desktop:build` 是正式发行入口。构建前要求本机钥匙串已有唯一匹配的 Developer ID Application 签名身份，以及通过 `xcrun notarytool store-credentials` 在本机保存的公证 profile；证书私钥、密码和 Apple 凭据不进入源码或聊天。通过 `CSC_NAME` 指定证书摘要或不含 `Developer ID Application:` 前缀的名称，通过 `APPLE_KEYCHAIN_PROFILE` 指定 profile；可用 `APPLE_KEYCHAIN` 指定所在钥匙串。缺少身份或 profile 时立即失败。凭据格式对应锁定的 [electron-builder v26 签名配置](https://www.electron.build/v26/docs/features/code-signing/)。

应用、原生服务、Node-API 模块、CTP SDK 与原生插件全部签名，安装包自身也签名。应用和 DMG 分别完成公证及票据装订，验证 Gatekeeper、签名团队、当前机器架构和完整资源集；随后从 DMG 复制到临时目录运行隔离安装验收。只有全部通过，才生成 `build/desktop/distribution-acceptance.json`，其中的 SHA-256 必须与实际 DMG 一致。失败不生成通过记录。当前保留 JIT 和禁用 library validation 权限：前者供 Electron 使用，后者允许用户显式安装的可信原生插件；插件仍是进程内可信代码。

无发行证书的 CI 用 `python3 scripts/desktop/desktop.py package-test` 生成 ad-hoc 签名的验收包，名称含 `TEST`，保存在 `build/desktop-test/`；对应校验入口是 `python3 scripts/desktop/desktop.py verify-test`。这类包仅用于隔离安装测试，不产生发行通过记录。GitHub 工作流的桌面任务依赖 style、Linux/macOS core、sanitizer 和 remote-linux 全部成功，macOS 分别在 Apple Silicon 与 Intel runner 上构建与安装测试，Linux 在 Ubuntu 24.04 runner 上构建与安装测试；[runner 架构对应关系](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)由 GitHub 定义。本地单架构通过不能代替另一架构或同提交的 CI 结果。

原生发行资源清单唯一来源为 `scripts/desktop/native-resources.json`，只包含当前程序与自带插件，两个平台的安装包共用。每次打包先验证来源，再用新目录替换生成的 native/remote-linux 目录；不从重复使用的构建目录搜集未知插件。Linux 资源也拒绝多余架构、文件或目录。上述操作只替换 `build/` 下的生成资源，不涉及 Agent、账本、任务或用户安装的插件。

## Linux 发行与安装验收

`pnpm desktop:build` 在 Linux 上生成 `build/desktop/Asterion-Terminal-<版本>-linux-amd64.deb`，安装到 `/opt/Asterion Terminal`，命令为 `asterion-terminal`。构建需要 `dpkg-dev`；electron-builder 首次打 Debian 包时会下载它自带的打包工具 fpm 到 `~/.cache/electron-builder`。包没有签名，也没有软件源；分发时以 `distribution-acceptance.json` 中的 SHA-256 核对文件。

包的 `Depends` 不是手写的：打包前用 `dpkg-shlibdeps` 分析 Electron 运行时和全部原生程序实际链接的库，得到包名和最低版本。因此**包能安装在哪些系统取决于构建机**：在 Debian 13 上构建的包要求不低于该系统的 glibc 与 libstdc++，在更旧的发行版上 `dpkg` 会以依赖不满足拒绝安装，而不是装上后启动失败。要支持更旧的系统，需要在那个系统上构建。只有记住凭据才需要的 `libsecret-1-0` 列为 `Recommends`。2026-10-09 在 Debian 13 上构建的包（要求 `libc6 >= 2.38`、`libstdc++6 >= 14`）在 Ubuntu 26.04.1 容器中用 `apt install` 安装成功；Ubuntu 24.04 因 `libxcomposite1 (>= 1:0.4.6)` 拒绝安装，面向 24.04 的包要在 24.04 上构建（CI 的 Linux 桌面任务即在该系统上构建）。

打包后的校验（`python3 scripts/desktop/desktop.py verify`，测试包为 `verify-test`）把包解到临时目录而不安装：核对包名、版本和架构，原生资源与清单完全一致，随包的远程 Linux 服务通过校验，重新分析解出的程序所需的依赖都已声明，各服务程序可启动，并用包内程序跑任务恢复和厂商 CTP SDK 回环两项回归。`desktop:build` 随后用解出的应用运行隔离的安装验收（`tests/desktop/electron_installer.py`），全部通过才写出 `build/desktop/distribution-acceptance.json`。

这套验收不执行 `dpkg -i`，包的安装脚本（`/usr/bin` 下的命令链接、桌面菜单项、`chrome-sandbox` 权限和 AppArmor 配置）没有被执行；在目标发行版上的真实安装需要另行验收。安装到已有旧版 `asterion-terminal` 包的机器会替换旧包的文件，旧版留下的用户服务和数据不会被处理。卸载包不会停止已注册的用户服务，也不会删除 `~/.local/share/asterion` 下的数据；需要先在 Terminal 的“设置 → 连接与部署”停止服务，或用 `systemctl --user disable --now me.asterion.node-agent.service`。

无需发行验收的 CI 用 `python3 scripts/desktop/desktop.py package-test` 生成名称含 `TEST` 的包到 `build/desktop-test/`，对应校验入口是 `verify-test`。

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

钥匙串访问由 Terminal 的 `native/keychain_store.cpp` 管理助手进程 `asterion-keychain`：macOS 助手使用登录钥匙串，Linux 助手在运行时加载桌面的 `libsecret-1.so.0`，把凭据存入 Secret Service 的登录密钥环。助手不继承 Terminal 的环境变量，Linux 上只得到会话总线地址。Linux 会话没有可用的 Secret Service 时，Terminal 启动时的一次探测（查询一个从不保存的账户，不弹出解锁提示，最多等待 3 秒）判定凭据存储不可用，此后凭据只能保存在本次运行的内存中，勾选保存会明确失败。Secret Service 不按应用隔离，同一登录会话中的其它程序可以读取这些条目。输入、输出及退出等待共享 30 秒期限，管道使用非阻塞读写；超过 4,096 字节的响应明确拒绝。超时、断管或异常输出会终止并回收本次助手进程，后续显式操作仍可使用该接口。凭据通过匿名管道传递，不放进命令行、环境变量、临时文件或错误消息。设置或删除操作超时可能发生在钥匙串已经提交之后；本机配置不因此自动重写，也不会自动重试，应显式确认当前凭据状态再继续。
