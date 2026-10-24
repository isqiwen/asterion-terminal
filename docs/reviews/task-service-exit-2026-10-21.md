# 独立 Task 退出与 Data / Research 所有权交付

2026-10-21，实际仓库：research `/home/qiwen/Code/quant/asterion-terminal`，`main @ a7178026c829a43b2c0dffa486f5e25eeba35320`，包含全部既有与本轮未提交改动。开发始终通过 air 的既有 `ssh research`；没有换用本地/云端副本，没有 commit、push、部署或实际服务维护。

## 目标核实与实际交付

依据 `architecture-design-review.md` 约 168、251、504—511 行及 `requirements-architecture-assessment-2026-10-21.md` 约 127、364 行：Research 替代独立 Task，Data 拥有采集；公共任务概念保留，各业务所有者持久自己的状态，不能将整个 TaskHost/Store 搬进 Core。

- 删除独立 Task 源目录、可执行目标、服务类型、默认配置和通用提交 RPC；当前程序为 `asterion_market`、`asterion_data`、`asterion_research`、`asterion_trading`。`task.proto` 仅保留公共记录/观察结构，`asterion_task_store` 是共享静态机制，不是服务进程。
- Data 自主管理分钟下载、缺失表更新和单分区刷新：预览、授权、接纳、持久账本、两个固定执行槽、取消、显式重试、发布、结果和恢复。账本在 Data 的 `acquisition-tasks/`；接纳前核对任务与授权身份。仅恢复原持久发布决定，不自动重跑供应商请求；重复接纳不产生重复发布。
- Research 仅接纳回测/因子，通过 `ResearchRequest` 显式声明绑定 Data。共享持久机制位于 `apps/services/task-runtime`，采集算法只链接 Data，没有搬入 Core 或建立第二套任务框架。
- 移除 Data → Task 反向配对及一对一约束；Data 可独立部署，多个 Research 可绑定同一 Data。Research 部署必须显式选择 Data，当前受管绑定仍限同节点。TCP Data 同时提供受管本机公共 Socket 给 Research，私有采集接口保持隔离；这不等于已实现任意跨节点组合。
- Node Agent、Manager、部署表单、默认/实例 `research.json`、Electron 程序定位、Native/远端资源清单、CI 入口和相应夹具同步。普通启动恢复已有运行工作区，不自动创建固定配对或改回用户的停止意愿。
- Terminal 分别维护 Data 与 Research 的任务页，任务中心按节点/服务/任务聚合。读控必须携带 `owner {kind,node_id,service,connection_id}`；同名任务不会串控，旧连接/错误所有者在发送前拒绝。Data 历史使用报告包含其自有采集任务引用。

## 已复现并修复的实质问题

1. 原单执行槽破坏既有重叠采集验收，现为两个固定槽，目录与状态发布仍由 Data 串行写入器裁决；延迟补齐不能覆盖后发布分区。
2. TCP Data 缺少同节点公共 Socket，导致远程 Research 读不到绑定 Data。已补充公共本机入口，隔离远程部署的真实回测/因子链路通过。
3. Research 持久 claim 后，输入确认拒绝会在令牌交回调度前抛错，使任务停在 RUNNING。保留已持久领取令牌，原失败分支能结束该尝试；既有无效输入回归由 60 秒超时恢复为约 2.68 秒通过。
4. 下载身份、固定配对、启动步骤、安装配置和 UI/SSH 参数夹具存在漂移。按当前所有权同步，并将发布恢复测试移交真实 Data 所有者；未删断言、跳过失败或降低合同。

## 实际验证

证据目录为 `build/reviews/task-service-exit-2026-10-21/`。以下组有重叠，不相加冒充全集。

| 验证 | 实际结果 | 证据 |
| --- | --- | --- |
| Linux Debug 全量联编 | 通过；最终 `ninja: no work to do` | `final-full-build.log` |
| TypeScript / Vite、lint、格式 | 全部通过；格式检查 429 个 C/C++/Proto 文件 | `frontend-build.log`、`frontend-lint.log`、`final-format-check.log` |
| 完整前端单测 | 48 文件，142/142 通过；启动恢复另有 10/10 定向通过 | `frontend-all-tests.log`、`startup-tests.log` |
| Data / Research / 安装合同最终整组 | 27/27 通过，67.10 秒 | `final-owner-contract-tests.log` |
| 节点与终端复验 | 14/14 通过，121.87 秒；含远程部署 65.61 秒 | `node-terminal-followup.log` |
| 固定 Data 输入与公共端点 | 5/5 通过，13.44 秒 | `final-fixture-tests.log` |
| 持久化、授权和停止 | 45 项通过；资源清单唯一旧指纹失败经最终联编后单项通过 | `persistence-contract-tests.log`、`final-integration-tests.log` |
| 单分区所有权恢复 | 取消、Data 崩溃/显式重试、重复接纳、重启读结果通过；预算静默期保留，恢复约 62.64 秒 | `remaining-owner-tests.log` |
| 同名任务所有者与后台恢复 | 同名 Data/Research 各只收到自己的控制；旧连接拒绝；Terminal 退出后后台任务恢复通过 | `final-integration-tests.log` |
| SSH 隔离验证 | `ssh_keys` 通过；`ssh_bootstrap` 最终通过，9.63 秒 | `final-ssh-tests.log`、`ssh-bootstrap-followup.log` |
| Native addon / 桌面分钟种子 | 真实 Node-API 通过；仅 Data 进程恢复排队种子、发布两版本，无供应商读取 | `native-addon-test.log`、`desktop-minute-owner-test.log` |
| 最终 E2E | 3/3 通过，约 1.1 分钟：显式部署切换、严格快照/结果合同、F07 刷新保留旧实验与同值修订 | `final-owner-e2e.log` |

历史失败日志保留。发布恢复、无效固定输入、旧部署字段和资源源码指纹问题均有后续通过证据；未把旧 CI 或旧阶段通过当成本轮结果。

## 受控修改与数据边界

此前未经约束的全局批量操作被自动审批拒绝且未执行。用户随后批准按明确文件白名单分组修改；本轮每组预读/核对后写入并检查差异，审计见 `group-*-files.txt` 与 `format-*-files.txt`。没有恢复原批量方案，没有 reset、clean、stash、重写历史或自动解冲突。此前修改和实验数据保留，旧构建产物未擅自清理。

不自动迁移旧身份或历史数据；不支持的旧记录明确拒绝并保留文件。未读取或输出真实私钥、令牌、密码；未使用真实供应商额度、交易、付费服务或外部代理。

## 未验证与下一步

本轮未跑完整 CTest、完整桌面 GUI、macOS、sanitizer、发行安装/签名、公证或真实柜台/供应商验收；旧 Release/服务包不能据此称为当前发行品，发行前须重建并核验。上游 a717802 CI 的失败不包含这些未提交改动，本轮未重新触发 CI。

本轮独立 Task 退出及现有 Data/Research 职责迁移已完成，没有待批操作或本轮已复现未解决失败。Intelligence、研究目标中的新功能、任意跨节点组合及实盘执行所有权/失联策略不由本次迁移自动实现或批准。后续按最新 FR 与评审选择下一功能；真实运行/发行验收保持独立授权与证据。
