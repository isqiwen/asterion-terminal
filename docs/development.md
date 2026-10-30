# 开发指南

## 环境

- macOS（Apple Silicon；不支持 Intel Mac），Xcode 26 及以上的 Command Line Tools（Apple Clang；Xcode 16 的标准库没有 `std::stop_token` 和 `std::jthread`，编译不过）；或 Linux x86_64，GCC 13 及以上
- Node.js 22+、pnpm 10、Python 3、uv 0.12.9+、Conan 2、CMake 3.25+、Ninja
- clang-format 23.1.1，即 CI 固定的版本。`pnpm run format` 和格式检查使用 `PATH` 上的 `clang-format`，其它版本的排版结果不同（21.1.8 会判定现有源码未格式化）。
- 可选：OrbStack 或 Docker（构建交易与录制主机的安装脚本）。它和上面的工具链一样由开发者自己在机器上安装，项目的脚本和任务不安装系统软件；Ubuntu 上是 `sudo apt install docker.io`。Linux 上当前用户还要能不经 `sudo` 运行 `docker`，例如 `sudo usermod -aG docker $USER`；新的组要等该用户的全部会话（包括 SSH）退出后重新登录，或重启后才生效，此前可以用 `sg docker -c '<命令>'` 运行。

Linux 桌面还需要：

- 图形会话（X11 或 Wayland）。Electron 的沙箱需要非特权用户命名空间；Ubuntu 24.04 起默认用 AppArmor 限制它，开发机需要按发行版说明放开（例如 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`），不要用 `--no-sandbox` 运行。未放开时 Electron 以“SUID sandbox helper binary … not configured correctly”退出。从 deb 安装的 VS Code 的终端和任务里启动不受这一限制：进程继承 Ubuntu 为 VS Code 提供的 AppArmor 配置，其中允许用户命名空间。
- 可选：Secret Service（GNOME Keyring、KWallet 等）及 `libsecret-1.so.0`，用于记住行情与数据源凭据。没有时 Terminal 正常运行，凭据只在本次运行期间有效。

首次准备：

```sh
conan profile detect --exist-ok   # 生成本机 Conan profile；已有时不改动
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
pnpm exec install-electron        # 显式准备锁定版本的 Electron 二进制
```

随后用 `pnpm desktop --prepare-python` 显式准备 Python 运行时并完成第一次构建与启动；不打开窗口时使用 `pnpm desktop:check --prepare-python`，两者均不要求 Docker。后续使用 `pnpm desktop` 或 `pnpm desktop:check`；已有 CMake 构建目录时，也可单独运行 `pnpm prepare:python`。ConanCenter 没有本机编译器的预编译包时，这一步会从源码编译全部依赖，之后的构建复用 Conan 缓存。

拉取更新后，若 Conan 报锁定的 `ctp/6.7.7#…` 不在缓存或远端，重新运行 `python3 scripts/prepare_ctp.py`，注册当前仓库的 CTP 和 DuckDB 本地配方；它们不由 ConanCenter 提供。前端依赖变化后运行 `pnpm install --frozen-lockfile`，Python 代码或锁定依赖变化后重新执行显式准备命令。

`build/local-profile` 存在时，桌面脚本会使用它。macOS 强制系统 Apple Clang；Linux 使用 Conan profile 中的编译器，依赖按 Release 构建，项目自身在开发模式下按 Debug 构建。

### Python 运行时

Terminal 随包携带一份可重定位的 CPython 3.12.12（python-build-standalone，即 uv 安装的版本），锁定的依赖和 Terminal 的 Python 代码直接装进它自己的 site-packages，不建虚拟环境：虚拟环境记录绝对路径，独立解释器则按自身位置找到前缀。安装版不需要系统 Python，启动和运行时都不下载任何东西。程序以 `python3 -I -B -m <模块>` 启动：不读用户环境变量和用户目录的包，也不往安装目录写字节码；导入数值库因此每次多花约 2 秒。

界面的 Protobuf 类型由 `pnpm generate:protocol` 生成（`@bufbuild/buf` 与 `protoc-gen-es`，pnpm 锁定，不需要 protoc），输出 `apps/clients/terminal/src/generated/` 并提交，不经 Prettier 与 ESLint；修改对应 proto 后重新生成。CI 的样式作业重新生成并要求没有差异。

`pnpm prepare:python` 是唯一联网的入口：按 `apps/services/lab/uv.lock` 安装 QuantStats 0.0.86、Polars 2.0.0、skfolio 1.9.0 等依赖，用 Conan 锁定的 protoc 生成 Python IPC 类型，把分析助手装为 `asterion_analytics` 模块、Python 研究服务装为 `asterion_lab` 包，结果写入 `build/electron-resources/python/`。默认使用 `build/Debug` 的 CMake 配置；可通过 `ASTERION_CPP_BUILD` 指定本 checkout 的其他构建目录。准备时删去用不到的部分（tkinter、标准库测试、头文件、pip 和依赖安装的命令行脚本），并只保留 Polars 的兼容 CPU runtime。依赖只用 wheel 安装，唯一例外是 AKShare 依赖的纯 Python 包 jsonpath（只发布源码包），按锁定哈希从源码构建。Linux 上约 890 MiB，压缩前大小，其中 AKShare 依赖的 V8 运行库约 110 MiB。

NumPy 2.3.5、SciPy 1.17.1 固定为仍提供 macOS 13 可用 wheel 的版本，macOS 准备显式指定 arm64/macOS 13 目标；Linux 使用 x86_64/manylinux 2.28 目标。不临时从源码构建 Python 依赖。

`desktop`、`desktop:check`、`desktop:build` 只检查运行时的平台与解释器，不计算源码哈希或扫描依赖文件摘要。修改分析助手、Python 依赖、analytics Protobuf 或准备脚本后，显式运行 `pnpm prepare:python` 并运行相关测试；启动检查不替代构建。Electron 与开发桥接都通过 `ASTERION_PYTHON` 指向同一解释器。打包时作为独立桌面资源携带，CI 显式准备；macOS 安装包继续核验应用完整签名。

`ctest --test-dir build/Debug -R '^(QuantStats|Portfolio)\.' --output-on-failure` 实际启动冻结助手并验证 Native/Protobuf/统计链路，须先准备产物。此处不是新增独立 CLI 客户端。

## 修改入口与验证边界

常驻业务服务及其内部任务执行在 `apps/services/`；Terminal 与安装器共用的路径、插件目录与
状态序列化在 `apps/common/control/`。Terminal 业务客户端、本机服务看护和展示快照在
`apps/clients/terminal/native/`。安装器原生执行器与 Electron
宿主统一位于 `apps/clients/installer/`，其 React 页面暂与 Terminal 共用前端工程。
完整职责见[架构](architecture.md)。

按[现行架构](architecture.md)选择修改位置：界面功能在 Terminal `features/`，应用编排在 Native，业务状态在所属服务，算法与适配放在所属服务，跨服务复用放在 `apps/services/common/`，原生数据源扩展在 `extensions/data/native/`。Core 不导入应用或扩展；Terminal 的服务看护不解释业务结果。

UI 贡献与宿主同步修改并编译。原生插件目录可选资格由 Native 统一发布；修改能力支持时同步目录/实际消费者，在 C++ 验证资格，在组件测试验证界面消费，在隔离 E2E 验证真实装配。新增动态 ABI 需要纯 C 契约及所有权验证；不要仅为内部模块建立运行时版本计数。

现行说明位于 `docs/` 和模块 README，见[文档导航](README.md)。

## 构建

CMake 按工程目录逐层 `add_subdirectory()`：根目录管理项目选项、依赖和模块组合；
修改一个模块的源码或链接关系时，编辑该模块的 `CMakeLists.txt`，不要在根目录追加
源文件或跨目录修改目标。公共告警设置由 `asterion_target_defaults(target)` 在目标
所属目录调用。测试继续集中登记在 `tests/CMakeLists.txt`。产物仍输出到对应构建目录。


CMake 目标和项目自有可执行程序、库文件统一使用下划线，例如目标 `asterion_market`
生成同名程序，`asterion_foundation` 生成 `libasterion_foundation.a`。尽量使用默认目标名，
只有 Node 原生模块 `asterion_terminal.node` 和公开插件名称等确实不同的场合设置 `OUTPUT_NAME`。
测试产物、开发桥、桌面与安装器可执行文件、分析助手遵循同一规则。第三方库文件、包标识、
系统服务登记身份和用户数据目录不随产物改名。单独构建和运行可以直接对应同一个名字：
`cmake --build build/Debug --target asterion_market`、`build/Debug/asterion_market`。

| 命令 | 说明 |
| --- | --- |
| `pnpm desktop` | Conan → CMake Debug → Node-API → Vite，启动 Electron（Vite 端口 1422） |
| `pnpm desktop:check` | 本机 Debug 构建，验证原生模块、桥接、分析资源和 Electron 脚本语法；不打开窗口 |
| `pnpm desktop:build` | Release 构建并验证发行安装包：macOS 为签名、公证的 `.dmg`，Linux 为 `.tar.gz` |
| `pnpm desktop:package-test` | Release 构建并验证带 `TEST` 标记的本机测试安装包，不作为发行产物 |
| `pnpm dev` | 仅界面开发：浏览器 + C++ 开发桥（端口 1423），需要已构建的 `build/Debug` |
| `cmake --build build/Debug` | 只重建 C++ |

三个 `desktop` 命令由 `scripts/desktop/desktop.mjs` → `scripts/desktop/desktop.py` 统一编排。`scripts/` 下按用途分组：`desktop/` 是桌面的构建、开发入口和打包校验，`services/` 是交易与录制主机安装脚本的容器构建；`prepare_ctp.py` 和 `format_cpp.py` 留在顶层。
桌面入口接受 macOS 和 Linux，三个命令在两个系统上都可用；安装包在 macOS 是 DMG，在 Linux 是图形安装器归档，只能在各自的系统上生成。交易与录制主机的安装脚本由下面的独立构建流程提供，在 Linux 开发机上同样通过容器构建，以保持 Ubuntu 24.04 的运行库基线。
Conan 与 CTP SDK 只接受 macOS armv8 和 Linux x86_64；CMake 拒绝其它系统及非 x86_64 Linux。SDK 准备入口不提供 Windows 下载清单，显式指定 `--os linux --arch x86_64` 可在 macOS 上准备 Linux 主机程序的依赖；不支持的目标在下载前拒绝。原生资源清单 `scripts/desktop/native-resources.json` 列出程序名和不带后缀的动态库名，macOS 取 `.dylib`，Linux 取 `.so`。仓库不含 Windows 专属的延迟加载、SCM 服务入口、Task Scheduler 验收分支和语法检查脚本。

排查正在运行的桌面时，先核对 Electron 的工作目录、Vite 提供的源码和加载的 `build/electron-resources/native/asterion_terminal.node`。其它 worktree 中的修复不会自动进入当前桌面；单独重编译 C++ 也不会替换已加载的模块。应用原生修复后从对应目录通过 `pnpm desktop` 正常重启。当前构建与验证记录。

## 编辑器（VS Code）

仓库的 `.vscode/` 在 macOS 和 Linux 上通用：

- **扩展**：打开仓库时会推荐 clangd、C/C++（只用作调试器）、CMake Tools、Prettier、ESLint 和 EditorConfig。Playwright 扩展被列为不推荐：它直接调用 `playwright test`，会操作本机正在使用的服务。
- **C++**：clangd 读取 `build/Debug/compile_commands.json`，这个文件由 `pnpm desktop` 或 `pnpm desktop:check` 的配置步骤生成，所以要先跑一次其中之一。C++ 不在保存时自动格式化（编辑器自带的 clang-format 版本可能与 CI 固定的版本不同），以 `pnpm run format` 为准；TypeScript、JavaScript 和 CSS 在保存时用仓库锁定的 Prettier 格式化。
- **日常任务**（“终端 → 运行任务”）：准备依赖（`Prepare dependencies`）、运行桌面（`Desktop: run`）、检查桌面（`Desktop: check`，默认构建任务）、检查代码（`Check: types, lint, format`）、C++ 测试（`C++: test`，默认测试任务）、UI 测试（`UI: unit tests`）和格式化（`Format`）。命令在两个系统上相同。新机器先运行 `Prepare dependencies`（等同上文“首次准备”的四条命令），再用 `--prepare-python` 完成首次桌面运行或检查。
- **安装包任务**：只提供 `Installer: build release`，从工作区根目录调用 `pnpm desktop:build`，输出至 `build/desktop/`。匹配的 Linux 服务包直接复用，缺失或过期时由打包流程调用 Docker／OrbStack 构建，不需要另选服务构建任务。测试打包命令 `pnpm desktop:package-test` 留给专项验收及 CI，不放入日常任务列表；打包不会自动上传或发布。
- **专项命令**：浏览器开发、E2E、Electron 宿主测试从仓库根目录终端运行；具体命令见本指南对应章节。
- **调试**：C++ 调试在 Linux 用 gdb、在 macOS 用 lldb。在 CMake Tools 状态栏选择启动目标后，“C++: debug test” 会询问 GoogleTest 过滤条件并带上测试节点容量；还可以附加到运行中的服务，或附加到 `pnpm desktop` 的 Electron 主进程调试原生模块。Linux 上附加到不是调试器启动的进程需要 `kernel.yama.ptrace_scope=0`。“UI: debug in Chrome” 用于在浏览器入口调试界面，先在仓库根目录终端运行 `pnpm dev`。

CMake Tools 使用 Conan 生成的 preset，同样要先跑过一次 `pnpm desktop` 或 `pnpm desktop:check`。只需增量编译 C++ 时，通过 CMake Tools 选择 `conan-debug` 构建 preset 后执行构建，或在终端运行 `cmake --build --preset conan-debug`。

## 代码排版

`.clang-format` 是 C/C++ 与 Protobuf 的排版来源，使用 CI 固定的 clang-format 23.1.1。
`scripts/format_cpp.py` 在写入前核对工具版本，收集 Git 跟踪的文件和未被忽略的新文件；
覆盖 `.c/.cpp/.h/.hpp/.proto`，不处理构建目录、生成代码或下载的依赖。

- C/C++ 使用 4 空格缩进、100 列行宽；命名空间不增加缩进，`public/private` 与类型对齐。
- 函数、类、结构体和枚举的左花括号独立成行；控制语句和 lambda 的左花括号跟随语句。
  非空函数、lambda、条件和循环展开；函数定义和类型定义之间自动保留空行。
- 长调用/声明换行后每个参数一行，使用固定续行缩进；构造初始化列表逐项排列。
- 指针和引用跟随类型；换行的运算符放在下一行开头。保留 include/using 顺序、字符串内容和注释段落。
- Protobuf 保持 2 空格，展开消息/枚举字段；前端继续使用锁定的 Prettier 和 2 空格。

`pnpm run format` 同时执行 Prettier 与 clang-format，`pnpm run format:check` 检查同一范围，
CI 和编辑器检查任务统一调用该入口。只检查 C/C++/Protobuf 可运行
`python3 scripts/format_cpp.py --check`。`.editorconfig` 和 VS Code 的 C/C++ 缩进设置与此一致。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --preset conan-debug -j 6` | C++ 单元测试与进程集成测试（约 10 分钟；更高并发会因负载超时） |
| `pnpm test:unit` | 界面的单元测试与组件测试（Vitest），几秒钟，不需要构建和服务 |
| `ASTERION_CPP_BUILD="$PWD/build/Debug" pnpm test:desktop` | Electron 宿主与行情深度专项测试，使用本 checkout 的 Debug 程序 |
| `pnpm run test:e2e:journeys` | 端到端的关键链路，日常改动后跑这一档 |
| `pnpm run test:e2e [spec...]` | 全部端到端测试，发布前和 CI 跑 |
| `pnpm run lint` | ESLint |
| `pnpm run format:check` | 前端与 C/C++/Protobuf 格式检查 |
| `npx tsc --noEmit -p apps/clients/terminal/tsconfig.json` | 类型检查 |

`tests/` 按被测模块分目录，C++ 测试、Python 进程测试和它们的夹具放在一起，由 `tests/CMakeLists.txt` 统一登记：

| 目录 | 内容 |
| --- | --- |
| `core/` | 基础层、内核、领域和协议的单元测试 |
| `market/` | CTP 行情与合约目录、行情服务，行情 SDK 替身 |
| `trading/` | 实盘会话、CTP 交易、风控限额、交易服务，交易 SDK 替身 |
| `data/` | 历史数据源、存储、数据服务，数据集种子 |
| `tasks/` | 研究服务、回测与因子引擎、内部执行池 |
| `node/` | 各服务共同遵守的约定：命令行、配置缺失、正常停止；TLS 测试证书 |
| `terminal/` | 终端原生层：命令、凭据、插件管理 |
| `desktop/` | Electron 宿主、安装包及其内容校验 |
| `support/` | 不属于某个模块的公用部分：隔离节点运行器、E2E 包装器、计时与监听辅助、示例 C 插件 |
| `acceptance/` | 需要手动运行、接触真实系统（系统服务、sshd、CTP 柜台、在线数据源）的验收探针，CTest 不运行 |

夹具跟着它所替代的东西所在的模块放，其它模块按路径引用（C++ 写 `#include "data/bar_fixture.hpp"`）。测试程序和产品程序一样生成在构建目录根下。服务包不带测试构建，`tests/` 下的改动不影响服务源码指纹。


测试分三层，新用例放在能证明它的最低一层：

- **C++ 测试**是主体：定价、撮合、费用、保证金、风控、账本和服务间协议都在这里验证。
- **界面单元与组件测试**放在源码旁（`*.test.ts(x)`），验证“界面拿到某个回复后怎么做”：步骤、提示、重试、草稿、语言。组件测试用 `src/testing/core.ts` 顶替界面访问 C++ 核心的唯一通道，每个回复由用例自己给出；`readySnapshot()` 是一份带类型的“全部就绪”快照，契约变化时由类型检查指出。它不模拟核心的行为，核心做了什么仍由 C++ 测试和端到端测试证明。
- **端到端测试**只保留必须穿过真实核心和服务才能证明的行为。其中标记 `@journey` 的是每项能力一条的完整链路（行情、下单与授权、交易恢复、下载到回测、回测、因子、数据集持久化、C++ 与界面的类型契约）；`e2e/snapshot-contract.spec.ts` 保证真实核心的输出符合组件测试所依赖的类型。新增端到端用例前先确认它不能写成组件测试；每个用例文件都要重新部署一套隔离服务，在 Linux 的 Debug 构建上约 6 秒。

端到端测试必须用 `pnpm run test:e2e`（或它的 `test:e2e:journeys`）。首次运行或升级 Playwright 后先执行 `pnpm exec playwright install chromium`，准备与锁定版本对应的测试浏览器；缺少时全部用例在启动浏览器时失败。其中两个用例和 `pnpm test:desktop` 会启动真实的 Electron 窗口，Linux 上需要图形会话（无显示器的机器用 `xvfb-run -a`）。每个用例文件用一个新的隔离根目录，服务程序以符号链接指向构建目录，换到下一个用例文件时上一个即被删除。macOS 上 CTest 统一使用 `TMPDIR=/private/tmp`，因为 Unix socket 路径不能超过 104 字节。从没有图形会话的终端（例如 SSH）运行时，`DISPLAY` 为空，那两个 Electron 用例和 `pnpm test:desktop` 会因“Missing X server”失败，需要指向一个可用的显示。它通过 `tests/support/e2e.py` 使用临时根目录、测试钥匙串和测试 CTP SDK；直接运行 `playwright test` 会连接并修改你本机正在使用的服务。运行前确认没有其他进程占用 1423 端口。

`ctp_sdk_smoke` 要求真实行情和交易 SDK 同时存在，缺失明确失败。交易 SDK 检查版本与 ABI，通过本机临时回环监听器验证连接、断开、重新创建和释放；安装包验证显式使用包内的两套库。测试只使用临时状态，不提供柜台协议应答，不等于真实认证、成交、跨日或流文件验收。 Linux CTP 库的模块级状态随服务进程保留，避免 SDK 卸载时遗失分配；每个 API 连接仍单独 `Release`，SDK 更新通过停止并重启服务生效。

需要录制界面测试时，设置 `ASTERION_E2E_VIDEO=1` 后运行 `pnpm run test:e2e`，
所有浏览器场景会保留 1440 × 900 的 WebM 录像。可用 Playwright 的 `--output`
参数指定本次结果目录，避免覆盖此前证据。Electron 窗口由独立进程创建，需同时将
`ASTERION_UI_VIDEO_DIR` 设置为绝对目录；此变量也适用于 `pnpm test:desktop`，
录像按测试脚本分目录，在窗口关闭后写完。默认不录制。

## 交易与录制主机程序包

交易服务和行情录制程序在各自的 Linux 主机上独立安装，每个是一个自解压安装脚本（见[交易](trading.md)与
[行情录制](recorder.md#录制主机)）。它们在 Ubuntu 24.04 x86_64 容器里构建（Docker 或 OrbStack，首次需编译全部依赖）：

```sh
scripts/services/build_linux_services.sh
```

脚本把当前源码打包进容器，执行 `scripts/services/linux-services-container.sh`，把 `asterion-trading-setup.sh` 与
`asterion-recorder-setup.sh` 输出到 `build/linux-bundles/`（`ASTERION_LINUX_BUNDLES` 可指定目录）。可用
`ASTERION_LINUX_IMAGE` 指定已缓存 Conan 依赖的镜像以加速。桌面构建不需要它们。

当前支持的构建/回归基线是 Ubuntu 24.04 x86_64（glibc 2.39、GCC 13.3）。程序动态依赖宿主的 glibc、`libstdc++.so.6`、
`libgcc_s.so.1` 等运行库，安装包不带这些系统库；其它发行版、旧 Ubuntu 或 musl 环境未通过兼容性验收。

## 本机服务

`pnpm desktop` 和 `pnpm dev` 默认使用开发环境；安装的 App 使用日常环境。首次开发启动是独立空环境，需要单独配置数据源和账户，不复制或迁移日常数据。

| 内容 | 安装版 | 开发版 |
| --- | --- | --- |
| 各程序的安装目录根（macOS），见[安装目录](architecture.md#安装目录) | `~/Library/Application Support/Asterion` | `~/Library/Application Support/Asterion Development` |
| 各程序的安装目录根（Linux） | `~/.local/share/asterion` | `~/.local/share/asterion-development` |
| Electron 界面配置（macOS） | `~/Library/Application Support/me.asterion.terminal` | `~/Library/Application Support/me.asterion.terminal.dev` |
| Electron 界面配置（Linux） | `~/.config/me.asterion.terminal` | `~/.config/me.asterion.terminal.dev` |

### 单机拓扑

安装包的 `native/` 资源包含全部本机业务服务。Terminal 工作台按需启动本机行情、数据与研究服务，它们都是
Terminal 的子进程（见[本机服务](services.md#本机服务)）；任务在所属服务内部执行。交易服务在交易主机上，
市场和交易功能另外使用专用测试 SDK 验证，不连接真实柜台。

### 配置与运行状态

每个程序的配置在它自己安装目录的 `config/`，状态在 `data/`；不把多个进程的配置和状态合并成一个多进程共同改写的文件。
凭据仍使用钥匙串或受保护的 TLS 身份文件，不放入配置。`<根>` 见[安装目录](architecture.md#安装目录)。

| 文件 / 位置 | 内容与拥有者 |
| --- | --- |
| `<根>/research/config/research/research.json` | 研究服务的任务并发策略；研究服务启动只读 |
| `<根>/<程序>/config/logging.json` | 该程序的日志：控制台与文件各自的开关、级别、格式，文件路径与保留天数，见[运行日志](services.md#运行日志) |
| `<根>/terminal/data/services/<服务>.json` | Terminal 保存的本机服务运行意图与插件选择，由界面操作修改；不是人工运行配置 |
| `<根>/terminal/data/plugins/`、`node-id` | 服务选用的插件副本（按摘要命名）与本机数据的稳定标识 |
| `<根>/<程序>/data/<服务>/` | 任务、行情来源及数据等业务配置和状态，由对应业务服务管理 |
| `<根>/terminal/data/` | 另有数据源与 CTP 连接、交易连接与证书、安装记录 |
| Electron 界面配置目录（上表） | 语言、窗口等界面偏好 |

仓库根目录的 `config/research.json` 和 `config/logging.json` 是维护中的默认配置。
CMake 原样复制到构建目录 `config/`，桌面打包携带在 `native/config/`。
安装器首次安装时把 `logging.json` 复制到 `terminal/config/`。Terminal 首次启动一种服务时从应用目录的 `config/` 把
`logging.json` 复制到该服务的 `config/`，研究服务另得 `research/config/research/research.json`。已有目标文件保持原文，
程序升级与普通重启不补写、不重置配置。研究服务启动通过 `--config` 读取它。默认内容为：

```json
{"task_concurrency": 1}
```

`task_concurrency`允许1–2，限制一个研究服务实例同时准备、执行和确认的任务。
修改后重启该服务生效。
未知字段明确报错并保留原文；程序不会自动重写用户配置。
日志配置在程序启动时读取，需要重启相应程序生效。配置不热更新，必需配置缺失或无效时明确报告文件路径，保留原文件。

插件确有静态默认配置时，源码放在 `extensions/data/native/<来源>/config/` 并随插件发布；部署后的
可编辑实例配置放在 `<根>/<程序>/config/<服务实例>/plugins/<插件 ID>.json`，由宿主读取并
通过插件接口传入。当前原生插件由宿主提供数据源和业务参数，没有独立的静态配置文件，
本轮不新增空文件或通用加载框架。账户风控、任务固化参数属于领域配置，不随插件默认文件覆盖；
凭据不能存进这些普通文件。`config/futures-sessions.json` 是前端构建使用的交易时段定义，
不作为服务运行配置下发。

开发窗口标题及标题栏显示“开发环境”。每次运行的本机服务套接字在私有的 `/tmp/ast-<实例>/` 下。关闭窗口或按 Ctrl+C
时 Terminal 先停研究、行情与 lab，最后停数据服务，再退出界面和 Vite；数据与服务运行意图保留，下次启动按它恢复进程；
柜台连接和交易授权不会自动恢复。Electron 被强制结束时，各服务观察到 Terminal 进程消失后自行退出。钥匙串账户按数据源配置目录的摘要区分，两个环境相同连接名称不会读取或覆盖对方凭据。行情密码和授权码可由用户明确选择保存在本机钥匙串，账户身份及前置地址参与隔离；交易登录密码不持久化。

开发入口不复制程序：各服务 `bin/` 下是指向当前构建目录（`build/Debug`）中程序的符号链接，重新构建后重启服务即运行新程序。

同一开发环境同时只能由一个 Terminal 管理，第二个启动失败并报告 `terminal/data/services.lock`。开发中未完成任务按服务现有中断恢复规则保留记录，不声称完成；关闭进程不会自动撤销已经发送到柜台的委托。

浏览器开发入口为 `http://127.0.0.1:1423`，其界面偏好与 Electron 分开，业务服务与开发桌面使用同一开发目录，正常关闭 Vite 也会停止本机开发服务。不要复用旧浏览器入口的连接配置。环境隔离不是权限沙箱：两个环境连接同一交易主机仍会操作相同账户。生产交易服务按稳定账户 ID 取得当前操作系统用户范围内的记录独占权，开发与安装环境共用固定归属目录；同一 ID 只能恢复其原账本。不同 ID 不被自动判定为同一外部资金账户，保证不覆盖其他用户、软件或机器；测试使用专用账户与隔离节点。

`ASTERION_ROOT` 只作为显式临时测试目录覆盖。测试同时使用独立 `--user-data-dir`；不能只改界面目录就声称业务已隔离。自动化测试继续通过隔离包装器运行。

原生插件的 ABI、独立编译、安装目录和契约测试见 [原生插件 SDK](native-plugins.md)。修改插件后需要重新构建动态库；首次启动时选择数据服务的插件集合；之后在「设置 → 插件」停止服务、保存启用清单，再启动生效。应用升级保留原插件集合；所选插件按摘要固定，内置插件随新版本出现新摘要时须在「设置 → 插件」重新选择。任何更新都发生在服务停止时，账本锁定的插件摘要、结构及领域约束校验仍生效，不迁移或改写账本。


## macOS 发行与安装验收

`pnpm desktop:build` 是正式发行入口。构建前要求本机钥匙串已有唯一匹配的 Developer ID Application 签名身份，以及通过 `xcrun notarytool store-credentials` 在本机保存的公证 profile；证书私钥、密码和 Apple 凭据不进入源码或聊天。通过 `CSC_NAME` 指定证书摘要或不含 `Developer ID Application:` 前缀的名称，通过 `APPLE_KEYCHAIN_PROFILE` 指定 profile；可用 `APPLE_KEYCHAIN` 指定所在钥匙串。缺少身份或 profile 时立即失败。凭据格式对应锁定的 [electron-builder v26 签名配置](https://www.electron.build/v26/docs/features/code-signing/)。

应用、原生服务、Node-API 模块、CTP SDK 与原生插件全部签名，安装包自身也签名。应用和 DMG 分别完成公证及票据装订，验证 Gatekeeper、签名团队、Apple Silicon 架构和完整资源集；随后从 DMG 复制到临时目录运行隔离安装验收。只有全部通过，才生成 `build/desktop/distribution-acceptance.json`，其中的 SHA-256 必须与实际 DMG 一致。失败不生成通过记录。当前保留 JIT 和禁用 library validation 权限：前者供 Electron 使用，后者允许用户显式安装的可信原生插件；插件仍是进程内可信代码。

无发行证书的 CI 用 `python3 scripts/desktop/desktop.py package-test` 生成 ad-hoc 签名的验收包，名称含 `TEST`，保存在 `build/desktop-test/`；对应校验入口是 `python3 scripts/desktop/desktop.py verify-test`。这类包仅用于隔离安装测试，不产生发行通过记录。GitHub 工作流的桌面任务依赖 style、Linux/macOS core、sanitizer 和 linux-hosts 全部成功，macOS 在 Apple Silicon runner 上构建与安装测试，Linux 在 Ubuntu 24.04 runner 上构建与安装测试；[runner 架构对应关系](https://docs.github.com/en/actions/reference/runners/github-hosted-runners)由 GitHub 定义。本机通过不能代替同提交的 CI 结果。

原生发行资源清单唯一来源为 `scripts/desktop/native-resources.json`，只包含当前程序与自带插件，两个平台的安装包共用。每次打包先验证来源，再用新目录替换生成的 native 目录；不从重复使用的构建目录搜集未知插件。Linux 资源也拒绝多余架构、文件或目录。上述操作只替换 `build/` 下的生成资源，不涉及账本、任务或用户安装的插件。

## Linux 发行与安装验收

`pnpm desktop:build` 在 Linux 上生成 `build/desktop/Asterion-Installer-<版本>-linux-x64.tar.gz`。
解压后打开 `asterion_installer`，由独立界面执行当前用户安装、升级或卸载。
构建使用 `dpkg-shlibdeps` 记录 Electron 和全部本机程序需要的系统库及最低版本，
记录随包保存于 `resources/terminal/resources/native/runtime-requirements.json`。
目标系统需事先提供这些库；安装器不提升权限，也不自动安装系统软件。
部署程序到 `~/.local/opt/asterion-terminal`，用户应用菜单入口随安装创建。
卸载必须通过维护窗口，先停止业务与后台服务，再移除应用和系统服务注册；默认保留用户资料，
仅在用户取消保留并确认删除范围后清理本机资料。
发行验收会提取内嵌 Terminal，在隔离数据目录运行桌面验收；这不等于真实安装维护验收。

无需发行验收的 CI 用 `python3 scripts/desktop/desktop.py package-test` 生成名称含 `TEST` 的包到 `build/desktop-test/`，对应校验入口是 `verify-test`。

## 运行诊断

POSIX 服务启动时显式忽略 `SIGPIPE`，使断管写入返回 `EPIPE`，避免供应商库持有的连接关闭时直接终止服务。该策略不依赖父进程的信号设置；`SIGINT` / `SIGTERM` 仍请求正常停止。回归同时检查默认信号环境中的断管和停止行为。

每个程序写入自己安装目录的 `logs/`（见[安装目录](architecture.md#安装目录)和[运行日志](services.md#运行日志)），
例如开发版 Linux 的 `~/.local/share/asterion-development/market/logs/`；Terminal 和安装器写入 `<根>/terminal/logs/`。
程序从自己的位置找到日志配置，不经环境变量。隔离测试的日志在 `<ASTERION_ROOT>/<程序>/logs/`。交易与录制主机的日志留在各自机器，不自动下载到桌面。
旧位置的日志保留原处，不自动搬迁或删除。

桌面启动错误区和服务连接详情提供“打开本机日志目录”，打开的是根目录，各程序的 `logs/` 在其下；服务连接详情同时显示完整路径。
Electron 从 Native 快照的 `diagnostics.log_directory` 获取路径，再调用系统文件管理器打开，
不接受界面传入的任意目录。开发桥界面仍可查看错误中的路径，但没有桌面文件管理器入口。

日志按程序、PID 与本地日期分文件，每行一条文本记录，格式由各程序的 `config/logging.json` 决定，见[运行日志](services.md#运行日志)。构建输出仍在启动终端，前端 `console.*` 在开发者工具；日志不替代交易账本、任务库和柜台记录。

- `terminal-<PID>_YYYY-MM-DD.log` 记录命令名称、`trace_id`、耗时和成功状态；常规状态读取仅在 debug 级别记录。每个 Terminal 实例的标识含本次进程生成的随机范围，跨重启不会复用。
- `rpc started: trace_id=… correlation_id=…` 将 `trace_id` 连接到一次独立的 `correlation_id`。服务的 `rpc completed`／`rpc failed` 行使用相同请求标识，记录操作名、稳定错误码和允许的任务/交易标识；它表示处理完成，不保证回复已经到达客户端。
- 下载、回测和因子任务用 `task_id` 与数字 `attempt` 连接服务内部的开始/完成记录。进度更新不逐次写成功日志，凭据与尝试令牌不记录。
- 交易 `journal.committed` 只在 SQLite 提交成功后记录 `account_id`、`request_id`、`order_id` 和已分配的 `broker_key`。`broker.order_observed`、`broker.trade_observed` 在服务读取柜台快照时关联委托、柜台及交易所标识。采样可能合并两次读取之间的中间回报，不能替代柜台原始成交历史；SDK 回调不因此写磁盘。
- 进程日志中 `module=transport` 记录连接处理、握手、接入和健康通道故障。重复故障在累计次数 1、2、4、8 等位置报告，空闲监听超时不报告为故障。记录只含错误码和计数，不写请求体、凭据或对端异常原文。

日志写入、刷新或日志实例创建失败时，会增加进程累计失败计数，并通过独立的 stderr 输出 `logging.failed` 固定诊断，同样按上述次数限制。通知不包含原始日志内容；如果 stderr 本身不可写，仍保留进程计数。Terminal 服务详情显示本次运行发生过的日志丢失；该提示不宣称当前仍在故障。后台状态刷新失败显示最近成功视图和待确认状态，成功刷新后恢复，累计失败次数保留在 `diagnostics`。

原生 C ABI 的 `asterion_terminal_create(char** error)` 在失败时返回空句柄，并通过可选输出返回含 `code` / `message` 的 JSON 错误；成功时清空输出。错误字符串只能由 `asterion_terminal_free` 释放，分配失败时错误指针也可能为空。Node-API 保留错误码，开发桥把同一结构写入 stderr；不存在旧的无参数创建入口。

钥匙串访问由 Terminal 的 `native/keychain_store.cpp` 管理助手进程 `asterion_keychain`：macOS 助手使用登录钥匙串，Linux 助手在运行时加载桌面的 `libsecret-1.so.0`，把凭据存入 Secret Service 的登录密钥环。助手不继承 Terminal 的环境变量，Linux 上只得到会话总线地址。Linux 会话没有可用的 Secret Service 时，Terminal 启动时的一次探测（查询一个从不保存的账户，不弹出解锁提示，最多等待 3 秒）判定凭据存储不可用，此后凭据只能保存在本次运行的内存中，勾选保存会明确失败。Secret Service 不按应用隔离，同一登录会话中的其它程序可以读取这些条目。输入、输出及退出等待共享 30 秒期限，管道使用非阻塞读写；超过 4,096 字节的响应明确拒绝。超时、断管或异常输出会终止并回收本次助手进程，后续显式操作仍可使用该接口。凭据通过匿名管道传递，不放进命令行、环境变量、临时文件或错误消息。设置或删除操作超时可能发生在钥匙串已经提交之后；本机配置不因此自动重写，也不会自动重试，应显式确认当前凭据状态再继续。

### 因子离线容量探针

`cmake --build build/Release --target asterion_factor_capacity_probe` 显式构建
`tests/tasks/factor_capacity.cpp`。它只生成与读取独立测试输入，分开报告读取/解析、校验、
对齐、算法、完整引擎和结果校验的耗时及进程峰值内存，不访问供应商或生产目录。
测量记录包含完整命令、环境、输入范围和解释限制。

上期所保证金插件的显式公网验收入口位于 `tests/acceptance/shfe_margin_probe.cpp`。
先构建 `asterion_shfe_margin_probe`，再传入绝对路径 `--plugins <build>/plugins`
和尚不存在的 `--directory <review>/provider-store`。探针通过安装的插件读取六份
官方报表，保存到独立临时仓库并输出固定证据；不注册到 CTest，不接触用户仓库。
自动回归 `ShfeMargins.*` 使用 `tests/data/fixtures/shfe_margin_reports.json` 的
缩减报表形状；它仅用于测试，不是生产数据源。

## 类型检查

`pnpm build` 的 TypeScript 阶段同时检查 `src/bridge/commands.typecheck.ts` 的正反调用示例。
UI 命令的参数和方法名称以 `src/bridge/commands.ts` 为同一来源；包装回调时保留 `CommandCall`
或 `CommandHandler`，不重新声明为 `Record<string, unknown>`。新增命令仍须实现 Native 的
输入校验，类型声明不能替代真实协议验证。

内部架构约定见 [架构文档](architecture.md)，通过模块组织和代码评审维护。
构建不附加依赖图或源码边界扫描；验证以正常编译链接及有实际行为意义的测试为准。

### 实盘观察规模测量

先构建 Release，再显式运行只读测试 SDK 夹具：

```sh
build/Release/asterion_live_tests --gtest_filter=Live.DISABLED_ObservationScaleMeasurements --gtest_also_run_disabled_tests --gtest_output=json:build/live-snapshot-measurements.json
```

该测量在 `tests/trading/`，不连接真实柜台、不提交订单；普通 CTest 默认不运行。
输出包括不同委托/成交数量与多账户下的观察、编码和 UI 投影耗时、总字节及进程 RSS 高水位。
测量口径和限制。

## 独立安装维护程序

`pnpm build` 同时构建 Terminal 与 installer 页面；构建原生目标 `asterion_maintenance` 后，
`pnpm installer` 打开独立维护窗口。开发预览如需可安装源，显式设置
`ASTERION_INSTALL_SOURCE` 指向已打包 Terminal 应用目录；不提供时只能检查与卸载既有安装，
不会把源码目录当作安装源。此入口操作正式用户安装，日常开发继续使用 `pnpm desktop`。

`pnpm desktop:build` 先生成完整 Terminal 目录，再将其嵌入 Asterion Installer；Linux 输出
`.tar.gz`（解压后运行 `asterion_installer`），macOS 输出含安装器应用的 `.dmg`。
每次发行按平台提供一个安装包，而非一个跨平台通用文件。安装包携带完整 Terminal、原生维护
执行器、本机业务服务、插件、默认配置与分析运行时。
普通用户不需要另行下载 Terminal 或服务程序。安装、升级、修复和卸载由同一安装程序处理。

从仓库根目录生成，或在 VS Code 的“终端 → 运行任务”选择对应安装包任务：

```sh
pnpm desktop:package-test  # 本机测试，不要求 macOS 发行签名身份
pnpm desktop:build         # 正式发行构建与验收；不会自动上传
```

| 产物 | Linux x86_64 | macOS Apple Silicon |
| --- | --- | --- |
| 正式发行 | `build/desktop/Asterion-Installer-<版本>-linux-x64.tar.gz` | `build/desktop/Asterion-Installer-<版本>-mac-arm64.dmg` |
| 本机测试 | `build/desktop-test/Asterion-Installer-TEST-<版本>-linux-x64.tar.gz` | `build/desktop-test/Asterion-Installer-TEST-<版本>-mac-arm64.dmg` |

只能在对应系统构建该平台安装包。`build/terminal-package/`、`build/electron-resources/` 和
`build/linux-bundles/` 是构建中间产物，不作为额外桌面安装包发布。此前评审产生的
`build/installer-*-test/` 是临时测试输出，不是标准打包入口的产物目录。

打包前须完成依赖及分析运行时准备；测试包与正式包使用相同的资源准备流程。
Linux 必须预装包中 `resources/terminal/resources/native/runtime-requirements.json` 列出的
系统运行库；安装器不申请 sudo 或自动安装系统依赖。
macOS 正式分发继续要求 Developer ID 签名和公证。`package-test` 仅用于本机测试。

安装程序、只读策略和业务数据的位置见上文[本机服务](#本机服务)。
本机程序存在未完成安装记录时，先回到安装器继续同一操作，再启动 Terminal。
安装器按登记状态进入安装、升级或维护界面：同版本提供修复／卸载，新版本提供升级／卸载，
旧安装包不能降级或修复较新版本。修复重新部署程序与桌面启动器，不回填用户配置或业务资料。Terminal 运行时安装器拒绝修改。
中断后继续必须使用登记的目标版本安装包；开发构建版本未变化时应选择修复，而非升级。
卸载默认勾选保留配置与业务数据。明确取消并确认后，删除根目录下各程序目录中的资料，
保留各程序的诊断日志、锁文件，并先删除本机保存在系统钥匙串里的凭据；交易与录制主机上的资料不受影响。卸载中断后按已登记选择继续；
安装、升级及修复没有删除资料的选项。
程序替换不提供旧格式迁移；配置与原始资料保留，不要删除业务数据来绕过格式错误。
