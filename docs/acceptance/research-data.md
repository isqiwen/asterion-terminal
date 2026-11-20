# 真实研究数据验收

先选一个含夜盘的具体交割月，写下验收的起止交易日。在目标机完成 Warehouse 采集，记录一次实际的每日更新、Terminal 数据页显示的数据版本和目标分区。在主机同一版本的研究清单中核对分区来源。公开读接口提供分区键、行数和 SHA-256，来源见 [Warehouse 数据仓库](../warehouse/README.md#仓库与固定版本)。

## 核对短样本

在匹配提交的开发机上准备 Debug 构建与 Lab Python 运行时。Linux 示例使用 Terminal 已登记的 Warehouse 连接，`DATASET_VERSION` 取上面记录的完整版本号。

```sh
build/electron-resources/python/bin/python3 -I -B tests/acceptance/research_minutes.py \
  --terminal-data "$HOME/.local/share/asterion/terminal/data" --lab-directory build/acceptance/research \
  --version "$DATASET_VERSION" --contract SHFE/rb/2027-01 \
  --first-day 2026-11-10 --last-day 2026-11-17 --minutes 5 --report build/acceptance/research/rb-2027-01.json
```

报告逐日列出日历开闭市、分钟分区及摘要、原始分钟和聚合周期的缺口、来源标记、零成交分钟、分钟合计量和日线成交量。时间区间的两端均为北京时间的**周期结束标签**。已缓存全部所需分区时可省略 `--terminal-data`，用相同版本离线复核。

退出码 0 表示请求范围在合约存续期内至少覆盖一个开市日，所需日历、结算价和分钟覆盖齐全，成交量逐日相符。明确的全日零成交可以只有日线。报告中 `minute_coverage_complete` 表示时段覆盖，`volume_difference` 为分钟合计量减去日线成交量。任一项不通过时写出 `passed: false` 并返回 1。按合约、日期和原始来源核实差异，临时调整交易时段须与当日公告核对。

## 扩展到长期样本

- 分钟线覆盖选定品种至少一年及回放涉及的实际交割月，主力回放额外采集样本前一开市日的结算与持仓量。六所多年日线分别按实际开市日和合约存续期核对首末日期与中间缺口。
- 回测记录保留数据版本、源码、参数、运行 ID 和实际首末估值日。首次读取和缓存复跑分别计时，核对两次运行的固定版本、成交、每日结算、分钟排除统计与指标。性能目标见[产品说明](../product.md#三个循环)。
