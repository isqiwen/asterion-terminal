# Asterion

多资产量化交易平台。统一 C++ 核心，插件化能力，沿用 React / TypeScript 终端与 Electron 桌面外壳。

![Asterion 当前架构](docs/assets/architecture.png)

上图为唯一当前架构图；更新时覆盖同一路径，历史版本由 Git 保存。实现范围与验收状态以文档记录为准。

当前优先交付期货与 Asterion Terminal。原主题、工作台、窗口框架和总览布局编辑器已复用，Electron 通过异步 Node-API / C ABI 桥调用 C++ 核心；支持中国期货实际合约校验、CSV 历史逐笔预览和行情展示。已接入单合约历史模拟交易：账户/持仓/冻结、下单撤单、成交入账、手动结算与本机日志恢复。已接入只读 CTP 实时行情链路，真实账号与交易时段验收待完成，见 [CTP 行情](docs/ctp-market-data.md)。自动跨日结算、独立风险插件与实盘交易尚未开放。模拟会话现在运行于独立的 `asterion-trading`，通过 Protobuf 本机 IPC 或 TCP + mTLS 与 Terminal 通信；设置 → 连接支持远程服务配置、Node Agent 部署与状态管理，C++ 后台提供心跳、有限重连和模拟进程异常重启。C++ 命令行入口统一使用 CLI11。使用方式与限制见 [期货模拟交易](docs/paper-trading.md) 和 [进程架构](docs/process-architecture.md)。

## 开发与构建（当前仅 macOS Terminal）

桌面开发统一从 pnpm 入口执行。需要 Node.js 22.22.2+、pnpm 10、Python 3、Conan 2、CMake 3.25+、Ninja 和系统 Apple Clang / macOS SDK。首次使用 Conan 且没有 profile 时运行 `conan profile detect`。

首次准备：

```sh
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
```

按目的选择一个命令，无需依次执行：

| 命令 | 用途 |
| --- | --- |
| `pnpm desktop` | 构建 Debug C++、Node-API 和界面，启动 Electron 开发窗口 |
| `pnpm desktop:check` | 构建并验证 Debug 桌面链路，不启动窗口、不生成安装包 |
| `pnpm desktop:build` | 构建 Release 桌面并生成 macOS `.dmg`，输出到 `build/desktop/` |

三个命令共享 `scripts/desktop.mjs` → `scripts/desktop.py` 编排。内部依次调用 Conan 管理 C++ 依赖、CMake 编译核心/服务/Node-API、TypeScript/Vite 构建界面；仅安装包模式调用 Electron Builder。**Conan/CMake 是同一构建链的底层步骤，不是另一套需要重复执行的 Terminal 构建方式。** `pnpm build` 只构建前端，不能代替完整桌面构建。

macOS 安装包包含本机服务及远程 Linux 部署材料。打包前需将同版本的 `asterion-services-linux-x86_64.zip` 放入 `build/linux-bundles/`（或设置 `ASTERION_LINUX_BUNDLES`）；缺少材料会明确拒绝打包。Linux 构建环境通过 `python3 scripts/deployment_bundle.py build/Release` 生成该材料，它是远程服务制品，不是 Linux Terminal 客户端。

桌面开发使用 Vite 端口 1422，可通过 `ASTERION_DESKTOP_PORT` 指定。`pnpm dev` 仅供界面开发与浏览器测试，依赖已经构建的 Debug C++ 开发桥，不是独立 Web 客户端，也不是另一种正式产品运行方式。

首次打开准备本机服务并验证连接后进入工作台，后续启动自动检查服务。远程 Linux 在设置中配置。

### 底层 C++ 调试与测试

日常桌面开发不需要手动重复 Conan/CMake 命令。完成 `pnpm desktop:check` 后，可单独运行 C++ 测试：

```sh
ctest --test-dir build/Debug --output-on-failure
```

只有定位 C++ 构建问题或开发独立服务时，才直接操作 Conan/CMake target；这仍使用同一份 `CMakeLists.txt` 和 Conan 依赖契约。Terminal API 测试通过 CTest 的隔离节点包装运行，不直接运行测试二进制污染日常 Agent。

Core 的基础与领域实现依赖标准库；终端应用 API 使用 Conan 管理的 `nlohmann_json/3.12.0`，配有 `conan.lock`。前端和桌面依赖由 `pnpm-lock.yaml` 锁定。CMake target 不依赖 Conan 缓存的绝对路径。

桌面脚本自动选择系统 Apple Clang；存在 `build/local-profile` 时加载该 profile，并显式设置 macOS 工具链。不要把某台开发机的编译器版本或缓存路径当作通用构建要求。

架构与后续验收见 [架构说明](docs/architecture.md)、[完整实现矩阵](docs/implementation.md) 和 [实施进度](docs/ROADMAP.md)。已实现的行情链路见 [CSV 使用说明](docs/csv-market-data.md)。旧代码保存在 `rust` 分支，新实现不调用旧服务。

## 工程组织

- `core/`：C++ 基础、领域与内核运行时机制。
- `plugins/`：跨应用能力插件。Terminal 插件接口定义在 `apps/clients/terminal/plugins/contract.ts`，注册校验在应用宿主内。
- `apps/clients/terminal/src/ui/`：Terminal 内置主题与基础 UI 库。
- `apps/clients/terminal/src/`：内置桌面宿主；`apps/clients/terminal/plugins/`：Terminal 专属工作区插件。
- `apps/clients/terminal/native/`：C++ 应用编排；`bindings/c/`：C ABI 封装。
- `protocol/`：跨进程/跨机器 Protobuf 消息契约、校验与表示转换；通用传输机制归 Core kernel。
- `apps/clients/terminal/dev/`：浏览器开发与测试桥接，通过 `asterion_terminal_dev_bridge` 调用真实 C++，不用于正式桌面传输。

插件按复用范围归属目录，具体边界见 [架构说明](docs/architecture.md)。当前 UI 插件是内置注册与按需加载，不支持第三方动态安装。

## 当前平台范围

当前只开发和交付 macOS Terminal。Windows/Linux Terminal 及其他客户端仅保留架构考虑；旧跨平台记录不代表当前实施任务。远程 Linux 服务部署能力继续保留。权威范围见 [AGENTS.md](AGENTS.md)。

Core 基础能力、运行时机制及实际边界见 [Core 基础设施](docs/core-infrastructure.md)。

日志使用 spdlog；C++ 单元测试使用 GoogleTest，由 Conan 锁定依赖并通过 CTest 执行。线程池支持可配置工作线程数与有界队列。

远程节点由 Terminal 经 SSH 安装 Agent，再通过 mTLS 上传对应平台程序并管理服务；默认本机同样由随包提供的 Node Agent 按需部署和监督，关闭 Terminal 不停止服务。安装与边界见 [服务管理](docs/service-management.md)。

目标 Linux 机器的管理员初始化步骤见 [机器初始化](docs/host-initialization.md)，完成后由 Terminal 通过 SSH 安装 Node Agent。

终端支持简体中文与英文，可在首次启动页或“设置 → 外观 → 语言”切换，选择会在本机保存。实现归属见 [Terminal 多语言](docs/localization.md)。

桌面宿主、隔离边界和构建说明见 [Electron 桌面宿主](docs/electron-desktop.md)。
