# 新基线首个里程碑：安装维护预检与恢复（2026-10-21）

## 同步及范围

实际仓库 `/home/qiwen/Code/quant/asterion-terminal`，分支main，远端 `git@github.com:isqiwen/asterion-terminal.git`。开始时工作区干净，HEAD为e9c643f；已fetch并确认落后1提交、无分叉，使用 `git pull --ff-only origin main` 快进至 `a7178026c829a43b2c0dffa486f5e25eeba35320`。没有stash、reset、clean、冲突处理、提交、推送或用户资料变更。仅见常驻Codex服务，未见并发构建/拉取；不能据此断言没有其他登录使用者。

完整读完新的目标架构及10月21日全面评审，核对需求总纲、产品/交付、共同契约、页面指南、责任映射入口，以及本里程碑的安装配置功能和UI。未声称已逐条审阅/实现全部304 FR。最新评审A01—A15是建议及待处理项，不能把实盘唯一归属、监控失联和15分钟方案当成已批准行为。当前AGENTS已经取消内部引擎/格式版本累积和旧格式兼容承诺；旧v19/v20问题不再作为这次独立开发的暂停理由，也没有新增兼容例外。

先恢复开发验收阻塞，交付一条有限纵向链：安装器读取状态→区分远程检查结果→显示具体阻塞→用户显式重查→重新确认维护；以及已经发布但未完成升级的明确继续入口。本批不是Data/Research职责切换或五类服务已实现声明。

## 已实现

1. Installer的accept/refresh采用稳定callback，并完整声明effect依赖，修复当前HEAD可复现的lint错误；普通重渲染不会反复启动预检。
2. 整体升级或本机加全部远程联合卸载，在远程状态检查及最终本机复查未完成时不能确认执行。检查失败显示现有错误与新增中英文重查提示，不把失败解释成“无远程节点”；显式重查成功仍须重新确认。执行器继续负责最终预检及受理判断，UI不取代后端安全边界。
3. 同版本本机修复仍不受远程检查失败阻止；本机只卸载不被误套整体远程门禁。已持久记录的pending操作按其原动作显式继续，不强制重新发起一轮全新维护。
4. 新恢复回归发现原界面以deployment_action限制pending upgrade：发布后当前安装身份变化可能让“继续升级”不可用。现优先采用原生执行器的pending动作；冲突、活动进程及用户确认检查仍在。
5. CTP连接两个聚合初始化显式初始化新增trading字段；安装器macOS空实现显式处理其未使用参数，修复已定位的Clang编译告警原因。没有读取、修改任何真实账户或凭据。
6. CI remote-linux删除已移除的backtest/factor/data_pipeline worker可执行文件检查，保留当前五种服务程序。没有恢复worker、别名或任务派发路径。
7. NodeProgramPublication配置保留测试按现行默认task_concurrency=1、显式编辑值2验证；原来的2/5断言与当前1–2契约漂移。仍检查部署不覆盖原文件及修改时间，不改用户配置。

## 本轮验证

| 项目 | 观察结果 |
| --- | --- |
| 新增升级预检回归 | 修复前失败：远程检查失败后确认框仍可勾选；修复后通过 |
| Installer + RemoteNodes | 2文件14项通过；含显式重查、同版本本机修复、pending升级继续及原资料保留场景 |
| CtpConnections / Installation / InstallationStartup | 22项不同CTest通过；安装13项在重新链接后再验通过，不重复计数 |
| NodeProgramPublication | 5项通过，含当前默认配置、保留运营编辑、发布与目录同步失败恢复 |
| Electron维护顺序 | `node tests/desktop/installer_orchestration.cjs`通过；完全模拟进程，无实际安装/停服务 |
| 原生构建 | maintenance、CTP连接、Terminal、installation及publication测试目标构建成功 |
| 前端 | 最终pnpm lint、pnpm build通过；format:check含425个C/C++/Protobuf文件通过，git diff --check通过 |
| CLI入口 | Debug中五种现行服务--version正常返回；未启动业务服务 |

未运行实际安装、systemd/launchd维护、真实SSH主机维护、真实柜台/供应商请求、桌面E2E、完整测试全集、sanitizer或macOS构建。macOS源级修复待原生macOS CI验证。研究机器没有build/Release/asterion_node_agent，因此本地Release版本入口未验，随后只检查实际存在的Debug程序；不冒称重新打包通过。

## CI与更深层失败准确边界

当前HEAD的CI为 https://github.com/isqiwen/asterion-terminal/actions/runs/39583801170 ，最后读取时仍in_progress，style已因同一Installer lint失败，其余执行job尚未给最终结论。未提交本批修复，远端CI未包含这些修改。

前一实现提交e9c643f的run39577659772是历史诊断线索。本批下载其失败日志，未把其24/33项失败写成最新HEAD全量结果，亦未据sanitizer job失败断言内存缺陷。

当前HEAD仅探测两项Terminal测试，直接CTest均先因“agent directory is unavailable”失败；通过已有isolated_node.py临时目录包装再次诊断：

- TerminalApplication.DelayedSnapshotKeepsItsRevisionWithoutReplacingANewerPublication通过，说明该探测的首个失败为隔离安装目录准备缺失，不能据原失败认定发布并发机制坏了。
- TerminalLive.SlowSettingsWorkDoesNotBlockAccountControlsOrPublication进一步报“choose the trading account environment”，说明夹具尚未跟随新环境字段，不据此认定并发或账户控制缺陷。

这两项没有被本批改成生产宽松初始化，也没有记为全部通过。旧CI还包含TableRefresh.ConcurrentFixedSelectionsCannotOverwriteAnotherCorrection的状态/更正竞争失败，以及SSH相关入口失败，尚未完成根因验证。应在新隔离契约下分组修正测试准备并核对真实业务错误，不延长超时掩盖问题、不立刻反复跑全集。

## 下一优先项

1. 先修复Terminal/SSH测试的安装目录准备与明确柜台环境夹具，沿生产初始化边界验证；单独复核分区更正竞争。补齐当前开发可用验收基线。
2. 随后按目标架构实施顺序1，做Data接管一条采集/更新任务、Research保留计算的纵向切换：必须同时处理稳定服务作用域、结果/部分发布、取消与重启、引用及删除保护、现有配对消费者和打包，不能只改服务名或建立空目录。
3. 七模块导航、五类服务以及新评审中的实盘政策尚未交付；不以本次维护修复替代这些目标。涉及未决真实执行归属/监控政策时先单独收敛，不启用实盘。

本轮没有真实权限阻塞或未确定的变更结果；剩余项为明确未修复/未执行，代码与证据保留供继续开发。

证据目录：`build/reviews/maintenance-preflight-2026-10-21/`。主要文件为ui-before.log、ui-tests.log、lint-before.log、lint.log、frontend-build.log、format-check.log、native-build.log、native-tests.log、installation-tests.log、publication-tests.log、orchestration.log、terminal-probes.log、terminal-isolated-probes.log、latest-ci-status.json、previous-ci-failures.log。
