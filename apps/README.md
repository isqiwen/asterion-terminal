# 应用

- `clients/terminal/`：macOS 与 Linux 桌面终端（Electron + React + C++ 编排）。
- `services/`：由 Node Agent 托管的独立服务和按任务启动的工作程序。

各应用装配 `core/`、`plugins/` 与 `protocol/`，不复制业务实现。进程职责见 [架构](../docs/architecture.md)。
