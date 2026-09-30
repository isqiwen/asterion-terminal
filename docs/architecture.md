# 架构

![架构](assets/architecture.png)

架构图固定为 `docs/assets/architecture.png`，更新时覆盖同一文件。

## 分层

```mermaid
flowchart TB
  UI[React 工作区插件] --> Host[Terminal 宿主]
  Host --> Bridge[Electron 预加载 → Node-API → C ABI]
  Bridge --> App[Terminal C++ 编排]
  App -->|Protobuf IPC / mTLS| Services[独立服务进程]
  Services --> Plugins[插件]
  Services --> Domain[领域]
  Plugins --> Domain
  Domain --> Kernel[内核]
  Kernel --> Foundation[基础]
```

| 层 | 目标 | 内容 |
| --- | --- | --- |
| 基础 | `asterion_foundation` | `Decimal`（8 位小数定点）、ID、时钟、错误码、序列化 |
| 内核 | `asterion_kernel` | 插件生命周期、本机 IPC 与 TCP+mTLS 通道、服务宿主、子进程与文件锁、持久文件写入、线程池、日志、Terminal 命令运行时 |
| 领域 | `asterion_domain` | 合约与订单、单合约期货账本、交易时段、当日分钟线、执行/风险/策略/因子端口 |

领域依赖内核，内核依赖基础，不能反向依赖。

## 进程

所有服务由 Node Agent 启动和监督。Terminal 是客户端，关闭不影响服务。

| 程序 | 职责 |
| --- | --- |
| `asterion-node-agent` | 部署、启停、健康探测、有限重启、程序升级 |
| `asterion-market-data` | CTP 实时行情、合约目录、事件流、当日分钟线与 1 分钟涨速 |
| `asterion-trading` | 模拟交易会话（账本、撮合、风控、持久日志） |
| `asterion-task-service` | 研究任务与执行尝试的持久化队列 |
| `asterion-backtest` / `asterion-factor` / `asterion-data-pipeline` | 按任务启动的工作程序 |
| `asterion-strategy` | 可信策略宿主，输出目标持仓意图 |

本机通信使用 Unix Socket，远程使用 TCP + 双向 TLS，消息都是 `protocol/proto/asterion/v1/` 中的 Protobuf。

## 插件

| 类别 | 实现 |
| --- | --- |
| 数据 | `data/ctp`（行情与合约目录）、`data/tushare`（分钟线、日线）、`data/registry`（历史数据源组合与目录入口） |
| 执行 | `execution/paper`（历史撮合）、`execution/ctp`（交易接口，未开放） |
| 存储 | `storage/sqlite`（账本、策略与任务的有序日志和索引）、`storage/filesystem`（历史数据版本目录） |
| 策略 | `strategy/cta`（SMA） |
| 风控 | `risk/order-limits`（交易前限额） |
| 工具 | `tools/chart_indicators`（均线、MACD）、`tools/factor_analysis`、`tools/runtime_info` |

插件在编译期注册，运行在宿主进程内，是可信代码。

## Terminal

| 位置 | 职责 |
| --- | --- |
| `apps/clients/terminal/electron/` | 主进程、预加载、窗口 |
| `bindings/node/`、`bindings/c/` | Node-API 与 C ABI |
| `apps/clients/terminal/native/` | C++ 编排：服务客户端、命令、状态快照 |
| `apps/clients/terminal/src/` | React 宿主：工作台、设置、启动、多语言、桥接 |
| `apps/clients/terminal/plugins/` | 工作区插件：市场、自选、合约、总览、数据、研究、交易 |
| `apps/clients/terminal/dev/` | 浏览器开发桥（`pnpm dev` 与 e2e 使用） |

### 状态同步

C++ 后台每 0.5–2 秒汇总各服务状态并发布带修订号的快照。界面按修订号轮询：修订号未变则不返回内容；行情部分只返回自持有版本以来变化的报价行，合约目录未变则不重复发送。界面把增量合并回完整快照，插件看到的数据形状不变。

命令通过同一个操作锁执行，锁内只做校验和状态读写。耗时的服务调用在锁外进行：历史分页、分钟线、数据仓库查询和研究结果读取取出共享的服务客户端后释放锁；远程节点的 SSH 引导、防火墙检查与变更、服务部署与更新、Agent 升级和插件配置也在锁外执行，彼此之间由单独的节点操作锁串行，第二个节点操作会得到明确的冲突提示。其间其他命令和状态刷新照常进行；节点客户端以共享指针持有，断开连接不会影响进行中的操作。

## 历史数据边界

统一身份与来源语义位于 Domain；Tushare 通过原生插件注册入口动态加载。Terminal 通过研究服务查询合约目录和历史仓库。下载请求使用完整合约身份、明确来源与供应商映射；文件插件负责 v2 数据块、摘要、恢复、锁和版本索引。详见 [行情与数据](market-data.md) 和 [原生插件 SDK](native-plugins.md)。
