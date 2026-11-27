# 内置因子与策略

内置研究库面向期货的完整 K 线、成交量、持仓量、主力期限结构及 PIT 仓单/会员持仓/库存数据，源码位于 `research/`。研究页自动发现其中全部公开因子函数与策略类，并与 Trading 共用同一份源码及 NumPy / SciPy 运行环境。接口契约与数据口径见 [Lab](README.md#因子)。

## 因子口径

下表“历史长度”均包含当前 K 线，其中 `w` 表示 `window`，`L` 表示 `lookback`。除 `roll_yield` 与显式传入 `periods` 的因子外，收益与波动均按单根 K 线计，不自动年化。历史不足、输入缺测或统计量无定义时返回 `NaN`；价格恒定时 RSI、MFI、%K 取中性值 `50`，EWMAC、Z-Score、CCI、效率比与半方差平衡取 `0`。因子基于当前已完成 K 线计算，策略目标自下一根 K 线起执行。

| 模块 / 函数 | 定义 | 历史长度 |
| --- | --- | --- |
| `price.momentum` | 当前收盘价 / L 根前收盘价 − 1 | L + 1 |
| `price.reversal` | 动量取负 | L + 1 |
| `price.skip_momentum` | L 根前至 `skip` 根前的收益（跳过最近一段） | L + 1 |
| `price.volatility` | 简单收益的样本标准差 | w + 1 |
| `price.momentum_strength` | 简单收益均值 / 均值标准误 | w + 1 |
| `price.ewmac` | 有限窗口截断指数加权快慢均线差 /（当前收盘价 × 慢速收益波动） | slow + 1 |
| `price.overnight_return` | 窗口内开盘价 / 前收盘价的累计复利收益（隔夜/跨时段跳空收益） | w + 1 |
| `price.intraday_return` | 窗口内收盘价 / 开盘价的累计复利收益（日内/单根内收益） | w |
| `price.distance_to_high` | 当前收盘价 / 窗口最高收盘价 − 1 | w |
| `price.blended_momentum` | 快慢窗口平均简单收益的符号加权，零收益归正 | slow + 1 |
| `price.information_discreteness` | 窗口累计收益符号 ×（负收益根数 − 正收益根数）/ w，越低越连续 | w + 1 |
| `price.return_autocorrelation` | 窗口内相隔 `lag` 根的简单收益对的 Pearson 相关系数 | w + 1 |
| `price.variance_ratio` | Lo–MacKinlay 重叠对数收益方差比（有限样本修正） | w + 1 |
| `price.hurst_exponent` | 对数收盘价 `q=2` 结构函数广义 Hurst 指数（< 0.5 均值回复，> 0.5 持续） | w |
| `price.mean_reversion_half_life` | 对数收盘价离散 AR(1) 回归估计的 OU 均值回复半衰期（非回复时为 `NaN`） | w + 1 |
| `technical.moving_average_gap` | 快速 SMA / 慢速 SMA − 1 | slow |
| `technical.price_zscore` | 当前收盘价相对窗口均值的总体标准差倍数 | w |
| `technical.rsi` | Cutler RSI：上涨价差和 / 绝对价差和 × 100 | w + 1 |
| `technical.stochastic` | 当前收盘在窗口高低价区间中的相对位置 × 100 | w |
| `technical.channel_position` | 当前收盘在前 w 根高低通道中的位置（> 1 或 < 0 为突破） | w + 1 |
| `technical.efficiency_ratio` | 净价差绝对值 / 逐根价差绝对值之和 | w + 1 |
| `technical.log_price_slope` | 对数收盘价对 K 线序号的 OLS 斜率 | w |
| `technical.average_true_range` | 真实波幅（含前收盘跳空）的简单均值 | w + 1 |
| `technical.commodity_channel_index` | 典型价偏离均值 /（0.015 × 平均绝对偏差） | w |
| `risk.downside_volatility` | 负收益平方均值开方（分母为全部 w 根） | w + 1 |
| `risk.upside_volatility` | 正收益平方均值开方（分母为全部 w 根） | w + 1 |
| `risk.realized_skewness` | 三阶中心矩 / 二阶中心矩的 1.5 次幂 | w + 1 |
| `risk.excess_kurtosis` | 四阶中心矩 / 二阶中心矩平方 − 3 | w + 1 |
| `risk.expected_shortfall` | 最差 `ceil(w × tail)` 个收益的均值取负（全为正收益时可为负） | w + 1 |
| `risk.maximum_return` | 窗口内最大单根简单收益（MAX；全为负收益时仍为负） | w + 1 |
| `risk.semivariance_balance` |（正收益平方和 − 负收益平方和）/ 全部收益平方和 | w + 1 |
| `risk.volatility_ratio` | 短窗口收益标准差 / 长窗口收益标准差（波动率扩张/压缩比） | slow + 1 |
| `risk.parkinson_volatility` | `sqrt(mean(log(H/L)^2) / (4 log(2)))` | w |
| `risk.garman_klass_volatility` | `sqrt(mean(0.5 log(H/L)^2 − (2 log(2)−1) log(C/O)^2))` | w |
| `risk.rogers_satchell_volatility` | `sqrt(mean(log(H/O) log(H/C) + log(L/O) log(L/C)))` | w |
| `risk.yang_zhang_volatility` | 跳空方差、开收盘方差与 Rogers–Satchell 方差的加权和开方 | w + 1 |
| `liquidity.relative_volume` | 当前成交量 / 窗口平均成交量 − 1 | w |
| `liquidity.volume_weighted_return` | 按同期成交量加权的单根简单收益均值 | w + 1 |
| `liquidity.volume_return_correlation` | 简单收益与同期成交量的 Pearson 相关系数 | w + 1 |
| `liquidity.turnover` | 成交量 / 持仓量的窗口均值 | w |
| `liquidity.return_per_volume` | 绝对简单收益 / 成交手数的窗口均值 | w + 1 |
| `liquidity.money_flow_index` | 按典型价涨跌分组的典型价 × 成交量之比（0–100） | w + 1 |
| `liquidity.chaikin_money_flow` | `sum(((C−L)−(H−C))/(H−L) × volume) / sum(volume)`（−1 至 1） | w |
| `liquidity.corwin_schultz_spread` | 相邻 K 线高低价隐含买卖价差（Corwin–Schultz）的窗口均值 | w + 1 |
| `liquidity.vwap_deviation` | 当前收盘价 / 窗口成交量加权均价（VWAP） − 1 | w |
| `positions.interest_change` | 持仓量相对 L 根前的变化率 | L + 1 |
| `positions.oi_price_confirmation` | 收益符号 × 单根持仓量变化率的窗口均值（增仓顺势确认度） | w + 1 |
| `term_structure.roll_yield_carry` | 年化对数展期收益率的窗口均值 | w |
| `term_structure.roll_yield_momentum` | 年化展期收益率相对 L 根前的变化 | L + 1 |
| `term_structure.carry_to_risk` | 窗口平均年化 carry /（收益标准差 × `sqrt(periods)`） | w + 1 |
| `term_structure.carry_zscore` | 当前 carry 相对窗口分布的 Z-Score | w |
| `term_structure.basis_momentum` | 窗口内同合约近月累计收益减次主力远月累计收益 | w |
| `fundamentals.receipt_change` | PIT 注册标准仓单量相对 L 根前的变化率 | L + 1 |
| `fundamentals.receipt_to_oi` | PIT 注册标准仓单量与持仓量之比的窗口均值 | w |
| `fundamentals.receipt_zscore` | PIT 注册标准仓单量相对窗口分布的样本 Z-Score | w |
| `fundamentals.member_net_ratio` | PIT 前排会员净多空失衡 `(多 − 空) / (多 + 空)` 窗口均值 | w |
| `fundamentals.member_net_change` | PIT 前排会员净多空失衡相对 L 根前的变化 | L + 1 |
| `fundamentals.inventory_change` | PIT 周度交易所总库存相对 L 根前的变化率 | L + 1 |
| `fundamentals.inventory_zscore` | PIT 周度交易所总库存相对窗口分布的样本 Z-Score | w |

实现细节说明：

- RSI、ATR 与 `price.ewmac` 均采用有限窗口统计（EWMAC 在最近 `slow` 根内做截断归一化的指数权重），确保因子评价与实盘短窗口逐值一致（RSI 明确为 Cutler 口径）。价格比率与对数估计式均以正价格为前提。Parkinson、Garman–Klass 与 Rogers–Satchell 使用单根内部价差；Yang–Zhang 另计 `log(O / 前收盘)` 样本方差，开收盘项权重为 `k = 0.34 / (1.34 + (w+1)/(w−1))`，Rogers–Satchell 项权重为 `1−k`。成交手数类指标跨品种受合约规模影响，`return_per_volume` 为每手收益率，并非按成交金额计算的 Amihud 指标。
- 信息离散度将零收益根数保留在分母中，累计收益为零时取 `0`。方差比令 `q = lag`：单期对数收益平方离差和除以 `w−1`，重叠 `q` 期收益扣除 `q` 倍单期均值后的平方和除以 `q(w−q+1)(1−q/w)`，再取两者之比（> 1 表示同向依赖，< 1 表示反向依赖，显著性需另行检验）。`hurst_exponent` 要求 `w >= 8` 且 `min(max_lag, w // 2) >= 3`。Chaikin Money Flow 采用[标准 CMF 公式](https://chartschool.stockcharts.com/table-of-contents/technical-indicators-and-overlays/technical-indicators/chaikin-money-flow-cmf)，高低价相等时当根贡献为 `0`，无成交窗口返回 `NaN`；它度量量价相对位置，不代表真实主动买卖流。

## 策略选择

| 策略标识 | 决策规则 |
| --- | --- |
| `ma_cross.MaCross` | 快慢均线交叉，输出手数 |
| `momentum.Momentum` | 时间序列累计收益符号，输出手数 |
| `breakout.Breakout` | 前高低价通道突破入场，较短通道退出 |
| `reversion.Reversion` | 价格标准差通道反转，回归均值退出 |
| `oscillators.RsiReversion` | RSI 超买超卖反转，回归 50 退出 |
| `oscillators.AtrBreakout` | 前一窗口均值 ± ATR 倍数突破，穿越均值退出 |
| `momentum_rank.MomentumRank` | 截面动量或反转排序，可选逆波动加权 |
| `risk_premia.DualMomentum` | 截面动量排序叠加绝对收益方向过滤 |
| `risk_premia.ContinuousMomentumRank` | 筛选信息离散度不高于 `max_discreteness` 的品种后按动量排序 |
| `risk_premia.MaximumReturnRank` | 低 MAX 做多、高 MAX 做空 |
| `risk_premia.CarryRank` | 高年化展期收益率做多、低者做空 |
| `risk_premia.BasisMomentumRank` | 高近远月基差动量做多、低者做空 |
| `risk_premia.SkewnessRank` | 负偏度做多、正偏度做空 |
| `risk_premia.LowVolatilityRank` | 低波动做多、高波动做空 |
| `risk_premia.DownsideRiskRank` | 低下行半标准差做多、高者做空 |
| `risk_premia.OpenInterestRank` | 持仓量增长率截面排序 |
| `risk_premia.TrendCarryRank` | 动量与真实 carry 截面排名混合 |
| `risk_premia.SectorNeutralRank` | 动量、偏度与可选 carry 混合后按中国期货板块去均值并约束板块总敞口 |
| `fundamentals.InventoryCarryRank` | PIT 仓单/库存稀缺度（`-zscore`，仓单缺测时回退周度库存）与 carry 截面排名混合 |
| `fundamentals.MemberFlowRank` | PIT 前排会员净多空失衡水平与变化率截面排名混合 |
| `trend.VolatilityTargetMomentum` | 时间序列动量，按实现波动率缩放并约束单品种与总权重 |
| `trend.MultiHorizonTrend` | 三档 EWMAC（`fast/4×`、`2×/8×`、`4×/16×`）经 `tanh` 压缩与 EWMA 波动率目标化，配板块上限与换手死区 |
| `trend.TurningPointMomentum` | 快慢平均收益符号混合，等权时方向分歧处空仓 |
| `pairs.KalmanPairs` | 双品种对数价格 2 状态 Kalman 动态对冲价差套利，含 Z-Score 开平仓、止损与可选 OU 半衰期过滤 |
| `composite.MultiFactorRank` | 动量、短期反转与持仓量变化复合排序 |
| `composite.CompositePortfolio` | `MultiFactorRank` 趋势分支与 `CarryRank` 展期收益分支合成 |
| `composite.RiskParityPortfolio` | `MultiFactorRank`、`CarryRank`、`BasisMomentumRank` 与 `SkewnessRank` 四分支按因果 HRP / ERC 动态配置风险预算，配板块上限与换手死区 |
| `learning.RidgeForecast` | 价格特征的滚动线性岭回归预测 |
| `learning.RandomFeatureForecast` | 随机 Fourier 特征的滚动岭回归预测 |
| `panel.PanelRidgeRank` | 跨品种合并样本训练共享岭回归，按预测收益排序 |
| `panel.PanelRandomFeatureRank` | 跨品种共享随机 Fourier 特征岭回归，按预测收益排序 |

组合与权重分配口径：

- `risk_premia.*`、`fundamentals.*`、`trend.*`、`pairs.*` 与 `panel.*` 仅在全部所选品种均具备完整 K 线的共同观察时点推进历史与调仓计数；缺任一品种时跳过该时刻、保留原目标。换月时从 `Context` 重读共同时间历史以取得当时复权价格。默认参数按日线交易日设定，用于分钟组合时须自行调整窗口、调仓周期与年化期数（理由：同一根数在不同频率下对应完全不同的时间跨度）。
- `risk_premia.*`、`fundamentals.*` 与 `panel.*` 排序前剔除因子缺测、收益波动为零及未通过量仓过滤的品种。多空两侧各选 `count` 个、各配 `gross_weight / 2` 并在侧内按逆波动加权；不足两侧数量或边界无法区分时输出零权重。`DualMomentum` 额外按绝对收益符号过滤，空余预算留为现金。`SectorNeutralRank` 按 `asterion_strategy.sector_neutralize` 在中国期货板块内去均值（单品种孤立板块去均值后得 `0`），再按 `max_sector_gross` 限额。固定月份合约无 `roll_yield` 与近远月收益，carry / basis-momentum 类策略须配置主力连续。
- `MomentumRank` 按每入选品种 `weight` 汇总总预算，开启 `volatility` 后在全部入选品种间按逆波动分配；输出权重向零舍入，防止舍入放大总敞口。
- `VolatilityTargetMomentum.target_volatility` 与 `MultiHorizonTrend.target_volatility` 为单品种年化波动配置尺度，随后受 `max_weight`、`max_sector_gross` 与 `gross_weight` 裁剪（组合实际波动受相关性影响）；`MultiHorizonTrend` 各档 EWMAC 按随机游走解析方差归一后再经 `tanh` 压缩。`TurningPointMomentum.weight` 为每品种名义权重，实例总权重预算须覆盖所选品种。
- `pairs.KalmanPairs` 仅接受恰好两个品种，窗口内对数价格以窗口起点归零以保证跨换月缩放完全不变；按动态对冲比 $\beta_t > 0$ 将 `gross_weight` 按 $1/(1+\beta_t)$ 与 $\beta_t/(1+\beta_t)$ 分配到两腿并向零舍入。`composite.RiskParityPortfolio` 通过 `CombinedStrategy` 在因果代理收益窗口上以 Ledoit–Wolf 收缩协方差计算 HRP / ERC 权重。全部委托最终仍由 C++ 账户授权与风控规则把关。

## 论文来源与期货适配

| 论文 | 对应实现 | 适配说明 |
| --- | --- | --- |
| Uhlenbeck、Ornstein，[On the Theory of the Brownian Motion](https://doi.org/10.1103/PhysRev.36.823)，Phys. Rev. 1930 | `mean_reversion_half_life`、`KalmanPairs` | 对数收盘价或价差离散 AR(1) 滚动 OLS 估计半衰期；非回复窗口返回 `NaN` |
| Hurst，[Long-Term Storage Capacity of Reservoirs](https://doi.org/10.1061/TACEAT.0006518)，ASCE 1951；Di Matteo，[Multi-scaling in finance](https://doi.org/10.1080/14697680600969727)，QF 2007 | `hurst_exponent` | 实现式 (2) `q=2` 对数价格结构函数斜率，改为有限滚动窗口 |
| Markowitz，*Portfolio Selection*，1959；Barndorff-Nielsen 等，[Measuring Downside Risk — Realised Semivariance](https://doi.org/10.1093/acprof:oso/9780199549498.003.0007)，2010 | `downside_volatility`、`upside_volatility` | 按全部 `window` 根计算的负/正简单收益均方根 |
| Kalman，[A New Approach to Linear Filtering and Prediction Problems](https://doi.org/10.1115/1.3662552)，JBE 1960；Vidyamurthy 2004；Chan 2013 | `KalmanPairs` | 对窗口起点归零的对数价格做 2 状态 Kalman 动态回归，以一步预测残差 Z-Score 驱动配对开平仓与止损 |
| Donchian 1960；Faith，*Way of the Turtle*，2007；Keltner 1960；Wilder 1978；Lambert 1980；Lane 1984；Quong & Soudack 1989；Kaufman 1995；Bollinger 2001 | `rsi`、`stochastic`、`efficiency_ratio`、`average_true_range`、`commodity_channel_index`、`money_flow_index`、`Breakout`、`Reversion`、`RsiReversion`、`AtrBreakout` | 全部改为有限窗口简单统计（RSI 为 Cutler 口径、ATR 为简单均值），保证短窗口研究与实盘一致 |
| Parkinson，[The Extreme Value Method for Estimating the Variance of the Rate of Return](https://doi.org/10.1086/296071)，JB 1980 | `parkinson_volatility` | 实现式 (2.6) 高低价对数范围估计，按 K 线窗口平均方差后开方 |
| Garman、Klass，[On the Estimation of Security Price Volatilities from Historical Data](https://doi.org/10.1086/296072)，JB 1980 | `garman_klass_volatility` | 实现式 (20) 高低价范围与开收盘收益简化二次估计式 |
| Lo、MacKinlay，[Stock Market Prices Do Not Follow Random Walks: Evidence from a Simple Specification Test](https://doi.org/10.1093/rfs/1.1.41)，RFS 1988 | `variance_ratio` | 实现式 (12) 有限样本方差修正与式 (13) 比值，直接输出 VR |
| Rogers、Satchell，[Estimating Variance From High, Low and Closing Prices](https://doi.org/10.1214/aoap/1177005835)，AAP 1991 | `rogers_satchell_volatility` | 实现式 (3.1) 允许非零漂移的连续路径估计式 |
| Jegadeesh、Titman，[Returns to Buying Winners and Selling Losers](https://doi.org/10.1111/j.1540-6261.1993.tb04702.x)，JF 1993；Miffre、Rallis，[Momentum strategies in commodity futures markets](https://doi.org/10.1016/j.jbankfin.2006.10.020)，JBF 2007 | `skip_momentum`、`MomentumRank`、`DualMomentum` | 期货共同观察 K 线截面动量排序，支持跳过近期反转段与逆波动配权 |
| Bessembinder、Seguin，[Price Volatility, Trading Volume, and Market Depth: Evidence from Futures Markets](https://doi.org/10.2307/2331149)，JFQA 1993 | `oi_price_confirmation` | 以收益符号乘以单根持仓量变化率的窗口均值度量顺势增仓确认度 |
| Yang、Zhang，[Drift-Independent Volatility Estimation Based on High, Low, Open, and Close Prices](https://doi.org/10.1086/209650)，JB 2000 | `yang_zhang_volatility` | 合并式 (7)、(18) 的开盘跳空与日内波动，改为可配置 K 线窗口 |
| Amihud，[Illiquidity and stock returns](https://doi.org/10.1016/S1386-4181(01)00024-6)，JFM 2002 | `return_per_volume` | 式 (1) 改为按期货成交手数计算（非成交金额），跨品种受合约乘数影响 |
| Acerbi、Tasche，[On the coherence of expected shortfall](https://doi.org/10.1016/S0378-4266(02)00283-2)，JBF 2002 | `expected_shortfall` | 实现式 (3.2) 有限样本经验尾部均值取负 |
| George、Hwang，[The 52-Week High and Momentum Investing](https://doi.org/10.1111/j.1540-6261.2004.00695.x)，JF 2004 | `distance_to_high` | 改为当前收盘价相对可配置窗口最高收盘价的偏离 |
| Ang、Hodrick、Xing、Zhang，[The Cross-Section of Volatility and Expected Returns](https://doi.org/10.1111/j.1540-6261.2006.00836.x)，JF 2006；Ang、Chen、Xing，[Downside Risk](https://doi.org/10.1093/rfs/hhj035)，RFS 2006 | `LowVolatilityRank`、`DownsideRiskRank` | 改用期货总波动率与下行半标准差做多空等预算逆波动排序 |
| Rahimi、Recht，[Random Features for Large-Scale Kernel Machines](https://papers.nips.cc/paper/2007/hash/013a006f03dbc5392effeb8f18fda755-Abstract.html)，NeurIPS 2007 | `asterion_strategy.random_fourier_features`、`RandomFeatureForecast`、`PanelRandomFeatureRank` | 与 Kelly 等 (2024) 配合构成固定种子随机 Fourier 特征映射 |
| Maillard、Roncalli、Teïletche，[The Properties of Equally Weighted Risk Contribution Portfolios](https://doi.org/10.3905/jpm.2010.36.4.060)，JPM 2010；López de Prado，[Building Diversified Portfolios that Outperform Out-of-Sample](https://doi.org/10.3905/jpm.2016.42.4.059)，JPM 2016 | `RiskParityPortfolio` | 在四条低相关期货风险溢价子策略的因果代理收益上用 Ledoit–Wolf 收缩协方差求 HRP / ERC 权重 |
| Bali、Cakici、Whitelaw，[Maxing out: Stocks as lotteries and the cross-section of expected returns](https://doi.org/10.1016/j.jfineco.2010.08.014)，JFE 2011 | `maximum_return`、`MaximumReturnRank` | 原文为股票月度 MAX；此处按期货 K 线窗口计算，以低 MAX 多头、高 MAX 空头作研究基线 |
| Corwin、Schultz，[A Simple Way to Estimate Bid-Ask Spreads from Daily High and Low Prices](https://doi.org/10.1111/j.1540-6261.2012.01729.x)，JF 2012 | `corwin_schultz_spread` | 实现式 (14)–(18) 相邻高低价隐含价差，并做可配置窗口因果均值 |
| Hong、Yogo，[What Does Futures Market Interest Tell Us about the Macroeconomy and Asset Prices?](https://doi.org/10.1016/j.jfineco.2012.04.005)，JFE 2012 | `interest_change`、`OpenInterestRank` | 改为单品种持仓量变化率与截面排序（非全市场汇总持仓） |
| Moskowitz、Ooi、Pedersen，[Time Series Momentum](https://doi.org/10.1016/j.jfineco.2011.11.003)，JFE 2012；Antonacci 2014 | `Momentum`、`VolatilityTargetMomentum`、`DualMomentum` | 采用本项目当时复权收盘收益、有限窗口样本波动与显式权重上限 |
| Asness、Moskowitz、Pedersen，[Value and Momentum Everywhere](https://doi.org/10.1111/jofi.12021)，JF 2013 | `skip_momentum`、`SectorNeutralRank` | 动量、偏度与可选 carry 混合后按中国期货板块去均值并做板块限额 |
| Gorton、Hayashi、Rouwenhorst，[The Fundamentals of Commodity Futures Returns](https://doi.org/10.1093/rof/rfs019)，RoF 2013 | `receipt_change`、`receipt_to_oi`、`receipt_zscore`、`inventory_change`、`inventory_zscore`、`InventoryCarryRank` | 采用中国交易所 PIT 注册仓单与周度总库存的稀缺度与变化率，并结合真实展期收益排序 |
| Da、Gurun、Warachka，[Frog in the Pan: Continuous Information and Momentum](https://doi.org/10.1093/rfs/hhu003)，RFS 2014 | `information_discreteness`、`ContinuousMomentumRank` | 因子实现式 (1)；策略改为先按离散度阈值筛选再排序期货动量（原文为股票双重排序） |
| Baz 等，[Dissecting Investment Strategies in the Cross Section and Time Series](https://ssrn.com/abstract=2695101)，2015；Carver 2015；Hurst、Ooi、Pedersen，[A Century of Evidence on Trend-Following Investing](https://doi.org/10.3905/jpm.2017.44.1.015)，JPM 2017 | `momentum_strength`、`ewmac`、`MultiHorizonTrend` | 因子采用截断有限窗口 EWMAC 与收益 t 统计量；策略组合三档 `ts_ema`/`ts_ewma_vol` 经随机游走方差归一与 `tanh` 压缩后按目标波动率配权 |
| Patton、Sheppard，[Good Volatility, Bad Volatility: Signed Jumps and The Persistence of Volatility](https://doi.org/10.1162/REST_a_00503)，REStat 2015 | `semivariance_balance` | 实现式 (4) 有符号已实现半方差差并用总已实现方差归一化 |
| Koijen 等，[Carry](https://doi.org/10.1016/j.jfineco.2017.11.002)，JFE 2018 | `roll_yield_carry`、`CarryRank`、`TrendCarryRank`、`InventoryCarryRank` | 采用主力与后续次主力交割月的年化对数价差；合约选择与缺测规则见[Lab 主力连续](README.md#主力连续) |
| Fernandez-Perez 等，[The Skewness of Commodity Futures Returns](https://doi.org/10.1016/j.jbankfin.2017.06.015)，JBF 2018 | `realized_skewness`、`SkewnessRank`、`SectorNeutralRank` | 保留多负偏度、空正偏度方向，支持可调窗口、尾部个数与侧内逆波动权重 |
| Boons、Prado，[Basis-Momentum](https://doi.org/10.1111/jofi.12738)，JF 2019 | `basis_momentum`、`BasisMomentumRank` | 实现式 (2) 近远月累计收益差，输入改为同合约单根收益 `near_return` / `far_return`，窗口可配置 |
| Lou、Polk、Skouras，[A tug of war: Overnight versus intraday expected returns](https://doi.org/10.1016/j.jfineco.2019.03.011)，JFE 2019 | `overnight_return`、`intraday_return` | 实现式 (1)–(2) 隔夜跳空与日内收益分解，扩展至期货夜盘/日盘开收盘口径 |
| Kang、Rouwenhorst、Tang，[A Tale of Two Premiums: The Role of Hedgers and Speculators in Commodity Futures Markets](https://doi.org/10.1111/jofi.12845)，JF 2020 | `member_net_ratio`、`member_net_change`、`MemberFlowRank` | 改用中国交易所 PIT 前排会员多空持仓失衡水平与变化替代 CFTC 商业/非商业分类持仓 |
| Gu、Kelly、Xiu，[Empirical Asset Pricing via Machine Learning](https://doi.org/10.1093/rfs/hhaa009)，RFS 2020 | `PanelRidgeRank` | 采用式 (4)、(7)、(8) 跨资产合并损失与岭惩罚，输入改为期货价量仓及可选 carry，滚动重估共享参数 |
| Goulding、Harvey、Mazzoleni，[Momentum Turning Points](https://doi.org/10.1016/j.jfineco.2023.05.007)，JFE 2023 | `blended_momentum`、`TurningPointMomentum` | 实现式 (11) 静态快慢符号混合；将股票月度超额收益改为期货 K 线简单收益，窗口与混合权重可配置 |
| Kelly、Malamud、Zhou，[The Virtue of Complexity in Return Prediction](https://doi.org/10.1111/jofi.13298)，JF 2024 | `RandomFeatureForecast`、`PanelRandomFeatureRank`（配线性岭回归基线） | 实现式 (20) 高斯投影 sin/cos 特征与滚动岭回归；改用期货特征、保留未惩罚截距，单品种输出手数、面板输出组合权重 |

## 滚动学习与面板训练

- **单品种滚动学习（`learning.*`）**：输入 6 项特征——快慢动量、快速收益波动、慢速价格 Z-Score、快速 RSI 与慢速效率比。设 `h = rebalance`，时点 `s` 的标签为 `open[s+h+1] / open[s+1] − 1`（对应下一根开盘建仓、下次调仓开盘平仓的可执行持有收益）。预测时点 `t` 仅取标签已实现的最近 `train` 条样本，末条训练特征为 `X[t−h−1]`、标签止于当前开盘，`X[t]` 只作预测（理由：训练标签必须严格对齐可执行时点）。特征均值/标准差、Fourier 尺度、截距与回归系数均在当前训练窗口内拟合；`seed` 固定随机投影，保证相同源码、参数与历史完全复现。
- **岭回归与超参**：目标函数最小化均方误差加 `ridge × 系数平方和`；`features` 为 sin/cos 总数（须为正偶数），`gamma` 控制投影尺度。预热需 `slow + train + rebalance + 1` 根；`rebalance` 同时控制重估频率与预测持有期；预测值越过正负 `threshold` 时分别做多或做空。重估窗口含缺测时输出零手数，标签绝不跨越未观察 K 线。模型比较须在同一固定 Warehouse 版本、相同费用/滑点与留出期下进行，再通过滚动验证选参。
- **跨品种面板学习（`panel.*`）**：将慢速窗口命名为 `window`，在上述 6 项特征基础上增加慢速信息离散度、MAX、CMF 与持仓量变化共 10 项，`use_carry=true` 时再加入快速窗口平均真实 carry。每次从通过风险与量仓筛选的品种中合并最近 `train` 个共同观察时点的已实现标签样本，等权拟合共享的特征尺度、截距与系数（`train` 指每品种时点数，含缺测的训练行逐行剔除；全池至少 2 行且当前特征完整才预测，缺测 carry 保持缺测）。按预测收益选多空尾部，预热需 `max(window + train + rebalance + 1, volatility + 1)` 个共同观察时点。
- **面板标签与可执行价格**：持有期按共同观察时钟推进，入场与出场价均取对应信号后该品种**自身的第一根开盘价**（即使另一品种当时未出线也保留该可执行价格）。最新训练特征仍为 `X[t−h−1]`，最晚出场价在共同观察 `t` 之前或该根开盘已知。模型严格在当前时点之前重估，样本外验证沿用[参数扫描与滚动验证](README.md#参数扫描与滚动验证)，多年面板对照流程见[真实研究数据验收](../acceptance/research-data.md#面板模型对照)。

## 数据扩展方向

近年文献也涉及当前接口之外的数据与模型，如 [Commodity Futures Characteristics and Asset Pricing Models](https://doi.org/10.1002/fut.22559)（JFM 2025）的 IPCA 面板模型，以及 [Risk premia in commodity markets](https://doi.org/10.1016/j.jbankfin.2026.107760)（JBF 2026）的多类可交易与不可交易风险因子；内置动量、偏度与预期短缺已可作为基础输入，完整面板模型需另行界定样本与估计流程。

近远月同合约收益（`basis_momentum`）与[仓单、会员持仓与库存周报](../warehouse/README.md#仓单会员持仓与库存周报)的 PIT 对齐字段已接入标准策略与因子接口，并同步纳入预热摘要契约。现货基差、商业套保者分类持仓与逐笔盘口因子还分别需要现货价格、分类持仓与 Tick 数据，未来接入时须沿用同一 PIT 规则与预热契约。
