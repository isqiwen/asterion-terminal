# Asterion Terminal

面向中国期货的桌面量化交易终端，运行于 macOS（Apple Silicon）与 Linux x86_64。C++20 核心与独立服务进程，Electron + React 桌面界面。

当前范围：**只开发 macOS（Apple Silicon）与 Linux x86_64 上的 Terminal，只做期货。**

行情、数据、任务、账户交易分别运行于独立服务进程，由 Node Agent 管理；
下载、回测和因子计算使用按需工作进程。见[架构与状态所有权](docs/architecture.md)。

## 能做什么

- **行情**：CTP 实时行情（全市场合约目录、期货全景、自选、五档、分时、分钟 K 线）；Tushare 历史分钟下载、全市场日线表及图表查看。
- **回测与因子分析**：持久化任务服务，K 线数据集驱动的策略回测（均线交叉、通道突破、时序动量、布林回归，多空），单合约与多合约截面的因子评价，独立历史版本仓库。
- **交易**：CTP 交易（柜台仿真或实盘：授权、合约白名单、价格与限额风控、先记录后发送、断线不重发）；可保存多个 CTP 账户并同时交易，行情使用其中一个。
- **服务管理**：随桌面提供的 Node Agent 托管本机服务；可通过 SSH 引导部署到远程 Linux x86_64。

## 快速开始

依赖：Node.js 22+、pnpm 10、Python 3、uv、Conan 2、CMake 3.25+、Ninja；macOS 用 Xcode 26 及以上的 Command Line Tools（Apple Clang），Linux 用 GCC 13 及以上。

```sh
conan profile detect --exist-ok     # 生成本机 Conan profile；已有时不改动
python3 scripts/prepare_ctp.py      # 下载并校验 CTP SDK
pnpm install --frozen-lockfile
pnpm exec install-electron          # 下载锁定版本的 Electron 运行时
pnpm desktop --prepare-analytics    # 首次显式准备分析助手、构建并启动；之后用 pnpm desktop
```

| 命令 | 用途 |
| --- | --- |
| `pnpm desktop` | 构建 Debug C++、Node-API、界面，启动 Electron |
| `pnpm desktop:check` | 构建并验证本机桌面链路，不打开窗口，不要求 Linux 服务包 |
| `pnpm desktop:build` | 生成 Release 安装包到 `build/desktop/`：macOS 为 `.dmg`，Linux 为 `.deb` |
| `ctest --test-dir build/Debug` | C++ 测试 |
| `pnpm test:unit` | 界面的单元测试与组件测试 |
| `pnpm run test:e2e:journeys` | 隔离环境下的端到端关键链路，日常使用 |
| `pnpm run test:e2e` | 全部端到端测试，发布前使用 |

构建、测试和发布细节见 [开发指南](docs/development.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `core/` | C++ 基础（Decimal、ID、时间、错误）、内核（插件、IPC、进程、日志）、期货领域模型 |
| `plugins/` | 数据（CTP、Tushare、SHFE、历史数据源注册）、执行（回测撮合、CTP）、存储、策略、风控、工具插件 |
| `protocol/` | Protobuf 契约与校验 |
| `apps/services/` | 常驻服务：node-agent、market-data、trading、data-service、task-service；任务工作程序：backtest、factor、data-pipeline |
| `apps/clients/terminal/` | 桌面终端：Electron 主进程、React 宿主与工作区插件、C++ 应用编排 |
| `bindings/` | C ABI 与 Node-API 绑定 |
| `packages/client-ui/` | 终端使用的图表组件 |
| `tests/` | C++ 单元与进程集成测试 |

## 文档

- [产品需求：个人期货研究与实盘交易](docs/requirements/README.md)
- [架构](docs/architecture.md)
- [开发指南](docs/development.md)
- [终端界面](docs/terminal.md)
- [行情与数据](docs/market-data.md)
- [交易与风控](docs/trading.md)
- [任务管理](docs/tasks.md)
- [回测](docs/backtest.md)
- [策略](docs/strategies.md)
- [因子分析](docs/factors.md)
- [服务与部署](docs/services.md)
- [现状与下一步](docs/status.md)
- [文档导航](docs/README.md) · [设计与验收记录](docs/reviews/README.md) · [整改记录](docs/reviews/audit-progress.md)
- [贡献约束](AGENTS.md)

UI 工作区随应用统一编译；原生数据源与风控可独立安装，策略、存储和内部工具按实际需要保持静态模块。第三方数据源与风控插件接入：[原生插件 SDK](docs/native-plugins.md)。
