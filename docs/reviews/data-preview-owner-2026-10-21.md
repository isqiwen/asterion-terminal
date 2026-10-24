# Data 预览职责切片与终端验收夹具修复

日期：2026-10-21。实际仓库：research `/home/qiwen/Code/quant/asterion-terminal`，分支 `main`，基线 `a7178026c829a43b2c0dffa486f5e25eeba35320`。保留上一阶段全部未提交改动；未提交、推送或部署。

## 已交付

终端测试现在自行创建唯一的临时节点目录，测试结束恢复原环境并只清理自身目录。此前 App 在连接服务前创建 terminal.lock，未准备安装目录使测试提前失败；没有放松生产目录或锁要求。Live 测试服务将输入中的 simulation 环境写入快照，继续满足交易环境的严格校验。SSH 密钥测试创建自身 state 目录；bootstrap 测试按当前默认并发 1、编辑值 2 验证配置及修改时间保留，没有修改真实节点配置。

Data 工作台复用现有 Data 计划接口，分钟档案补全/延伸及日线分区重取预览不再依赖 Task 在线。Task 离线时仍可查看计划，执行按钮明确不可用；恢复在线不会自动提交。分钟下载执行仍要求本机 Task。Native 提交在重新核对计划、连接代际后，先拒绝已知离线的 Task，再进入凭据解析与授权；没有宣称该检查消除了后续断线竞态。缺少 Data 时的提示也准确指向 Data。

新增前端回归覆盖离线预览和重新连接不自动提交；Native IPC 回归验证计划可读、离线提交明确失败、测试源未收到授权或任务提交。没有新增来源请求、任务种类、持久化结构或后台 worker。

## 实际验证

| 范围 | 结果 | 证据（仓库相对路径） |
| --- | --- | --- |
| 终端隔离、并发及应用定向 CTest | 16/16 通过 | build/reviews/terminal-isolation-2026-10-21/terminal-tests.log |
| SSH keys/bootstrap 与节点生命周期 | 3/3 通过 | build/reviews/terminal-isolation-2026-10-21/ssh-tests.log |
| Data 预览前端两份测试文件 | 11/11 通过 | build/reviews/data-preview-owner-2026-10-21/ui-tests.log |
| HistoryUpdate 与 ServiceClients Native IPC | 4/4 通过 | build/reviews/data-preview-owner-2026-10-21/native-tests.log |
| 前端 lint、生产构建、格式检查 | 通过；Vite 既有大 chunk 提示仍在 | 同目录 lint.log、frontend-build.log、format-check.log |
| Native 查询测试、dev bridge 与 Node addon 构建 | 通过 | native-build.log、native-boundary-build.log |

先复现再修复：ssh-before.log 记录两个 SSH 夹具失败；ui-before.log 记录新增的两项离线预览测试失败。终端与 IPC 共 20 项不同测试。前端实际运行两份文件 11 项，不把不存在的 ArchivedDataPanel 测试文件计入证据。构建产物包括 build/Debug/asterion_terminal.node，未替换已运行桌面的资源。

测试仅使用临时目录、模拟 SSH 程序和回环测试服务；未使用真实供应商额度、真实账户、真实 SSH 维护或实盘。没有改动旧数据、凭据、安全配置或运行中服务。

## 范围及下一步

这是 Data 预览可用性切片，不是完整 Data/Research 服务拆分。执行下载、持久任务、队列及 Data/Task 部署配对仍由现有实现承担。既有共享代际检查仍保留。评审中的跨节点实盘执行所有权及监控失联策略尚未定案，本阶段不依赖也不改变它们。

本阶段没有运行完整 CTest、sanitizer、桌面 E2E、macOS 或发行包验收。旧 CI 的 TableRefresh.ConcurrentFixedSelectionsCannotOverwriteAnotherCorrection 失败尚未在当前工作区复现或解释，不能当作已修复；也不能将旧 CI 失败冒充当前未提交代码的结果。未修改服务构建输入，未重新发行服务包。

下一优先项是选取单分区刷新，将接纳、持久状态、取消、局部发布与恢复作为完整链路移交 Data，并覆盖第二调用方和带服务身份的任务控制；Research 保留计算职责。应先核对当前任务状态与恢复合同，不能仅重命名 Task 或先铺空服务。
