# Market 开发者导读

先读[功能与协议](market.md)，再按本文的图找到代码入口；安装和数据目录见[录制与部署](recording.md)，产品边界见[总体架构](architecture.md)。图中的类名对应当前 C++ 实现，省略私有辅助类和字段。

## 服务边界与数据流

Market 是由 systemd 看护的 Linux C++ 进程，拥有共享行情、录制文件、采集任务和正式仓库。Terminal 与 Lab 是客户端；Trading 独立连接柜台，不经过 Market 获取策略行情。理由：研究和看盘的生命周期不影响交易执行。

```mermaid
flowchart LR
    CTP[CTP 行情与合约目录] --> Live[实时状态与分钟聚合]
    subgraph Market[Market 进程]
        RPC[TLS / Protobuf RPC]
        Live --> Record[录制]
        Record --> Archive[录制 Parquet]
        Archive --> Merge[盘后合并]
        Merge --> Publish[自录发布待办]
        Publish --> Acquire[采集管理]
        Archive --> Acquire
        Acquire --> Store[来源仓库]
        Store --> Research[research 固定版本]
        Live --> RPC
        Research --> RPC
        RPC --> Acquire
    end
    Provider[Tushare / 交易所] --> Acquire
    Terminal[Terminal] <-->|请求与报价流| RPC
    Lab[Lab] <-->|固定版本读取| RPC
```

- 实时报价快照供界面展示，有序行情事件供聚合和录制，避免界面合并报价时丢掉录制输入。
- 行情状态只由行情线程更新；SDK 回调只把事件放进队列，SDK 调用和释放交给其执行器，避免柜台调用拖住行情线程。
- 历史读取、采集、录制写入和合并通过各自的后台执行路径完成。录制与仓库使用不同的 Parquet 执行通道，避免历史查询占住录制写入。
- 合并替换录制文件时与录制历史读取互斥；固定仓库版本引用不可变文件，因此后续合并不会改变研究已选的数据。

## 核心类关系

Market 进程里有三条执行路径：RPC 主机的 I/O 线程收发帧；行情线程拥有 `Live` 和 `Recording`；读取线程池读仓库、管理采集任务。每个调用走哪条路径，只由 `market.cpp` 的 `route()` 决定。

```mermaid
classDiagram
    class RpcHost {
        run()
    }
    class Market {
        serve()
        drain()
        stop()
    }
    class Live {
        maintain()
        poll()
        apply()
        frame()
        status()
    }
    class Recording {
        observe()
        maintain()
        status()
    }
    class Feed {
        connect()
        subscribe()
        poll()
        snapshot()
    }
    class Catalog {
        refresh()
        poll()
    }
    class IntradayBars
    class Recorder
    class Compactor
    class RecordedPublication
    class Manager
    class QuoteStream
    RpcHost ..> Market : 每个请求帧
    Market *-- Live : 行情线程
    Market *-- Recording : 行情线程
    Market *-- Manager
    Market *-- QuoteStream : 每条报价流
    Live *-- Feed
    Live *-- Catalog
    Live *-- IntradayBars
    Recording *-- Recorder
    Recording *-- Compactor
    Recording *-- RecordedPublication
    Recording ..> IntradayBars : 只读
    RecordedPublication --> Manager
```

| 调用 | 路径 | 权限 |
| --- | --- | --- |
| `get_status`、`get_board`、`get_minutes` | 行情线程 | `read` |
| `stream_quotes` | 行情线程每 250ms 产出一帧 | `read` |
| `start_acquisition`、`cancel_acquisition` | 读取线程池 | `manage` |
| 其余调用 | 读取线程池 | `read` |

采集与仓库分开：来源适配器负责供应商数据转换，`Manager` 负责任务与提交，`Store` 负责固定版本。两个 `Reader` 分别读录制归档和正式仓库，不要混淆。

```mermaid
classDiagram
    class Manager
    class Source {
        <<abstract>>
        supports()
        daily()
        minutes()
    }
    class Tushare
    class Exchange
    class Recorded
    class RecordingReader["recording::Reader"]
    class WarehouseReader["warehouse::Reader"]
    class Store {
        current()
        version()
        writer()
    }
    class Writer {
        put()
        merge_days()
        adopt()
        commit()
    }
    Source <|-- Tushare
    Source <|-- Exchange
    Source <|-- Recorded
    Manager ..> Source : 为任务创建
    Manager *-- Store : 来源与 research
    Manager ..> Writer
    Recorded ..> RecordingReader
    WarehouseReader ..> Store
    Writer --> Store : 锁定写入并发布
```

## 实时通信时序

服务启动时自行登录、读取目录缓存并刷新目录，随后订阅全市场；这些操作不依赖客户端打开页面。合约查询使用 CTP 交易接口，入口是 `CatalogQuery`；行情接收入口是 `Feed`。

行情线程每 10ms 按固定顺序走一轮：

```mermaid
sequenceDiagram
    participant SDK as CTP SDK
    participant Live
    participant Recording
    participant Stream as QuoteStream
    participant RPC as RpcHost
    participant Client as Terminal 原生层
    SDK->>Live: 回调入队
    Note over Live: 1. maintain()：登录重试、目录刷新、订阅
    Live->>Live: 2. poll()：取走本轮的连接变化和报价
    loop 每个事件
        Live->>Recording: observe(事件)
        Live->>Live: apply(事件)：更新分钟线
    end
    Note over Recording: 3. maintain()：落盘、盘后合并、发布
    Note over Live: 4. 回答排队的 get_status / get_board / get_minutes
    Live->>Stream: 5. 到期的报价流放入下一帧
    Client->>RPC: Request.stream_quotes
    RPC-->>Client: Response.stream_quotes.full
    loop 同一条连接
        RPC->>Stream: 取走下一帧
        RPC-->>Client: Response.stream_quotes.changed
    end
    Note over Client,Stream: 重开连接取得完整首帧；客户端筛选不改变全市场订阅
```

`Recording::observe()` 在 `Live::apply()` 之前调用，是为了先保留换交易日前的旧分钟序列。报价乱序仍录制，但不推进实时分钟聚合。页面通过原生层取走报价帧；原生层持有 TLS 连接，页面按生成的 Protobuf 类型合并报价。

## 采集、发布与固定版本读取

```mermaid
sequenceDiagram
    participant Client as Terminal / Lab
    participant RPC as RpcHost / Market
    participant Manager
    participant Source
    participant Writer
    participant Research as compose_research
    Client->>RPC: start_acquisition
    RPC->>RPC: 由已验证证书检查 manage 权限
    RPC->>Manager: submit(request)
    Manager->>Manager: 持久化 QUEUED 任务
    Manager-->>Client: 经 RPC 返回已接纳任务
    Manager->>Source: 后台读取供应商或自录数据
    Source-->>Manager: 标准化行与观测段
    Manager->>Writer: put() / merge_days()
    Manager->>Writer: commit()
    Writer->>Writer: 分区文件 → 月度分片 → 版本清单 → current
    Manager->>Research: 合成 research
    Research->>Research: 持有研究写锁，读取来源版本并发布
    Manager->>Manager: 持久化任务进度或完成状态
    Client->>RPC: get_dataset_version(research，当前版本)
    RPC-->>Client: 固定版本 ID、分区与摘要
    loop 分页与文件下载
        Client->>RPC: get_dataset_version / read_dataset_file，携带同一版本 ID
        RPC-->>Client: 该版本的元数据或 Parquet 字节块
    end
```

录制自动发布复用上述采集路径：`Compactor` 报告完成日期，`RecordedPublication` 将日期保存到 `recordings/unpublished.json`，再提交 `recorder` 任务。任务成功且期间没有再次合并才移除待办；失败保留待办并延后再试，重启重新读取待办。理由：录制文件已存在后，研究发布可以独立补做。

`research` 的选择规则只维护在[功能文档](market.md#仓库与固定版本)。`Writer::adopt()` 用硬链接引用选中的分区，旧版本仍引用原文件；无内容变化不生成新版本。

## 生命周期与错误边界

| 情况 | 行为与代码入口 |
| --- | --- |
| 客户端关闭 | 释放该连接与它的报价流；服务仍录制，已接纳的采集继续，见 `market.cpp` |
| 行情中断 | 中断分钟观测、结束录制段；重连后继续，见 `Live::apply()`、`Recording::observe()` |
| SDK 入口丢事件 | 结束本次服务运行，由 systemd 重启，见 `Feed::poll()` 与 `Market` 的行情线程 |
| 磁盘积压 | 录制丢弃积压区间并报告丢失计数，行情继续，见 `Recorder::flush()` |
| 交易日结束 | 收尾分钟落盘、盘后合并、提交发布待办，见 `Recorder`、`Compactor`、`RecordedPublication` |
| 服务正常停止 | 报价流发出最后一帧、结束后台资源、写完录制缓冲，见 `main.cpp` 与 `Recording` 析构 |
| 服务重新启动 | 恢复合并日志、读取发布待办；旧采集任务标为中断，见 `Market`、`Manager` |

## 代码阅读路线

| 顺序 | 入口 | 阅读目标 |
| --- | --- | --- |
| 1 | [main.cpp](../apps/services/market/main.cpp)、[settings.cpp](../apps/services/market/settings.cpp) | 配置、数据目录锁、TLS 身份、权限和停止流程 |
| 2 | [market.cpp](../apps/services/market/market.cpp) | 调用路径表、行情线程的一轮、报价流 |
| 3 | [live.cpp](../apps/services/market/live.cpp)、[ctp_feed.cpp](../apps/services/common/market/ctp/ctp_feed.cpp)、[catalog.hpp](../apps/services/market/catalog.hpp) | 登录与目录刷新、全市场订阅、报价帧 |
| 4 | [recording.cpp](../apps/services/market/recording.cpp)、[intraday_bars.cpp](../core/src/futures/intraday_bars.cpp)、[recorder.cpp](../apps/services/market/recording/recorder.cpp) | 录制观测段、分钟完整性、盘后合并的时机 |
| 5 | [compaction.cpp](../apps/services/market/recording/compaction.cpp)、[recorded_publication.cpp](../apps/services/market/acquisition/recorded_publication.cpp) | 归档替换与发布恢复 |
| 6 | [manager.cpp](../apps/services/market/acquisition/manager.cpp)、[source.hpp](../apps/services/market/acquisition/source.hpp) | 来源扩展、任务生命周期与批量提交 |
| 7 | [store.cpp](../apps/services/market/warehouse/store.cpp)、[research.cpp](../apps/services/market/warehouse/research.cpp) | 不可变版本、月度分片、来源合成 |
| 8 | [通信协议](protocol.md)、[market.proto](../protocol/proto/asterion/market.proto) | 客户端契约与固定版本读取 |

改动后的行为验证从 [tests/market](../tests/market) 对应测试开始；跨客户端与重启行为看 [concurrency_process.py](../tests/market/concurrency_process.py)，真实行情验收入口是 [ctp_market.py](../tests/acceptance/ctp_market.py)。协议或上述所有权发生变化时，同步更新对应图与说明。
