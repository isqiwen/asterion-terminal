# 配置文件

配置文件按所属程序存放在各子项目的 `config/` 目录，仓库根目录不放配置文件。`*.example.json` 仅展示结构，不随安装包发布；其中的地址、账号与证书指纹均为占位值，真实凭据全部交由 systemd 加密保存。

| 程序 | 仓库文件 | 使用方式 |
| --- | --- | --- |
| Terminal | [logging.json](../apps/clients/terminal/config/logging.json) | 随桌面打包，首次启动写入 `terminal/config/logging.json` |
| Core / Market / Trading / Terminal | [futures-sessions.json](../core/config/futures-sessions.json) | 编译期嵌入 C++ 核心并由前端构建导入，统一实时分钟边界与分时图横轴 |
| Lab | [logging.json](../apps/services/lab/config/logging.json) | 桌面携带为 `config/lab-logging.json`，首次启动写入 `lab/config/logging.json` |
| Market | [market.example.json](../apps/services/market/config/market.example.json) | 展示安装脚本生成的 `config/market.json` 结构 |
| Market | [logging.json](../apps/services/market/config/logging.json) | 随安装包发布，首次安装复制为 `config/logging.json`，升级保留已有文件 |
| Warehouse | [warehouse.example.json](../apps/services/warehouse/config/warehouse.example.json) | 展示安装脚本生成的 `config/warehouse.json` 结构 |
| Warehouse | [exchange_rules.json](../apps/services/warehouse/config/exchange_rules.json) | 随安装包发布，保存经核验的六所历史最小变动价位与大商所结算计费规则；升级时若本机核验截止日更晚则保留较晚截止日 |
| Warehouse | [logging.json](../apps/services/warehouse/config/logging.json) | 随安装包发布，首次安装复制为 `config/logging.json`，升级保留已有文件 |
| Trading | [account.example.json](../apps/services/trading/config/accounts/account.example.json) | 展示安装脚本生成的 `config/accounts/<账户>.json` 结构 |
| Trading | [logging.json](../apps/services/trading/config/logging.json) | 随安装包发布，首次安装复制为 `config/logging.json`，升级保留已有文件 |

Lab 的研究任务全部通过通信协议提交，没有单独的任务配置文件。日志配置字段见[运行日志](services.md#运行日志)，控制台与文件输出均支持可选 `pattern`。

## Market

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 服务监听地址与端口 |
| `data` | 已存在的数据目录绝对路径 |
| `counter.market_front`、`trade_front` | CTP 行情前置与交易前置地址（交易前置仅用于查询合约目录） |
| `counter.broker`、`user`、`app_id` | 经纪商代码、行情账号与应用标识（`app_id` 可为空） |
| `clients[].fingerprint` | 允许连接的客户端证书 SHA-256 指纹 |

密码与授权码不写入 JSON：服务从 systemd 凭据目录读取 `asterion.ctp.password`，`app_id` 非空时另读 `asterion.ctp.auth_code`。

## Warehouse

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 服务监听地址与端口 |
| `data` | 已存在的数据目录绝对路径 |
| `clients[].fingerprint` | 允许连接的客户端证书 SHA-256 指纹（已登记客户端均可读取数据与管理采集） |
| `acquisition.exchanges` | 启用公开来源采集与内置规则核验的交易所列表，可为空（非空时自动启用新浪补缺，含非郑商所时自动启用国泰君安报价单位核验） |
| `acquisition.tushare` | `null` 表示关闭；开启时填写 `requests_per_minute`（1–500） |
| `acquisition.update_at` | 每日自动更新的北京时间 `HH:MM`，`null` 表示关闭 |

Warehouse 不连接柜台，不持有任何 CTP 凭据。开启 Tushare 时，服务从 systemd 凭据目录读取 `asterion.warehouse.tushare.token`，token 不写入 JSON。

## Trading

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 该账户实例的监听地址与端口 |
| `environment` | `simulation` 或 `real` |
| `broker.front`、`broker_id`、`user_id`、`app_id` | CTP 交易前置、经纪商代码、投资者账号与应用标识 |
| `market_front` | Trading 自身直连的行情前置地址 |
| `strategy_memory_limit_mb` | 策略内存上限（MiB，范围 `512–65536`，默认 `2048`），同时作用于策略子进程与 systemd cgroup `MemoryMax` |
| `terminals` | 允许连接的 Terminal 证书 SHA-256 指纹列表 |

配置文件名决定 `--account` 的账户实例名。柜台密码与授权码由 systemd 分别以 `password`、`auth_code` 提供，可选脱机告警地址由加密凭据 `asterion.trading.<账户>.webhook` 提供；风控政策与交易授权通过 Terminal 管理，不写入账户配置文件。

## 交易时段表

`core/config/futures-sessions.json` 中，`day_sessions` 与 `night_sessions` 定义各交易时段，`products` 将交易所品种映射到对应时段，`utc_offset` 指定时区；`source` 记录参考来源与核对日期，`version` 标识格式版本。
