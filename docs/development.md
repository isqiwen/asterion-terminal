# 开发指南

## 环境

- macOS（Apple Silicon）配 Xcode 26 及以上的 Command Line Tools，或 Linux x86_64 配 GCC 13 及以上。
- Node.js 22+、pnpm 10、Python 3、uv 0.12.9+、Conan 2、CMake 3.25+、Ninja。
- clang-format 23.1.1（与 CI 相同；其他版本排版结果不同）。
- 可选：Docker 或 OrbStack，用于构建 Market 和 Trading 主机的安装脚本。Linux 上当前用户要能不经 `sudo` 运行 `docker`（加入 `docker` 组后重新登录，此前可用 `sg docker -c '<命令>'`）。
- Linux 桌面需要图形会话。Electron 沙箱需要非特权用户命名空间：Ubuntu 24.04 起要放开 AppArmor 限制（例如 `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`），不要用 `--no-sandbox`。具体权限取决于系统的 AppArmor 配置。

首次准备：

```sh
conan profile detect --exist-ok
python3 scripts/prepare_ctp.py      # 下载并校验 CTP SDK，注册 CTP 和 DuckDB 的本地 Conan 配方
pnpm install --frozen-lockfile
pnpm exec install-electron
pnpm desktop                       # 自动准备 Python 运行时、构建并启动
```

拉取更新后：Conan 报 `ctp/…` 不在缓存时重新运行 `python3 scripts/prepare_ctp.py`；前端依赖变化后 `pnpm install --frozen-lockfile`；Lab 的 Python 代码、Protobuf 或回测引擎变化后重新运行 `pnpm desktop`。Conan 依赖变化（例如报 pybind11 找不到）时，按 `.github/workflows/ci.yml` 的 `conan install` 命令重装。

### VS Code

打开仓库根目录，安装工作区推荐扩展。C++ 跳转和补全由 clangd 提供，C/C++ 扩展用于调试；系统需要可执行的 `clangd`。

首次运行 `pnpm desktop:check` 生成编译数据库和 Protobuf 头文件。新增 C++ 源文件或修改 CMake 后，运行任务 `C++: refresh code index`。`.clangd` 从仓库根目录定位 `build/Debug`，使头文件和源文件使用实际构建参数。没有跳转时，在命令面板运行 `clangd: Restart language server`；找不到该命令时，先启用 clangd 扩展并运行 `Developer: Reload Window`。

Code Spell Checker 使用根目录的 `cspell.json`；项目名、交易所和供应商缩写放在 `words` 中，生成代码和依赖不做拼写检查。

Market 和 Trading 从启动路径的 `bin/` 上级确定安装目录，配置、库和日志共用这个目录，不接受 `--installation`。Trading 仍用 `--account` 选择账户。

Linux 本地调试选择 `Market: debug installation` 或 `Trading: debug installation`，输入已准备的开发安装目录；其中配置、证书和真实供应商库按服务安装规则准备。将该目录的 `bin/asterion_market` 或 `bin/asterion_trading` 链接到本地 Debug 构建产物，程序仍以启动链接所在目录定位配置。不要直接选择 `build/Debug` 中的服务程序运行。

真实凭据由 systemd 提供，普通 VS Code 启动不会自动获得它们；有真实凭据的服务通过 systemd 启动后，使用 `C++: attach to process` 调试。隔离测试自行准备临时安装目录和测试凭据。

### Python 运行时

- Terminal 用一份可重定位的 CPython 3.12，锁定的依赖直接装进它的 `site-packages`，不建虚拟环境，也不需要系统 Python。
- 安装包携带运行时清单（由 `scripts/desktop/runtime_manifest.py` 从 `uv.lock` 生成）：第一次启动时下载解释器和 wheel（先走国内镜像，失败退回官方源），按清单核对摘要后离线安装；升级按新清单重建，失败时保留原环境。
- `pnpm desktop` 自动准备缺失或过期的 Python 运行时；依赖锁文件、项目位置或构建目录变化时重新准备，环境可用时直接复用。开发运行时直接读取当前项目的 Python 源码，避免运行旧副本；安装包另带一份代码副本。`pnpm prepare:python` 可单独准备测试环境。
- Lab 以 `python3 -I -B -m asterion_lab` 运行，不读用户的 Python 环境。
- Trading 的策略运行时是另一份：构建时按 `apps/services/trading/uv.lock` 准备，随 Trading 安装包发布。

## 配置

各程序的默认配置和展示示例放在所属子项目的 `config/`；文件用途、运行路径和参数见[配置文件](configuration.md)。

## 构建

| 命令 | 说明 |
| --- | --- |
| `pnpm desktop` | Conan → CMake Debug → Node-API → Vite，启动 Electron（Vite 端口 1422） |
| `pnpm desktop:check` | 构建并检查桌面链路，不打开窗口 |
| `pnpm desktop:build` | Release 构建并验证 Terminal 发布包：macOS `.dmg`（签名、公证），Linux `.deb` |
| `pnpm desktop:package-test` | 本机测试包（名称含 `TEST`），不作发行用 |
| `pnpm dev` | 只开发界面：浏览器 + C++ 开发桥（端口 1423），需要已构建的 `build/Debug` |
| `cmake --build --preset conan-debug` | 只重建 C++ |
| `scripts/services/build_linux_services.sh` | 在 Ubuntu 24.04 容器里构建 `asterion-trading-setup.sh` 和 `asterion-market-setup.sh`，输出到 `build/linux-bundles/` |

- CMake 目标写在各模块自己的 `CMakeLists.txt`；目标、程序和库名用下划线（目标 `asterion_market` 生成同名程序）。
- 公共告警设置由 `asterion_target_defaults(target)` 在目标所在目录调用。测试集中登记在 `tests/CMakeLists.txt`。
- 平台只支持 macOS armv8 和 Linux x86_64。主机程序的基线是 Ubuntu 24.04（glibc 2.39、GCC 13.3）。
- 排查正在运行的桌面时，先确认 Electron 加载的是哪个目录的原生模块；单独重编 C++ 不会替换已加载的模块，要重启 `pnpm desktop`。

## 协议代码生成

消息定义集中在 `protocol/proto/asterion/`，生成文件不手工修改：

| 语言 | 工具与入口 | 输出位置 |
| --- | --- | --- |
| C++ | Conan 提供的 `protobuf::protoc`，CMake 构建时调用 `--cpp_out` | `<构建目录>/generated/asterion/` |
| Python | 同一 `protoc`，CMake 构建时调用 `--python_out` | `<构建目录>/generated-python/asterion/` |
| TypeScript | pnpm 安装的 Buf 与 `protoc-gen-es`，执行 `pnpm generate:protocol` | `apps/clients/terminal/src/generated/`，提交到 Git |

C++ 和 Python 的生成规则在 [protocol/CMakeLists.txt](../protocol/CMakeLists.txt)，TypeScript 的规则在 [buf.gen.yaml](../protocol/buf.gen.yaml)。修改 `.proto` 后重新构建并生成 TypeScript，同时更新调用方与[通信协议](protocol.md)。

## 代码排版

`pnpm run format` 运行 Prettier 和 clang-format（`scripts/format_cpp.py` 先核对版本），`pnpm run format:check` 检查同一范围。C/C++ 用 4 空格缩进、100 列；Protobuf 和前端用 2 空格。C++ 不在保存时自动格式化，以 `pnpm run format` 为准。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --preset conan-debug -j 6` | C++ 单元测试和进程测试；`-L unit` 筛选单元测试，`-L process` 筛选进程测试 |
| `pnpm test:unit` | 界面单元和组件测试（Vitest） |
| `pnpm exec tsc --noEmit -p apps/clients/terminal/tsconfig.json`、`pnpm lint` | 类型检查、ESLint |
| `ASTERION_CPP_BUILD="$PWD/build/Debug" pnpm test:desktop` | Electron 宿主测试 |
| `pnpm run test:e2e:journeys` | 关键端到端链路，日常改动后跑 |
| `pnpm run test:e2e [spec...]` | 全部端到端测试 |

- 测试按被测模块放在 `tests/<模块>/`，夹具跟着它替代的东西放。Lab 的 Python 测试要先运行 `pnpm prepare:python` 才会登记。
- 新测试放在能证明行为的最低一层：C++ 测试是主体；组件测试用 `src/testing/core.ts` 代替界面访问核心的通道；端到端只保留必须穿过真实服务的链路，标 `@journey` 的每项能力一条。
- 端到端测试只能用 `pnpm run test:e2e`：它用临时根目录、测试凭据和测试 CTP SDK；直接运行 `playwright test` 会操作本机正在使用的服务。首次运行先 `pnpm exec playwright install chromium`；运行前确认 1423 端口空闲。需要 Electron 窗口的测试要有图形会话（无显示器时用 `xvfb-run -a`）。
- macOS 上 CTest 用 `TMPDIR=/private/tmp`，因为 Unix socket 路径不能超过 104 字节。
- 主机安装包的隔离测试：`python3 tests/market/package_test.py <market 安装包>`、`python3 tests/trading/package_test.py <trading 安装包>`，在断网的 Ubuntu 24.04 容器里安装、升级并运行。
- 真实柜台的验收见 [CTP 仿真验收](acceptance/ctp-simulation.md)。

## 本机数据目录

从项目启动（`pnpm desktop`、`pnpm dev`）和安装的应用只有一套环境，用同一份数据：

| | macOS | Linux |
| --- | --- | --- |
| 程序根目录 | `~/Library/Application Support/Asterion` | `~/.local/share/asterion` |
| 界面配置 | `~/Library/Application Support/me.asterion.terminal` | `~/.config/me.asterion.terminal` |
| 策略目录 | `~/Library/Application Support/Asterion/research` | `~/.local/share/asterion/research` |
| 内置策略的来源（从项目启动） | 项目的 `research/` | 同左 |
| 内置策略的来源（安装的应用） | `Asterion Terminal.app/Contents/Resources/research/` | `/opt/Asterion Terminal/resources/research/` |

- 每个程序的配置在自己的 `config/`，状态在 `data/`；`terminal/data/` 保存 Market 和 Trading 连接与客户端证书。
- 策略目录固定，不能更换。每次 Lab 启动时，内置的策略和因子复制进去并设为只读：新版本替换它们，不再自带的删掉；同名的用户模块不动。
- 隔离测试通过 `ASTERION_ROOT` 指定临时目录，同时使用独立的 `--user-data-dir`。
- 关闭开发窗口或按 Ctrl+C 会停止本机 Lab；Market 和 Trading 不受影响。

## 发行

发布包一共三个：

| 发布包 | 构建 | 安装 |
| --- | --- | --- |
| Market：`asterion-market-setup.sh` | `scripts/services/build_linux_services.sh` | Market 主机上 `sudo bash asterion-market-setup.sh`，见[录制与部署](market/deployment.md) |
| Trading：`asterion-trading-setup.sh` | 同上 | Trading 主机上 `sudo bash asterion-trading-setup.sh`，见[交易](trading/deployment.md#安装与账户配置) |
| Terminal：`Asterion-Terminal-<版本>-mac-arm64.dmg` 或 `…-linux-amd64.deb` | `pnpm desktop:build`，输出到 `build/desktop/` | macOS 拖进“应用程序”；Linux `sudo apt install ./Asterion-Terminal-….deb` |

- Terminal 升级就是装新版本，卸载就是删除应用（Linux 用 `sudo apt remove asterion-terminal`）。用户资料在程序根目录，第一次启动时由 Terminal 创建，卸载时保留。
- **macOS**：`pnpm desktop:build` 要求本机钥匙串里有唯一的 Developer ID Application 签名身份和 `notarytool` 保存的公证 profile（`CSC_NAME`、`APPLE_KEYCHAIN_PROFILE`）。应用和 DMG 都签名、公证；验收通过后生成 `build/desktop/distribution-acceptance.json`。
- **Linux**：`.deb` 的依赖由打包时 `dpkg-shlibdeps` 从包内程序算出，apt 会一起安装；构建机的系统版本决定包能装在哪些系统上。
- 原生资源清单只有一个：`scripts/desktop/native-resources.json`。

## 运行诊断

- 服务忽略 `SIGPIPE`；`SIGINT` / `SIGTERM` 请求正常停止。
- 每个程序写自己安装目录下的 `logs/`，按程序、日期分文件，见[运行日志](services.md#运行日志)。Terminal 日志记录每条命令的名称、`trace_id`、耗时和结果；服务日志用同一 `trace_id` 关联请求。交易日志在委托写入记录后记下 `order_id` 等标识。
- 日志写入失败时在标准错误输出固定的 `logging.failed` 诊断，Terminal 通知中心报告日志写入故障。
- 本机日志入口见[运行日志](services.md#运行日志)。
