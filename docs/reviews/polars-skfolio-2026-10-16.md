# Polars 与 skfolio 产品接入

## 范围与设计

2026-10-16，用户明确要求在项目中使用两库。基线为主 checkout `main` 的 `0d52b8f`
加上已完成、尚未提交的因子验证比较；保留这些改动。已复核架构、实施和审计记录。

实际入口为回测比较中的组合研究：选定 2–3 个服务已核验的回测结果及训练截止日，
Polars 将真实结算权益转换为收益表并按相同收益区间对齐，skfolio 只用训练段拟合
非负权重、合计为一的最小方差配比，随后对验证段预测，并与等权配比并列展示。
训练和验证均至少二十个共同观测；结果记录源任务/摘要、实际区间、排除数量和库版本。

比较独立新增 Python 进程/运行环境与扩展现有冻结助手两种方案，选择后者。将原
QuantStats 专用助手改为一个桌面 analytics 助手，保留 QuantStats 业务命令，使用明确的
Protobuf oneof 区分两项分析。算法仍位于 plugins，Native 拥有输入核验、子进程和超时，
UI 只提交任务身份与截止日。没有新客户端、常驻服务、数据导入、任务类型或执行路径。

Polars 对 Decimal 权益先求差再转统计浮点；收益先按各自连续结算计算，再对齐
`(前一结算日期, 本次结算日期)`，不能把某实验跨两日的收益与另一实验一日收益混合。
缺失记录不补零、不前向填充。实际共同记录才进入训练/验证，原实验始终不变。
QuantStats 同样消费该 Polars 收益准备路径，删除原 Python 收益求差循环。

skfolio 使用 Ledoit–Wolf 协方差收缩和 CLARABEL 求解最小方差，不使用验证段拟合或选优；
失败明确返回错误，不换求解器或算法。验证收益为固定权重逐期配比的研究模拟，原回测
费用已包含，但不模拟组合再平衡、成交、手数、保证金共享或新增费用。
它不构成期货可执行组合，也不能把已回看/选优的原实验重新称为未经查看的样本外证据。

依赖以 uv 锁定，显式构建冻结程序随桌面发布；运行时无下载。平台保持 Linux x86_64
与 macOS arm64。使用 Polars 官方兼容 CPU runtime，避免扩大 Linux CPU 指令集要求。
保持现有 Unix Socket 分帧、Protobuf 和 ErrorCode 诊断，中文/英文同步。

API 依据：[Polars 安装及 CPU 支持](https://docs.pola.rs/user-guide/installation/)、
[Polars join](https://docs.pola.rs/api/python/stable/reference/dataframe/api/polars.DataFrame.join.html)、
[skfolio MeanRisk](https://skfolio.org/generated/skfolio.optimization.MeanRisk.html)、
[EmpiricalPrior](https://skfolio.org/generated/skfolio.prior.EmpiricalPrior.html)、
[LedoitWolf](https://skfolio.org/generated/skfolio.moments.LedoitWolf.html)。

## 实施与验证

当前实现文件：`plugins/tools/analytics/report.py`、`protocol/proto/asterion/v1/analytics.proto`、
`apps/clients/terminal/native/analytics.cpp`、`commands_task.cpp`、研究界面的 `PortfolioReport.tsx`。
已移除原 QuantStats 专用运行时入口并更新所有开发、Electron、签名/依赖校验及 CI 引用；
现行准备命令为 `pnpm prepare:analytics`，或首次启动使用 `pnpm desktop --prepare-analytics`。

主 checkout Linux x86_64 / Debug 的实际验证：

- Native、开发桥、Node-API、相关服务与数据测试夹具重新构建通过。
- 冻结程序的 6 项 CTest 全部通过：QuantStats 原统计与高精度差值、无效权益/样本；
  组合仅训练段拟合、两个验证配比的逐期收益及复利、缺失区间排除、短验证段与常数训练拒绝。
  两项架构检查也通过，共 8/8；没有把历史全量 CTest 的结果当成本轮结论。
- Vitest 42 文件 117/117 通过，新增组件测试验证请求身份与日期、配比/排除数量展示、
  日期改变后清除报告及错误摘要拒绝；既有因子比较改动保留并参加本轮测试。
- 3/3 隔离 E2E 通过：QuantStats 无效样本及成功报告、Polars/skfolio 经 Task 核验、Native、
  冻结助手、比较界面显示的完整链路；过期摘要明确冲突，原任务数量和结果摘要不变。
  夹具使用测试历史数据与隔离 CTP SDK，不代表真实行情或柜台。
- TypeScript/Vite、ESLint、格式和 diff 检查通过。Vite 的主包体积提示仍存在。
- 冻结资源清单与字节摘要验证通过，确认仅包含 Polars compat runtime。Linux 助手资源为
  680,192,859 字节（约 649 MiB，未压缩，包含 Python/三库/传递依赖和许可证）。
  真实 ELF 依赖扫描通过；macOS 13 arm64 锁定 wheel 的 `uv sync --dry-run --no-build`
  解析通过，这不代替 macOS 运行或签名验收。

初轮真实冻结程序测试发现 CVXPY 1.9 在启动时枚举包目录，默认 PyInstaller 归档缺少该目录；
通过一个明确的 `hook-cvxpy.py` 保留实际模块文件，未修改或绕过 CVXPY。随后修正
EqualWeighted 的导入位置为 `skfolio.optimization`，完整 Python 分析路径先验证，再重新冻结。
这些失败由真实子进程测试发现，未以环境内 import 成功冒充冻结程序通过。

未完成发行级桌面安装、macOS 执行/签名/公证、真实柜台或生产数据验收。服务源码指纹因
协议及插件变化而改变，旧远程服务包不能代表当前源码；正式发布前须按开发指南重建。
没有更改用户账本、任务或数据集，也没有提交或推送。日志与身份记录放在
`build/reviews/polars-skfolio-2026-10-16/`。
