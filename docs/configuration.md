# 配置文件

配置按所属程序保存在子项目的 `config/`，仓库根目录不放配置。`*.example.json` 只展示文件结构，不随安装包发布；地址、账户和证书指纹都是占位值，真实凭据由 systemd 加密保存。

| 程序 | 仓库文件 | 使用方式 |
| --- | --- | --- |
| Terminal | [logging.json](../apps/clients/terminal/config/logging.json) | 桌面打包，首次启动写入 `terminal/config/logging.json` |
| Terminal | [futures-sessions.json](../apps/clients/terminal/config/futures-sessions.json) | 界面构建时导入，用于分时图横轴 |
| Lab | [logging.json](../apps/services/lab/config/logging.json) | 桌面携带为 `config/lab-logging.json`，首次启动写入 `lab/config/logging.json` |
| History | [history.example.json](../apps/services/history/config/history.example.json) | 展示安装脚本生成的 `config/history.json` |
| History | [logging.json](../apps/services/history/config/logging.json) | 随服务安装包发布，首次安装复制为 `config/logging.json`，升级保留已有配置 |
| Trading | [account.example.json](../apps/services/trading/config/accounts/account.example.json) | 展示安装脚本生成的 `config/accounts/<账户>.json` |
| Trading | [logging.json](../apps/services/trading/config/logging.json) | 随服务安装包发布，首次安装复制为 `config/logging.json`，升级保留已有配置 |

Lab 的研究任务通过通信协议提交，没有单独的研究或任务配置文件。日志字段见[运行日志](services.md#运行日志)，两个输出都支持可选的 `pattern`。

## History

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 服务监听地址和端口 |
| `data` | 已存在的数据目录绝对路径 |
| `recording.tick_days` | Tick 保留的交易日数，分钟线和观测段长期保留 |
| `counter.market_front`、`trade_front` | CTP 行情、交易前置地址；交易前置用于获取合约目录 |
| `counter.broker`、`user`、`app_id` | 经纪商、账户和应用标识；`app_id` 可为空 |
| `clients[].fingerprint`、`role` | 客户端证书 SHA-256 指纹；`read` 只读，`manage` 可管理采集 |
| `acquisition.exchanges` | 开启日线采集的交易所列表，可为空 |
| `acquisition.tushare` | `null` 表示关闭；开启时填写 `requests_per_minute`（1–500） |

密码、授权码和 Tushare token 不属于 JSON 字段。服务从 systemd 提供的凭据目录读取 `asterion.ctp.password`、非空 `app_id` 对应的 `asterion.ctp.auth_code`，以及开启 Tushare 后的 `asterion.history.tushare.token`。

## Trading

| 字段 | 含义 |
| --- | --- |
| `bind`、`port` | 该账户实例的监听地址和端口 |
| `environment` | `simulation` 或 `real` |
| `broker.front`、`broker_id`、`user_id`、`app_id` | CTP 交易前置、经纪商、账户和应用标识 |
| `market_front` | Trading 自己连接的行情前置 |
| `terminals` | 允许连接的 Terminal 证书 SHA-256 指纹列表 |

文件名决定 `--account` 的账户实例名。密码和授权码由 systemd 以 `password`、`auth_code` 提供；风控政策和交易授权通过 Terminal 管理，不写在账户配置中。

## Terminal

交易时段表的 `day_sessions`、`night_sessions` 定义时段，`products` 将交易所品种映射到时段，`utc_offset` 指定时区；`source` 记录来源和核对日期，`version` 标识文件格式。
