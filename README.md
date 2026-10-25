# Asterion Terminal

面向中国期货的个人量化研究与交易桌面终端，运行于 macOS（Apple Silicon）与 Linux x86_64。
C++20 核心与独立服务进程，Electron + React 界面。

## 当前能力

- **行情**：CTP 实时行情、合约目录、自选、五档、分时与分钟 K 线。
- **历史数据**：Tushare 分钟线与全市场数据表、SHFE 历史保证金（窄范围），固定快照与数据集。
- **研究**：规则策略回测（均线交叉、通道突破、时序动量、布林回归及截面排序）、因子评价、
  研究项目与变量、样本与统计、规则逻辑、策略包与评估报告。
- **交易**：CTP 账户（柜台仿真或实盘）：授权、合约白名单、限额与价格风控、先记录后发送、断线不重发。
- **节点**：本机完整运行；可通过 SSH 部署到远程 Linux 节点。

能力边界与已知限制见 [现状](docs/status.md)。

## 快速开始

依赖：Node.js 22+、pnpm 10、Python 3、uv、Conan 2、CMake 3.25+、Ninja；
macOS 需要 Xcode 26 Command Line Tools，Linux 需要 GCC 13+。

```sh
conan profile detect --exist-ok
python3 scripts/prepare_ctp.py      # 下载并校验 CTP SDK
pnpm install --frozen-lockfile
pnpm exec install-electron
pnpm desktop --prepare-analytics    # 首次：准备分析助手、构建并启动；之后用 pnpm desktop
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
| `plugins/` | 数据源、执行、存储、策略、风控与分析工具 |
| `protocol/` | Protobuf 契约与边界校验 |
| `apps/services/` | 业务服务：行情、数据、研究、交易 |
| `apps/management/` | Node Manager 与 Node Agent |
| `apps/common/control/` | 客户端与管理进程共用的控制机制 |
| `apps/clients/` | Terminal 桌面端与安装器 |
| `bindings/` | C ABI、Node-API 与原生插件 SDK |
| `config/` | 节点默认配置 |
| `tests/` | C++ 与进程测试 |

## 文档

[文档导航](docs/README.md) · [架构](docs/architecture.md) · [产品需求](docs/requirements/README.md) ·
[重构计划](docs/reviews/project-assessment-2026-10-25.md) · [贡献约束](AGENTS.md)
