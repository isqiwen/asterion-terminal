# Lab 部署与运行

Lab 随 Terminal 发布，在当前用户本机运行，由 Terminal 自动启动与重启。没有独立安装包或 systemd 服务；日常使用不需要手动传启动参数。

## 程序与运行时

Terminal 首次启动时准备 CPython 与研究依赖，之后按运行时清单更新。开发启动直接使用项目中的 Python 源码，安装版使用随包携带的源码；准备过程见[Python 运行时](../development.md#python-运行时)。

Lab 使用 `python3 -I -B -m asterion_lab` 启动。`LabSupervisor` 提供拥有者 PID、业务和健康套接字、研究记录目录及 Terminal 连接配置目录。理由：解释器、目录和生命周期由同一个宿主管理，避免手工启动使用另一套数据。

## 本机目录

下图的 `<根>` 在 Linux 为 `~/.local/share/asterion`，在 macOS 为 `~/Library/Application Support/Asterion`。

```text
<根>/
├── research/
│   ├── strategies/                 # 策略源码
│   └── factors/                    # 因子源码
├── terminal/
│   ├── runtime/                    # 安装版准备的研究运行时
│   └── data/
│       ├── warehouse.json            # 共享的 Warehouse 地址与服务指纹
│       └── tls/                    # 本机客户端身份
└── lab/
    ├── config/logging.json
    ├── logs/
    └── data/lab/
        ├── warehouse-cache/research/  # 固定版本清单与分区缓存
        ├── sources/                # 研究接纳时复制的源码
        ├── factors/<评价ID>.binpb  # 因子评价结果
        ├── runs.sqlite             # 运行状态和指标索引
        └── runs/<运行ID>/
            ├── run.json            # 输入、版本、参数与复现信息
            └── <表>.parquet        # 权益、委托、成交等结果
```

缓存按查询范围下载，属于可重建资料；源码副本和运行结果用于复现，备份时应保留。研究页可以删除运行记录和因子评价，连同结果表和源码副本；Lab 启动时删除没有记录引用的源码副本，理由：每次接纳都复制一份源码，记录删除后副本不再有用。策略目录的内置模块随发布版本同步，用户模块保留，具体规则见[Lab 策略](README.md#策略)。

业务套接字 `lab.sock` 与健康套接字 `lab.health` 位于每次运行私有的 `/tmp/ast-<实例>/`，不属于长期数据。

## 配置与连接

- 日志配置来自 [logging.json](../../apps/services/lab/config/logging.json)，首次启动复制，字段见[运行日志](../services.md#运行日志)。
- Warehouse 地址和证书由 Terminal 设置维护；Lab 使用同一客户端身份，经双向 TLS 读取 `research` 固定版本。无需单独填写供应商配置或密钥。
- 回测、扫描和因子设置通过协议提交，不保存为另一份服务配置。

## 停止、升级与恢复

| 操作或情况 | 处理方式 |
| --- | --- |
| 正常退出 | 等待需要保留的计算完成，再关闭 Terminal |
| Lab 意外退出 | Terminal 自动重启；遗留运行标为中断，由使用者决定是否重新运行 |
| Terminal 升级 | 先结束研究并关闭应用，再安装新版本，研究记录保留 |
| 缓存缺失 | 读取时从原 Warehouse 版本重新下载；该版本不可用时明确失败 |
| 备份 | 关闭 Terminal 并确认 Lab 已退出，复制源码、运行记录和源码副本 |

完整冷备份范围见[备份与恢复](../services.md#备份与恢复)。排查顺序是研究状态、通知中的诊断、`lab/logs/`；数据访问失败同时核对 Warehouse 的连接配置与版本是否可读取。
