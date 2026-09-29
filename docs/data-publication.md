# 数据发布与统一内容身份

Data Pipeline 的第一条真实执行链为：类型化 CSV 导入请求 → 读取固定字节快照 → 源摘要验证 → CSV 插件逐行校验 → 不可变数据发布。独立 CLI 和 Task Service 工作进程两种入口共用导入实现，已接入 Agent 与 Terminal。

## 契约与归属

- `protocol/proto/asterion/v1/data.proto` 定义 TradeDataset、CsvImport、CsvSnapshot 和 DatasetPublication。
- `protocol/src/data.cpp` 负责严格格式、摘要和领域约束校验。TradeDataset 版本 1 包含实际合约与有序 Decimal 成交，不包含交易成本、策略参数、路径或交易日假设。
- CSV 解析仍在 `plugins/data/csv/`。支持文件流和拥有所有权的字节快照，两种入口共用解析器；导入器对同一快照计算 SHA-256 并解析。
- `apps/services/data-pipeline/` 编排导入和发布；文件持久化复用 filesystem-journal 的公开契约与原子记录机制，不在 Core 写入 CSV 业务规则。

内容 revision 对规范化合约与完整有序成交计算 SHA-256。不同换行或小数拼写，只要规范化数据完全相同，即得到相同 revision。时间相同的多笔成交按原顺序保留，不擅自合并。

publication ID 另外绑定内容、源文件名、原始字节摘要、字节数和导入器版本。相同内容来自不同源文件时可有相同 revision 和不同 publication ID；不能把发布来源身份当作成交内容身份。发布物不记录完整本机路径，不依赖源 CSV 后续存在。

BacktestInput 版本 5 和 FactorInput 版本 4 共用 TradeDataset 内容摘要。旧回测输入版本 1/2/3/4 明确拒绝，不提供自动迁移或旧摘要回退。回测仍单独校验显式交易日/时段约束，因子仍单独要求正价格和足够的标签样本；数据发布不冒充交易所日历，不将某个算法的价格限制强加给所有数据消费者。

## 校验与发布

CsvImport 必须包含版本、完整合约、源文件绝对路径和预期源摘要。拒绝源文件符号链接、未知 Protobuf 字段、缺失字段、非实际合约、未对齐价格/数量、乱序、空数据和超限输入。当前至多 10000 笔、原始 CSV 至多 32 MiB；不静默截断、排序、去重或清洗。

先完成全部校验，才写入调用者指定的专用既存目录。成功目录只有一条 dataset.published 记录及单写者锁。记录包含完整类型化数据的规范 JSON 表示与来源信息。相同发布再次提交不追加记录；不同发布不能覆盖；读取时重新检查规范化摘要与来源身份。中断临时文件保留并报告，不清理或冒充已完成发布。

现有文件日志提供同步与原子记录提交；当前测试覆盖独立进程结束、读取恢复和中断文件拒绝，不代表三平台断电故障验收。

## 持久化任务与界面

Terminal 对导入时的同一份字节快照进行解析和来源摘要计算。点击“发布数据版本”将 CsvSnapshot 的内容、来源名称、摘要与合约提交至当前研究服务，不上传本机路径。任务上传上限为 4 MiB / 10000 笔；单文件 CLI 保持 32 MiB 上限。数值 CSV 采用 ASCII，原换行与小数拼写保留，不自动修复未知编码。

Task Service 为 DATA_IMPORT 任务保存完整输入，支持进度保活、取消、旧尝试令牌隔离与显式重试。数据解析由独立 Data Pipeline 执行，结果必须与输入重新计算一致才可提交；非法行导致任务失败，不发布部分结果。进度以源字节总数表示：计算阶段保持未完成，完整校验后确认全部字节；不展示虚构的线性百分比。

Agent 按任务类型选取 Data Pipeline，与 Backtest、Factor 共用每服务最多两个活动工作进程的上限。程序摘要在部署和执行时检查，进程由 Agent 持有，退出 Terminal 不终止任务。

“数据 → 数据发布”列出当前研究服务上的发布任务，显示源文件名、合约、状态及操作。已完成任务可以“使用此版本”，恢复完整成交快照并用于行情预览、回测或因子分析。载入版本后收起导入表单，来源标识和内容摘要置于详情。任务中心由数据插件贡献发布任务，研究插件只展示回测和因子，避免重复。支持中文与英文。

发布结果保存在 Task Service 的完成结果日志中，不依赖原 CSV 或 Terminal 进程。独立 CLI 的单目录发布和服务端任务结果使用同一 DatasetPublication 契约，但由各自的应用管理提交边界。发布身份相同的不同任务仍分别保留历史，不自动清理或合并。

本机和远程 Linux 包均加入 Data Pipeline。研究服务配置必须同时包含 Backtest、Factor、Data Pipeline 摘要；缺少字段的旧配置明确拒绝，不迁移、不删除。

## 尚未完成

尚无跨服务统一数据目录、分片/断点续传、供应商抓取、Parquet、完整质量统计或数据保留策略。CsvImport 的 source_path 仅用于独立程序所在机器；远端任务始终使用 CsvSnapshot。

合约规格来自调用者提供的输入，未接入交易所正式合约资料；校验符合当前领域格式不代表供应商来源真实性已获证明。

本轮 macOS 全量 CTest 90 项中 89 项通过，Linux 专用部署 1 项跳过（build/dataset-full-tests.log）；真实 C++ 后端的导入、回测、因子与断连恢复 UI 4/4 通过（build/dataset-ui-tests.log）。新增测试包括规范化内容同一性、不同来源身份、修改源文件拒绝、重复成交保留、非法/超限/取消、持久化幂等与拒绝覆盖、损坏与中断文件拒绝、独立程序发布和删除源后的检查。

前述独立 CLI 阶段没有更新 DMG；本次任务集成阶段的包验收以文末记录为准。

Linux x86_64（Ubuntu 24.04，macOS 上的 amd64 仿真容器）GCC 编译及完整 CTest 90/90 通过（build/dataset-linux-tests.log）；最后的取消检查、严格记录版本和中文源文件路径调整另行 5/5 通过（build/dataset-linux-final.log），本机同组 5/5 通过（build/dataset-final-validation.log）。Windows 原生仍未验收。

## 任务集成验收

macOS 全量 CTest 92 项中 91 项通过、Linux 专用部署 1 项跳过（build/data-integrated-tests.log）；Linux x86_64 仿真容器 92/92（build/data-integrated-linux.log）。最终源文件名元数据与非法 CSV 工作进程失败路径另行验证，本机及 Linux 各 8/8（build/data-label-tests.log、build/data-linux-final.log）。全量 UI 20 项通过、1 项平台专用跳过（build/data-full-ui.log），最后列表文件名与折叠导入交互 1/1（build/data-label-ui.log）。

真实 Agent 验证上传后删除源 CSV、强制结束 Terminal、重启研究服务，发布物与因子结果仍可恢复；载入发布物再次计算因子，内容版本与完整结果一致。Linux 的 TCP/mTLS 发布验证是容器回环链路，不是两台物理机器网络验收。Windows 原生尚未运行。

新 macOS ARM64 DMG 已构建并验证（build/data-desktop-build.log）：镜像校验、包内签名、本机 Data Pipeline/Task Service/Factor/Backtest 链路和删除源后的任务恢复通过；内置 Linux x86_64 九个服务资源/初始化文件按摘要验证。签名为 ad-hoc，未公证；包内程序通过真实 C++ 桥验证，本轮未操作打包后的原生窗口，未替换已安装应用或升级用户现有 Agent。

## 结算表发布

已增加独立的结算表 CSV 插件、共享交易日契约与不可变 CalendarPublication，可由 Data Pipeline CLI 发布/检查，供多日回测复用。已加入持久化任务、Agent 派发、Terminal 导入和回测版本选择，完整边界见 [结算表](settlement-calendar.md)。

Tushare 期货历史分钟使用独立 Bar 数据集与本机托管下载任务，不进入本页的逐笔 CSV 契约。下载、取消、恢复及后续研究边界见 [Tushare 期货分钟](tushare-futures.md)。
