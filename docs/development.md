# 开发指南

## 环境

- macOS（Apple Silicon）配 Xcode 26 及以上 Command Line Tools，或 Linux x86_64 配 GCC 13 及以上。
- Node.js 22+、pnpm 10、Python 3、uv 0.12.9+、Conan 2、CMake 3.25+、Ninja。
- clang-format 23.1.1（与 CI 一致；其他版本排版存在差异）。
- 可选：Docker 或 OrbStack，用于容器化构建 Market、Warehouse、Trading 的 Linux 安装包。Linux 下当前用户须无需 `sudo` 即可运行 `docker`（加入 `docker` 组并重新登录，生效前可用 `sg docker -c '<命令>'`）。
- Linux 桌面运行需要图形会话。Electron 沙箱依赖非特权用户命名空间，Ubuntu 24.04 起须放开 AppArmor 限制（例如 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`），禁止使用 `--no-sandbox`。具体配置视系统 AppArmor 策略而定。

首次准备或拉取更新后执行：

```sh
pnpm sync      # 同步 CTP/DuckDB 配方、前端与 Conan 依赖、Debug 构建及 Python 运行时
pnpm desktop   # 构建 Release 并启动桌面端（按平台自动同步 Release 依赖）
```

如需单独重建测试用 Python 运行时，执行 `pnpm prepare:python`。

### VS Code

打开仓库根目录并安装工作区推荐扩展。C++ 跳转与补全由 clangd 提供（系统须存在可执行 `clangd`），C/C++ 扩展负责调试。

首次运行 `pnpm sync` 会生成 `build/Debug` 编译数据库与 Protobuf 头文件。新增 C++ 源文件或修改 CMake 后，运行任务 `C++: refresh code index`。根目录 `.clangd` 指向 `build/Debug`，确保头文件与源文件按实际构建参数解析。若跳转失效，在命令面板执行 `clangd: Restart language server`；找不到该命令时，先启用 clangd 扩展并运行 `Developer: Reload Window`。

Code Spell Checker 使用根目录 `cspell.json`，项目专有名词、交易所与数据源缩写登记在 `words` 中，生成代码与第三方依赖不参与拼写检查。

Market、Warehouse 与 Trading 均从自身 `bin/` 上级路径定位安装目录，不接受 `--installation` 参数（Trading 通过 `--account` 指定账户实例）。

Linux 本地调试可选择 `Market: debug installation`、`Warehouse: debug installation` 或 `Trading: debug installation`，输入已按服务规则准备好的开发安装目录（含配置、证书与真实供应商库），并将其中 `bin/asterion_<服务>` 符号链接至本地 Debug 构建产物；切勿直接启动 `build/Debug` 中的服务程序。

真实凭据由 systemd 注入，普通 VS Code 启动无法自动获取；需要调试带真实凭据的服务时，先经 systemd 启动，再用 `C++: attach to process` 附加。隔离测试会自动准备临时安装目录与测试凭据。

### Python 运行时

- Terminal 使用一份可重定位的 CPython 3.12，锁定的依赖直接安装在其 `site-packages` 中，不建虚拟环境，也不依赖系统 Python。
- 安装包随附运行时清单（由 `scripts/desktop/runtime_manifest.py` 从 `uv.lock` 生成）：首次启动按清单下载解释器与 wheel（优先国内镜像，失败回退官方源），核对摘要后离线安装；升级时按新清单重建，失败保留原环境。
- `pnpm desktop` 会自动补齐或更新缺失、过期的 Python 运行时（锁文件、仓库路径或构建目录变动时重建，可用时直接复用，并补齐缺失清单）。开发运行时直接指向当前仓库源码，安装包则自带独立源码副本。`pnpm prepare:python` 可单独准备测试环境。
- Lab 以 `python3 -I -B -m asterion_lab` 启动，完全隔离用户本机 Python 环境。
- Trading 的策略运行时为独立环境，构建时依据 `apps/services/trading/uv.lock` 准备，随 Trading 安装包离线分发。

## 配置

默认配置与部署示例分别放在各子项目的 `config/` 目录，文件用途与参数说明见[配置文件](configuration.md)。

## 构建

| 命令 | 说明 |
| --- | --- |
| `pnpm sync` | 同步本地配方、前端与 Conan 依赖，配置并增量构建 `build/Debug`，同时确保 Python 运行时就绪 |
| `pnpm desktop` | 按需同步 Conan/CMake 并增量构建桌面 Release 产物（`asterion_terminal.node`、Python 协议与回测引擎），启动 Vite（端口 1422）与 Electron |
| `pnpm desktop --debug` | 使用 Debug 构建启动桌面端，便于 C++ 调试 |
| `pnpm desktop:check` | 构建 Release 并校验桌面链路，不打开窗口（可加 `--debug` 切换 Debug） |
| `pnpm desktop:build` | 构建并校验正式发行包，输出到 `build/desktop/`：macOS `.dmg`（签名并公证）、Linux `.deb` |
| `pnpm desktop:package-test` | 构建供 CI 下载验证的 Release 安装包，输出到 `build/desktop-test/`（macOS 采用临时签名，不公证） |
| `pnpm dev` | 仅前端开发模式：浏览器 + C++ 开发桥（端口 1423），依赖已构建的 `build/Debug` |
| `cmake --build --preset conan-debug` | 仅增量重建 C++ 目标 |
| `scripts/services/build_linux_services.sh` | 在 Ubuntu 24.04 容器中构建 `Asterion-Trading-<版本>-linux-x86_64.sh`、`Asterion-Market-<版本>-linux-x86_64.sh` 与 `Asterion-Warehouse-<版本>-linux-x86_64.sh`，输出到 `build/linux-bundles/`（支持 `ASTERION_CTP_PROFILE=prod\|eval`、`ASTERION_CTP_LOCAL_DIR` 与 `ASTERION_CTP_REQUIRE_OFFICIAL=1`，见[CTP SDK 溯源与切换](trading/deployment.md#ctp-sdk-溯源与测评版--生产版切换)） |

- 公共编译告警由各模块调用 `asterion_target_defaults(target)` 统一施加，测试目标集中登记在 `tests/CMakeLists.txt`。
- 支持平台为 macOS（Apple Silicon / arm64）与 Linux x86_64；Linux 服务基线为 Ubuntu 24.04（glibc 2.39、GCC 13.3）。
- Linux 桌面与服务构建脚本从源码编译 `cpptrace` 以启用当前 glibc 下的信号安全崩溃取栈；手动执行 `conan install` 时亦须加上 `--build='cpptrace/*'`。
- 调试运行中的桌面端时须确认 Electron 加载的原生模块目录；单独重编 C++ 不会热替换已加载模块，需重启 `pnpm desktop`。

## 协议代码生成

消息契约集中在 `protocol/proto/asterion/`，各语言绑定生成方式如下：

| 语言 | 工具与入口 | 输出位置 |
| --- | --- | --- |
| C++ | Conan 提供的 `protobuf::protoc`，CMake 构建时以 `--cpp_out` 生成 | `<构建目录>/generated/asterion/` |
| Python | 同一 `protoc`，CMake 构建时以 `--python_out` 生成 | `<构建目录>/generated-python/asterion/` |
| TypeScript | pnpm 安装的 Buf 与 `protoc-gen-es`，执行 `pnpm generate:protocol` | `apps/clients/terminal/src/generated/`（提交至 Git） |

C++ 与 Python 生成规则见 [protocol/CMakeLists.txt](../protocol/CMakeLists.txt)，TypeScript 规则见 [protocol/buf.gen.yaml](../protocol/buf.gen.yaml)。修改 `.proto` 后须重新构建并重新生成 TypeScript 类型，协议详情见[通信协议](protocol.md)。

## 代码排版

`pnpm run format` 运行 Prettier 与 clang-format（`scripts/format_cpp.py` 会先校验 clang-format 版本），`pnpm run format:check` 执行同范围检查。C/C++ 采用 4 空格缩进、100 列宽；Protobuf 与前端代码采用 2 空格缩进。C++ 不依赖保存时自动格式化，统一以 `pnpm run format` 为准。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --preset conan-debug -j 6` | C++ 单元与进程测试；`-L unit` 仅跑单元测试，`-L process` 仅跑进程测试 |
| `pnpm test:unit` | 界面单元与组件测试（Vitest） |
| `pnpm exec tsc --noEmit -p apps/clients/terminal/tsconfig.json`、`pnpm lint` | TypeScript 类型检查、ESLint 与 Ruff |
| `ASTERION_CPP_BUILD="$PWD/build/Debug" pnpm test:desktop` | Electron 宿主测试 |
| `pnpm run test:e2e:journeys` | 关键端到端链路（`@journey`） |
| `pnpm run test:e2e [spec...]` | 全部端到端测试 |

- 模块测试位于 `tests/<模块>/`，UI 组件测试紧邻对应源码放置。Lab 的 Python 测试需先运行 `pnpm prepare:python` 后才会被 CTest 登记。
- UI 组件测试通过 `src/testing/core.ts` 提供原生通道替身；`@journey` 标记端到端核心路径。
- 端到端测试必须通过 `pnpm run test:e2e` 启动：它会自动准备临时根目录、测试凭据与测试 CTP SDK；直接运行 `playwright test` 会误操作本机真实环境。首次运行前执行 `pnpm exec playwright install chromium`，运行前确保 1423 端口空闲；含 Electron 窗口的测试需具备图形会话（无显示器环境用 `xvfb-run -a`）。
- macOS 下运行 CTest 应设置 `TMPDIR=/private/tmp`，避免 Unix socket 路径超过 104 字节限制。
- CI 包含源码检查（排版、生成协议一致性、类型检查、lint、ShellCheck、文档链接检查及前端单元测试）、双平台 Release 回归与 Linux Debug Sanitizer（ASan + UBSan、TSan）。Linux Release 作业负责全量 CTest（含服务端与实盘流测试）、全量浏览器 E2E、单 CPU 并发重复检查、Linux Electron 宿主测试与交易恢复门禁，并复用同一构建打包上传 Market、Warehouse、Trading 与 Linux Terminal 安装包；macOS Release 作业仅运行跨平台核心、Lab 与 Terminal 测试（`ctest -LE service`）及 macOS Electron 宿主测试并上传 `.dmg`。Conan 缓存按平台、工具链指纹与依赖配置隔离（Linux 各作业共享同一 Release 依赖缓存）。
- CI Artifacts 直接提供五个安装文件（三个 `.sh`、一个 `.deb`、一个 `.dmg`），保持原始文件名，下载后无需二次解压。
- 真实柜台验收流程见[CTP 仿真验收](acceptance/ctp-simulation.md)。

## 本机数据目录

源码启动（`pnpm desktop`、`pnpm dev`）与安装包共用同一套本机目录：

| 目录用途 | macOS | Linux |
| --- | --- | --- |
| 程序根目录 | `~/Library/Application Support/Asterion` | `~/.local/share/asterion` |
| 界面配置 | `~/Library/Application Support/me.asterion.terminal` | `~/.config/me.asterion.terminal` |
| 策略目录 | `~/Library/Application Support/Asterion/research` | `~/.local/share/asterion/research` |
| 内置策略来源（源码启动） | 仓库 `research/` | 同左 |
| 内置策略来源（安装版） | `Asterion Terminal.app/Contents/Resources/research/` | `/opt/Asterion Terminal/resources/research/` |

- 各程序配置位于自身 `config/`，状态位于 `data/`；`terminal/data/` 保存 Market、Warehouse、Trading 连接配置与客户端证书。
- 策略目录路径固定不可更换。Lab 每次启动都会将内置策略与因子同步为只读文件：新版本自动替换、不再内置的自动清理，同名用户自建模块保持不动。
- 隔离测试通过 `ASTERION_ROOT` 指定临时根目录，同时使用独立的 `--user-data-dir`。
- 关闭开发窗口或按 Ctrl+C 仅停止本机 Lab，不影响远程 Market、Warehouse 与 Trading。

## 发行

系统共产出四个发行包：

| 发行包 | 构建入口 | 安装方式 |
| --- | --- | --- |
| Market：`Asterion-Market-<版本>-linux-x86_64.sh` | `scripts/services/build_linux_services.sh` | Market 主机执行 `sudo bash "Asterion-Market-<版本>-linux-x86_64.sh"` |
| Warehouse：`Asterion-Warehouse-<版本>-linux-x86_64.sh` | 同上 | Warehouse 主机执行 `sudo bash "Asterion-Warehouse-<版本>-linux-x86_64.sh"`，见[部署](warehouse/deployment.md) |
| Trading：`Asterion-Trading-<版本>-linux-x86_64.sh` | 同上 | Trading 主机执行 `sudo bash "Asterion-Trading-<版本>-linux-x86_64.sh"`，见[部署](trading/deployment.md#安装与账户配置) |
| Terminal：`Asterion-Terminal-<版本>-mac-arm64.dmg` / `…-linux-amd64.deb` | `pnpm desktop:build`（输出到 `build/desktop/`） | macOS 拖入“应用程序”；Linux 执行 `sudo apt install ./Asterion-Terminal-….deb` |

- 三个远程服务安装包均在构建时由各自 `scripts/setup.sh` 拼接公共 `apps/services/common/scripts/setup.sh` 并附带归档生成；交互向导、凭据加密、证书生成、systemd 单元与重启调度集中在公共脚本，三台主机的运维体验保持一致。
- Terminal 升级即直接安装新版本，卸载仅删除应用（Linux 用 `sudo apt remove asterion-terminal`）；用户数据在首次启动时创建于程序根目录，卸载时保留。
- **macOS**：`pnpm desktop:build` 要求本机钥匙串具备唯一 Developer ID Application 证书与 `notarytool` 公证 profile（`CSC_NAME`、`APPLE_KEYCHAIN_PROFILE`），应用本体与 `.dmg` 均完成签名与公证。
- **Linux**：`.deb` 运行时依赖在打包期由 `dpkg-shlibdeps` 从二进制自动推导并交由 `apt` 安装，因此构建机系统版本决定可安装的目标发行版。
- 桌面原生资源清单唯一维护在 `scripts/desktop/native-resources.json`。

## 运行诊断

- 服务进程忽略 `SIGPIPE`，收到 `SIGINT` / `SIGTERM` 后执行优雅停机。
- 各程序将日志写入安装目录 `logs/`，按程序名与日期滚动，详见[运行日志](services.md#运行日志)。Terminal 日志记录每条命令名称、`trace_id`、耗时与结果（失败行附错误码），服务端以同一 `trace_id` 串联日志；交易日志在委托落盘后记录 `order_id` 等标识。
- 日志写盘失败时向标准错误输出固定 `logging.failed` 诊断，Terminal 通知中心同步上报故障。
- 本机日志打开方式见[运行日志](services.md#运行日志)。

## 产品版本

根目录 `VERSION` 是唯一产品版本来源，四个发行包共用该版本号。CMake 据此生成 C++ 版本信息与 `product.json`，桌面打包与 Lab 运行时直接读取生成产物，Conan 直接读取 `VERSION`。每次构建自动注入 Git 短提交号，供服务 `--version`、Terminal 诊断快照与 Lab 运行记录追溯，安装包文件名则保持纯产品版本号。

Lab 与 Trading 的 `pyproject.toml` 仅声明 `runtime` 依赖组，不重复维护产品名称与版本，导出统一使用 `uv export --only-group runtime`。变更产品版本时仅需修改 `VERSION` 并重新构建打包。
