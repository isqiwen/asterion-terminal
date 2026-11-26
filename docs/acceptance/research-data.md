# 真实研究数据验收

验收前选定一个含夜盘的具体交割月合约并写明起止交易日。在目标机完成 Warehouse 采集后，记录一次真实每日自动更新、Terminal 数据页展示的数据版本号及目标分区，并在主机同版本研究清单中核对各分区来源。公开读接口返回分区键、行数与 SHA-256，来源规则见[仓库与固定版本](../warehouse/README.md#仓库与固定版本)。

## 核对短样本

在同提交的开发机准备好 Debug 构建与 Lab Python 运行时。以下 Linux 示例复用 Terminal 已登记的 Warehouse 连接，`DATASET_VERSION` 填写上一步记录的完整版本号：

```sh
build/electron-resources/python/bin/python3 -I -B tests/acceptance/research_minutes.py \
  --terminal-data "$HOME/.local/share/asterion/terminal/data" --lab-directory build/acceptance/research \
  --version "$DATASET_VERSION" --contract SHFE/rb/2027-01 \
  --first-day 2026-11-10 --last-day 2026-11-17 --minutes 5 --report build/acceptance/research/rb-2027-01.json
```

输出报告逐日列出日历开闭市、分钟分区与摘要、原始分钟及聚合周期的缺口、来源标记、零成交分钟、分钟合计量与日线成交量，时间区间两端均为北京时间的**周期结束标签**。当全部目标分区已缓存至本机时，可省略 `--terminal-data` 以同一版本离线复核。

退出码 `0` 表示请求区间在合约存续期内至少覆盖一个开市日，且所需日历、结算价与分钟覆盖齐全、成交量逐日一致（全日零成交日允许仅有日线）。报告中 `minute_coverage_complete` 表示时段完整覆盖，`volume_difference` 为分钟合计量减日线成交量；任一项不满足时写出 `passed: false` 并返回退出码 `1`。发现差异须按合约、日期与原始来源核实，临时交易时段调整须对照当日交易所公告。

## 仓单、会员持仓与库存

- 在部署主机确认 Tushare 账户对仓单、会员持仓与库存周报的积分权限，按[仓单、会员持仓与库存周报](../warehouse/README.md#仓单会员持仓与库存周报)选取有真实报告的 SHFE、INE 及其他商品交易所短区间，记录可取得的最早日期与缺失区间。
- Warehouse 采集完成后，在 Terminal“数据 → 浏览”确认新报告自动刷新且交易所筛选与页码保持不变；对照同日交易所原报核对品种、仓库、数量单位、明细与合计；会员持仓分别核对品种级与合约级记录，确认 INE 数据未混入 SHFE。
- 上期所库存周报重点核对同名仓库与厂库的 `warehouse_type`、燃料油独立品级、黄金仅存一条汇总，以及空值与零值保持原报语义。
- 记录固定版本号，通过 Lab 读取相同记录并核对 `Decimal` 精度、空值与时间字段：来源未提供发布时间时 `published_ms` 为空，`observed_ms` 为本次响应时间；重采修订后旧版本数值与观测时间保持不变。
- 核对一次真实自动更新：确认仓单、会员持仓与库存周报自动跟随已采行情补齐，且库存周报在报告日正常出现、非报告日不报日频缺口。

## 扩展到长期样本

- 分钟线至少覆盖选定品种一年及回放涉及的全部交割月，主力连续回放另须覆盖样本前一开市日的结算价与持仓量；六家交易所多年日线按实际开市日与合约存续期分别核对首末日期与中间缺口。
- 回测记录须完整保留数据版本、冻结源码、参数、运行 ID 与实际首末估值日。分别计时首次拉取与缓存复跑，核对两次运行的版本、成交明细、逐日结算、分钟排除统计与指标完全一致；性能目标见[三个循环](../product.md#三个循环)。
- 多品种分钟策略记录累计 K 线数、峰值内存、历史重放与换月耗时；多因子评价记录品种数、交易日数、因子数与完整耗时。

## 面板模型对照

- 在同一固定 Warehouse 版本上选取横跨多产业、含缺线与换月的多年主力日线，记录共同观察日数、各品种实际范围与 carry 缺测比例。
- 固定相同品种池、费用、滑点、总权重、调仓周期与留出日，横向对照 `MomentumRank`、`ContinuousMomentumRank`、`MaximumReturnRank`、`PanelRidgeRank` 与 `PanelRandomFeatureRank`；面板模型分别运行 `use_carry=false/true` 并记录训练窗口与随机种子。
- 在训练段扫描 `window`、`train`、`ridge` 与随机特征规模，再经滚动验证产出样本外记录，比较扣费收益、回撤、换手与分阶段表现；核对留出段未参与选参，且同源码、同参数、同版本复跑得到完全一致的目标与成交。
- 抽查跨品种出线不同步与主力换月的调仓日，对照策略目标、下一根可执行开盘价与实际成交，核验缺测退出与多空预算符合[内置因子与策略](../lab/catalog.md)口径。记录首次运行与缓存复跑耗时并保留运行 ID。

## 历史容量夹具

在 Linux 开发机上使用合成 K 线单独测量共享策略上下文（沿用 Trading 的地址空间限制），输出历史数组规模、峰值内存、回放、因子计算与换月耗时。复测须在同一台机器执行，期间暂停构建与其他重计算任务：

```sh
build/electron-resources/python/bin/python3 -I -B tests/lab/strategy_capacity.py --symbols 64 --bars 65536
build/electron-resources/python/bin/python3 -I -B tests/lab/strategy_capacity.py --symbols 64 --bars 131073 --history-limit 2048
```

当 `status` 为 `memory_exhausted` 时，依据 `phase` 与 `completed_bars` 定位容量边界；真实固定版本数据的长期验收按上一节执行。
