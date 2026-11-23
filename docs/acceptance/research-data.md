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

## 仓单、会员持仓与库存

- 使用者在部署主机确认 Tushare 的仓单、会员持仓及库存周报权限，按[数据口径](../warehouse/README.md#仓单会员持仓与库存周报)选取有真实报告的 SHFE、INE 及其他商品交易所短区间，记录可取得的最早日期和缺失区间。
- Warehouse 采集完成后，在 Terminal“数据 → 浏览”确认新增报告自动出现、交易所筛选与表格页码保持不变，对照同日交易所报告核对品种、仓库、数量单位、明细与合计；会员持仓分别核对品种级和合约级记录，确认 INE 数据没有混入 SHFE 品种。
- 记录固定数据版本，用 Lab 读取相同记录并核对 Decimal 精度、空值与时间字段；来源未给出发布时间时应为空，采集时间应是本次响应时间。重采后旧版本的数值与观测时间仍不变。
- 核对一次真实自动更新：仓单、会员持仓和库存周报自动跟随已采行情补齐，周报在实际报告日出现数据，非报告日不显示日频缺口。

## 扩展到长期样本

- 分钟线覆盖选定品种至少一年及回放涉及的实际交割月，主力回放额外采集样本前一开市日的结算与持仓量。六所多年日线分别按实际开市日和合约存续期核对首末日期与中间缺口。
- 回测记录保留数据版本、源码、参数、运行 ID 和实际首末估值日。首次读取和缓存复跑分别计时，核对两次运行的固定版本、成交、每日结算、分钟排除统计与指标。性能目标见[产品说明](../product.md#三个循环)。
- 多品种分钟策略记录累计 K 线数、峰值内存、历史重放与换月耗时；多因子评价记录品种数、交易日数、因子数与完整评价耗时。

## 面板模型对照

- 在同一 Warehouse 固定版本上选取覆盖不同产业、缺线和换月的多年主力日线；记录共同观察日数、每品种的实际历史范围及 carry 缺测情况。
- 固定相同品种池、费用、滑点、总权重、调仓周期和留出日，对照 `MomentumRank`、`ContinuousMomentumRank`、`MaximumReturnRank`、`PanelRidgeRank` 与 `PanelRandomFeatureRank`。面板模型分别运行 `use_carry=false/true`，记录训练窗口和随机种子。
- 在训练段扫描 `window`、`train`、`ridge` 和随机特征规模，再以滚动验证生成样本外记录；比较扣费收益、回撤、换手和分阶段表现。核对留出段没有参与参数选择，同一源码、参数与固定版本复跑得到相同目标和成交。
- 抽查跨品种出线不同步及主力换月的调仓日，对照策略目标、下一根可执行开盘和实际成交，确认缺测退出与多空预算符合[策略定义](../lab/catalog.md)。记录首次运行与缓存复跑耗时，并保留运行 ID。

## 历史容量夹具

Linux 开发机用合成 K 线单独测量共享策略上下文，沿用 Trading 的地址空间限制；输出历史数组大小、峰值内存、回放、因子调用和换月耗时。用同一台机器复测，运行期间暂停构建与其他计算任务。

```sh
build/electron-resources/python/bin/python3 -I -B tests/lab/strategy_capacity.py --symbols 64 --bars 65536
build/electron-resources/python/bin/python3 -I -B tests/lab/strategy_capacity.py --symbols 64 --bars 131073
```

`status` 为 `memory_exhausted` 时按 `phase` 和 `completed_bars` 定位容量边界；固定版本真实数据的长期验收按上一节执行。
