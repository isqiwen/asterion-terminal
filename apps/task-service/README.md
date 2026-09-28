# asterion-task-service

持久化回测任务、队列、执行尝试、进度、取消和确认结果，支持同一输入幂等提交、显式重试、服务重启恢复及旧尝试隔离。

使用现有 filesystem-journal 插件持久化，采用顶层 `protocol/proto/asterion/v1/research.proto`。支持本机 IPC 和 TCP/mTLS。任务服务负责业务任务状态，不直接创建工作子进程；Node Agent 每秒扫描队列，每个研究服务最多并发两个回测进程。

启动参数与精确边界见 [研究任务说明](../../docs/research-tasks.md)。Terminal 页面与本机 Agent 调度已接入，Linux x86_64 部署与 macOS 打包已验证，独立机器网络与 Windows 原生仍待验收。
