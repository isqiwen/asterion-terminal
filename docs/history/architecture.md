# History 开发者导读

先读[功能与协议](README.md)，再按本文的图找到代码入口；安装和数据目录见[部署](deployment.md)，产品边界见[总体架构](../architecture.md)。图中的类名对应当前 C++ 实现，省略私有辅助类和字段。

## 服务边界与数据流

History 是由 systemd 看护的 Linux C++ 进程，拥有采集任务和正式仓库。Terminal 与 Lab 是客户端；实时行情在 [Quotes](../quotes/README.md)，Trading 独立连接柜台。理由：三类服务各管一件事，一个的问题和升级不影响另外两个。

```mermaid
flowchart LR
    Snapshot[Quotes 留下的每日合约快照] --> Ingest[入库]
    Provider[Tushare / 交易所 / 期货公司 / 新浪财经] --> Ingest
    Ingest --> Sources[来源仓库]
    Sources --> Compose[合成]
    Compose --> Research[统一数据]
    Research --> Read[读取]
    Read --> Terminal[Terminal]
    Read --> Lab[Lab]
```

箭头只往右走，后面的模块不调用前面的模块。

| 模块 | 代码 | 只回答 | 不管 |
| --- | --- | --- | --- |
| 入库 | `acquisition/` | 这个来源这一天的数据取回来了吗 | 研究该用哪份 |
| 合成 | `warehouse/composer`、`warehouse/research` | 研究该用哪份 | 数据从哪取、怎么取 |
| 读取 | `warehouse/reader`、`service.cpp` | 这个固定版本里有什么 | 数据缺了怎么办 |

- 每个来源只写自己的仓库，统一数据只由合成写入，客户端只读统一数据。

## 核心类关系

RPC 接入负责收发帧；`SnapshotIntake` 在自己的线程里把已结束交易日的合约快照入库；`Composer` 在自己的线程里更新统一数据。每个调用走哪条路径，只由 `service.cpp` 的 `route()` 决定。可能耗时的扫描单独排队，理由：Lab 下载和任务查询不等它们。

| 调用 | 路径 | 权限 |
| --- | --- | --- |
| `start_acquisition`、`cancel_acquisition` | 读取线程 | `manage` |
| 摘要、覆盖、参考表、版本历史与比较 | 分析线程 | `read` |
| 其余调用 | 读取线程 | `read` |

来源适配器负责供应商数据转换，`Manager` 负责任务与提交，`Store` 负责固定版本。客户端看不到来源：协议只有一份数据集。

```mermaid
classDiagram
    class Manager
    class Source {
        <<abstract>>
        supports()
        daily()
        minutes()
        price_ticks()
    }
    class Tushare
    class Exchange
    class Guotai
    class CatalogSource
    class Sina
    class Store {
        current()
        version()
        locate()
        writer()
    }
    class Writer {
        put()
        adopt()
        commit()
    }
    Source <|-- Tushare
    Source <|-- Exchange
    Source <|-- Guotai
    Source <|-- CatalogSource
    Source <|-- Sina
    Manager ..> Source : 为任务创建
    Manager *-- Store : 各来源
    Manager ..> Writer
    Writer --> Store : 锁定写入并发布
```

## 采集、发布与固定版本读取

```mermaid
sequenceDiagram
    participant Client as Terminal / Lab
    participant RPC as RpcHost / History
    participant Manager
    participant Source
    participant Writer
    participant Research as Composer
    Client->>RPC: start_acquisition
    RPC->>RPC: 由已验证证书检查 manage 权限
    RPC->>Manager: submit(request)
    Manager->>Manager: 持久化 QUEUED 任务
    Manager-->>Client: 经 RPC 返回已接纳任务
    loop 按优先级每个来源，只取前面没有的分区
        Manager->>Source: 后台读取供应商数据
        Source-->>Manager: 标准化行
    end
    Manager->>Writer: put()
    Manager->>Writer: commit()
    Writer->>Writer: 分区文件 → 月度分片 → 版本清单 → current
    Manager-->>Research: 通知合成，不等待
    Manager->>Manager: 持久化任务进度或完成状态
    Research->>Research: 在自己的线程读取来源版本并发布
    Client->>RPC: get_dataset_version(当前版本)
    RPC-->>Client: 固定版本 ID、分区与摘要
    loop 分页与文件下载
        Client->>RPC: get_dataset_version / read_dataset_file，携带同一版本 ID
        RPC-->>Client: 该版本的元数据或 Parquet 字节块
    end
```

每日合约快照也走上述路径：`SnapshotIntake` 只看磁盘，发现交易日已结束且未入库就提交任务，只用 `ctp` 来源；失败的日期一小时后再试，不影响其他日期。

`research` 的来源选择规则见[功能文档](README.md#仓库与固定版本)。来源顺序只在 `sources_of` 维护，`Manager` 和合成共用。`Writer::adopt()` 用硬链接引用选中的分区，旧版本仍引用原文件；无内容变化不生成新版本。`Composer` 在启动时和每次来源提交后运行，运行期间的通知合并为一次，所以任务完成后 `research` 稍后才更新；合成失败不影响任务，一分钟后重试。

## 生命周期与错误边界

| 情况 | 行为与代码入口 |
| --- | --- |
| 客户端关闭 | 释放该连接；已接纳的采集继续，见 `service.cpp` |
| 来源失败 | 后面的来源照常补缺，任务最后标为失败，见 `Manager::work()` |
| 服务正常停止 | 结束后台任务后退出，见 `main.cpp` 与 `service.cpp` |
| 服务重新启动 | 旧采集任务标为中断，损坏的任务文件只影响它自己，见 `Manager` |

## 代码阅读路线

| 顺序 | 入口 | 阅读目标 |
| --- | --- | --- |
| 1 | [main.cpp](../../apps/services/history/main.cpp)、[settings.cpp](../../apps/services/history/settings.cpp) | 配置、数据目录锁、TLS 身份、权限和停止流程 |
| 2 | [service.cpp](../../apps/services/history/service.cpp) | 调用路径表 |
| 3 | [manager.cpp](../../apps/services/history/acquisition/manager.cpp)、[source.hpp](../../apps/services/history/acquisition/source.hpp)、[snapshot_intake.cpp](../../apps/services/history/acquisition/snapshot_intake.cpp) | 来源扩展、任务生命周期与批量提交 |
| 4 | [store.cpp](../../apps/services/history/warehouse/store.cpp)、[research.cpp](../../apps/services/history/warehouse/research.cpp)、[composer.cpp](../../apps/services/history/warehouse/composer.cpp) | 不可变版本、月度分片、来源合成 |
| 5 | [通信协议](../protocol.md)、[history.proto](../../protocol/proto/asterion/history.proto) | 客户端契约与固定版本读取 |

模块测试见 [tests/history](../../tests/history)；跨客户端与重启行为看 [concurrency_process.py](../../tests/history/concurrency_process.py)。
