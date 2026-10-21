# 需求到架构与工程责任映射

日期：2026-10-21。对应[目标架构](architecture-design-review.md)。

本表从当前需求正文的304条FR标题提取身份，并逐子模块分配目标责任。正文是业务规则唯一来源；
这里只记录设计落点，不改写FR、不声明已实现或已通过验收。所有目标新增模块尚须按实施计划交付。
58项UI及42条JRN的完整关联继续使用[需求追踪](../requirements/traceability.md)，不复制第二份产品规格。

执行方式表示该责任方可使用的路径，保存草稿、查询、人工确认等短操作不因同组有长计算任务而变成长任务。
同一行出现多个进程时，业务状态按目标设计的对象责任分开；不是双写同一事实。

| 模块 | FR数量 |
| --- | ---: |
| 数据 | 36 |
| 情报 | 26 |
| 分析 | 37 |
| 策略 | 32 |
| 回测 | 25 |
| 评估 | 24 |
| 实盘 | 64 |
| 系统管理 | 40 |
| 通用规范 | 20 |
| 合计 | 304 |

## 数据

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-DAT-ACQ-01-01 登记数据来源及其授权范围](../requirements/modules/data/acquisition.md#fr-dat-acq-01-01) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-01-02 核对连接与数据更新健康度](../requirements/modules/data/acquisition.md#fr-dat-acq-01-02) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-01-03 按合约识别数据是否可用](../requirements/modules/data/acquisition.md#fr-dat-acq-01-03) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-01-04 显式更换或断开数据来源](../requirements/modules/data/acquisition.md#fr-dat-acq-01-04) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-01-05 接入情报材料并登记来源及获取状态](../requirements/modules/data/acquisition.md#fr-dat-acq-01-05) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-01-06 核对来源能力清单与任务可执行范围](../requirements/modules/data/acquisition.md#fr-dat-acq-01-06) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-02-01 定义历史取数范围并查看已有覆盖](../requirements/modules/data/acquisition.md#fr-dat-acq-02-01) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-02-02 审阅并确认取数消耗](../requirements/modules/data/acquisition.md#fr-dat-acq-02-02) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-02-03 启动取数并控制后续请求](../requirements/modules/data/acquisition.md#fr-dat-acq-02-03) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-02-04 核对部分结果并显式继续未完成范围](../requirements/modules/data/acquisition.md#fr-dat-acq-02-04) | Data/acquisition；实时部分Market/acquisition | 来源/计划命令；批量采集用Data执行池 |
| [FR-DAT-ACQ-03-01 创建实时采集计划并核对范围](../requirements/modules/data/acquisition.md#fr-dat-acq-03-01) | Market/acquisition、distribution；Data保存归档 | 持续采集/订阅；消费与留存状态分开 |
| [FR-DAT-ACQ-03-02 监控采集时效与断流区间](../requirements/modules/data/acquisition.md#fr-dat-acq-03-02) | Market/acquisition、distribution；Data保存归档 | 持续采集/订阅；消费与留存状态分开 |
| [FR-DAT-ACQ-03-03 调整采集与留存并处理影响](../requirements/modules/data/acquisition.md#fr-dat-acq-03-03) | Market/acquisition、distribution；Data保存归档 | 持续采集/订阅；消费与留存状态分开 |
| [FR-DAT-PRC-01-01 定义加工规则并预览变更](../requirements/modules/data/processing.md#fr-dat-prc-01-01) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-PRC-01-02 按来源语义检查质量并定位问题](../requirements/modules/data/processing.md#fr-dat-prc-01-02) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-PRC-01-03 对齐时间并按交易时段聚合](../requirements/modules/data/processing.md#fr-dat-prc-01-03) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-PRC-01-04 按明确规则生产连续合约序列](../requirements/modules/data/processing.md#fr-dat-prc-01-04) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-PRC-01-05 核对加工结果并交接发布](../requirements/modules/data/processing.md#fr-dat-prc-01-05) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-PRC-01-06 解析材料基础结构并报告可读范围](../requirements/modules/data/processing.md#fr-dat-prc-01-06) | Data/processing | 加工/质量/解析用Data执行池 |
| [FR-DAT-SRV-01-01 检索并确认唯一合约身份](../requirements/modules/data/management.md#fr-dat-srv-01-01) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-01-02 查询指定日期的生效规则](../requirements/modules/data/management.md#fr-dat-srv-01-02) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-01-03 计算价格步长与手数的金额含义](../requirements/modules/data/management.md#fr-dat-srv-01-03) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-01-04 将合约及规则上下文用于后续任务](../requirements/modules/data/management.md#fr-dat-srv-01-04) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-01-05 查看交易日历与跨午夜时段](../requirements/modules/data/management.md#fr-dat-srv-01-05) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-01-06 核对来源代码映射和资料修订](../requirements/modules/data/management.md#fr-dat-srv-01-06) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-01 浏览数据目录和覆盖证据](../requirements/modules/data/management.md#fr-dat-srv-02-01) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-02 请求质量核对并定位问题证据](../requirements/modules/data/management.md#fr-dat-srv-02-02) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-03 比较数据修订并识别受影响实验](../requirements/modules/data/management.md#fr-dat-srv-02-03) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-04 固定研究所用快照并核对引用](../requirements/modules/data/management.md#fr-dat-srv-02-04) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-05 定义数据集并发布固定快照](../requirements/modules/data/management.md#fr-dat-srv-02-05) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-02-06 管理存储占用与依赖删除](../requirements/modules/data/management.md#fr-dat-srv-02-06) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-03-01 按固定身份查询历史数据](../requirements/modules/data/management.md#fr-dat-srv-03-01) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-03-02 订阅实时数据并查看分发状态](../requirements/modules/data/management.md#fr-dat-srv-03-02) | Market/acquisition、distribution；Data保存归档 | 持续采集/订阅；消费与留存状态分开 |
| [FR-DAT-SRV-03-03 说明数据可用范围和时间证据](../requirements/modules/data/management.md#fr-dat-srv-03-03) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-03-04 统一保存与交付材料及已发布情报](../requirements/modules/data/management.md#fr-dat-srv-03-04) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |
| [FR-DAT-SRV-03-05 检查跨模块输入契约并记录交付结果](../requirements/modules/data/management.md#fr-dat-srv-03-05) | Data/management；实时分发Market/distribution | 目录/引用/查询；重检查用Data执行池 |

## 情报

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-INT-EXT-01-01 选择材料并检查解析范围](../requirements/modules/intelligence/extraction.md#fr-int-ext-01-01) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-01-02 提取实体、事件和数量时间](../requirements/modules/intelligence/extraction.md#fr-int-ext-01-02) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-01-03 逐字段回看证据和陈述类型](../requirements/modules/intelligence/extraction.md#fr-int-ext-01-03) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-01-04 交接完整、部分与无提取结果](../requirements/modules/intelligence/extraction.md#fr-int-ext-01-04) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-01-05 定义提取结构与缺失字段语义](../requirements/modules/intelligence/extraction.md#fr-int-ext-01-05) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-02-01 确认模型处理方案和材料发送范围](../requirements/modules/intelligence/extraction.md#fr-int-ext-02-01) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-02-02 保留处理证据并比较重处理结果](../requirements/modules/intelligence/extraction.md#fr-int-ext-02-02) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-02-03 控制消耗并处理部分完成和重试](../requirements/modules/intelligence/extraction.md#fr-int-ext-02-03) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-EXT-02-04 用独立标注集评估提取方法](../requirements/modules/intelligence/extraction.md#fr-int-ext-02-04) | Intelligence/extraction | 方案与记录；提取/评测用Intelligence异步I/O/执行池 |
| [FR-INT-SRV-01-01 建立事件及其演变关系](../requirements/modules/intelligence/services.md#fr-int-srv-01-01) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-01-02 按用途与截止时点查询](../requirements/modules/intelligence/services.md#fr-int-srv-01-02) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-01-03 固定情报集合并交接研究](../requirements/modules/intelligence/services.md#fr-int-srv-01-03) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-01-04 约束历史重处理的结论](../requirements/modules/intelligence/services.md#fr-int-srv-01-04) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-02-01 构建查询并查看带证据的回答](../requirements/modules/intelligence/services.md#fr-int-srv-02-01) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-02-02 创建订阅和条件提醒](../requirements/modules/intelligence/services.md#fr-int-srv-02-02) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-02-03 识别交付缺口并核对补取](../requirements/modules/intelligence/services.md#fr-int-srv-02-03) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-SRV-02-04 向研究和策略交付发布内容](../requirements/modules/intelligence/services.md#fr-int-srv-02-04) | Intelligence/events＋Data发布内容存取 | 事件/查询/订阅；模型辅助不自动执行查询 |
| [FR-INT-VER-01-01 核对实体匹配与品种关联](../requirements/modules/intelligence/verification.md#fr-int-ver-01-01) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-01-02 核对数量单位及统计口径](../requirements/modules/intelligence/verification.md#fr-int-ver-01-02) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-01-03 表达事件时间与信息时间](../requirements/modules/intelligence/verification.md#fr-int-ver-01-03) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-01-04 保留语气、类别和标准化变更](../requirements/modules/intelligence/verification.md#fr-int-ver-01-04) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-02-01 检查证据支持和发布条件](../requirements/modules/intelligence/verification.md#fr-int-ver-02-01) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-02-02 识别重复报道和矛盾陈述](../requirements/modules/intelligence/verification.md#fr-int-ver-02-02) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-02-03 人工复核并发布或驳回](../requirements/modules/intelligence/verification.md#fr-int-ver-02-03) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-02-04 更正或撤回已发布情报](../requirements/modules/intelligence/verification.md#fr-int-ver-02-04) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |
| [FR-INT-VER-02-05 按明确质量门槛决定人工或自动发布](../requirements/modules/intelligence/verification.md#fr-int-ver-02-05) | Intelligence/verification | 服务裁决核验/发布；耗时计算用Intelligence异步I/O/执行池 |

## 分析

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-ANA-EDA-01-01 建立项目并记录可检验研究假设](../requirements/modules/analysis/exploration.md#fr-ana-eda-01-01) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-01-02 事前声明评价标准与验证安排](../requirements/modules/analysis/exploration.md#fr-ana-eda-01-02) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-01-03 将实验归入问题并记录尝试目的](../requirements/modules/analysis/exploration.md#fr-ana-eda-01-03) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-01-04 记录阶段结论和后续研究方向](../requirements/modules/analysis/exploration.md#fr-ana-eda-01-04) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-01-05 保存研究工作状态并派生可复核试验](../requirements/modules/analysis/exploration.md#fr-ana-eda-01-05) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-02-01 定义历史时点的样本资格](../requirements/modules/analysis/exploration.md#fr-ana-eda-02-01) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-02-02 提交前核对样本与规则所需事实](../requirements/modules/analysis/exploration.md#fr-ana-eda-02-02) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-02-03 固定多因子样本池与连续合约依据](../requirements/modules/analysis/exploration.md#fr-ana-eda-02-03) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-01 查看分布与有效样本覆盖](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-01) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-02 分析时间结构与市场状态](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-02) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-03 检查相关、滞后关系与共同样本](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-03) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-04 开展事件与条件统计](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-04) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-05 定位异常并交接数据核查](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-05) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EDA-03-06 保存探索报告与研究假设](../requirements/modules/analysis/exploration.md#fr-ana-eda-03-06) | Research/analysis/exploration | 研究记录；统计用Research执行池 |
| [FR-ANA-EVL-01-01 检查IC分层及因子相关冗余](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-01) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-02 对冻结合成信号进行样本外验证](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-02) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-03 运行因子评价并核对有效样本](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-03) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-04 评价规则条件相对基准的增量](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-04) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-05 检查分段稳定性和参数敏感性](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-05) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-06 检查冗余及加入后的增量价值](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-06) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-01-07 记录多重尝试与统计推断条件](../requirements/modules/analysis/evaluation.md#fr-ana-evl-01-07) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-01 解释信号结果并转化为策略研究草稿](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-01) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-02 交接冻结信号并保留研究来源](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-02) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-03 形成有理由的候选与剔除名单](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-03) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-04 冻结候选和独立验证使用记录](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-04) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-05 交接规则特征集与多因子信号](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-05) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-EVL-02-06 将研究候选交给策略而不直接作为交易方案](../requirements/modules/analysis/evaluation.md#fr-ana-evl-02-06) | Research/analysis/selection | 候选与证据；变量评价用Research执行池 |
| [FR-ANA-FTR-01-01 登记并辨认自定义因子定义](../requirements/modules/analysis/features.md#fr-ana-ftr-01-01) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-01-02 声明缺失异常与因子处理条件](../requirements/modules/analysis/features.md#fr-ana-ftr-01-02) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-01-03 训练确定并冻结合成权重](../requirements/modules/analysis/features.md#fr-ana-ftr-01-03) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-01-04 计算技术、统计与经济含义变量](../requirements/modules/analysis/features.md#fr-ana-ftr-01-04) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-01-05 控制多周期依赖与特征可得时间](../requirements/modules/analysis/features.md#fr-ana-ftr-01-05) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-01-06 冻结计算定义并复用结果](../requirements/modules/analysis/features.md#fr-ana-ftr-01-06) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-02-01 定义预测目标与训练预处理](../requirements/modules/analysis/features.md#fr-ana-ftr-02-01) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-02-02 预览目标标签的起止时间与口径](../requirements/modules/analysis/features.md#fr-ana-ftr-02-02) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-02-03 隔离重叠标签与验证边界](../requirements/modules/analysis/features.md#fr-ana-ftr-02-03) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |
| [FR-ANA-FTR-02-04 按训练窗口拟合处理并应用于验证](../requirements/modules/analysis/features.md#fr-ana-ftr-02-04) | Research/analysis/features | 固定定义；计算/拟合用Research执行池 |

## 策略

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-STR-DLV-01-01 建立有来源的交易候选说明](../requirements/modules/strategy/delivery.md#fr-str-dlv-01-01) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-01-02 核对目标环境与研究假设差异](../requirements/modules/strategy/delivery.md#fr-str-dlv-01-02) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-01-03 分别完成风险与实盘授权准备](../requirements/modules/strategy/delivery.md#fr-str-dlv-01-03) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-01-04 交接候选并保留启动前复核](../requirements/modules/strategy/delivery.md#fr-str-dlv-01-04) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-01 组装策略包并检查完整依赖](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-01) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-02 管理参数方案与固定内容](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-02) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-03 关联验证证据并分别展示准备状态](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-03) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-04 交付回测或交易准备草稿](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-04) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-05 管理弃用候选与引用保护](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-05) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-DLV-02-06 在交付前逐项核对消费环境能力](../requirements/modules/strategy/delivery.md#fr-str-dlv-02-06) | Research/strategy/delivery | 固定包/交接；授权由Trading另行确认 |
| [FR-STR-LOG-01-01 提供逻辑并声明输入输出约束](../requirements/modules/strategy/logic.md#fr-str-log-01-01) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-01-02 固定样例与预期并逐步试算](../requirements/modules/strategy/logic.md#fr-str-log-01-02) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-01-03 定位逻辑错误与不符合约束的输出](../requirements/modules/strategy/logic.md#fr-str-log-01-03) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-01-04 固定内容身份并交接可追溯实验引用](../requirements/modules/strategy/logic.md#fr-str-log-01-04) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-01-05 按支持能力编写规则并定位输入错误](../requirements/modules/strategy/logic.md#fr-str-log-01-05) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-02-01 建立模型方案和时间划分](../requirements/modules/strategy/logic.md#fr-str-log-02-01) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-02-02 预览训练空间与资源并运行](../requirements/modules/strategy/logic.md#fr-str-log-02-02) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-02-03 按训练计划选择并固定模型产物](../requirements/modules/strategy/logic.md#fr-str-log-02-03) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-02-04 定义预测契约和决策映射](../requirements/modules/strategy/logic.md#fr-str-log-02-04) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-02-05 产生模型更新候选并交接验证](../requirements/modules/strategy/logic.md#fr-str-log-02-05) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-03-01 定义规则、模型与混合决策结构](../requirements/modules/strategy/logic.md#fr-str-log-03-01) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-03-02 声明决策事件、状态和退出规则](../requirements/modules/strategy/logic.md#fr-str-log-03-02) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-03-03 核对混合策略的分支与异常](../requirements/modules/strategy/logic.md#fr-str-log-03-03) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-LOG-03-04 交付统一决策定义给组合与策略包](../requirements/modules/strategy/logic.md#fr-str-log-03-04) | Research/strategy/logic | 逻辑与训练定义；试算/训练用Research执行池 |
| [FR-STR-PRT-01-01 定义组合权重、基准与预算](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-01) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-02 将目标名义敞口转换为整数手数](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-02) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-03 处理共享账户策略目标与持仓归属](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-03) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-04 生成调仓差额并追踪部分成交](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-04) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-05 核对聚合风险与保证金压力](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-05) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-06 核对组合贡献与目标执行偏差](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-06) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-07 定义仓位政策及其参数适用范围](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-07) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |
| [FR-STR-PRT-01-08 声明调仓触发与目标失效条件](../requirements/modules/strategy/portfolio.md#fr-str-prt-01-08) | Research/strategy/portfolio；运行应用在Trading | 仓位政策；离线计算用Research执行池，实时意图由Trading执行 |

## 回测

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-BKT-EXP-01-01 选择固定策略包并核对参数与依赖](../requirements/modules/backtest/experiments.md#fr-bkt-exp-01-01) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-01-02 配置必要风控并确认运行资格](../requirements/modules/backtest/experiments.md#fr-bkt-exp-01-02) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-01-03 安排训练留出与滚动验证](../requirements/modules/backtest/experiments.md#fr-bkt-exp-01-03) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-01-04 核对数据覆盖并固定完整实验方案](../requirements/modules/backtest/experiments.md#fr-bkt-exp-01-04) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-01-05 事前固定模拟能力与不可识别的成交假设](../requirements/modules/backtest/experiments.md#fr-bkt-exp-01-05) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-02-01 审阅固定条件并提交独立回测](../requirements/modules/backtest/experiments.md#fr-bkt-exp-02-01) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-02-02 观察执行并显式取消回测](../requirements/modules/backtest/experiments.md#fr-bkt-exp-02-02) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-02-03 保存完整结果及可复核证据](../requirements/modules/backtest/experiments.md#fr-bkt-exp-02-03) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-02-04 按原条件重跑或派生新实验](../requirements/modules/backtest/experiments.md#fr-bkt-exp-02-04) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-02-05 复核固定实验并辨认不可重复部分](../requirements/modules/backtest/experiments.md#fr-bkt-exp-02-05) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-03-01 展开参数空间并预览候选数量和预算](../requirements/modules/backtest/experiments.md#fr-bkt-exp-03-01) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-EXP-03-02 提交批次并处理独立失败与取消](../requirements/modules/backtest/experiments.md#fr-bkt-exp-03-02) | Research/backtest/experiments | 实验与批次；单次模拟用Research执行池 |
| [FR-BKT-SIM-01-01 查看历史事件与信息可得顺序](../requirements/modules/backtest/simulation.md#fr-bkt-sim-01-01) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-01-02 配置决策与模拟成交规则](../requirements/modules/backtest/simulation.md#fr-bkt-sim-01-02) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-01-03 区分连续信号价与实际交易价](../requirements/modules/backtest/simulation.md#fr-bkt-sim-01-03) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-01-04 按历史截止执行滚动拟合与模型使用](../requirements/modules/backtest/simulation.md#fr-bkt-sim-01-04) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-02-01 逐事件追踪订单、部分成交和撤单](../requirements/modules/backtest/simulation.md#fr-bkt-sim-02-01) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-02-02 应用有限流动性与交易限制](../requirements/modules/backtest/simulation.md#fr-bkt-sim-02-02) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-02-03 预览换月日程和无法成交处置](../requirements/modules/backtest/simulation.md#fr-bkt-sim-02-03) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-03-01 配置并核对费用适用口径](../requirements/modules/backtest/simulation.md#fr-bkt-sim-03-01) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-03-02 区分价格影响和现金费用并防止重复扣减](../requirements/modules/backtest/simulation.md#fr-bkt-sim-03-02) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-04-01 维护模拟持仓与可平批次](../requirements/modules/backtest/simulation.md#fr-bkt-sim-04-01) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-04-02 核对资金、保证金和逐日结算](../requirements/modules/backtest/simulation.md#fr-bkt-sim-04-02) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-04-03 记录资金不足和风险处置结果](../requirements/modules/backtest/simulation.md#fr-bkt-sim-04-03) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |
| [FR-BKT-SIM-04-04 处理到期与回测结束时未平仓](../requirements/modules/backtest/simulation.md#fr-bkt-sim-04-04) | Research/backtest/simulation | Research执行池产生模拟事实；服务保存结果 |

## 评估

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-EVA-PER-01-01 查看权益与风险随时间变化](../requirements/modules/evaluation/performance.md#fr-eva-per-01-01) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-PER-01-02 追溯每笔成交及实际成本](../requirements/modules/evaluation/performance.md#fr-eva-per-01-02) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-PER-01-03 按合约或时间解释收益贡献](../requirements/modules/evaluation/performance.md#fr-eva-per-01-03) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-PER-01-04 核对指标口径并记录解释限制](../requirements/modules/evaluation/performance.md#fr-eva-per-01-04) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-PER-01-05 区分现金流、投资损益和收益率](../requirements/modules/evaluation/performance.md#fr-eva-per-01-05) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-PER-01-06 核对基准与同口径绩效对照](../requirements/modules/evaluation/performance.md#fr-eva-per-01-06) | Research/evaluation/performance | Research执行池计算固定切片；服务只读来源账本 |
| [FR-EVA-RSK-01-01 度量回撤、亏损持续和恢复](../requirements/modules/evaluation/risk.md#fr-eva-rsk-01-01) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-RSK-01-02 分解敞口、集中度与风险贡献](../requirements/modules/evaluation/risk.md#fr-eva-rsk-01-02) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-RSK-01-03 核对资金占用和尾部风险依据](../requirements/modules/evaluation/risk.md#fr-eva-rsk-01-03) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-RSK-02-01 固定情景并区分静态与动态方法](../requirements/modules/evaluation/risk.md#fr-eva-rsk-02-01) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-RSK-02-02 评估延迟与流动性情景的路径影响](../requirements/modules/evaluation/risk.md#fr-eva-rsk-02-02) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-RSK-02-03 解释压力损失、资金缺口与退出限制](../requirements/modules/evaluation/risk.md#fr-eva-rsk-02-03) | Research/evaluation/risk | 静态评价用Research执行池；动态重放交回测 |
| [FR-EVA-VAL-01-01 选择实验并核对比较口径](../requirements/modules/evaluation/validation.md#fr-eva-val-01-01) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-01-02 记录训练选择及验证信息的使用](../requirements/modules/evaluation/validation.md#fr-eva-val-01-02) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-01-03 保存比较结论并交接固定证据](../requirements/modules/evaluation/validation.md#fr-eva-val-01-03) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-01-04 比较参数邻域与成本扰动的稳健性](../requirements/modules/evaluation/validation.md#fr-eva-val-01-04) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-01-05 保留重复试验和选择记录再进入独立验证](../requirements/modules/evaluation/validation.md#fr-eva-val-01-05) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-02-01 固定定义身份、可见信息与运行状态起点](../requirements/modules/evaluation/validation.md#fr-eva-val-02-01) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-02-02 核对跨环境信号、组合分配及逐笔风控结果](../requirements/modules/evaluation/validation.md#fr-eva-val-02-02) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-02-03 按数据逻辑执行和成本归因偏差](../requirements/modules/evaluation/validation.md#fr-eva-val-02-03) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-03-01 固定评价上下文与证据清单](../requirements/modules/evaluation/validation.md#fr-eva-val-03-01) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-03-02 保存综合结论及候选交接](../requirements/modules/evaluation/validation.md#fr-eva-val-03-02) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-03-03 预览并导出统一评价报告](../requirements/modules/evaluation/validation.md#fr-eva-val-03-03) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |
| [FR-EVA-VAL-03-04 明确评价方法口径和报告可交付状态](../requirements/modules/evaluation/validation.md#fr-eva-val-03-04) | Research/evaluation/validation | 固定证据/报告；重算用Research执行池 |

## 实盘

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-LIV-ACC-01-01 查看资金组成与数值口径](../requirements/modules/live/accounts.md#fr-liv-acc-01-01) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-01-02 查看持仓方向数量与可平范围](../requirements/modules/live/accounts.md#fr-liv-acc-01-02) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-01-03 解释持仓估值与盈亏来源](../requirements/modules/live/accounts.md#fr-liv-acc-01-03) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-01-04 刷新账户并辨认不同时间的信息](../requirements/modules/live/accounts.md#fr-liv-acc-01-04) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-02-01 选择账户和期间取得核对资料](../requirements/modules/live/accounts.md#fr-liv-acc-02-01) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-02-02 逐笔匹配成交和汇总金额](../requirements/modules/live/accounts.md#fr-liv-acc-02-02) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-02-03 分析差异并记录解释或更正](../requirements/modules/live/accounts.md#fr-liv-acc-02-03) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-02-04 形成可审查的核对结果](../requirements/modules/live/accounts.md#fr-liv-acc-02-04) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-02-05 核对交易日切换与跨日账户状态](../requirements/modules/live/accounts.md#fr-liv-acc-02-05) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-03-01 为目标账户定义风险约束草稿](../requirements/modules/live/accounts.md#fr-liv-acc-03-01) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-03-02 检查规则完整性和执行所需信息](../requirements/modules/live/accounts.md#fr-liv-acc-03-02) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-03-03 确认规则生效和变更影响](../requirements/modules/live/accounts.md#fr-liv-acc-03-03) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-ACC-03-04 显示账户风险资格与不可用原因](../requirements/modules/live/accounts.md#fr-liv-acc-03-04) | Trading/accounts | 账户/政策/核对；只读重分析交Research |
| [FR-LIV-EXE-01-01 填写完整手动委托意图](../requirements/modules/live/execution.md#fr-liv-exe-01-01) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-01-02 检查价格数量与有效条件](../requirements/modules/live/execution.md#fr-liv-exe-01-02) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-01-03 核对委托摘要并明确确认](../requirements/modules/live/execution.md#fr-liv-exe-01-03) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-01-04 防止重复触发并显示提交结果](../requirements/modules/live/execution.md#fr-liv-exe-01-04) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-02-01 筛选委托并辨认当前状态](../requirements/modules/live/execution.md#fr-liv-exe-02-01) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-02-02 查看部分成交与订单事件过程](../requirements/modules/live/execution.md#fr-liv-exe-02-02) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-02-03 确认并撤销订单未成交部分](../requirements/modules/live/execution.md#fr-liv-exe-02-03) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-02-04 查询成交明细并关联账户影响](../requirements/modules/live/execution.md#fr-liv-exe-02-04) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-03-01 保留断线时的最后明确交易事实](../requirements/modules/live/execution.md#fr-liv-exe-03-01) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-03-02 恢复连接后只查询并关联结果](../requirements/modules/live/execution.md#fr-liv-exe-03-02) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-03-03 保留未解决事项并进行人工核实](../requirements/modules/live/execution.md#fr-liv-exe-03-03) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-03-04 把核对结果交接给订单和结算](../requirements/modules/live/execution.md#fr-liv-exe-03-04) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-03-05 在关键记录写入失败及进程中断后保持交易事实](../requirements/modules/live/execution.md#fr-liv-exe-03-05) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-04-01 执行统一价格数量与开平校验](../requirements/modules/live/execution.md#fr-liv-exe-04-01) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-04-02 计算资金和在途敞口占用](../requirements/modules/live/execution.md#fr-liv-exe-04-02) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-04-03 防止并发请求重复使用额度](../requirements/modules/live/execution.md#fr-liv-exe-04-03) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-EXE-04-04 依据风险拒绝修改独立交易意图](../requirements/modules/live/execution.md#fr-liv-exe-04-04) | Trading/execution | 统一账户执行边界，不进入批计算队列 |
| [FR-LIV-RUN-01-01 选择账户并辨认交易环境](../requirements/modules/live/operations.md#fr-liv-run-01-01) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-01-02 分别建立行情与交易授权](../requirements/modules/live/operations.md#fr-liv-run-01-02) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-01-03 审查交易授权范围与有效性](../requirements/modules/live/operations.md#fr-liv-run-01-03) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-01-04 撤回授权并核对连接变化](../requirements/modules/live/operations.md#fr-liv-run-01-04) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-01-05 完成开市前账户与运行准备检查](../requirements/modules/live/operations.md#fr-liv-run-01-05) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-02-01 浏览分类报价并排序筛选](../requirements/modules/live/operations.md#fr-liv-run-02-01) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-02 维护自选集合与关注顺序](../requirements/modules/live/operations.md#fr-liv-run-02-02) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-03 查看合约身份和交易属性](../requirements/modules/live/operations.md#fr-liv-run-02-03) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-04 观察分时走势与交易时段](../requirements/modules/live/operations.md#fr-liv-run-02-04) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-05 查看完整K线与未结束区间](../requirements/modules/live/operations.md#fr-liv-run-02-05) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-06 切换图表周期并保持合约上下文](../requirements/modules/live/operations.md#fr-liv-run-02-06) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-07 使用十字线缩放和平移检查价格细节](../requirements/modules/live/operations.md#fr-liv-run-02-07) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-08 同时查看盘口与最近成交](../requirements/modules/live/operations.md#fr-liv-run-02-08) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-09 管理行情订阅与连接范围](../requirements/modules/live/operations.md#fr-liv-run-02-09) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-02-10 识别延迟断流与数据恢复](../requirements/modules/live/operations.md#fr-liv-run-02-10) | Terminal/live行情工作区；Market事实；Data合约/历史 | UI直连业务服务；自选为用户偏好，不进入批计算队列 |
| [FR-LIV-RUN-03-01 从研究候选交接可验证策略方案](../requirements/modules/live/operations.md#fr-liv-run-03-01) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-03-02 配置并检查独立模拟验证](../requirements/modules/live/operations.md#fr-liv-run-03-02) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-03-03 限定账户与范围授予实盘权限](../requirements/modules/live/operations.md#fr-liv-run-03-03) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-03-04 识别方案变更并重新审查授权](../requirements/modules/live/operations.md#fr-liv-run-03-04) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-03-05 建立隔离的本地实时模拟账户](../requirements/modules/live/operations.md#fr-liv-run-03-05) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-03-06 保存模拟运行并明确继续或重新开始](../requirements/modules/live/operations.md#fr-liv-run-03-06) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-04-01 明确启动策略并防止重复运行](../requirements/modules/live/operations.md#fr-liv-run-04-01) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-04-02 监控策略数据依据与执行结果](../requirements/modules/live/operations.md#fr-liv-run-04-02) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-04-03 停止新意图并确认真正停止](../requirements/modules/live/operations.md#fr-liv-run-04-03) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-04-04 处理停止后的残余订单和持仓](../requirements/modules/live/operations.md#fr-liv-run-04-04) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-04-05 明确同一真实账户的执行归属](../requirements/modules/live/operations.md#fr-liv-run-04-05) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-05-01 关联信号目标、订单成交与策略归属](../requirements/modules/live/operations.md#fr-liv-run-05-01) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-05-02 限定受影响量化运行并确认暂停新增](../requirements/modules/live/operations.md#fr-liv-run-05-02) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-05-03 保留调查依据并重新验证修订范围](../requirements/modules/live/operations.md#fr-liv-run-05-03) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-05-04 按明确条件继续次日运行](../requirements/modules/live/operations.md#fr-liv-run-05-04) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-06-01 观察风险指标和阈值距离](../requirements/modules/live/operations.md#fr-liv-run-06-01) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-06-02 识别告警并检查触发依据](../requirements/modules/live/operations.md#fr-liv-run-06-02) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-06-03 暂停新增风险并保留安全处置入口](../requirements/modules/live/operations.md#fr-liv-run-06-03) | Trading/operations | 常驻运行/授权；重评估交Research |
| [FR-LIV-RUN-06-04 核对恢复条件并明确恢复授权](../requirements/modules/live/operations.md#fr-liv-run-06-04) | Trading/operations | 常驻运行/授权；重评估交Research |

## 系统管理

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-SYS-REC-01-01 查看个人资料范围和引用关系](../requirements/modules/system/records.md#fr-sys-rec-01-01) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-01-02 预览备份范围并验证备份结果](../requirements/modules/system/records.md#fr-sys-rec-01-02) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-01-03 恢复到独立位置并核对缺项](../requirements/modules/system/records.md#fr-sys-rec-01-03) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-01-04 预览删除后果并明确确认](../requirements/modules/system/records.md#fr-sys-rec-01-04) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-01-05 恢复后核对资料并重新准备业务](../requirements/modules/system/records.md#fr-sys-rec-01-05) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-02-01 用可理解的错误解释影响和下一步](../requirements/modules/system/records.md#fr-sys-rec-02-01) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-02-02 按对象与时间检索关键操作记录](../requirements/modules/system/records.md#fr-sys-rec-02-02) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-02-03 把操作意图和确认结果排成可核对时间线](../requirements/modules/system/records.md#fr-sys-rec-02-03) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-02-04 导出问题所需的最小诊断资料](../requirements/modules/system/records.md#fr-sys-rec-02-04) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-03-01 选择资料范围与输出口径并预览](../requirements/modules/system/records.md#fr-sys-rec-03-01) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-03-02 选择账户脱敏与策略内容保密范围](../requirements/modules/system/records.md#fr-sys-rec-03-02) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-03-03 遵守来源导出限制并保留数据解释](../requirements/modules/system/records.md#fr-sys-rec-03-03) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-03-04 保存导出并确认完整文件与实际数量](../requirements/modules/system/records.md#fr-sys-rec-03-04) | Terminal/system＋Data维护＋各记录拥有者 | 业务导出直连拥有者；节点诊断经Agent |
| [FR-SYS-REC-04-01 按来源时间和对象检索诊断日志](../requirements/modules/system/records.md#fr-sys-rec-04-01) | Agent节点诊断＋Core日志＋Terminal/system | 诊断检索/清理；业务事实仍向拥有者查询 |
| [FR-SYS-REC-04-02 区分诊断日志、操作审计和业务事实](../requirements/modules/system/records.md#fr-sys-rec-04-02) | Agent节点诊断＋Core日志＋Terminal/system | 诊断检索/清理；业务事实仍向拥有者查询 |
| [FR-SYS-REC-04-03 配置诊断留存并核对清理范围](../requirements/modules/system/records.md#fr-sys-rec-04-03) | Agent节点诊断＋Core日志＋Terminal/system | 诊断检索/清理；业务事实仍向拥有者查询 |
| [FR-SYS-REC-04-04 保护日志秘密并说明诊断缺口](../requirements/modules/system/records.md#fr-sys-rec-04-04) | Agent节点诊断＋Core日志＋Terminal/system | 诊断检索/清理；业务事实仍向拥有者查询 |
| [FR-SYS-RUN-01-01 提交长任务并在任务中心定位](../requirements/modules/system/tasks-nodes.md#fr-sys-run-01-01) | Terminal/system＋业务服务；节点由Manager/Agent | 任务状态/定时机制在所属服务；页面只聚合 |
| [FR-SYS-RUN-01-02 查看真实阶段、资源与结果完整性](../requirements/modules/system/tasks-nodes.md#fr-sys-run-01-02) | Terminal/system＋业务服务；节点由Manager/Agent | 任务状态/定时机制在所属服务；页面只聚合 |
| [FR-SYS-RUN-01-03 取消研究并区分请求与实际停止](../requirements/modules/system/tasks-nodes.md#fr-sys-run-01-03) | Terminal/system＋业务服务；节点由Manager/Agent | 任务状态/定时机制在所属服务；页面只聚合 |
| [FR-SYS-RUN-01-04 核实失联任务并决定后续工作](../requirements/modules/system/tasks-nodes.md#fr-sys-run-01-04) | Terminal/system＋业务服务；节点由Manager/Agent | 任务状态/定时机制在所属服务；页面只聚合 |
| [FR-SYS-RUN-02-01 查看本地与远程资源能力和可用性](../requirements/modules/system/tasks-nodes.md#fr-sys-run-02-01) | Manager节点登记＋Agent进程＋各服务业务停止 | 管理状态与业务结果分别确认；不自动换节点 |
| [FR-SYS-RUN-02-02 为工作选择归属并保持一致](../requirements/modules/system/tasks-nodes.md#fr-sys-run-02-02) | Manager节点登记＋Agent进程＋各服务业务停止 | 管理状态与业务结果分别确认；不自动换节点 |
| [FR-SYS-RUN-02-03 关闭窗口后明确后台仍在运行](../requirements/modules/system/tasks-nodes.md#fr-sys-run-02-03) | Manager节点登记＋Agent进程＋各服务业务停止 | 管理状态与业务结果分别确认；不自动换节点 |
| [FR-SYS-RUN-02-04 明确停止指定位置的工作并核对影响](../requirements/modules/system/tasks-nodes.md#fr-sys-run-02-04) | Manager节点登记＋Agent进程＋各服务业务停止 | 管理状态与业务结果分别确认；不自动换节点 |
| [FR-SYS-RUN-02-05 登记可信节点并处理信任失效与移除](../requirements/modules/system/tasks-nodes.md#fr-sys-run-02-05) | Manager节点登记＋Agent进程＋各服务业务停止 | 管理状态与业务结果分别确认；不自动换节点 |
| [FR-SYS-RUN-03-01 从业务任务创建计划并预览时间](../requirements/modules/system/tasks-nodes.md#fr-sys-run-03-01) | 任务所属Data/Intelligence/Research＋Core定时机制 | 持久触发与业务接纳在同一服务；系统页聚合 |
| [FR-SYS-RUN-03-02 确认授权预算及错过重叠行为](../requirements/modules/system/tasks-nodes.md#fr-sys-run-03-02) | 任务所属Data/Intelligence/Research＋Core定时机制 | 持久触发与业务接纳在同一服务；系统页聚合 |
| [FR-SYS-RUN-03-03 关联每次触发与唯一业务受理结果](../requirements/modules/system/tasks-nodes.md#fr-sys-run-03-03) | 任务所属Data/Intelligence/Research＋Core定时机制 | 持久触发与业务接纳在同一服务；系统页聚合 |
| [FR-SYS-RUN-03-04 暂停修改计划并处理离线缺口](../requirements/modules/system/tasks-nodes.md#fr-sys-run-03-04) | 任务所属Data/Intelligence/Research＋Core定时机制 | 持久触发与业务接纳在同一服务；系统页聚合 |
| [FR-SYS-SET-01-01 核对设备支持范围与首次可用条件](../requirements/modules/system/setup.md#fr-sys-set-01-01) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-01-02 选择工作资料位置并保护已有内容](../requirements/modules/system/setup.md#fr-sys-set-01-02) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-01-03 按研究与交易分别完成准备清单](../requirements/modules/system/setup.md#fr-sys-set-01-03) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-01-04 完成准备后进入明确的工作入口](../requirements/modules/system/setup.md#fr-sys-set-01-04) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-01-05 核对安装产物和支持环境](../requirements/modules/system/setup.md#fr-sys-set-01-05) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-01-06 维护插件并明确卸载影响](../requirements/modules/system/setup.md#fr-sys-set-01-06) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-02-01 按对象定位配置并跳转责任模块](../requirements/modules/system/setup.md#fr-sys-set-02-01) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-02-02 解释保存与生效条件及影响](../requirements/modules/system/setup.md#fr-sys-set-02-02) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-02-03 处理不可达对象和并发配置变化](../requirements/modules/system/setup.md#fr-sys-set-02-03) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |
| [FR-SYS-SET-02-04 保护配置秘密并关联变更记录](../requirements/modules/system/setup.md#fr-sys-set-02-04) | Installer＋Terminal/system＋配置拥有者 | 安装维护和配置索引；管理经Manager/Agent |

## 通用规范

| 功能与需求正文 | 目标责任 | 执行边界 |
| --- | --- | --- |
| [FR-COM-REL-01-01 并行研究期间保持关键交互响应](../requirements/common/reliability.md#fr-com-rel-01-01) | 各服务＋Core机制＋Terminal | 独立负载/故障验收，无独立可靠性服务 |
| [FR-COM-REL-01-02 呈现行情时效而不掩盖上游延迟](../requirements/common/reliability.md#fr-com-rel-01-02) | 各服务＋Core机制＋Terminal | 独立负载/故障验收，无独立可靠性服务 |
| [FR-COM-REL-01-03 及时反馈取消请求并保留停止等待](../requirements/common/reliability.md#fr-com-rel-01-03) | 各服务＋Core机制＋Terminal | 独立负载/故障验收，无独立可靠性服务 |
| [FR-COM-REL-01-04 中断后保留已保存资料并核对原工作](../requirements/common/reliability.md#fr-com-rel-01-04) | 各服务＋Core机制＋Terminal | 独立负载/故障验收，无独立可靠性服务 |
| [FR-COM-SEC-01-01 按用途填写凭据并核对权限](../requirements/common/security.md#fr-com-sec-01-01) | Terminal安全输入＋各服务用途边界 | 平台钥匙串/会话凭据；秘密不入普通配置 |
| [FR-COM-SEC-01-02 明确选择将行情凭据保存到系统安全存储](../requirements/common/security.md#fr-com-sec-01-02) | Terminal安全输入＋各服务用途边界 | 平台钥匙串/会话凭据；秘密不入普通配置 |
| [FR-COM-SEC-01-03 移除保存项并区分本地与外部授权](../requirements/common/security.md#fr-com-sec-01-03) | Terminal安全输入＋各服务用途边界 | 平台钥匙串/会话凭据；秘密不入普通配置 |
| [FR-COM-SEC-01-04 在记录与分享资料中排除秘密](../requirements/common/security.md#fr-com-sec-01-04) | Terminal安全输入＋各服务用途边界 | 平台钥匙串/会话凭据；秘密不入普通配置 |
| [FR-COM-SEC-01-05 明确计划任务的凭据前提](../requirements/common/security.md#fr-com-sec-01-05) | Terminal安全输入＋各服务用途边界 | 平台钥匙串/会话凭据；秘密不入普通配置 |
| [FR-COM-UX-01-01 选择并即时保存中英文显示偏好](../requirements/common/interaction.md#fr-com-ux-01-01) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-02 区分显示时区与业务交易日](../requirements/common/interaction.md#fr-com-ux-01-02) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-03 明确数值单位、精度和缺失状态](../requirements/common/interaction.md#fr-com-ux-01-03) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-04 统一选择菜单的可读背景和展开位置](../requirements/common/interaction.md#fr-com-ux-01-04) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-05 通过键盘完成导航并保持可见焦点](../requirements/common/interaction.md#fr-com-ux-01-05) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-06 离开未保存草稿前作明确选择](../requirements/common/interaction.md#fr-com-ux-01-06) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-01-07 危险动作明确对象后再确认](../requirements/common/interaction.md#fr-com-ux-01-07) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-02-01 聚合通知并按业务身份定位](../requirements/common/interaction.md#fr-com-ux-02-01) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-02-02 区分阅读状态与业务告警状态](../requirements/common/interaction.md#fr-com-ux-02-02) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-02-03 设置提醒方式并解释渠道失败](../requirements/common/interaction.md#fr-com-ux-02-03) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
| [FR-COM-UX-02-04 跟踪事件更新并完成业务跳转](../requirements/common/interaction.md#fr-com-ux-02-04) | Terminal/shell、i18n、system及client-ui | 公共交互；业务告警终态仍归源服务 |
