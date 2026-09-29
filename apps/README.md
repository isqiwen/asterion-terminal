# 应用工程

- `clients/terminal/`：当前唯一产品客户端（macOS），Electron + React 桌面终端及其原生编排、内置 UI 插件和开发桥。
- `services/node-agent/`：机器上的受控进程与制品管理。
- `services/task-service/`：业务任务、调度与执行尝试管理。
- `services/trading/`、`services/market-data/`、`services/strategy/`：独立交易、行情和策略宿主。
- `services/backtest/`、`services/factor/`、`services/data-pipeline/`：按任务启动的计算和数据工作程序，也保留各自明确的 CLI 用例。

两类应用复用顶层 Core、插件与协议；客户端不拥有服务端权威账本，也不因退出而停止已部署服务。服务不是一个合并后端，分类不改变可执行程序名称、进程隔离、安装包内容或部署范围。最新架构与验收见 [架构](../docs/architecture.md) 和 [实现矩阵](../docs/implementation.md)。
