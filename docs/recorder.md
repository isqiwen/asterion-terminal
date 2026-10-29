# 行情录制（recorder）

`asterion_recorder` 是录制主机上唯一的 Asterion 程序，代码在 `apps/services/recorder/`。它自己登录 CTP 行情，
订阅全市场，把每笔报价和分钟线写成 Parquet。它不属于任何节点：不由 Agent 或 Manager 启停，Terminal 和行情服务
的升级、重启都不影响它；Terminal 只读它的状态和文件。接入、分钟聚合和合约目录的代码与行情服务共用。

## 运行

录制程序装在一个目录里，没有命令行参数，从自己所在的 `bin/` 找到其余部分：

```
<安装目录>/
  bin/     asterion_recorder、setup.sh
  lib/     ctp-md.so、ctp-trader.so（交易库只用来读合约目录）
  config/  recorder.json、logging.json
  logs/    日志（文本，每条一行）
  data/    录制文件、合约目录缓存、CTP 流文件
```

目录或库文件缺失时启动失败，错误写明缺的路径。

- 启动后立即登录，不等任何客户端。每次登录成功（包括柜台夜间断开后的重连）都重新读取合约目录，新上市的合约随之
  订阅；读取失败时五分钟后再读，期间沿用已有订阅。合约目录按期货品种逐个查询，不读期权；上次成功读到的目录存在
  `ctp-catalog.pb`，启动时先用它订阅。
- 行情登录被拒时每分钟重试一次。这只是行情登录，不涉及任何交易命令。
- 收到 SIGTERM 时结束当前段并写完缓冲的报价、分钟线和段记录（最多等 20 秒）后退出。录制循环自己出错时进程退出，
  由 systemd 重新启动。

## 录制主机

录制主机（Ubuntu 24.04 x86_64，决定 D3）上只装录制程序，位置是 `/opt/asterion/recorder/`。发布物只有一个文件
`asterion-recorder-setup.sh`：自解压脚本，即 `apps/services/recorder/scripts/setup.sh` 后接一行 `__ARCHIVE__` 和安装目录
（`bin/`、`lib/`、`config/`）的 tar.gz，由 CMake 目标 `asterion_recorder_setup` 生成。它与远程服务包在同一个 Ubuntu 24.04
环境中构建：CI 的 `remote-linux` 作业把它作为单独的产物 `recorder-linux-x86_64` 上传，与 Terminal 部署的服务包分开；本地用
`scripts/services/build_linux_services.sh`，输出到 `build/linux-bundles/`。

```sh
sudo bash asterion-recorder-setup.sh         # 第一次：安装并配置；以后：升级
sudo /opt/asterion/recorder/bin/setup.sh     # 只修改配置
```

运行时脚本把自带的安装目录解到临时目录，装完即删除；装好的 `bin/setup.sh` 不带程序，只用来修改配置。

- 安装：建立目录，放入默认配置，写入并启用 `asterion-recorder.service`，然后询问配置并启动。录制程序以执行 `sudo` 的账户
  运行（不能直接以 root 安装），只能写 `logs/` 与 `data/`，`Restart=always` 且永不放弃重启，
  `ImportCredential=asterion.ctp.*` 解密凭据。
- 升级：先确认新程序能在本机运行，再逐个替换 `bin/` 与 `lib/` 中的文件（每个文件一次改名），配置文件不动；随后问一句是否
  修改配置，然后重启。
- 配置：依次询问监听地址、端口、行情前置、交易前置、经纪公司代码、投资者代码和 AppID，每项以配置文件里的当前值为默认值，
  直接回车即保留（第一次是仓库默认配置：SimNow 常规环境的前置、经纪公司 `9999`、AppID `simnow_client_test`，投资者代码为空
  必须填写；AppID 输入 `-` 表示没有）；格式不对会重问。然后询问密码和授权码（不回显、没有默认值，没有 AppID 时不问授权码）。
  前七项写入 `config/recorder.json`（只有运行账户可读），密码和授权码由 `systemd-creds` 加密到 `/etc/credstore.encrypted/`，
  不以明文写入任何文件。
- 交易时段：北京时间工作日 08:45–15:30 与 20:45–次日 02:45（脚本不知道节假日，节假日也按交易日对待）。录制程序正在运行时，
  这段时间内不重启它：升级的文件和配置照样立即放好——录制程序只在启动时读取程序、配置和凭据，不影响正在进行的录制——
  脚本告知在北京时间几点自动重启生效（日盘中为当天 15:30，夜盘中为 02:45），由 systemd 定时器
  `asterion-recorder-restart.timer` 执行；再次运行脚本会替换它，`systemctl stop asterion-recorder-restart.timer` 取消。还没运行
  （第一次启动）则立即启动。
- 网络：录制程序在配置的地址和端口上监听（默认 `0.0.0.0:7450`），不加密。每个请求带协议里的固定魔数 `0x41535452`（"ASTR"）
  标明来自 Asterion 客户端，没有魔数的请求被拒绝。魔数只是标识，不是密码；谁能连上端口谁就能读数据，所以防火墙只放行自己的机器。

## 配置文件

`config/recorder.json` 只放监听地址和柜台信息，默认值在仓库的 `apps/services/recorder/config/recorder.json`：

```json
{
  "bind": "0.0.0.0",
  "port": 7450,
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

| 键 | 含义 |
| --- | --- |
| `bind`、`port` | 监听的地址与 TCP 端口 |
| `counter` | 柜台的行情前置、交易前置、经纪公司代码、投资者代码和 AppID（没有则为空） |

`config/logging.json` 是录制程序的日志配置，默认值在仓库的 `apps/services/recorder/config/logging.json`：只写 `logs/` 下的文件，
info 级别，保留 30 天；控制台输出默认关闭，打开后写到标准错误，由 systemd 收进日志，可用 `journalctl -u asterion-recorder -f`
查看。各键的含义与其他程序相同，见[运行日志](services.md#运行日志)。安装时与 `recorder.json` 一起复制一次，升级不覆盖。

- 路径不是配置：都由安装目录决定。缺键、多出不认识的键、投资者代码未填（配置之前的默认配置）时启动失败，错误写明
  配置文件的路径和出错的键。
- 密码和授权码不进配置文件，由 `setup.sh` 用 `systemd-creds` 加密保存，加密绑定本机，别处无法解密。
  单元的 `ImportCredential=asterion.ctp.*` 让 systemd 在启动时把它们解密到只有运行账户可读的 `$CREDENTIALS_DIRECTORY`，
  程序从那里读取，只留在内存，不写日志。
- 缺少凭据时启动失败，错误写明缺的凭据文件。

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

## 收盘后合并

一个交易日的 Tick 与分钟文件（每 30 秒、每个实例一个）在收盘后合并为每个交易所一个文件：

```
recordings/ticks/trading_day=<YYYYMMDD>/<交易所>.parquet
recordings/minutes/trading_day=<YYYYMMDD>/<交易所>.parquet
```

- 时机：连续 30 分钟没有收到报价，且这个交易日的交易已结束——北京时间今天之前的交易日，或 15:30 之后的今天。
  夜盘属于下一交易日，所以夜盘结束后不会提前合并当天；午休也不会触发。一次合并一个交易日，在单独的线程上用
  独立的 DuckDB 进行，录制循环不等它。
- 合并读取当天全部 Parquet 文件，包括之前合并出的文件，所以合并之后才到的报价会在下一次合并时并入。
- 合并出的文件先以读取者忽略的名字写在原文件旁并落盘；行数与原文件之和一致后才写日志文件 `compaction.json`，
  然后删除原文件、把合并出的文件改为正式名字。在写日志文件之前中断，原文件不受影响；之后中断，下次启动时
  按日志文件完成。段文件不合并。
- 合并失败写进日志和状态的 `merge_error`，一小时后再试。录制的数据从不被自动删除。

## 只读接口

请求与回复是 `protocol/proto/asterion/recorder.proto` 中的 `Request` / `Response`，4 字节大端长度分帧，经不加密的 TCP
（测试中也可用本机 Unix socket）。请求的 `magic` 必须是 `0x41535452`。没有任何请求能停止、重连或重新配置录制。

| 请求 | 作用 |
| --- | --- |
| `status` | 实例、版本、启动时间；行情连接阶段与错误码；合约目录阶段、交易日与错误；订阅数；最近一笔报价的接收时间；本实例写入的行数、文件数、字节数、丢失行数与最近的写入错误；正在合并的交易日与最近的合并错误；数据目录所在磁盘的剩余空间 |
| `days` | 每个已录交易日的 Tick 与分钟文件数、字节数、是否已合并，以及按首笔报价排序的段 |
| `minutes` | 一个合约一个交易日已写出的分钟线，价格为精确小数文本 |

状态由录制循环每 250 毫秒发布一次，接口只读这份副本；`days` 与 `minutes` 在单独的读取线程上用独立的 DuckDB 读文件，
不占用写文件的通道。

## 限制

- Terminal 中还看不到录制状态（M1.6）。部署脚本只在测试中检查了规则；尚未在真实的 Ubuntu 24.04 主机上以 root
  安装、加密凭据、开机启动，也没从另一台机器经 TCP 读取过。
- 尚未在真实交易日运行过，每个交易日的数据量和合并耗时未实测。
