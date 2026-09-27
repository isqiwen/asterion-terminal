# asterion-strategy

独立可信策略宿主，接收有序成交事件并持久化目标持仓意图。当前复用 CTA SMA 插件，支持 Protobuf、本机 IPC、TCP/mTLS、健康检查、Agent 进程所有者约束和重启重放。

不持有权威账户账本，不直接调用券商下单，不以独立进程冒充安全沙箱。已连接 Agent 类型化服务注册、健康探测、有限重启和 desired 状态恢复。Terminal 可观察/启停已部署服务；交易进程已有可撤销授权和目标持仓交接命令；宿主已有不可变计划驱动的自动历史回放与交接恢复；Terminal 配置和实时行情订阅仍待接入，不默认创建实例或加入安装包。

构建：`cmake --build build/Debug --target asterion-strategy`。参数、10000 事件容量及恢复语义见 [策略宿主](../../docs/strategy-host.md)。
