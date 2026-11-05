# Market 录制与部署

录制是 Market 服务的一部分（`apps/services/market/recording/`）：同一个进程接收全市场行情，把 Tick 和分钟线写成 Parquet。服务由 systemd 看护；Terminal 关闭、断线或升级都不影响它。

## 安装

Market 主机是 Ubuntu 24.04 x86_64，安装在 `/opt/asterion/market/`。发布物是一个自解压脚本 `asterion-market-setup.sh`（CMake 目标 `asterion_market_setup`；本地用 `scripts/services/build_linux_services.sh` 构建，输出到 `build/linux-bundles/`），只含服务程序、安装脚本和两个 CTP 库，主机不需要 Python。

```sh
sudo bash asterion-market-setup.sh         # 第一次：安装并配置；以后：升级
sudo /opt/asterion/market/bin/setup.sh     # 只修改配置
```

- 安装时写入并启用 `asterion-market.service`：以执行 `sudo` 的账户运行，只能写 `logs/` 和数据目录，`Restart=always`。
- 配置时依次询问监听地址、端口、数据目录（默认 `/opt/asterion/market/data`，可以放到单独的磁盘）、Tick 保留的交易日数（默认 30）、行情和交易前置、经纪公司代码、投资者代码、AppID（回车保留当前值），登记客户端证书（`<指纹>=read` 或 `<指纹>=manage`），选择交易所来源和 Tushare 每分钟请求数，最后隐藏输入 CTP 密码、授权码和 Tushare token。秘密用 `systemd-creds` 加密到 `/etc/credstore.encrypted/`，不写入任何明文文件。
- 升级只替换程序和库，保留配置、TLS 身份、仓库、采集任务和录制文件。交易时段内（北京时间工作日 08:45–15:30、20:45–次日 02:45）不重启正在运行的服务，改为收盘后由定时器重启。
- 安装时生成服务端证书，脚本显示它的指纹，供 Terminal 登记。权限修改在重启后生效。

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
- 缺少配置、TLS 文件或已启用来源的凭据时拒绝启动，并报告路径；多出不认识的键同样拒绝。
- 数据目录下的 `market.lock` 保证同一份数据只有一个 Market 进程。
- 日志在 `logs/asterion_market_YYYY-MM-DD.log`，可用 `journalctl -u asterion-market -f` 查看。

## 运行

- 启动后立即登录 CTP，不等客户端。行情登录被拒时每分钟重试一次（只涉及行情，不涉及交易）。
- 收到 SIGTERM 时写完缓冲的报价和分钟线再退出（最多 20 秒）。原始行情队列写满时进程退出，由 systemd 重启。

## 录制文件

文件在数据目录的 `recordings/` 下，按柜台给的交易日分区，可以直接用 DuckDB 读取：

```
recordings/ticks/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/minutes/trading_day=<YYYYMMDD>/<实例>-<序号>.parquet
recordings/segments/trading_day=<YYYYMMDD>/<实例>.parquet
```

- **ticks**：每 30 秒一个文件；价格是精确小数，缺失保持为空；乱序报价也写入并标记。
- **minutes**：与实时分钟线是同一份聚合。`partial` 表示这一分钟的观测可能不完整（从分钟中间开始观测、期间丢过观测、累计量回退、停止时还在形成）。一根分钟线在下一根出现或交易日结束（北京时间 15:30）后写入，所以收盘前的最后一分钟在合并前就已落盘。
- **segments**：一段是一次不间断的观测，记首末时间、报价数和结束原因：断线（如 `reconnecting`）、`events_lost`、`feed_failed`、`write_backlog`（磁盘跟不上，这一段时间的数据被丢弃）、`stopped`。两段之间就是缺口，不补造。
- 写文件在单独的线程上进行。磁盘落后四个文件时，丢弃这一段时间的数据并计入丢失行数，行情线程不等待磁盘。重启后以新的实例名续写，不改已有文件。

**保留**：Tick 只保留最近 `recording.tick_days` 个已录制的交易日。每天合并完成后，更早交易日的 Tick 目录被删除并记入日志；分钟线和观测段永久保留，已发布的 `recorder` 研究版本不受影响。

**收盘后合并**：一个交易日的交易结束、并且 30 分钟没有新报价后，当天的 Tick 和分钟文件合并为每个交易所一个文件。合并先写新文件并核对行数，再记日志、删原文件；中途中断时下次启动完成或放弃这次合并，原数据不丢。合并失败一小时后重试，不影响实时接收。

## 读取录制

Market 的 TLS 端口上（见[通信协议](protocol.md#7-market)）：

- `get_status` 的 `recording`：交易日、写入和丢失计数、磁盘余量、合并状态。
- `list_recorded_days`：一段日期内每个交易日的观测段，直接读录制目录，包括还没发布的当天。
- `get_minutes`：本交易日一个合约的分钟线，来自 Market 内存。
- 更早的分钟线和观测段：读 `recorder` 来源或研究数据集的固定版本，按分区下载（`get_dataset_version`、`read_dataset_file`）。

## 发布研究版本

每个交易日合并完成后，Market 自动把当天全部合约的录制分钟线和对应观测段提交为 `recorder` 来源的新版本，再合成研究数据集（见 [Market](market.md#仓库与固定版本)）：Tushare 没有的合约交易日就用这些录制数据。待发布的交易日记在 `recordings/unpublished.json`，发布成功后才移除，所以重启后会接着发布；采集正忙时每分钟重试，发布失败时每小时重试，原因写进主机日志。收盘后迟到的报价再次合并时，这一天会重新发布。

- 也可以在 Terminal「数据 → 数据覆盖」选择「自录」，按合约和交易日范围手动发布或重新发布（需要 `manage` 权限）。
- 柜台给出的交易日、`partial` 标记和缺失字段原样保留，不补缺口。
- 录制保留柜台的 8 位小数，研究数据最多 6 位。成交均价是成交额除以成交量和合约乘数，发布时按银行家舍入到 6 位；其余价格超过 6 位时整次发布失败，不舍入。
- 每次发布生成新版本，旧版本保留；合并与发布互斥，实时录制不等待。
