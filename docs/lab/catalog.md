# 内置因子与策略

内置库面向期货的完整 K 线、成交量、持仓量和主力期限结构，源码位于 `research/`。研究页自动列出公开函数和策略类；它们与 Trading 使用同一份源码和 NumPy/SciPy 依赖。调用方式与数据口径见 [Lab](README.md#因子)。

## 因子口径

下表的历史长度包含当前 K 线，`w` 为 `window`，`L` 为 `lookback`。除 `roll_yield` 和显式使用 `periods` 的指标外，收益与波动均按一根 K 线计，不自动年化。历史不足、输入缺测或统计量无定义时返回 `NaN`；恒定价格的 RSI/MFI/%K 取中性值 50，Z-Score、CCI、效率和半方差平衡取 0。因子读取当前已完成 K 线，策略目标从下一根起执行。

| 模块 / 函数 | 定义 | 历史长度 |
| --- | --- | --- |
| `price.momentum` | 当前收盘价 / L 根前收盘价 − 1 | L + 1 |
| `price.reversal` | 动量取负 | L + 1 |
| `price.skip_momentum` | L 根前至 `skip` 根前的收益，跳过最近一段 | L + 1 |
| `price.volatility` | 简单收益的样本标准差 | w + 1 |
| `price.momentum_strength` | 简单收益均值 / 均值标准误 | w + 1 |
| `price.distance_to_high` | 当前收盘价 / 窗口最高收盘价 − 1 | w |
| `price.blended_momentum` | 快慢窗口平均简单收益的符号加权，零收益归正 | slow + 1 |
| `price.information_discreteness` | 窗口累计收益符号 ×（负收益根数 − 正收益根数）/ w，越低越连续 | w + 1 |
| `price.return_autocorrelation` | 窗口内相隔 `lag` 根的简单收益对的 Pearson 相关系数 | w + 1 |
| `price.variance_ratio` | Lo–MacKinlay 重叠对数收益方差比，使用有限样本修正 | w + 1 |
| `technical.moving_average_gap` | 快速 SMA / 慢速 SMA − 1 | slow |
| `technical.price_zscore` | 当前收盘价相对窗口均值的总体标准差倍数 | w |
| `technical.rsi` | Cutler RSI：上涨价差总和 / 绝对价差总和 × 100 | w + 1 |
| `technical.stochastic` | 当前收盘在窗口最高与最低价之间的位置 × 100 | w |
| `technical.channel_position` | 当前收盘在前 w 根高低价通道的位置；大于 1 / 小于 0 为突破 | w + 1 |
| `technical.efficiency_ratio` | 净价差绝对值 / 逐根价差绝对值之和 | w + 1 |
| `technical.log_price_slope` | 对数收盘价对 K 线序号的 OLS 斜率 | w |
| `technical.average_true_range` | 真实波幅的简单均值，包含前收盘至当前高低的跳空 | w + 1 |
| `technical.commodity_channel_index` | 典型价偏离均值 /（0.015 × 平均绝对偏差） | w |
| `risk.downside_volatility` | 负收益平方的均值开方，分母为全部 w 根 | w + 1 |
| `risk.upside_volatility` | 正收益平方的均值开方，分母为全部 w 根 | w + 1 |
| `risk.realized_skewness` | 三阶中心矩 / 二阶中心矩的 1.5 次幂 | w + 1 |
| `risk.excess_kurtosis` | 四阶中心矩 / 二阶中心矩平方 − 3 | w + 1 |
| `risk.expected_shortfall` | 最差 `ceil(w × tail)` 个收益的均值取负；全为正收益时可为负 | w + 1 |
| `risk.maximum_return` | 窗口内最大的单根简单收益（MAX），全为负收益时仍为负 | w + 1 |
| `risk.semivariance_balance` |（正收益平方和 − 负收益平方和）/ 全部收益平方和 | w + 1 |
| `risk.parkinson_volatility` | `sqrt(mean(log(H/L)^2) / (4 log(2)))` | w |
| `risk.garman_klass_volatility` | `sqrt(mean(0.5 log(H/L)^2 − (2 log(2)−1) log(C/O)^2))` | w |
| `risk.rogers_satchell_volatility` | `sqrt(mean(log(H/O) log(H/C) + log(L/O) log(L/C)))` | w |
| `risk.yang_zhang_volatility` | 跳空方差、开收盘方差与 Rogers–Satchell 方差的加权和开方 | w + 1 |
| `liquidity.relative_volume` | 当前成交量 / 窗口平均量 − 1 | w |
| `liquidity.volume_weighted_return` | 按同期成交量加权的单根简单收益均值 | w + 1 |
| `liquidity.volume_return_correlation` | 简单收益与同期成交量的 Pearson 相关系数 | w + 1 |
| `liquidity.turnover` | 成交量 / 持仓量的窗口均值 | w |
| `liquidity.return_per_volume` | 绝对简单收益 / 成交手数的窗口均值 | w + 1 |
| `liquidity.money_flow_index` | 按典型价涨跌分组的典型价 × 成交量之比，范围 0–100 | w + 1 |
| `liquidity.chaikin_money_flow` | `sum(((C−L)−(H−C))/(H−L) × volume) / sum(volume)`，范围 −1 至 1 | w |
| `positions.interest_change` | 持仓量相对 L 根前的变化率 | L + 1 |
| `term_structure.roll_yield_carry` | 年化对数展期收益率的窗口均值 | w |
| `term_structure.roll_yield_momentum` | 年化展期收益率相对 L 根前的变化 | L + 1 |
| `term_structure.carry_to_risk` | 窗口平均年化 carry /（收益标准差 × `sqrt(periods)`） | w + 1 |
| `term_structure.carry_zscore` | 当前 carry 相对窗口分布的 Z-Score | w |

RSI 和 ATR 使用有限窗口的简单统计，便于研究评价与交易短窗口逐值一致；RSI 的命名明确为 Cutler 版本。价格比率和对数模型适用于正价格。Parkinson、Garman–Klass 和 Rogers–Satchell 使用单根内部价格范围；Yang–Zhang 另计 `log(O / 前收盘)` 的样本方差，开收盘项权重为 `k = 0.34 / (1.34 + (w+1)/(w−1))`，Rogers–Satchell 项权重为 `1−k`。成交手数流动性指标跨品种比较时受合约规模影响，`return_per_volume` 的单位为每手收益率，不是按成交金额计算的 Amihud 指标。

信息离散度保留零收益根数作为分母的一部分，累计收益为零时取 0。方差比设 `q = lag`：单期对数收益平方离差和除以 `w−1`，重叠 q 期收益扣除 q 倍单期均值后的平方和除以 `q(w−q+1)(1−q/w)`，再取两者之比；大于 1 表示该尺度的同向依赖，小于 1 表示反向依赖，显著性需另行检验。Chaikin Money Flow 使用[标准 CMF 公式](https://chartschool.stockcharts.com/table-of-contents/technical-indicators-and-overlays/technical-indicators/chaikin-money-flow-cmf)，高低价相同时该根贡献为 0，无成交窗口为缺测；它衡量量价位置，不代表真实主动买卖流。

## 策略选择

| 策略标识 | 决策规则 |
| --- | --- |
| `ma_cross.MaCross` | 快慢均线交叉，输出手数 |
| `momentum.Momentum` | 时间序列累计收益符号，输出手数 |
| `breakout.Breakout` | 前高低价通道突破与较短通道退出 |
| `reversion.Reversion` | 价格标准差通道反转，回到均值退出 |
| `oscillators.RsiReversion` | RSI 超买超卖反转，回到 50 退出 |
| `oscillators.AtrBreakout` | 前一窗口均值 ± ATR 倍数突破，穿越均值退出 |
| `momentum_rank.MomentumRank` | 截面动量或反转，可选逆波动权重 |
| `risk_premia.DualMomentum` | 截面动量排名叠加绝对收益方向过滤 |
| `risk_premia.ContinuousMomentumRank` | 信息离散度不高于 `max_discreteness` 的品种按动量排序 |
| `risk_premia.MaximumReturnRank` | 低 MAX 多头、高 MAX 空头 |
| `risk_premia.CarryRank` | 做多年化展期收益率高的品种，做空低的品种 |
| `risk_premia.SkewnessRank` | 做多负偏度品种，做空正偏度品种 |
| `risk_premia.LowVolatilityRank` | 低波动多头、高波动空头 |
| `risk_premia.DownsideRiskRank` | 低下行半标准差多头、高下行半标准差空头 |
| `risk_premia.OpenInterestRank` | 持仓量增长率截面排序 |
| `risk_premia.TrendCarryRank` | 动量和真实 carry 的截面排名混合 |
| `trend.VolatilityTargetMomentum` | 时间序列动量，按实现波动率缩放并限制单品种和总权重 |
| `trend.TurningPointMomentum` | 快慢平均收益符号混合，等权时在方向分歧处持平 |
| `composite.MultiFactorRank` | 动量、短期反转和持仓量变化复合排序 |
| `composite.CompositePortfolio` | MultiFactorRank 趋势分支与 CarryRank 展期收益分支合成 |
| `learning.RidgeForecast` | 价格特征的滚动线性岭回归预测 |
| `learning.RandomFeatureForecast` | 随机 Fourier 特征的滚动岭回归预测 |
| `panel.PanelRidgeRank` | 跨品种合并样本训练一个岭回归，按预测收益排序 |
| `panel.PanelRandomFeatureRank` | 跨品种共享随机 Fourier 特征岭回归，按预测收益排序 |

`risk_premia.*`、`trend.*` 与 `panel.*` 只在所选品种都有完整 K 线的共同时间推进历史和调仓周期；缺一品种的时刻不计入窗口，保留原目标。换月时从 Context 重读共同时间的历史，使用当时复权后的价格。日线组合默认参数按交易日理解，分钟组合需自行填写窗口、调仓周期和年化期数，理由：同一个根数在不同频率下代表不同经济周期。

`risk_premia.*` 与 `panel.*` 排序前排除因子缺测、收益波动为零以及未通过启用的量仓过滤的品种。多空各取 `count` 个，各分配 `gross_weight / 2`，侧内按逆波动率加权；不足两侧数量或两侧边界无法区分时输出零权重。`DualMomentum` 再按收益正负过滤，空出的预算留现金。固定月份合约没有 `roll_yield`，因此 carry 策略应选择主力连续；次主力缺测时相关品种退出候选。

`MomentumRank` 每个入选品种贡献 `weight` 的总预算，启用 `volatility` 后在所有入选品种间按逆波动分配；输出权重向零舍入，以免舍入增加总敞口。

`VolatilityTargetMomentum.target_volatility` 是每个品种的年化风险配置尺度，随后应用 `max_weight` 与 `gross_weight`；组合实际波动还取决于相关性。`TurningPointMomentum.weight` 为每品种名义权重，实例的总权重预算需覆盖所选品种。所有委托仍由 C++ 的账户授权和风控规则决定。

## 论文来源与期货适配

| 论文 | 对应实现 | 适配范围 |
| --- | --- | --- |
| Parkinson，[The Extreme Value Method for Estimating the Variance of the Rate of Return](https://doi.org/10.1086/296071)，Journal of Business 1980 | `parkinson_volatility` | 高低价对数范围估计，按 K 线窗口平均方差后开方 |
| Garman、Klass，[On the Estimation of Security Price Volatilities from Historical Data](https://doi.org/10.1086/296072)，Journal of Business 1980 | `garman_klass_volatility` | 使用高低价范围与开收盘收益的简化二次估计式 |
| Rogers、Satchell，[Estimating Variance From High, Low and Closing Prices](https://doi.org/10.1214/aoap/1177005835)，Annals of Applied Probability 1991 | `rogers_satchell_volatility` | 使用允许非零漂移的基础估计式，按 K 线窗口平均方差后开方 |
| Yang、Zhang，[Drift-Independent Volatility Estimation Based on High, Low, Open, and Close Prices](https://doi.org/10.1086/209650)，Journal of Business 2000 | `yang_zhang_volatility` | 式 (7)、(18) 的开盘跳空与日内波动合并，改为可配置 K 线窗口 |
| Lo、MacKinlay，[Stock Market Prices Do Not Follow Random Walks: Evidence from a Simple Specification Test](https://doi.org/10.1093/rfs/1.1.41)，RFS 1988 | `variance_ratio` | 式 (12) 的方差修正与式 (13) 的比值，输出 VR 本身 |
| Da、Gurun、Warachka，[Frog in the Pan: Continuous Information and Momentum](https://doi.org/10.1093/rfs/hhu003)，RFS 2014 | `information_discreteness`、`ContinuousMomentumRank` | 因子实现式 (1)；策略改为离散度阈值筛选后排序期货动量，原文采用股票双重排序 |
| Bali、Cakici、Whitelaw，[Maxing out: Stocks as lotteries and the cross-section of expected returns](https://doi.org/10.1016/j.jfineco.2010.08.014)，JFE 2011 | `maximum_return`、`MaximumReturnRank` | 原研究为股票月度 MAX；此处按期货 K 线窗口计算，低 MAX 多头、高 MAX 空头作为研究基线 |
| Moskowitz、Ooi、Pedersen，[Time Series Momentum](https://doi.org/10.1016/j.jfineco.2011.11.003)，JFE 2012 | `Momentum`、`VolatilityTargetMomentum` | 使用本项目当时复权的期货收盘收益、有限窗口样本波动和显式权重上限；原研究使用多资产期货和远期样本 |
| Koijen 等，[Carry](https://doi.org/10.1016/j.jfineco.2017.11.002)，JFE 2018 | `CarryRank`、`TrendCarryRank` | 使用主力与后续次主力交割月的年化对数价差；合约选择与数据缺测规则见 Lab |
| Fernandez-Perez 等，[The Skewness of Commodity Futures Returns](https://doi.org/10.1016/j.jbankfin.2017.06.015)，JBF 2018 | `realized_skewness`、`SkewnessRank` | 保留多负偏度、空正偏度方向，使用可调窗口、尾部数量及侧内逆波动权重 |
| Goulding、Harvey、Mazzoleni，[Momentum Turning Points](https://doi.org/10.1016/j.jfineco.2023.05.007)，JFE 2023 | `blended_momentum`、`TurningPointMomentum` | 实现式 (11) 的静态快慢符号混合；将股票月度超额收益换为期货 K 线简单收益，快慢窗口和混合权重可配置 |
| Gu、Kelly、Xiu，[Empirical Asset Pricing via Machine Learning](https://doi.org/10.1093/rfs/hhaa009)，RFS 2020 | `PanelRidgeRank` | 采用式 (4)、(7)、(8) 的跨资产合并损失与岭惩罚，输入改为期货价量仓及可选 carry，滚动重估共享参数 |
| Kelly、Malamud、Zhou，[The Virtue of Complexity in Return Prediction](https://doi.org/10.1111/jofi.13298)，JF 2024 | `RandomFeatureForecast`、`PanelRandomFeatureRank`，配套线性岭回归基线 | 实现式 (20) 的高斯投影 sin/cos 特征与滚动岭回归；改用期货特征、保留未惩罚截距，单品种输出手数、面板预测输出组合权重 |

## 滚动学习与面板训练

`learning.*` 输入六项特征：快慢动量、快速收益波动、慢速价格 Z-Score、快速 RSI、慢速效率比。设 `h = rebalance`，时点 s 的训练标签为 `open[s+h+1] / open[s+1] − 1`，对应下一根开盘建仓、下次调仓开盘结束的持有收益。预测时点 t 只取标签已实现的最近 `train` 条样本，最后一个训练特征为 `X[t−h−1]`，标签结束于当前开盘；`X[t]` 只作预测，理由：训练收益应与可执行时点一致。原始特征均值与标准差、Fourier 特征尺度、截距和回归系数均仅由该训练窗口拟合。`seed` 固定高斯投影，源码副本、参数与同一历史可重复生成目标。

岭回归最小化平均平方误差加 `ridge × 系数平方和`；`features` 是 sin/cos 总数，必须为正偶数，`gamma` 控制投影尺度。预热需 `slow + train + rebalance + 1` 根；`rebalance` 控制重估频率和预测持有期；预测超过正负 `threshold` 后分别做多或做空。重估窗口含缺测时输出零手数，标签不跨入尚未观察的 K 线。模型比较应在固定 Warehouse 版本上使用相同费用、滑点和留出期，再以滚动验证选择参数。

`panel.*` 将慢速窗口命名为 `window`，在上述六项特征上增加慢速信息离散度、MAX、CMF 和持仓量变化，共十项；`use_carry=true` 再加入快速窗口的平均真实 carry。每次从当前通过风险与量仓筛选的品种中，合并最近 `train` 个共同观察时点的已完成标签样本，统一估计特征尺度、截距与系数，每个有效的“品种 × 时点”样本等权。`train` 指每品种的观察时点数，训练行中的缺测逐行排除；全池至少有两行且当前特征完整才预测，缺测 carry 保留为缺测。按预测收益取多空尾部，预热需 `max(window + train + rebalance + 1, volatility + 1)` 个共同时间。

面板标签的持有期按共同观察时钟推进，入场与出场价格均取相应信号后该品种**自己的第一根开盘**；即使另一品种尚未出线，也保留这个可执行价格。最新训练特征仍为 `X[t−h−1]`，最晚出场价在共同观察 t 之前或该根开盘时已知。模型只从当前预测时点之前重估，参数扫描与滚动样本外评价沿用[滚动验证](README.md#参数扫描与滚动验证)；固定版本的多年模型对照见[真实研究验收](../acceptance/research-data.md#面板模型对照)。

## 数据扩展方向

近年的研究也涉及当前接口之外的数据或模型：例如 [Commodity Futures Characteristics and Asset Pricing Models](https://doi.org/10.1002/fut.22559)（JFM 2025）的 IPCA 特征面板模型，以及 [Risk premia in commodity markets](https://doi.org/10.1016/j.jbankfin.2026.107760)（JBF 2026）的多类可交易与不可交易风险因素。库中的动量、偏度和预期短缺是可供研究的输入；这些论文的完整面板模型另需明确训练样本与估计流程。

严格的 [Basis-Momentum](https://doi.org/10.1111/jofi.12738)（JF 2019）需要分别跟踪同一组近远月合约收益；当前 `roll_yield_momentum` 只是展期收益率变化。Warehouse 已接入[仓单、会员持仓与库存周报](../warehouse/README.md#仓单会员持仓与库存周报)，Lab 可查询固定版本；下一步是明确可用时点与策略输入。现货基差、套保压力、订单流与盘口因子还分别需要现货、商业套保者分类持仓和逐笔盘口数据，进入 Trading 的字段需同时纳入预热契约。
