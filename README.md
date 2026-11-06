# Asterion Terminal

面向中国期货的个人量化研究与交易桌面终端，运行于 macOS（Apple Silicon）与 Linux x86_64。
C++20 核心与独立服务进程，Electron + React 界面。目标是用 Python 写策略，同一份策略在回测、仿真柜台和实盘上运行，
见[产品说明](docs/product.md)。

## 架构

远程 Market 是一个纯 C++ 服务进程，统一实时行情、录制、历史采集与正式仓库读写；
本机 Lab 用 Python 做研究并按版本读取 Market；Terminal 发布策略代码包；Trading 在交易主机直接连接行情与柜台，
策略使用 Trading 自行聚合的 K 线现场预热。关闭 Terminal 不影响已授权交易。策略目标经账户统一执行路径处理，见[架构](docs/architecture.md)。

## 当前能力

- **行情**：CTP 实时行情、合约目录、自选、五档、分时与分钟 K 线。
- **历史数据**：Market C++ 采集 Tushare 日线、分钟线、合约与日历，以及交易所日线和结算参数；录制分钟可发布为固定研究版本。部分在线来源待验收。
- **研究**：固定策略目录中的 Python 策略与因子；日线/分钟线回测、参数扫描、滚动验证、因子评价、图表与研究记录。
- **策略部署**：Terminal 发布小型源码包；Trading 从主机离线运行时加载、现场预热，目标经账户的唯一执行路径报单。
- **交易**：CTP 账户（柜台仿真或实盘）：授权、合约白名单、限额与价格风控、先记录后发送、断线不重发。
- **部署**：Market（行情与录制）和 Trading 在 Linux 独立部署；Terminal 只看护本机 Lab。历史采集与仓库由 Market C++ 管理，Lab 只读固定版本并缓存；录制分钟可由 Market 发布为固定研究版本。

能力边界与已知限制见[现状](docs/status.md)，先做什么见[开发计划](docs/plan.md)。

## 快速开始

依赖：Node.js 22+、pnpm 10、Python 3、uv、Conan 2、CMake 3.25+、Ninja；
macOS 需要 Xcode 26 Command Line Tools，Linux 需要 GCC 13+。

```sh
conan profile detect --exist-ok
python3 scripts/prepare_ctp.py      # 下载并校验 CTP SDK
pnpm install --frozen-lockfile
pnpm exec install-electron
pnpm desktop                       # 自动准备 Python 运行时、构建并启动
```

| 命令 | 用途 |
| --- | --- |
| `pnpm desktop` | 构建 Debug 并启动桌面端 |
| `pnpm desktop:check` | 构建并检查桌面链路，不打开窗口 |
| `pnpm desktop:build` | 生成发行安装包（macOS `.dmg`，Linux `.deb`） |
| `ctest --preset conan-debug -j 6` | C++ 测试 |
| `pnpm test:unit` | 界面单元与组件测试 |
| `pnpm run test:e2e:journeys` | 隔离环境下的关键端到端链路 |

细节见 [开发指南](docs/development.md)。

## 目录

| 路径 | 内容 |
| --- | --- |
| `core/` | 基础（Decimal、时间、错误）、内核（IPC、进程、日志、持久文件）、期货领域模型 |
| `apps/services/` | 各服务拥有自己的实现；跨服务复用在 `common/`，共享 SQLite 和订单限额在 `core/` |
| `apps/services/market/` | C++ 实时行情、录制、历史采集、正式仓库与固定版本读取 |
| `apps/services/lab/` | Python 研究服务与 C++ 回测扩展 |
| `apps/services/trading/` | 独立账户进程、内置风控与交易主机策略运行器 |
| `protocol/` | Protobuf 契约与边界校验 |
| `apps/clients/terminal/` | Terminal 桌面端：`src/`、`features/` 是界面，`native/` 是 C++ 原生层（Lab 看护、行情与交易连接） |
| `apps/clients/terminal/bindings/` | Terminal 的 Node-API 绑定 |
| 各子项目的 `config/` | 默认配置和部署配置示例，见[配置文件](docs/configuration.md) |
| `tests/` | C++ 与进程测试 |

## 文档

[文档导航](docs/README.md) · [产品说明](docs/product.md) · [交易规则](docs/safety.md) ·
[开发计划](docs/plan.md) · [架构](docs/architecture.md) · [贡献约束](AGENTS.md)
