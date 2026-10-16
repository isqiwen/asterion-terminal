# 因子容量测量与重复准备消除（2026-10-15）

基线 `768b05a` 加本轮结算保证金改动；优化前后均为同一工作目录的 Release 构建。
目标是辨别当前 200,000 条输入范围的读取、准备、计算与内存开销，依据测量做最小改动。

## 可重复的测量

`tests/tasks/factor_capacity.cpp` 是显式构建的离线探针，目标
`asterion_factor_capacity_probe`，不属于产品客户端，也不作为具有机器相关耗时阈值的 CTest。
只生成测试用输入到指定新文件，拒绝覆盖现有文件。生产服务不使用这些合成行情。

```sh
cmake --build build/Release --target asterion_factor_capacity_probe -j8
mkdir -p build/reviews/factor-capacity-2026-10-15
build/Release/asterion_factor_capacity_probe --generate --input build/reviews/factor-capacity-2026-10-15/daily-20k.pb --series 20 --rows 1000
build/Release/asterion_factor_capacity_probe --generate --input build/reviews/factor-capacity-2026-10-15/daily-200k.pb --series 100 --rows 2000
build/Release/asterion_factor_capacity_probe --generate --input build/reviews/factor-capacity-2026-10-15/bars-200k.pb --series 1 --rows 200000 --bars
build/Release/asterion_factor_capacity_probe --generate --input build/reviews/factor-capacity-2026-10-15/volatility-200k.pb --series 100 --rows 2000 --volatility --window 252
build/Release/asterion_factor_capacity_probe --input build/reviews/factor-capacity-2026-10-15/daily-200k.pb --phase engine
```

每组分别以 `read`、`validate`、`align`、`evaluate`、`engine`、`result` 运行三次，
每次新进程；下表为阶段耗时中位数。输入文件相同，SHA-256 随记录保存。
`engine` 包括输入校验/对齐/算法与输出构造；其它阶段用于定位，不能把它们再相加当成实际总耗时。
`result` 的结果校验计时排除先前执行，但其峰值 RSS 是该进程此前执行在内的高水位。
文件读取是本机热缓存的 Protobuf 读取，另记解析时间，不是供应商、SQLite 查询或远程 RPC 的测量。
未与构建、CTest 或 E2E 并行测量。本机 Linux x86_64、Intel Core i7-13700KF、
glibc 2.43，Release 优化构建；环境详情记录在 `environment.txt`。

原始数据：`build/reviews/factor-capacity-2026-10-15/{baseline,after}.jsonl`，各 72 条。

## 发现与实现

20 万条日线的动量算法约 12 ms，对齐约 90 ms，输入校验约 307 ms，完整引擎约 407 ms。
同一次计算中，定义校验为计数构造完整观测矩阵，输入校验又构造一次，引擎再次构造；
结果验收也为定义、时间轴、计数重复构造三次。数据准备明显比动量算法本身昂贵。

比较了跨调用缓存和本次调用内复用。选择后者：输入校验直接返回调用者拥有的
`FactorObservations`，引擎使用这个已校验结果；输入的正价格检查和边界计数使用同一份数据。
结果验收在自己的调用内使用一次观测数据。没有缓存失效、共享可变状态、新线程或通用准备框架。
现有数据完整性、唯一身份、缺口、窗口、留出、流动性和时间轴约束继续保留。
未改变动量/波动率公式、排序、浮点运算顺序、200,000 条上限或任务执行语义。

| 输入 | 输入校验 ms 前→后 | 引擎 ms 前→后 | 结果校验 ms 前→后 | 引擎峰值 MiB 前→后 |
| --- | ---: | ---: | ---: | ---: |
| 20 × 1,000 日线，动量 | 35.31 → 19.73 | 40.23 → 24.50 | 25.97 → 9.09 | 16.24 → 16.25 |
| 100 × 2,000 日线，动量 | 307.19 → 191.33 | 406.97 → 210.03 | 261.45 → 92.26 | 59.04 → 59.07 |
| 1 × 200,000 分钟线，动量 | 127.78 → 124.03 | 162.33 → 131.02 | 46.04 → 29.43 | 147.05 → 151.27 |
| 100 × 2,000 日线，252 窗口波动率 | 301.95 → 195.87 | 456.30 → 256.52 | 261.82 → 94.82 | 59.10 → 59.39 |

四组样本优化前后的结果 Protobuf SHA-256 全部一致。20 万条日线动量引擎降低约 48%，
结果校验降低约 65%。日线内存基本不变；分钟线峰值多约 4.2 MiB，因为验证后的矩阵
在数据摘要核验期间仍由本次调用持有。接受此明确、有限的生命周期成本，不宣称减少了全部内存。

## 当前结论与未覆盖范围

现有输入上限内，先减少重复准备比增加线程或改写计算公式更直接。未出现足以支持放大上限的
传输、调度、完整读写链证据，因此本次不提高上限，不引入分块框架。

进一步容量工作应量测真实固定日线表的 SQLite 读取/Task 准备/worker 重读、稀疏时间轴、
流动性过滤和多候选/滚动窗口。此次对齐的日线样本不能代表稀疏矩阵或多并发任务的峰值，
热缓存读取不能推出存储设备或网络吞吐，也不能据此宣称 F11 的大样本分块全部完成。
完整回归结果见本次结算保证金交付记录；没有真实数据请求、旧数据改写或部署。
