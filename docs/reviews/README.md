# 设计与验收记录索引

现行契约以[架构](../architecture.md)和[主题文档](../README.md)为准，当前能力与限制见[现状](../status.md)。本目录保存有日期、源码和平台边界的证据；旧报告不是另一套可执行架构或当前版本验收声明。

## 本轮与总体记录

- [2026-10-21 需求驱动架构与工程设计](architecture-design-review.md)：八模块、23子模块、五类业务服务、三类worker、状态与配置归属、实施顺序；[304项FR责任映射](requirements-architecture-map.md)。目标设计，未切换生产代码。

- [2026-10-21 按业务服务归属管理任务](service-owned-tasks-design.md)：数据／研究职责、Core 复用与服务实例并发；主动启动流程已实现，任务归属仍待切换。

- [2026-10-20 开发前需求与交互设计修订](requirements-specification-2026-10-20.md)：目标功能、逐页字段与操作、跨模块规则、设计图和派生阅读版的一致性。

- [2026-10-20 开发节点插件选择修复](development-plugin-repair-2026-10-20.md)：处理入口改名后旧开发插件引用阻断同步，保留数据并验证真实本机启动与关闭。

- [2026-10-20 项目自有命名清理](naming-cleanup-2026-10-20.md)：协议目录与命名空间、插件类型及入口去除实现版本号，保留必要ABI检查和持久数据身份。

- [2026-10-19 非必要哈希清理](hash-cleanup-2026-10-19.md)：禁止无依据新增校验，删除分析启动扫描、插件重复哈希与导出装饰性摘要，记录必要保留边界。

- [2026-10-19 需求驱动实施](requirements-implementation-2026-10-19.md)：拉取9a476a2后的产品导航、插件视图归属、原始绩效入口及全项目后续缺口。

- [2026-10-18 需求交付版审查](requirements-delivery-2026-10-18.md)：有限支持范围、逐页字段与状态、可测质量基线、全量追踪、交付判据及外部准入边界。

- [2026-10-18 项目需求文档全面审查](requirements-review-2026-10-18.md)：规范性、完整性、职责与流程一致性、目标界面和验证追踪；原10项发现及后续文档层整改；当前状态见需求交付版审查。

- [2026-10-18 需求界面与实际软件对齐](requirements-ui-alignment.md)：实际截图、导航和操作对应，未实现界面差距及文档、台账同步验证。

- [2026-10-16 Polars 与 skfolio 接入](polars-skfolio-2026-10-16.md)：Decimal 收益准备、共同区间对齐、仅训练段拟合与验证对照，共用冻结分析助手。

- [2026-10-16 因子留出与滚动比较](factor-validation-comparison-2026-10-16.md)：复用固定结果，核对评价边界与样本，分段展示并导出。

- [2026-10-16 macOS 编译与运行修复](macos-build-run-2026-10-16.md)：Apple Clang、macOS 13 部署目标、冻结统计助手冷启动和桌面复验。

- [2026-10-16 命令、政策与架构检查](command-boundaries-2026-10-16.md)：类型化接纳、自动失败用例、快照测量与交付复验。

- [2026-10-16 实盘观察出口类型化](live-observation-2026-10-16.md)：直接生成既有协议快照，移除 JSON 中转。

- [2026-10-16 类型化接口与架构约束](typed-boundaries-2026-10-16.md)：领域 JSON 清理、命令参数类型与验收。
- [2026-10-16 架构收敛](architecture-convergence-2026-10-16.md)：获批方案、实施和验证。
- [2026-10-16 插件比较](plugin-mechanisms-comparison-2026-10-16.md)：NMR 对比与本轮设计来源。
- [审计进度](audit-progress.md)：按时间记录修复与验收。
- [实施记录](architecture-implementation.md)：按日期记录实际改造、验证与未完成事项。
- [2026-10-05 至 06 验收](current-acceptance.md)、[发布候选推进](release-candidate.md)：历史平台/安装证据，不代表当前发布包。

## 专项记录

- [固定回测成交、持仓与资金核对 — 2026-10-15](account-reconciliation-2026-10-15.md)
- [F06：回测按月账户收益贡献](backtest-monthly-contributions-2026-10-15.md)
- [Lightweight Charts 与 QuantStats 直接接入](charts-quantstats-integration-2026-10-15.md)
- [2026-10-15：统一代码排版](code-style-2026-10-15.md)
- [开发期版本机制清理（2026-10-15）](development-version-cleanup-2026-10-15.md)
- [默认仓库原始证据覆盖与质量盘点 — 2026-10-15](evidence-inventory-2026-10-15.md)
- [F13 历史浏览返回恢复 — 2026-10-15](f13-history-resume-2026-10-15.md)
- [F13：1分钟档案与临时观测分段浏览（2026-10-15）](f13-minute-segments-2026-10-15.md)
- [因子容量测量与重复准备消除（2026-10-15）](factor-capacity-2026-10-15.md)
- [因子截面成员证据与比较口径](factor-members-2026-10-15.md)
- [F05 单窗口日线动量参数派生（2026-10-15）](factor-window-derivation-2026-10-15.md)
- [2026-10-16 本机全量测试与界面录像](full-tests-recording-2026-10-16.md)
- [项目功能完整度与实现优先级](functional-completeness-2026-10-14.md)
- [F02 历史保证金来源核验与最小基础（2026-10-15）](historical-margin-evidence-2026-10-15.md)
- [F03 历史交易时段证据核验（2026-10-15）](historical-session-evidence-2026-10-15.md)
- [F03 整日补齐预览公告冲突门禁（2026-10-15）](history-repair-preview-gate-2026-10-15.md)
- [全市场数据表：设计](history-tables-design.md)
- [F06 日期精度持有期与研究链路收口复核（2026-10-15）](holding-period-2026-10-15.md)
- [简单组合敞口与显式期末预算复核 — 2026-10-15](portfolio-budget-2026-10-15.md)
- [项目架构 代码与功能评估](project-review-2026-10-13.md)
- [单分区原始响应体证据闭环 — 2026-10-15](raw-response-2026-10-15.md)
- [本地量化项目功能与架构借鉴评审](reference-projects-2026-10-15.md)
- [显式单分区重取的独立观测 — 2026-10-15](refresh-observation-2026-10-15.md)
- [研究链联合验收 — 2026-10-15](research-integration-2026-10-15.md)
- [已留存响应与分区质量核对 — 2026-10-15](response-quality-2026-10-15.md)
- [逐结算日占用观测与只读预算轨迹 — 2026-10-15](settlement-budget-2026-10-15.md)
- [结算保证金调整：固定输入到结果解释（2026-10-15）](settlement-margin-implementation-2026-10-15.md)
- [F02：结算阶段保证金能力评估与账户验证（2026-10-15）](settlement-margin-phase-boundary-2026-10-15.md)
- [F02：SHFE au2512 四事件官方证据闭环（2026-10-15）](shfe-au2512-margin-evidence-2026-10-15.md)
- [SHFE historical margin integration](shfe-margin-integration-2026-10-15.md)
- [F10 固定数据滑点敏感性首批（2026-10-15）](slippage-sensitivity-2026-10-15.md)
- [F07 已入库日分区显式重新获取（2026-10-15）](table-partition-refresh-2026-10-15.md)
- [F07 单分区重取结果解释与恢复（2026-10-15）](table-refresh-explanation-2026-10-15.md)
- [任务服务架构与实现评审（2026-10-15）](task-service-review-2026-10-15.md)

