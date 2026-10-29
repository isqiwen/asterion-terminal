# 行情录制（recorder）

`asterion_recorder` 是录制主机上唯一的 Asterion 程序，代码在 `apps/services/recorder/`。它自己登录 CTP 行情，
订阅全市场，把每笔报价和分钟线写成 Parquet。它不属于任何节点：不由 Agent 或 Manager 启停，Terminal 和行情服务
的升级、重启都不影响它；Terminal 只读它的状态和文件。接入、分钟聚合和合约目录的代码与行情服务共用。

## 运行

```
asterion_recorder --directory <数据目录> --config <recorder.json> \
  --ctp-library <thostmduserapi_se.so> --ctp-catalog-library <thosttraderapi_se.so> \
  (--endpoint <socket> | --bind <地址> --port <端口> --tls-ca … --tls-cert … --tls-key …)
```

- 启动后立即登录，不等任何客户端。每次登录成功（包括柜台夜间断开后的重连）都重新读取合约目录，新上市的合约随之
  订阅；读取失败时五分钟后再读，期间沿用已有订阅。合约目录按期货品种逐个查询，不读期权；上次成功读到的目录存在
  `ctp-catalog.pb`，启动时先用它订阅。
- 行情登录被拒时每分钟重试一次。这只是行情登录，不涉及任何交易命令。
- 收到 SIGTERM 时结束当前段并写完缓冲的报价、分钟线和段记录（最多等 20 秒）后退出。录制循环自己出错时进程退出，
  由 systemd 重新启动。

## 登录配置与凭据

`recorder.json` 只放柜台地址和账户：

```json
{"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
```

- 密码和授权码不进配置文件，由 systemd 加密保存：管理员在录制主机上执行
  `sudo systemd-creds encrypt --name=asterion.ctp.password - /etc/credstore.encrypted/asterion.ctp.password`
  （授权码同理，名称为 `asterion.ctp.auth_code`；`app_id` 为空时不需要），输入密码后按 Ctrl+D。
  加密绑定本机，别处无法解密。录制程序的 systemd 单元带有 `ImportCredential=asterion.ctp.*`，systemd 在启动时把它们
  解密到只有运行用户可读的 `$CREDENTIALS_DIRECTORY`，程序从那里读取，只留在内存，不写日志。
- 配置文件缺失或不完整、缺少凭据时启动失败，错误写明缺的文件或配置路径。

## 文件

文件在数据目录的 `recordings/` 下，按柜台给的交易日分区，可直接用 DuckDB 以 `hive_partitioning` 读取：

```
recordings/ticks/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/minutes/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/segments/trading_day=<YYYYMMDD>/<实例>.parquet
```

- **ticks**：每 30 秒写一个文件，一段观测结束时立即写出。列为段号、本机接收时间、柜台时间（`source_ms`，不能解析时为空）
  及原始的 `action_day`、`update_time`，报价字段与[行情](market-data.md)相同，价格为 `DECIMAL(18,8)` 精确值，缺失保持为空。
  乱序报价也写入，以 `out_of_order` 标记；分钟线不使用它们。
- **minutes**：与行情服务给策略的分钟线是同一份聚合代码。一根分钟线在下一根出现或换交易日后写出，之后不再改动。
  `partial` 标记观测可能缺了这一分钟的一部分：当日观测从分钟中间开始、在这一分钟内或之前丢过观测、累计成交量回退，
  以及停止时正在形成的那一根。
- **segments**：一段是本进程一次不间断的观测，记首末接收时间、观测到的报价数和结束原因：断线时为连接阶段
  （如 `reconnecting`、`disconnected`），事件流断档为 `events_lost`，原始队列溢出为 `feed_failed`，正常停止为 `stopped`。
  没有结束原因的段属于仍在录制或来不及说明原因就停止的进程。两段之间就是缺口，不补造。段内文件的行数少于它记的
  报价数，说明有文件写入失败，失败也写进日志并计入状态的 `lost`。
- 写文件在单独的线程上进行，落后四个文件时录制循环才等待。重启后以新的实例名续写，不改动已有文件。

## 只读接口

请求与回复是 `protocol/proto/asterion/recorder.proto` 中的 `Request` / `Response`，4 字节大端长度分帧；本机为 Unix socket，
远程为 TCP + 双向 TLS。没有任何请求能停止、重连或重新配置录制。

| 请求 | 作用 |
| --- | --- |
| `status` | 实例、版本、启动时间；行情连接阶段与错误码；合约目录阶段、交易日与错误；订阅数；最近一笔报价的接收时间；本实例写入的行数、文件数、字节数、丢失行数与最近的写入错误；数据目录所在磁盘的剩余空间 |
| `days` | 每个已录交易日的 Tick 与分钟文件数、字节数，以及按首笔报价排序的段 |
| `minutes` | 一个合约一个交易日已写出的分钟线，价格为精确小数文本 |

状态由录制循环每 250 毫秒发布一次，接口只读这份副本；`days` 与 `minutes` 在单独的读取线程上用独立的 DuckDB 读文件，
不占用写文件的通道。

## 限制

- 尚无收盘后合并、录制主机的部署脚本（systemd 单元、证书、只在非交易时段升级）和 Terminal 中的录制状态；
  这些是 M1.4 与 M1.6 的后续工作。
- 尚未在真实交易日运行过，每个交易日的数据量未实测。
