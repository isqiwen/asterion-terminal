# Market 录制与部署

录制是 Market 服务的一部分（`apps/services/market/recording/`）：同一个进程接收全市场行情，把 Tick 和分钟线写成 Parquet。服务由 systemd 看护；Terminal 关闭、断线或升级都不影响它。

## 安装

Market 主机是 Ubuntu 24.04 x86_64，安装在 `/opt/asterion/market/`。发布物是一个自解压脚本 `Asterion-Market-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_market_setup`；本地用 `scripts/services/build_linux_services.sh` 构建，输出到 `build/linux-bundles/`），包含服务程序、安装脚本、默认日志配置和两个 CTP 库；服务运行不依赖 Python，安装脚本使用主机的 Python 3 处理配置。

```sh
sudo bash "Asterion-Market-<版本>-linux-x86_64.sh"         # 第一次：安装并配置；以后：升级
sudo /opt/asterion/market/bin/setup.sh     # 只修改配置
```

- 安装时写入并启用 `asterion-market.service`：以执行 `sudo` 的账户运行，只能写 `logs/` 和数据目录，`Restart=always`。
- 配置时依次询问监听地址、端口、数据目录（默认 `/opt/asterion/market/data`，可以放到单独的磁盘）、Tick 保留的交易日数（默认 30）、行情和交易前置、经纪公司代码、投资者代码、AppID（回车保留当前值），登记客户端证书（`<指纹>=read` 或 `<指纹>=manage`），选择交易所来源和 Tushare 每分钟请求数，最后隐藏输入 CTP 密码、授权码和 Tushare token。凭据的保存位置见下方目录结构。
- 升级只替换程序和库，保留配置、TLS 身份、仓库、采集任务和录制文件。交易时段内（北京时间工作日 08:45–15:30、20:45–次日 02:45）不重启正在运行的服务，改为收盘后由定时器重启。
- 安装时生成服务端证书，脚本显示它的指纹，供 Terminal 登记。权限修改在重启后生效。

## 部署目录

默认布局如下；数据目录中的文件按录制、采集和发布进度生成。`market.json` 的 `data` 可以指向独立磁盘，程序、配置和日志仍留在安装目录。

```text
/opt/asterion/market/
├── bin/
│   ├── asterion_market          # 服务程序
│   └── setup.sh                 # 修改配置；升级使用新的发布安装包
├── lib/
│   ├── ctp-md.so                # CTP 行情接口
│   └── ctp-trader.so            # 查询合约目录等柜台信息
├── config/
│   ├── market.json             # 安装时生成的非秘密配置
│   ├── logging.json            # 日志输出、格式和保留期限
│   └── tls/
│       ├── server.crt          # 服务端证书
│       └── server.key          # 服务端私钥
├── logs/                       # 默认按日保存服务日志
└── data/                       # 可通过配置改到其他绝对路径
    ├── market.lock             # 同一份数据只允许一个 Market 进程
    ├── ctp-catalog.pb          # 合约目录缓存
    ├── acquisition/
    │   └── <任务ID>.pb         # 采集请求、进度和结果
    ├── recordings/             # 原始录制归档
    │   ├── ticks/
    │   ├── minutes/
    │   ├── segments/           # 连续观测段与中断原因
    │   ├── catalog/            # 每个交易日的合约快照
    │   └── unpublished.json    # 等待发布的交易日
    ├── warehouse/              # Tushare 正式仓库
    └── sources/
        ├── recorder/warehouse/ # 自录发布后的正式仓库
        ├── research/warehouse/ # 合成研究数据集
        ├── shfe/warehouse/
        ├── ine/warehouse/
        ├── czce/warehouse/
        ├── dce/warehouse/
        ├── cffex/warehouse/
        └── gfex/warehouse/     # 各交易所独立的正式仓库
```

`recordings/` 是录制归档，`warehouse/` 是已发布的固定版本；自录数据如何进入 `recorder` 和 `research` 见[发布研究版本](#发布研究版本)。每份正式仓库包含 Parquet 分区文件、`versions/` 版本清单、`shards/` 清单分片和记录当前版本标识的 `current` 文件，版本规则见 [Market 仓库与固定版本](README.md#仓库与固定版本)。

安装目录之外的文件：

```text
/etc/systemd/system/
└── asterion-market.service

/etc/credstore.encrypted/
├── asterion.ctp.password
├── asterion.ctp.auth_code          # 配置授权码时生成
└── asterion.market.tushare.token   # 配置 Tushare token 时生成
```

CTP 密码、授权码和 Tushare token 由 `systemd-creds` 加密保存，不写入 `market.json`。systemd 启动服务时提供解密后的凭据目录，程序通过 `CREDENTIALS_DIRECTORY` 读取；TLS 私钥单独保存在 `config/tls/server.key`。

## 配置

`config/market.json` 只保存非秘密配置：

```json
{
  "bind": "0.0.0.0",
  "port": 7450,
  "data": "/opt/asterion/market/data",
  "recording": {"tick_days": 30},
  "clients": [{"fingerprint": "<客户端证书的 SHA-256 指纹>", "role": "manage"}],
  "acquisition": {"exchanges": ["SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"], "tushare": null},
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

- `data` 是仓库、录制、采集任务和合约目录缓存所在的目录，必须是绝对路径。改目录请用 `setup.sh`：它创建新目录并允许服务写入；旧目录里的数据不搬动，要继续使用就先停服务再自己移过去。
- `tushare` 为 `null` 时不采集 Tushare，否则为 `{"requests_per_minute": 100}`，token 来自主机凭据 `asterion.market.tushare.token`。
- `exchanges` 启用对应交易所的公开来源及[历史报价单位](README.md#历史最小变动价位)来源；非郑商所品种自动启用 `gtja`，无需额外凭据。
- 缺少配置、TLS 文件或已启用来源的凭据时拒绝启动，并报告路径；多出不认识的键同样拒绝。
- 默认日志在 `logs/asterion_market_YYYY-MM-DD.log`，保留 30 天；输出方式由 `config/logging.json` 控制。默认关闭控制台日志，`journalctl -u asterion-market -f` 用于查看服务启停和标准输出、错误，业务日志应读取日志文件。

## 运行

- systemd 直接启动 `/opt/asterion/market/bin/asterion_market`，程序自动定位旁边的配置和库，不需要传入 `--installation`。
- 默认监听 `0.0.0.0:7450`；Terminal 和 Lab 通过双向 TLS 连接，客户端指纹与权限配置见 [Terminal 和 Lab 的连接](README.md#terminal-和-lab-的连接)。
- 启动后立即登录 CTP，不等客户端。行情登录被拒时每分钟重试一次（只涉及行情，不涉及交易）。
- 收到 SIGTERM 后停止接收，写完录制缓冲再退出。原始行情队列写满时进程退出，由 systemd 重启。

## 录制文件

文件在数据目录的 `recordings/` 下，按柜台给的交易日分区，可以直接用 DuckDB 读取：

```
recordings/ticks/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/minutes/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/segments/trading_day=<YYYYMMDD>/<实例>.parquet
recordings/catalog/<YYYYMMDD>.pb
```

- **ticks**：分批写入；价格是精确小数，缺失保持为空；乱序报价也写入并标记。另存柜台的结算价、收盘价和成交额，成交额保持柜台的浮点值，理由：单日成交额可能超出精确小数的范围。
- **minutes**：与实时分钟线是同一份聚合。`partial` 表示这一分钟的观测可能不完整（从分钟中间开始观测、期间丢过观测、累计量回退、停止时还在形成）。一根分钟线在下一根出现或交易日结束（北京时间 15:30）后写入，所以收盘前的最后一分钟在合并前就已写入文件。
- **catalog**：每个交易日最后一次成功查询到的 CTP 合约目录，含合约、乘数和最小价位，永久保留。
- **segments**：一段是一次不间断的观测，记首末时间、报价数和结束原因：断线（如 `reconnecting`）、`events_lost`、`feed_failed`、`write_backlog`（磁盘跟不上，这一段时间的数据被丢弃）、`stopped`。两段之间就是缺口，不补造。
- 写入异步执行。磁盘写入积压达到上限时，丢弃待写区间的 Tick 并计入丢失行数，实时行情继续处理；分钟线留在内存，等磁盘恢复后写入。重启后以新的实例名续写，不改已有文件；同一分钟内重启留下的两段分钟线在读取和发布时合成一根，并标记 `partial`。

**保留**：Tick 只保留最近 `recording.tick_days` 个已录制的交易日。每天合并完成后，更早交易日的 Tick 目录被删除并记入日志；分钟线和观测段永久保留，已发布的 `recorder` 研究版本不受影响。

**收盘后合并**：一个交易日的交易结束、并且当天的文件 30 分钟没有新写入后，当天的 Tick 和分钟文件合并为每个交易所一个文件。合并先写新文件并核对行数，再记日志、删原文件；中途中断时下次启动完成或放弃这次合并，原数据不丢。某一天合并失败时一小时后重试，不影响实时接收和其他日期，这一天的 Tick 也不会被清理。

## 读取录制

Market 的 TLS 端口上（见[通信协议](../protocol.md#7-market)）：

- `get_status` 的 `recording`：交易日、写入和丢失计数、磁盘余量、合并状态。
- `list_recorded_days`：一段日期内每个交易日的观测段，直接读录制目录，包括还没发布的当天。
- `get_minutes`：本交易日一个合约的分钟线，来自 Market 内存。
- 更早的分钟线和观测段：读 `recorder` 来源或研究数据集的固定版本，按分区下载（`get_dataset_version`、`read_dataset_file`）。

## 发布研究版本

每个交易日合并完成后，Market 自动把当天全部合约的录制分钟线、对应观测段和当天的合约快照提交为 `recorder` 来源的新版本，不依赖任何外部来源，再合成研究数据集（见 [Market](README.md#仓库与固定版本)）：Tushare 没有的合约交易日就用这些录制数据。待发布的交易日记在 `recordings/unpublished.json`，发布成功后才移除，所以重启后会接着发布；采集正忙时每分钟重试；某一天发布失败时这一天每小时重试，不影响其他日期，原因写进主机日志。收盘后迟到的报价再次合并时，这一天会重新发布。

- 也可以在 Terminal「数据」页选择「自录」来源，按合约和交易日范围手动发布或重新发布（需要 `manage` 权限）。
- 柜台给出的交易日、`partial` 标记和缺失字段原样保留，不补缺口。
- 录制保留柜台的 8 位小数，研究数据最多 6 位。成交均价是成交额除以成交量和合约乘数，发布时按银行家舍入到 6 位；其余价格超过 6 位时整次发布失败，不舍入。
- 每次发布生成新版本，旧版本保留；合并与发布互斥，实时录制不等待。
