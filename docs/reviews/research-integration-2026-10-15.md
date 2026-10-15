# 研究链联合验收 — 2026-10-15

## 结论与环境

本批隔离联合验收通过，发现的分区重取任务恢复缺陷已修复；这不代表图形桌面、真实供应商或真实柜台验收通过。

实际工作目录为 research 的 `/home/qiwen/Code/quant/asterion-terminal`，分支 `main`，HEAD `bacf29d33e8748fbf4c4a8585f3bfaa0926ada41`。在原主仓库保留全部既有未提交改动；没有提交、推送、生产部署或架构迁移。联调开始和收尾均核对构建/测试进程，未覆盖其他工作。没有调用真实供应商、消耗真实配额或操作用户账本；使用临时隔离 Node、测试 CTP SDK 与合成历史数据。

## 代表性闭环与证据

新增 `apps/clients/terminal/e2e/research-integration.spec.ts`，在同一隔离环境中执行七步：

1. 固定 2024-01-02 至 2024-03-13 的 52 条日线/标的，三个合约共享同一快照；核对持仓量不足标的的资格排除理由。
2. 固定快照下，动量窗口 2→5 显式派生；取消不提交、原任务结果保持，比较并下载 JSON，核对文件内容和摘要。
3. 同一证据上的单合约动量回测产生实际模拟成交；检查费用、三个月贡献、已闭合轮次和未平仓日期统计，导出并核对完整回测证据。
4. 显式派生滑点 1、2 两个任务；除滑点外规范化输入与原任务一致，专用表展示基准与变体，原任务保持。
5. 同一日分区重取：更正发布新修订，结构化失败不改旧内容，显式重试后成功；空响应不发布、不推断覆盖；相同内容不再创建修订。
6. 显式使用新快照复用因子，核对其数据版本变化，同时旧固定实验仍保持原结果。
7. 停止并重新启动 Data、Task 服务，核对进程已更换；全部 10 个任务的结构化结果与重启前逐一完全相同，当前修订保留，原结果仍可展示。

持有期只验证新增单合约日线结果的交易日期差与固定输入结算日跨度；不宣称盘中时长或旧无日期结果已被补齐。测试中的合成盈亏仅证明敏感性与归因链路，不是策略收益结论。

HTML 报告：`build/reviews/research-integration-2026-10-15/html/index.html`。
`test-results/research-integration-resea-0fc17-vision-recovery-and-restart/` 中有 `joint-evidence.json`、两份导出 JSON、五张截图和 `trace.zip`。截图 02 是导出后的页面状态，不能单凭该截图认定月贡献/持仓表可见；对应断言和过程在测试代码及 trace 中。

## 实际修复与回归

重启暴露 `task index does not match state history`：`TaskStore::admit_as` 从持久 admitted 任务恢复摘要时漏掉 `table_refresh`，与已保存索引不一致。仅补回该字段投影，未重写、迁移或重建用户任务。

- 修复：`apps/services/task-service/task_store.cpp` 中 `admit_as`。
- 回归：`TaskStore.RefreshTargetsAndResultsSurviveRecoveryWithoutChangingEvidence`，位于 `tests/tasks/task_execution_test.cpp`。验证成功空响应和结构化失败任务恢复、原固定输入/任务/结果字节不变、失败任务显式重试后摘要仍保留。
- 修复前同一新回归重现异常：`recovery-before-fix.log`，这是预期失败证据，不计入通过项。
- 修复后上述用例及已有重启、失败码恢复用例，3/3 通过，1.06 秒：`recovery-after-fix.log`。
- E2E 包装器只增加临时 fixture control 路径传递；联合测试 finally 删除控制文件。同次运行原 F07 用例也通过，核对后续场景不受影响。

## 验证结果

| 验证 | 结果 | 证据 |
| --- | --- | --- |
| 定向 TaskStore 核心回归 | 3/3 通过 | `recovery-after-fix.log` |
| 联合场景及原 F07 回归，同一包装器 | 2/2 通过，约 1 分钟 | `joint-e2e.log`、HTML、trace |
| 修复后 C++ Debug 构建 | 通过 | `debug-build.log`、`regression-build.log` |
| 前端 TypeScript/Vite 构建、ESLint | 通过 | `frontend-build.log`、`lint.log` |
| 本批 TS/C++ 定向格式、git diff --check | 通过 | `format.log`、`diff-check.log` |
| Ubuntu 24.04 标准 Linux 服务包 | 构建通过 | `linux-bundle.log` |
| `pnpm desktop:check` | 通过：包校验、Native 桥真实响应及隔离环境退出 | `desktop-check.log` |
| 包 manifest、源码、13 文件 SHA-256 | 全部一致 | `verification.json` |
| Debug 与 staged Native SHA-256 | 一致 | `verification.json` |
| Playwright 报告服务 | 127.0.0.1:9323 返回 HTTP 200，HTML 可读；核验后停止自建进程组 | `report-viewer.log`、`verification.json` |

构建有既存测试 nodiscard 警告与前端 chunk 大小提示；本批没有因此扩展重构。未重复全仓库测试；通过项仅限表列范围。最终摘要如下：

- 源码指纹：`8351b5f0c715e6c74b5fb0b6b9370fa568b68b68edd0cf57ca690e6cc6c9a2b6`。
- 服务包 SHA-256：`1822f7b98a34231ed09bca8ab460e275baff216fbc75098f5f13ac09ae1b36f7`。
- Native SHA-256：`c556dee8a0ccf86c51896341a5cbff2a6a47ef75411540b1337870629577876e`。

包位于 `build/linux-bundles/asterion-services-linux-x86_64.zip`。本包是服务资源产物，不是已经验收的桌面安装发行包。

## 可复现报告与安全演示

以下从 research 实际仓库执行。现有报告无需重新运行测试即可查看：

```sh
cd /home/qiwen/Code/quant/asterion-terminal
pnpm exec playwright show-report build/reviews/research-integration-2026-10-15/html --host 127.0.0.1 --port 9323
```

此报告服务命令已实际启动并核验 HTTP 200，随后停止。若从 air 浏览，可另开终端转发：

```sh
ssh -N -L 9323:127.0.0.1:9323 research
```

浏览 `http://127.0.0.1:9323/`，打开联合用例，依次查看七步、截图、JSON 和 trace。SSH 转发命令本批未实际运行，不列为已验项目；保持现有主机密钥校验，不添加绕过选项。查看完分别用 Ctrl-C 停止自建查看器与转发。

若需重新演示隔离闭环，先确认无其他构建和 1423 端口冲突，再在 research 仓库运行实际已通过的包装器命令（新报告另放 demo 目录，保留本次验收证据）：

```sh
PLAYWRIGHT_HTML_OPEN=never PLAYWRIGHT_HTML_OUTPUT_DIR=build/reviews/research-integration-demo/html pnpm run test:e2e --grep 'research chain integration @joint|F07 explicitly refreshes' --trace on --reporter=list,html --output build/reviews/research-integration-demo/test-results
```

该命令的同一测试选择及参数已通过；demo 输出目录只改变生成产物位置，没有单独重跑。包装器创建临时环境，结束后清理，因此这是可重放的隔离研究流程演示，不是留在日常账本中的示例项目。不要直接调用 `playwright test`。

实际 Electron 交互入口仍为同一 research 仓库的 `pnpm desktop`，见 `docs/development.md`。本 SSH 环境没有 DISPLAY/WAYLAND，未启动窗口，也未核验图形操作。需要可用的 research 图形会话后才能执行该项；本批不安装显示组件、不绕过认证、不换用 air 本地副本。

## 未验边界与下一步

未验：research 图形桌面交互、macOS、签名/安装包、真实 CTP/供应商、长期运行和负载、完整全仓库测试。浏览器+Native+隔离服务通过不能替代这些验收。

来源证据仍阻塞 F02 历史费率/保证金单位及生效期、完整 F03 历史夜盘/临时停盘和无成交证据、F12 历史合约规格、F07 供应商历史日期覆盖保证。合成测试没有填补这些事实，也未开启真实历史成本接入。

下一优先是人工复核当前联合证据；图形验收需可用的 research 图形会话，再按需要运行同版本跨平台 CI/安装验收。独立开发候选是固定回测证据的期货手数、敞口、保证金与资金/费用核对：先复核现有实现与缺口，再限定一个最小交付，不先引入优化器或新的权威账本。若要推进历史事实，应先由用户明确可用来源与权限；真实请求、柜台连接、外部模型/自动代理、架构迁移均不从本次通过结果推导授权。PostgreSQL、Polars、Qlib、RD-Agent、skfolio、Lightweight Charts、QuantStats 均未新增或接入。

## 已确认的本地参考项目

用户授权按需参考后，只检查了项目父目录及直接同级 quant 候选。实际集合是 `/home/qiwen/Code/quant`，与 asterion-terminal 共处其中；`/home/qiwen/Code/quant/quant` 不存在。没有递归扫描无关私人目录。

本批只读了以下本地项目的顶层结构、README 与 LICENSE，另外读取 Nautilus 的三份概念文档；没有安装、执行或复制参考代码。

| 项目 | 本地许可证声明 | 本次有帮助的设计与边界 |
| --- | --- | --- |
| `nautilus_trader` | LICENSE 标示 LGPL v3 | `docs/concepts/data/catalog.md`：列式事实与查询接口分层；`portfolio.md`：账户/持仓派生的敞口、保证金、权益；`execution/reconciliation.md`：柜台明确持仓报告是实盘对账依据，回测自控成交不等于真实对账。仅参考思想，不引入其 Rust/DataFusion 栈。 |
| `wondertrader` | LICENSE 标示 MIT | README 的理论部位、组合目标与执行通道分层，适合进一步评估简单组合风险预算及研究目标到执行结果的解释。暂未审查或复制具体实现。 |
| `vnpy` | LICENSE 标示 MIT | README 列出组合策略、组合管理和风控模块，说明相关能力边界；这些多为独立外部仓库，本地 README 不等于已审查这些模块的代码。 |

下个可独立交付候选：对一个固定单合约日线回测，核验成交→手数/持仓→按结算日资金与费用的守恒，明确可由固定输入证实的名义敞口/保证金，以及缺失事实的未知状态；从现有绩效/账户机制复用，先补实际缺口与针对性回归。Nautilus 的 Portfolio 与 Reconciliation 文档最直接有助于划清派生统计、模拟核对和柜台事实三者的边界。这里记录候选范围，未宣称该功能已实现。
