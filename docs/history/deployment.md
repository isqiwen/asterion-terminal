# History 部署

History 接收全市场行情并管理历史仓库。服务由 systemd 看护；Terminal 关闭、断线或升级都不影响它。

## 安装

History 主机是 Ubuntu 24.04 x86_64，安装在 `/opt/asterion/history/`。发布物是一个自解压脚本 `Asterion-History-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_history_setup`；本地用 `scripts/services/build_linux_services.sh` 构建，输出到 `build/linux-bundles/`），包含服务程序、安装脚本、默认日志配置和两个 CTP 库；服务运行不依赖 Python，安装脚本使用主机的 Python 3 处理配置。

```sh
sudo bash "Asterion-History-<版本>-linux-x86_64.sh"         # 第一次：安装并配置；以后：升级
sudo /opt/asterion/history/bin/setup.sh     # 只修改配置
```

- 安装时写入并启用 `asterion-history.service`：以执行 `sudo` 的账户运行，只能写 `logs/` 和数据目录，`Restart=always`。
- 配置时依次询问监听地址、端口、数据目录（默认 `/opt/asterion/history/data`，可以放到单独的磁盘）、行情和交易前置、经纪公司代码、投资者代码、AppID（回车保留当前值），登记客户端证书（`<指纹>=read` 或 `<指纹>=manage`），选择交易所来源和 Tushare 每分钟请求数，最后隐藏输入 CTP 密码、授权码和 Tushare token。凭据的保存位置见下方目录结构。
- 升级只替换程序和库，保留配置、TLS 身份、仓库、采集任务和合约快照。交易时段内（北京时间工作日 08:45–15:30、20:45–次日 02:45）不重启正在运行的服务，改为收盘后由定时器重启。
- 安装时生成服务端证书，脚本显示它的指纹，供 Terminal 登记。权限修改在重启后生效。

## 部署目录

默认布局如下；数据目录中的文件按采集进度生成。`history.json` 的 `data` 可以指向独立磁盘，程序、配置和日志仍留在安装目录。

```text
/opt/asterion/history/
├── bin/
│   ├── asterion_history          # 服务程序
│   └── setup.sh                 # 修改配置；升级使用新的发布安装包
├── lib/
│   ├── ctp-md.so                # CTP 行情接口
│   └── ctp-trader.so            # 查询合约目录等柜台信息
├── config/
│   ├── history.json             # 安装时生成的非秘密配置
│   ├── logging.json            # 日志输出、格式和保留期限
│   └── tls/
│       ├── server.crt          # 服务端证书
│       └── server.key          # 服务端私钥
├── logs/                       # 默认按日保存服务日志
└── data/                       # 可通过配置改到其他绝对路径
    ├── history.lock             # 同一份数据只允许一个 History 进程
    ├── ctp-catalog.pb          # 合约目录缓存
    ├── acquisition/
    │   └── <任务ID>.pb         # 采集请求、进度和结果
    ├── catalog/
    │   └── <YYYYMMDD>.pb       # 每个交易日的合约快照
    ├── warehouse/              # Tushare 正式仓库
    └── sources/
        ├── ctp/warehouse/      # 合约快照入库后的正式仓库
        ├── research/warehouse/ # 合成研究数据集
        ├── gtja/warehouse/     # 期货公司报表的历史报价单位
        ├── sina/warehouse/     # 新浪财经补缺的行情
        ├── shfe/warehouse/
        ├── ine/warehouse/
        ├── czce/warehouse/
        ├── dce/warehouse/
        ├── cffex/warehouse/
        └── gfex/warehouse/     # 各交易所独立的正式仓库
```

`warehouse/` 是已发布的固定版本。每份正式仓库包含 Parquet 分区文件、`versions/` 版本清单、`shards/` 清单分片和记录当前版本标识的 `current` 文件，版本规则见 [History 仓库与固定版本](README.md#仓库与固定版本)。

安装目录之外的文件：

```text
/etc/systemd/system/
└── asterion-history.service

/etc/credstore.encrypted/
├── asterion.ctp.password
├── asterion.ctp.auth_code          # 配置授权码时生成
└── asterion.history.tushare.token   # 配置 Tushare token 时生成
```

CTP 密码、授权码和 Tushare token 由 `systemd-creds` 加密保存，不写入 `history.json`。systemd 启动服务时提供解密后的凭据目录，程序通过 `CREDENTIALS_DIRECTORY` 读取；TLS 私钥单独保存在 `config/tls/server.key`。

## 配置

`config/history.json` 只保存非秘密配置：

```json
{
  "bind": "0.0.0.0",
  "port": 7450,
  "data": "/opt/asterion/history/data",
  "clients": [{"fingerprint": "<客户端证书的 SHA-256 指纹>", "role": "manage"}],
  "acquisition": {"exchanges": ["SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"], "tushare": null},
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

- `data` 是仓库、采集任务、合约快照和目录缓存所在的目录，必须是绝对路径。改目录请用 `setup.sh`：它创建新目录并允许服务写入；旧目录里的数据不搬动，要继续使用就先停服务再自己移过去。
- `tushare` 为 `null` 时不采集 Tushare，否则为 `{"requests_per_minute": 100}`，token 来自主机凭据 `asterion.history.tushare.token`。
- `exchanges` 启用对应交易所的公开来源及[历史报价单位](README.md#历史最小变动价位)来源；非郑商所品种自动启用 `gtja`，无需额外凭据。
- 缺少配置、TLS 文件或已启用来源的凭据时拒绝启动，并报告路径；多出不认识的键同样拒绝。
- 默认日志在 `logs/asterion_history_YYYY-MM-DD.log`，保留 30 天；输出方式由 `config/logging.json` 控制。默认关闭控制台日志，`journalctl -u asterion-history -f` 用于查看服务启停和标准输出、错误，业务日志应读取日志文件。

## 运行

- systemd 直接启动 `/opt/asterion/history/bin/asterion_history`，程序自动定位旁边的配置和库，不需要传入 `--installation`。
- 默认监听 `0.0.0.0:7450`；Terminal 和 Lab 通过双向 TLS 连接，客户端指纹与权限配置见 [Terminal 和 Lab 的连接](README.md#terminal-和-lab-的连接)。
- 启动后立即登录 CTP，不等客户端。行情登录被拒时每分钟重试一次（只涉及行情，不涉及交易）。
- 收到 SIGTERM 后停止接收，结束后台任务再退出。原始行情队列写满时进程退出，由 systemd 重启。

## 合约快照

每次查询合约目录（登录和每个新交易日）后，结果按交易日保存到 `catalog/<YYYYMMDD>.pb`，含合约、乘数和最小价位，永久保留。交易日结束后自动入库到 `ctp` 来源，作为当天的历史报价单位，不依赖外部来源；失败时这一天每小时重试，原因写进主机日志。
