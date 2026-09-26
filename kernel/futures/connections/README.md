# Connections · L2

Rust 拥有具名连接档案、当前格式秘密绑定、配置修订、唯一活动会话、通道代次、订阅意图、查询标识及响应准入。通用密码学与原子文件发布复用固定 L1 机制；CTP 字段、SDK、限流与报文转换属于 L3 接入插件。

当前由 `bindings/python/src/connections.rs` 把 Python SDK 对象接入 Rust 定义的 `Connector` / `Session` 端口，`src/asterion/connections/service.py` 只保留 DTO、对象转换和监听回调。Rust 持有不透明会话对象并决定其创建、撤权、关闭顺序；Python 不另存配置或生命周期状态。

纯连接 DTO 由 Rust `models.rs` 唯一定义，经 `scripts/generate_domain_models.py` 生成 `bindings/python/asterion_bindings/connections.py` 与 `contracts/connections.schema.json`。Python 构造、复制和反序列化均调用 Rust 校验；生成器只采用 Rust 显式声明的默认值。SDK 回调、HTTP 请求封装及来源合约/账户观察内容仍由 L3 适配，不纳入连接状态模型。

这是 **L2 权威状态与 L3 适配** 的接线。`manifest.json` 声明 L2 所有者及已经实现的访问能力，供静态层级与依赖验证，尚未作为第二个运行时提供者激活。现有 `asterion.connections` 运行时登记仍属于 L3 控制器，负责身份保护与 HTTP 生命周期；原生 L2 插件独立激活和 L2/L3 装配身份拆分仍未完成，届时必须统一替换，不能让两个层级共用同一活动登记。

生命周期操作串行，但状态锁不跨供应商回调。同步行情回调可以更新状态；回调内重入生命周期操作明确失败。旧会话关闭失败时保留所有权，阻止新会话创建。读取持有不可伪造的取消句柄，响应必须匹配 L2 发出的连接、代次、请求 ID 与开始时间；来源不得自行生成这些身份。

当前文件仍为 `connections/profiles.json` version 2。加载失败保留原文件并拒绝修改；发布前写入失败不变更内存状态；若 L1 报告已发布但持久化确认失败，内存同步已发布内容并返回明确错误。重启只恢复档案与选择，不自动联网。此实现不增加交易写能力，也不声称 Python 接入插件是进程沙箱。
