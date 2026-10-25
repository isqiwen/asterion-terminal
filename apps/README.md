# 应用

- `services/`：业务服务，由 Node Agent 托管，各自拥有状态并在进程内执行自己的任务。
- `management/`：Node Manager 与 Node Agent，由操作系统服务管理器托管。
- `common/control/`：客户端与管理进程共用的服务 I/O、端点与节点路径。
- `clients/terminal/`：桌面终端（Electron + React + C++ 编排）；`clients/installer/`：安装维护程序。

进程职责见 [架构](../docs/architecture.md)。
