# 配置文件

配置按所属程序保存在子项目的 `config/`，仓库根目录不放配置。`*.example.json` 只展示文件结构，不随安装包发布；地址、账户和证书指纹都是占位值，真实凭据由 systemd 加密保存。

| 程序 | 仓库文件 | 使用方式 |
| --- | --- | --- |
| Terminal | [logging.json](../apps/clients/terminal/config/logging.json) | 桌面打包，首次启动写入 `terminal/config/logging.json` |
| Core / Market / Trading / Terminal | [futures-sessions.json](../core/config/futures-sessions.json) | 编译期嵌入 C++ 核心并由界面构建导入，统一实时分钟边界与分时图横轴 |
| Lab | [logging.json](../apps/services/lab/config/logging.json) | 桌面携带为 `config/lab-logging.json`，首次启动写入 `lab/config/logging.json` |
| Market | [market.example.json](../apps/services/market/config/market.example.json) | 展示安装脚本生成的 `config/market.json` |
| Market | [logging.json](../apps/services/market/config/logging.json) | 随服务安装包发布，首次安装复制为 `config/logging.json`，升级保留已有配置 |
| Warehouse | [warehouse.example.json](../apps/services/warehouse/config/warehouse.example.json) | 展示安装脚本生成的 `config/warehouse.json` |
| Warehouse | [exchange_rules.json](../apps/services/warehouse/config/exchange_rules.json) | 随服务安装包发布，保存经核验的六所历史最小变动价位与大商所结算计费规则；升级时若本机核验截止日更晚则保留较晚截止日 |
| Warehouse | [logging.json](../apps/services/warehouse/config/logging.json) | 随服务安装包发布，首次安装复制为 `config/logging.json`，升级保留已有配置 |
| Trading | [account.example.json](../apps/services/trading/config/accounts/account.example.json) | 展示安装脚本生成的 `config/accounts/<账户>.json` |
| Trading | [logging.json](../apps/services/trading/config/logging.json) | 随服务安装包发布，首次安装复制为 `config/logging.json`，升级保留已有配置 |

Lab 的研究任务通过通信协议提交，没有单独的研究或任务配置文件。日志字段见[运行日志](services.md#运行日志)，两个输出都支持可选的 `pattern`。

## Market

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 服务监听地址和端口 |
| `data` | 已存在的数据目录绝对路径 |
| `counter.market_front`、`trade_front` | CTP 行情、交易前置地址；交易前置只用于获取合约目录 |
| `counter.broker`、`user`、`app_id` | 经纪商、行情账户和应用标识；`app_id` 可为空 |
| `clients[].fingerprint` | 允许连接的客户端证书 SHA-256 指纹 |

密码和授权码不属于 JSON 字段。服务从 systemd 提供的凭据目录读取 `asterion.ctp.password`，`app_id` 非空时另读 `asterion.ctp.auth_code`。

## Warehouse

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 服务监听地址和端口 |
| `data` | 已存在的数据目录绝对路径 |
| `clients[].fingerprint` | 客户端证书 SHA-256 指纹；登记的客户端都能读取数据和管理采集 |
| `acquisition.exchanges` | 开启日线采集的交易所列表，可为空 |
| `acquisition.tushare` | `null` 表示关闭；开启时填写 `requests_per_minute`（1–500） |
| `acquisition.update_at` | 每日自动更新的北京时间 `HH:MM`，`null` 表示关闭 |

Warehouse 不连接柜台，也没有 CTP 凭据。开启 Tushare 后，服务从 systemd 凭据目录读取 `asterion.warehouse.tushare.token`，token 不写在 JSON 里。

## Trading

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 该账户实例的监听地址和端口 |
| `environment` | `simulation` 或 `real` |
| `broker.front`、`broker_id`、`user_id`、`app_id` | CTP 交易前置、经纪商、账户和应用标识 |
| `market_front` | Trading 自己连接的行情前置 |
| `terminals` | 允许连接的 Terminal 证书 SHA-256 指纹列表 |

文件名决定 `--account` 的账户实例名。密码和授权码由 systemd 以 `password`、`auth_code` 提供，可选脱机告警地址由 `asterion.trading.<账户>.webhook` 加密凭据提供；风控政策和交易授权通过 Terminal 管理，不写在账户配置中。

## Terminal

交易时段表的 `day_sessions`、`night_sessions` 定义时段，`products` 将交易所品种映射到时段，`utc_offset` 指定时区；`source` 记录来源和核对日期，`version` 标识文件格式。
