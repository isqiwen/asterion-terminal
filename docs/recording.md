# Market 录制与部署

录制属于独立 Market 服务，代码在 `apps/services/market/recording/`。同一个 `asterion_market`
进程拥有 CTP 接入、全市场实时流、Tick 与分钟 Parquet 录制和历史读取。systemd 看护整个服务，
Terminal 关闭、断线、升级不影响它。客户端在“设置 → 数据源 → Market 行情服务”保存服务地址，
并核对服务端证书指纹，不接收柜台或历史供应商凭据。Market 已提供 C++ 历史采集、仓库与有权限的采集管理；
Terminal 数据页可将录制分钟发布为固定研究版本，Lab 只读这些版本。已有用户数据不自动搬动或重建。见[M3.0 开发计划](reviews/project-assessment-2026-10-28.md#m30-纯-c-market-开发计划)。

## 运行

服务由 `asterion_market --installation <安装目录>` 启动：

```
<安装目录>/
  bin/     asterion_market、setup.sh
  lib/     ctp-md.so、ctp-trader.so（交易库只用来读合约目录）
  config/  market.json、logging.json、tls/server.crt 与 server.key
  logs/    日志（文本，每条一行）
  data/    录制文件、固定版本仓库、采集任务、合约目录缓存与 CTP 流文件
```

目录或库文件缺失时启动失败，错误写明缺的路径。`data/market.lock` 保证同一安装数据目录只有一个写入进程；即使改用不同端口，第二个进程也拒绝启动。

- 启动后立即登录，不等任何客户端。每次登录成功（包括柜台夜间断开后的重连）都重新读取合约目录，新上市的合约随之
  订阅；读取失败时五分钟后再读，期间沿用已有订阅。合约目录按期货品种逐个查询，不读期权；上次成功读到的目录存在
  `ctp-catalog.pb`，启动时先用它订阅。
- 行情登录被拒时每分钟重试一次。这只是行情登录，不涉及任何交易命令。
- 收到 SIGTERM 时结束当前段并写完缓冲的报价、分钟线和段记录（最多等 20 秒）后退出。录制循环自己出错时进程退出，
  由 systemd 重新启动。

## 录制主机

Market 主机（Ubuntu 24.04 x86_64）运行独立 C++ 服务，位置是 `/opt/asterion/market/`。发布物只有一个文件
`asterion-market-setup.sh`：自解压脚本，即 `apps/services/market/scripts/setup.sh` 后接一行 `__ARCHIVE__` 和安装目录
（仅 `bin/`、`lib/`）的 tar.gz，由 CMake 目标 `asterion_market_setup` 生成。它与交易服务的安装脚本在同一个 Ubuntu 24.04
环境中构建：CI 的 `linux-hosts` 作业把它作为单独的产物 `market-linux-x86_64` 上传；本地用
`scripts/services/build_linux_services.sh`，输出到 `build/linux-bundles/`。
包内只有服务程序、安装脚本和两份 CTP 动态库，运行主机不需要 Python。配置由首次安装生成，
升级只替换程序与库，保留配置、TLS 身份、仓库、采集任务和录制文件。

```sh
sudo bash asterion-market-setup.sh         # 第一次：安装并配置；以后：升级
sudo /opt/asterion/market/bin/setup.sh     # 只修改配置
```

运行时脚本把自带的安装目录解到临时目录，装完即删除；装好的 `bin/setup.sh` 不带程序，只用来修改配置。

- 安装：建立目录，放入默认配置，写入并启用 `asterion-market.service`，然后询问配置并启动。录制程序以执行 `sudo` 的账户
  运行（不能直接以 root 安装），只能写 `logs/` 与 `data/`，`Restart=always` 且永不放弃重启，
  `ImportCredential=asterion.ctp.*` 和 `ImportCredential=asterion.market.*` 解密凭据。
- 升级：先确认新程序能在本机运行，再逐个替换 `bin/` 与 `lib/` 中的文件（每个文件一次改名），配置文件不动；随后问一句是否
  修改配置，然后重启。
- 配置：依次询问监听地址、端口、行情前置、交易前置、经纪公司代码、投资者代码和 AppID，每项以配置文件里的当前值为默认值，
  直接回车即保留（第一次是仓库默认配置：SimNow 常规环境的前置、经纪公司 `9999`、AppID `simnow_client_test`，投资者代码为空
  必须填写；AppID 输入 `-` 表示没有）；格式不对会重问。随后登记客户端（空格分隔的 `<指纹>=read` 或 `<指纹>=manage`），
  选择公开交易所来源（`-` 禁用），设置 Tushare 每分钟请求数（`0` 禁用，启用为 1–500）。
  启用 Tushare 时输入 token；最后输入 CTP 密码和授权码（所有秘密均不回显、无默认值，无 AppID 时不问授权码）。
  非秘密配置写入 `config/market.json`（只有运行账户可读），密码、授权码和 Tushare token 由 `systemd-creds` 加密到 `/etc/credstore.encrypted/`，
  不以明文写入任何文件。
- 交易时段：北京时间工作日 08:45–15:30 与 20:45–次日 02:45（脚本不知道节假日，节假日也按交易日对待）。录制程序正在运行时，
  这段时间内不重启它：升级的文件和配置照样立即放好——录制程序只在启动时读取程序、配置和凭据，不影响正在进行的录制——
  脚本告知在北京时间几点自动重启生效（日盘中为当天 15:30，夜盘中为 02:45），由 systemd 定时器
  `asterion-market-restart.timer` 执行；再次运行脚本会替换它，`systemctl stop asterion-market-restart.timer` 取消。还没运行
  （第一次启动）则立即启动。
- 网络：默认 `0.0.0.0:7450`，统一使用双向固定证书指纹 TLS。首次安装生成服务端证书与仅运行账户可读的私钥，
  升级保留身份；脚本显示服务端指纹供 Terminal 登记。客户端必须事先登记其证书，`read` 只读，`manage` 可启动和取消采集。
  普通客户端不能改来源、柜台连接或全市场订阅。权限修改在重启后生效。

## 配置文件

`config/market.json` 存放监听地址、柜台信息、客户端权限和供应商非秘密设置，默认值集中定义在 `apps/services/market/scripts/setup.sh`，首次配置时回车采用默认值，之后回车保留当前实际值：

```json
{
  "bind": "0.0.0.0",
  "port": 7450,
  "clients": [{"fingerprint": "<客户端证书的64位十六进制SHA-256指纹>", "role": "manage"}],
  "acquisition": {"exchanges": ["SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"], "tushare": null},
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

| 键 | 含义 |
| --- | --- |
| `bind`、`port` | 监听的地址与 TCP 端口 |
| `clients` | 1–64 个唯一证书指纹，每个角色为 `read` 或 `manage` |
| `acquisition` | 启用的交易所及 Tushare 配置；`tushare` 为 `null` 时禁用，否则为 `{"requests_per_minute": 100}` |
| `counter` | 柜台的行情前置、交易前置、经纪公司代码、投资者代码和 AppID（没有则为空） |

启用 Tushare 时必须在主机提供 `asterion.market.tushare.token` 系统凭据；配置文件不接受 `token` 字段。
配置、TLS 文件或已启用来源的凭据缺失均拒绝启动，错误包含相应路径。

`config/logging.json` 是录制程序的日志配置，由安装脚本首次生成：只写 `logs/` 下的文件，
文件名为 `asterion_market_YYYY-MM-DD.log`，按日切分，同一天重启继续追加，不按 PID 分文件。
info 级别，保留 30 天；控制台输出默认关闭，打开后写到标准错误，由 systemd 收进日志，可用 `journalctl -u asterion-market -f`
查看。各键的含义与其他程序相同，见[运行日志](services.md#运行日志)。安装包不携带默认 JSON 文件；脚本首次生成实际配置，升级不覆盖。

- 路径不是配置：都由安装目录决定。缺键、多出不认识的键、投资者代码未填时启动失败，错误写明
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
  及原始的 `action_day`、`update_time`，报价字段与[实时行情](market.md)相同，价格为 `DECIMAL(18,8)` 精确值，缺失保持为空。
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
  独立的 DuckDB 进行；查找待合并日期的目录扫描也在该线程完成，行情状态线程只调度和读取进度。
- 合并读取当天全部 Parquet 文件，包括之前合并出的文件，所以合并之后才到的报价会在下一次合并时并入。
- 合并出的文件先以读取者忽略的名字写在原文件旁并落盘；行数与原文件之和一致后才写日志文件 `compaction.json`，
  然后删除原文件、把合并出的文件改为正式名字。在写日志文件之前中断，原文件不受影响；之后中断，下次启动时
  按日志文件完成。段文件不合并。
- 扫描或合并失败写进日志和状态的 `merge_error`，一小时后再试，不让维护错误终止实时接收。
  服务停止可中断归档锁等待和 DuckDB 合并查询；已写入日志的替换仍完成，未提交的合并保留原文件。

## 只读接口

请求与回复使用 `protocol/proto/asterion/market.proto` 中的 `Request` / `Response`，
`recording.proto` 只定义录制数据消息。连接要求双向固定证书指纹 TLS，以 4 字节大端长度分帧，不发送明文 `ASTR` 前缀，服务身份为 `market-data`。
该 TLS 端口同时提供实时流和历史读取，所有客户端共享相同文件，读取游标互不干扰。

| 请求 | 作用 |
| --- | --- |
| `recording_status` | 实例、版本、启动时间；行情连接阶段与错误码；合约目录阶段、交易日与错误；订阅数；最近一笔报价的接收时间；本实例写入的行数、文件数、字节数、丢失行数与最近的写入错误；正在合并的交易日与最近的合并错误；数据目录所在磁盘的剩余空间 |
| `recorded_days` | 每个已录交易日的 Tick 与分钟文件数、字节数、是否已合并，以及按首笔报价排序的段 |
| `recorded_minutes` | 一个合约一个交易日已写出的分钟线，价格为精确小数文本 |

状态从行情拥有者读取录制计数；`recorded_days` 与 `recorded_minutes` 在独立的有界读取线程上用 DuckDB 读文件，
不占用写文件的通道。

## 发布研究版本

在 Terminal「数据 → 数据覆盖」选择「Market 录制」，填写 `EXCHANGE/product/YYYY-MM` 合约与交易日范围，
点击「发布录制分钟」。Market 使用现有持久任务队列读取自己已落盘的归档，按合约与交易日将分钟和对应观测区间
一起提交到 `recorder` 来源的不可变仓库版本；没有额外 Python 进程、网络采集或供应商凭据。
原始归档仍用于录制状态和历史读取，研究版本是 Market 管理的发布快照，Lab 只保留可丢弃的下载缓存。

- 默认跳过已发布分区。录制仍在继续或后续补录时，显式勾选「重新发布已有分区，保留旧数据集版本」，
  才将当时已落盘的数据发布到新版本；未落盘和仍形成的分钟不会提前进入研究。旧版本及其文件不被覆盖。
- 沿用柜台给出的交易日、分钟起点、`partial`、缺失字段和观测结束原因。观测段属于服务，不能证明该合约全时段完整；
  不填补缺口、不将不完整分钟改为完整。仓库仍用 `DECIMAL(28,6)`；超过六位且无法精确保存的小数使分区发布失败，不静默舍入。
- 归档合并与历史读取、研究发布互斥，避免读取时原文件被替换；实时接收和录制写入不等待该锁。
  启动时先完成未结束的归档合并，再开放读取。发布任务等待合并锁时也可取消；取消或失败不撤回此前已提交的完整分区。
- Lab 通过 `minute_provider=recorder` 选择固定分钟版本；日线来源独立指定。历史图表同样可选择 `recorder`。
  后续发布或归档合并不改变已开始研究的版本，缓存缺失时仍下载原版本。

## 验证边界

- Terminal 数据页已有录制监控，直接读取 Market 的写入、磁盘与故障状态。连接关闭或失联不改变录制生命周期。
- 独立服务和多客户端读取已在 macOS 及 `research` 的 Ubuntu 26.04 x86_64 隔离测试目录验证，使用测试柜台。
  按使用者要求暂不配置真实账户。正式 systemd 安装、加密真实凭据、开机启动与跨主机读取尚未验收；
  安装脚本的隔离测试需要 root 用户命名空间，`research` 未开放此能力，对应用例跳过。
- 尚未在真实交易日运行过，每个交易日的数据量和合并耗时未实测。
