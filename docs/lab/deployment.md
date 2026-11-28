# Lab 部署与运行

Lab 随 Terminal 桌面端一并发布，在当前用户本机运行，由 Terminal 自动启动和看护。它没有独立安装包或 systemd 单元，日常使用无需手动传参启动。

## 程序与运行时

Terminal 首次启动时准备可重定位 CPython 3.12 与锁定研究依赖，后续随版本清单自动更新。开发模式直接加载仓库 Python 源码，安装版使用随包分发的源码副本，详情见[Python 运行时](../development.md#python-运行时)。

Lab 以 `python3 -I -B -m asterion_lab` 启动，由 `LabSupervisor` 统一传入拥有者 PID、业务与健康套接字路径、研究记录目录及 Terminal 连接配置目录。理由：解释器、数据目录与生命周期由同一宿主托管，避免手动启动误用另一套目录。

## 本机目录

下文 `<根>` 在 Linux 为 `~/.local/share/asterion`，在 macOS 为 `~/Library/Application Support/Asterion`：

```text
<根>/
├── research/
│   ├── strategies/                 # 策略源码
│   └── factors/                    # 因子源码
├── terminal/
│   ├── runtime/                    # 安装版准备的研究运行时
│   └── data/
│       ├── warehouse.json          # 共享的 Warehouse 地址与服务端指纹
│       └── tls/                    # 本机客户端证书与私钥
└── lab/
    ├── config/logging.json
    ├── logs/
    └── data/lab/
        ├── warehouse-cache/research/  # 固定版本清单与 Parquet 分区缓存
        ├── sources/                   # 研究接纳时冻结的源码副本
        ├── factors/<评价ID>.binpb     # 因子评价结果
        ├── runs.sqlite                # 运行状态与指标索引
        └── runs/<运行ID>/
            ├── run.json               # 输入、版本、参数与复现信息
            └── <表>.parquet           # 权益、委托、成交等明细表
```

数据缓存按需从 Warehouse 拉取，可随时删除重建；策略源码、运行记录与源码副本用于复现，备份时须完整保留。研究页删除运行或因子评价时会连带清理结果与对应源码副本；Lab 每次启动也会回收无记录引用的孤立源码副本。策略目录中的内置模块随版本同步更新、用户模块保留，详见[Lab 策略](README.md#策略)。

业务套接字 `lab.sock` 与健康套接字 `lab.health` 位于每次运行私有的 `/tmp/ast-<实例>/`，不属于持久数据。

## 配置与连接

- 日志配置源自 [logging.json](../../apps/services/lab/config/logging.json)，首次启动自动复制，字段说明见[运行日志](../services.md#运行日志)。
- Warehouse 地址与客户端证书由 Terminal 设置页维护；Lab 复用同一身份经双向 TLS 读取固定版本，无需单独配置数据源密钥。
- 回测、参数扫描与因子评价设置均通过通信协议提交，无独立任务配置文件。

## 停止、升级与恢复

| 情况 | 处理方式 |
| --- | --- |
| 正常退出 | 等待需要保留的计算完成后再关闭 Terminal |
| Lab 意外退出 | Terminal 自动重启；遗留的运行中记录标为中断，由使用者决定是否重跑 |
| Lab 心跳延迟 | 冷启动期间的研究请求会等待 Lab 首次应答。启动超时或已就绪的 Lab 连续心跳失败时，Terminal 报错并重启 Lab |
| Terminal 升级 | 等待研究结束并关闭应用后安装新版；记录结构不变时保留既有记录 |
| 开发期研究索引结构过期 | 关闭 Terminal 后清理 `runs.sqlite`、同名 `-wal`/`-shm` 文件和 `runs/` 可重置运行记录；重启建立空索引，运行历史、指标和扫描分组不会恢复。理由：当前开发版本只保留一种记录结构 |
| 本地缓存缺失 | 查询时按原固定版本从 Warehouse 重新下载；若原版本不可用则明确报错 |
| 本机备份 | 关闭 Terminal 并确认 Lab 退出后，备份策略目录、运行记录与源码副本 |

完整冷备范围见[备份与恢复](../services.md#备份与恢复)。排查异常依次查看研究状态、通知中心诊断与 `lab/logs/`；若涉及读数故障，同步核对 Warehouse 连接状态与目标版本是否可读。
