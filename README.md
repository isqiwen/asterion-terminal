# Asterion

多资产量化交易平台。统一 C++ 核心，插件化能力，沿用 React / TypeScript 终端与 Tauri 桌面外壳。

![Asterion 当前架构](docs/assets/architecture.png)

上图为唯一当前架构图；更新时覆盖同一路径，历史版本由 Git 保存。实现范围与验收状态以文档记录为准。

当前优先交付期货与 Asterion Terminal。原主题、工作台、窗口框架和总览布局编辑器已复用，Tauri 通过薄 C ABI 桥调用 C++ 核心；支持中国期货实际合约校验、CSV 历史逐笔预览和行情展示。已接入单合约历史模拟交易：账户/持仓/冻结、下单撤单、成交入账、手动结算与本机日志恢复。已接入只读 CTP 实时行情链路，真实账号与交易时段验收待完成，见 [CTP 行情](docs/ctp-market-data.md)。自动跨日结算、独立风险插件与实盘交易尚未开放。模拟会话现在运行于独立的 `asterion-trading`，通过 Protobuf 本机 IPC 或 TCP + mTLS 与 Terminal 通信；设置 → 连接支持远程服务配置、Node Agent 部署与状态管理，C++ 后台提供心跳、有限重连和模拟进程异常重启。C++ 命令行入口统一使用 CLI11。使用方式与限制见 [期货模拟交易](docs/paper-trading.md) 和 [进程架构](docs/process-architecture.md)。

## Asterion Terminal

需要 Node.js 22+、pnpm 10、Rust stable 及所在平台的 Tauri 构建依赖。C++ 构建工具见下方。脚本先用 Conan / CMake 构建核心，再构建 Tauri，不启动旧 Rust/Python 后端。

```sh
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
pnpm desktop
# 编译并链接桌面程序，不打安装包
pnpm desktop:check
# Release 安装包：macOS DMG / Linux DEB / Windows NSIS EXE
pnpm desktop:build
```

`desktop:build` 在当前原生系统依次执行 Conan 安装依赖、CMake 编译 C++、TypeScript/Vite 构建界面、Cargo/Tauri 编译外壳并打包。每个平台只分发一种桌面安装包，内含本机 Agent、交易和行情程序，以及 Linux x86_64 远程服务和初始化脚本，用户不需要分别下载它们：

| 构建系统 | 唯一桌面安装包 | 默认输出目录 |
| --- | --- | --- |
| Windows | NSIS `.exe`（不生成 MSI） | `apps/terminal/src-tauri/target/release/bundle/nsis/` |
| Linux | `.deb`（不生成 AppImage / RPM） | `apps/terminal/src-tauri/target/release/bundle/deb/` |
| macOS | `.dmg` | `apps/terminal/src-tauri/target/release/bundle/dmg/` |

三种安装包分别在对应操作系统编译；Windows 使用 Visual Studio 开发者终端。构建完成会检查当前平台输出目录中只有一个安装包并打印路径，旧版本残留会报错而不会自动删除。CI 先在 Linux x86_64 编译同版本服务，再将该构建材料嵌入每个桌面包。手动打包前，将 `asterion-services-linux-x86_64.zip` 放到 `build/linux-bundles/`（或设置 `ASTERION_LINUX_BUNDLES`）；缺少该文件会拒绝打包。ZIP 由各 Linux 构建环境运行 `python3 scripts/deployment_bundle.py build/Release` 生成，仅作为构建材料，终端用户只需下载桌面安装包。

本机 Rust 工具链位于 `.state/toolchain/`，启动脚本自动设置其 Cargo/Rust 环境。其他机器使用 PATH 中的 Rust 工具链。开发浏览器入口为 `pnpm dev`（先构建 Debug C++ 核心），它只用于开发验收，通过同一 C++ API 的进程传输工作；正式产品使用 Tauri 本机调用。

首次打开显示欢迎与初始化页面，点击“开始设置”后准备本机服务、验证连接，再进入工作台。后续启动自动检查服务；失败可在启动页重试。远程 Linux 在工作台设置中配置。

## 构建

要求 C++20 编译器、Conan 2、CMake 3.25+、Ninja。首次使用 Conan 且没有 profile 时运行 `conan profile detect`。

```sh
python3 scripts/prepare_ctp.py
conan install . -s build_type=Debug -s compiler.cppstd=20 -c tools.cmake.cmaketoolchain:generator=Ninja --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
./build/Debug/asterion
```

Core 的基础与领域实现依赖标准库；终端应用 API 使用 Conan 管理的 `nlohmann_json/3.12.0`，配有 `conan.lock`。前端和桌面依赖分别由 `pnpm-lock.yaml` 与 Tauri 的 `Cargo.lock` 锁定。CMake target 不依赖 Conan 缓存的绝对路径。

本机已有默认 profile 与 PATH 编译器版本不一致，本次验证使用 Apple Clang 21，并将 host/build profile 写入忽略的 `build/local-profile`。在当前工作区重建时，安装命令改为 `conan install . -pr:h ./build/local-profile -pr:b ./build/local-profile --build=missing`。更换编译器后先执行 `cmake --fresh --preset conan-debug`；其他机器需使用与实际编译器一致的 profile。

架构与后续验收见 [架构说明](docs/architecture.md)、[完整实现矩阵](docs/implementation.md) 和 [实施进度](docs/ROADMAP.md)。已实现的行情链路见 [CSV 使用说明](docs/csv-market-data.md)。旧代码保存在 `rust` 分支，新实现不调用旧服务。

## 工程组织

- `core/`：C++ 基础、领域与内核运行时机制。
- `plugins/`：跨应用能力插件。Terminal 插件接口定义在 `apps/terminal/plugins/contract.ts`，注册校验在应用宿主内。
- `apps/terminal/src/ui/`：Terminal 内置主题与基础 UI 库。
- `apps/terminal/src/`：内置桌面宿主；`apps/terminal/plugins/`：Terminal 专属工作区插件。
- `apps/terminal/native/`：C++ 应用编排；`bindings/c/`：C ABI 封装。
- `protocol/`：跨进程/跨机器 Protobuf 消息契约、校验与表示转换；通用传输机制归 Core kernel。
- `apps/terminal/dev/`：浏览器开发与测试桥接，通过 `asterion_terminal_dev_bridge` 调用真实 C++，不用于正式桌面传输。

插件按复用范围归属目录，具体边界见 [架构说明](docs/architecture.md)。当前 UI 插件是内置注册与按需加载，不支持第三方动态安装。

## 三平台支持

Linux、Windows、macOS 共用核心和 Terminal，分别提供 DEB、NSIS EXE、DMG。开发环境、构建入口和验证范围见 [平台说明](docs/platforms.md)。

Core 基础能力、运行时机制及实际边界见 [Core 基础设施](docs/core-infrastructure.md)。

日志使用 spdlog；C++ 单元测试使用 GoogleTest，由 Conan 锁定依赖并通过 CTest 执行。线程池支持可配置工作线程数与有界队列。

远程节点由 Terminal 经 SSH 安装 Agent，再通过 mTLS 上传对应平台程序并管理服务；默认本机同样由随包提供的 Node Agent 按需部署和监督，关闭 Terminal 不停止服务。安装与边界见 [服务管理](docs/service-management.md)。

目标 Linux 机器的管理员初始化步骤见 [机器初始化](docs/host-initialization.md)，完成后由 Terminal 通过 SSH 安装 Node Agent。

终端支持简体中文与英文，可在首次启动页或“设置 → 外观 → 语言”切换，选择会在本机保存。实现归属见 [Terminal 多语言](docs/localization.md)。
