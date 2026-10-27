# 项目全面评估与重构计划

日期：2026-10-25。评审基线：`b7e68b5e`（`main`，工作树干净）。

状态：**评估已完成；第 9 节的重构计划待使用者确认第 8 节的决定后按工作包执行。**
本文不是功能验收声明。需求正文仍是目标行为的权威入口；本文只记录本次评估的事实、判断和
可执行的整改顺序。上一轮[需求与架构评估](requirements-architecture-assessment-2026-10-21.md)
（A01—A15）偏重需求与目标契约；本文补上代码、工程、UI、缺陷和交付过程，并把两者合成一个执行顺序。

## 0. 结论摘要

**安全原则和领域判断是对的，但项目在用远超当前交付能力的速度扩张；当前最需要的是止血、收缩范围、
删减重复机制，而不是继续增加功能。**

1. **主干约 4 周没有可信基线。** GitHub CI 最后一次通过是 2026-09-28（`d378705e`）；之后最近 63 次运行
   中 51 次失败、11 次取消。HEAD 的提交说明写“1061 项 CTest 通过”，同一提交的 CI 有
   Linux 6 项、macOS 18 项、sanitizer 数十项失败，格式检查也失败。按 macOS 13.0 部署目标构建时，HEAD 在本机编译失败。
2. **投入严重偏向证据和基础设施，研究能力本身很小。** 生产 C++ 约 10.6 万行，其中策略、因子、
   模型、撮合与绩效算法合计约 6,500 行（约 6%）。“研究模型”目前是单特征线性/岭回归；
   策略是 4 条 CTA 规则加 3 条截面排序。其余代码主要是固定输入、摘要、谱系、保管、退役、冷备、部署和远程维护。
3. **需求规模与使用者不匹配，且没有首个交付切片。** 一位个人使用者面对 304 条 FR、58 项 UI、42 条旅程和
   1002 条验收；需求明确“优先级不是排期、不自动删减范围”。实现于是横向铺开：导航 23 个子模块中有 7 个为空，
   已有的功能则大多停在“能提交、能保存证据”，还没有形成一条可日常使用的研究到交易闭环。
4. **几处结构性问题在持续制造缺陷。** `ResearchHost` 单文件约 5,000 行，靠 `has_*` 链和字符串分派；
   14 种任务类型的 if/else 分散在至少 6 处；共享的 `task-runtime` 并不通用，它认识 Data 和 Research 的全部
   输入类型，还把 SQLite 连接借给 6 个研究类；`asterion_protocol` 同时承担校验、领域计算和界面 JSON
   （151 个返回 JSON 的函数、46 个从 JSON 编码的函数，全部手写），前端又手写一遍 224 个导出类型。
5. **违反仓库自己规则的地方。** 每次读取任务结果都重新哈希结果和输入、并重跑完整语义校验，与
   “Hash and Integrity Checks”冲突；Research 每次元数据请求都向 Agent 重新发现自己的 node ID，
   使普通读请求依赖 Agent 存活；Terminal 用本机是否存在 `journal.sqlite` 判断交易记录，越过了 Trading 的所有权。
6. **文档成了工作日志。** Markdown 约 6.6 万行，其中 `audit-progress.md` 9,185 行、评审记录 3.5 万行，
   `status.md` 已变成更新日志；派生的 `requirements.html`/`prototype.html` 共 6.2 万行进了 Git。
   文档量已经超过人能维护和核对的范围，新旧表述互相矛盾（见上一轮 A01）。

应保留的长处：交易链路“先记录后发送、许可门、断线不重发、授权与风控必经”实现得很严谨；`Decimal`、
持久写入、Electron 隔离（`contextIsolation`/`sandbox`/CSP）都扎实；回测 bar N 决策、bar N+1 开盘成交的时序正确；
状态单一所有者、Manager/Agent 不转发业务流量的原则清楚；前端类型检查、lint 和 243 项单元测试全部通过。

## 1. 范围、方法与证据

| 项目 | 内容 |
| --- | --- |
| 阅读 | `AGENTS.md`、README、`docs/architecture.md`、目标架构、上一轮评估、需求总纲/产品/交付规格、`terminal.md`、`status.md`、整改进度尾部 |
| 代码抽查 | core（Decimal、durable_file、PolledTask、broker_send_gate）、Research/Data/Intelligence/Trading 服务、task-runtime、protocol、Agent/Manager、Terminal native、bridge、导航与主要 React 组件、Electron 主进程 |
| CI | `gh run` 最近 63 次记录；HEAD 运行 `40074205360` 各作业的失败日志 |
| 本机执行 | `conan install` + `cmake --build`（`build/Debug`，macOS 13.0 目标）；scratchpad 内另建 macOS 26.0 目标的完整构建并运行 CTest；`tsc`、`pnpm lint`、`pnpm test:unit`、`pnpm format:check` |
| 未执行 | E2E、真实柜台、真实数据源、安装包、远程 SSH、Linux 本机、sanitizer 本机复现；没有启动桌面窗口，UI 结论来自代码、设计稿和规格 |

本机结果（macOS，Apple Clang 21）：

| 检查 | 结果 |
| --- | --- |
| `build/Debug`（部署目标 13.0） | **失败**：`feature_expression.cpp:175` 浮点 `std::from_chars` 需 macOS 26；`program_files_test.cpp` 浮点 `std::format`/`to_chars` 需 macOS 13.3 |
| scratchpad 构建（部署目标 26.0） | 通过 |
| CTest（scratchpad，`-j 6`） | 1059 项中 1027 通过、32 失败，归因见 [附录 A](#附录-a本机-ctest-结果) |
| `tsc --noEmit` / `pnpm lint` | 通过 |
| `pnpm test:unit` | 82 个文件、243 项通过 |
| `pnpm format:check` | **失败**：4 个 `.proto` 文件（`research_statistics.proto` 108 处） |

## 2. 项目画像

| 指标 | 数值 |
| --- | --- |
| 项目时长 | 2026-09-17 首次提交至今约 5.5 周，228 次提交 |
| 提交粒度 | 平均每次 52 个文件；21 次超过 100 个文件；HEAD 为 328 个文件、+15,133/−7,689 |
| C++ 生产代码 | 106,045 行（其中算法类约 6,500 行） |
| C++ 测试 | 60,322 行，约 1,060 个 CTest 用例；CI core 作业约 43 分钟 |
| TypeScript 生产 / 单元测试 / E2E | 45,494 / 11,904 / 17,118 行 |
| CSS / 语言包 JSON | 6,777 / 13,218 行（诊断文案两种语言各 2,323 行） |
| Markdown | 需求 25,714、评审 35,070、主题文档 5,233 行；另有派生 HTML 61,802 行 |
| 最大文件 | `research_host.cpp` 4,987；`task_store.cpp` 3,651；`bridge/client.ts` 3,419；`task_execution_test.cpp` 9,420 |

## 3. 需求文档评价

**优点。** 时间语义（事件、发布、取得、可得时点分开）、连续合约只做信号、真实合约执行、完整 bar 后决策、
训练/验证/留出分离、授权与风控不可绕过、未知结果不重发，这些规则正确且有算例。上一轮评估第 3.1 节列出的优点成立，不再重复。

**问题。**

| 编号 | 问题 | 影响 |
| --- | --- | --- |
| R1 | 规模远超单人产品：304 FR / 58 UI / 42 JRN / 1002 AC，单页最长 2,016 行 | 需求无法被逐条核对，实现只能横向铺开；“覆盖率”替代了“能用” |
| R2 | 没有首个交付切片：`product.md` 声明优先级不是排期，`delivery.md` 声明不自动删减范围 | 每个模块都开了头、没有一个闭环完成；上一轮 A14 已指出，至今未采纳 |
| R3 | 规格大量使用实现层概念（固定输入、内容身份、保管、退役、谱系），并直接写进 UI 字段 | 用户需要理解系统内部模型才能操作；UI 文本密度极高（见第 6 节） |
| R4 | 派生文件 `requirements.html`、`prototype.html` 入库 | 每次改需求都产生数万行 diff，评审噪声大 |
| R5 | 需求、现行契约和实施状态混写（上一轮 A01，未处理） | 同一事实有多个版本，后续执行者无法判断哪一个算数 |
| R6 | 目标设计图是线框（字段名加“待填写”），没有真实视觉稿 | “视觉风格”只能靠 `terminal.md` 的文字约束，无法验收 |

**建议。** 不重写需求，增加一份很短的**发布切片文件**（决定 D1），把首个可用版本的支持集合、
冻结项和完成判据写死；其余需求保持“目标”身份。需求正文删除实施状态。派生 HTML 改为按需生成、不入库（决定 D4）。

## 4. 架构设计评价

**方向正确的部分**：按状态所有权划分进程；Trading 每账户独立进程；Manager/Agent 只管生命周期、不转发业务；
任务在所属服务内执行、不设中央调度；插件是可信进程内代码而非沙箱。这些决定应保留。

**问题。**

| 编号 | 问题 | 证据 | 判断 |
| --- | --- | --- | --- |
| H1 | 部署拓扑对单人桌面产品过重 | 5 类业务服务 + Manager + Agent + 安装器 + SSH 远程维护 + mTLS + 冷备 | 远程节点、远程升级与联合卸载在本机闭环完成前不应继续扩展；属于范围决定（D3） |
| H2 | Research 是“万能服务” | `research_host.cpp` 处理项目、变量、模型、逻辑、策略、批次、评估、样本、统计、账户复核、公告复核、事件集复核、保管核对等 | 进程边界可以保留，但进程内必须按模块拆分（P2.3） |
| H3 | `task-runtime` 名为公共机制，实为混合领域存储 | `task_store.hpp` 为 13 种输入各有 `submission()` 重载，`friend` 6 个研究类，`research_metadata()` 借出数据库连接 | 违反“公共机制不带业务”；新增任务类型要改公共库（P2.1、P2.2） |
| H4 | `asterion_protocol` 是“万能库” | 15,752 行；含校验、`factor_observations` 等领域计算、`decode_*`/`encode_*` 界面 JSON | 领域计算应在 domain/插件，展示 JSON 应在 Terminal（P2.7） |
| H5 | 界面契约写了三遍 | proto → C++ 手写 JSON（约 197 个函数）→ TS 手写类型（`client.ts` 224 个导出） | 每次字段变更改三处，是界面缺陷的主要来源之一（P3.1） |
| H6 | Research 每次元数据请求都向 Agent 发现自己的 node ID | `data_.node_id(...)` 共 21 处；Data 与 Intelligence 从配置读取 | Agent 故障会让 Research 读请求失败，与“Agent 故障不影响业务”冲突；也增加延迟（P0.5） |
| H7 | 元数据读写全部串行在一条日志线程上 | 单个 `metadata_work_` 槽；`wait_for_store` 让所有读等待任何未确认写；写入使用 `F_FULLFSYNC` | 读请求排在 fsync 后面，负载下易超时（P2.4） |
| H8 | 在 Asio 之上自研轮询式协程 | `PolledTask` 每轮评估 `ready()` 谓词；服务端 `RpcHost` 无条件每 10 ms 唤醒一次（`rpc_host.cpp:598`），客户端 `ServiceIo` 有活动工作时每 2 ms 唤醒（`service_io.cpp:109`） | 能工作，但空闲也在唤醒、调试困难；短期不重写，只去掉不必要的定时轮询（P2.12） |
| H9 | 命名三套并存 | 目录 `market-data/`、`data-service/`、`research-service/`；程序 `asterion_market/data/research`；枚举 `MARKET_DATA`、`DATA_SERVICE`、`LIVE_TRADING`；`ResearchHost` 在 `asterion::tasks`；研究线协议复用 `task::proto` | 违反 AGENTS 的“稳定职责命名”（P2.8） |
| H10 | 交易时段是无生效日期的静态表 | `config/futures-sessions.json` 只有一份“2024 年 8 月”汇总 | 跨年回测会用今天的夜盘时段解释历史；属于数据模型缺口，进入 R1 数据实证（P4） |

## 5. 代码实现评价

**扎实的部分。**
- `Decimal`：64 位定点、显式舍入、溢出检查，乘除不经浮点也不用 128 位整数（`core/src/foundation/decimal.cpp`）。
- 交易发送：重复订单号拒绝 → 授权与持仓核对 → 等报价后重新核对 → 风控 → 持久记录意图 → 门禁修订号校验 →
  一次性许可 → SDK 调用；任何一步失败都先持久写 `OrderNotSent`（`apps/services/trading/live_session.cpp:827` 起）。
- 持久写：独占临时文件、fsync/`F_FULLFSYNC`、rename、同步目录（`core/src/kernel/durable_file.cpp`）。
- 回测时序：bar N 下单、bar N+1 成交，未成交余量在下次决策前失效（`backtest/engine.cpp:280`）。

**需要整改的部分。**

| 编号 | 位置 | 问题 |
| --- | --- | --- |
| C1 | `research_host.cpp:4595` `handle()` | 同一条请求连续三次 `call.admitted()`；对同一个 `storage_->describe(request.retry().id())` 调用 12 次；`resources.kind` 用字符串分派；`has_*` 长链判断哪些请求要占用准备名额 |
| C2 | `research_host.cpp` `respond()` | 任何非 `Error`/`invalid_argument` 的 `std::exception` 都把整个服务置为 `degraded_`，且从不复位；`std::out_of_range` 一律报告“unknown task”，会把编程错误伪装成“找不到” |
| C3 | `task_store.cpp:1628` `ResultRead::verify()` | 每次读结果：重新哈希结果文件、重读并哈希输入、按 14 种类型重跑完整语义校验。同一所有者写入、没有中间信任边界 |
| C4 | `task_store.cpp`、`research_host.cpp`、`protocol/src/task*.cpp` | 14 种任务类型的分派重复出现在 `verify`、`load_for_claim`、`confirm_attempt`、`submission` 重载、重试判定、准备名额判定、提交分派中 |
| C5 | `task_store.hpp:37`、`protocol/task.hpp:9-13` | 注释仍描述已删除的 worker 租约与续租 |
| C6 | `node_storage_identity.cpp:40`、`node_identity.cpp:105` 等 5 处 | 身份/配置首次创建使用截断写 `write_file_durably`，崩溃会留下半个文件并阻止启动；应改用 `replace_file_durably` |
| C7 | `commands_connections.cpp:47,182` | Terminal 以本机 `journal.sqlite` 是否存在判断“已有交易记录”。账户绑定远程 Trading 实例时判断错误，并可能放行本应拒绝的柜台资料修改 |
| C8 | `apps/clients/terminal/CMakeLists.txt:30` | Terminal 链接 `asterion_task_store`、`asterion_sqlite`，源码中未见使用；应确认后删除 |
| C9 | core、plugins、apps 生产代码 | `catch (...)` 109 处、`catch (const std::exception&)` 78 处；多数用于回滚后重抛，但需逐一确认没有吞掉编程错误 |
| C10 | `node-agent/main.cpp:1201` | 服务 socket 放在 `/tmp/ast-*`。macOS 的周期清理可能删除长期未访问的 `/tmp` 条目，需实测确认常驻服务不受影响 |

## 6. UI 评价

本次没有启动桌面窗口（开发桥会操作真实本机服务），以下判断来自源码、设计稿和 `terminal.md`。

**做得好的**：深色、细线、单一强调色的令牌化样式（`tokens.css`、`style.css` 统一定义控件）；中英文资源齐全并有缺键测试；
空状态写明原因；未实现的子模块显示“尚未实现”，没有冒充功能；Electron 窗口加固完整。

**问题。**

| 编号 | 问题 | 证据 |
| --- | --- | --- |
| U1 | 认知负担过高：界面直接暴露内部概念 | `terminal.md` 的“策略回测”一段是一句约 600 字的规则；表单要求用户理解固定输入、服务位置、来源证据、身份摘要 |
| U2 | 导航承诺大于实现 | 23 个子模块中 7 个没有业务视图（数据加工与质量、信息提取、组合与仓位、交易与账户模拟、系统的 3 个分区）；“行情/自选/合约”放在“实盘 → 准备与运行”下，日常看盘要进实盘模块 |
| U3 | 文案与实现不一致 | `App.tsx:396` 显示“资料备份、恢复和诊断导出尚未实现”，但设置窗口已有 `backup` 页（`settings/DataBackup.tsx`，HEAD 新增） |
| U4 | 组件过大 | `Backtest.tsx` 1,269 行、`LivePanel.tsx` 1,246 行（14 个 `useState`）、`NoticeSubscriptions.tsx` 828 行、`FactorForm.tsx` 815 行 |
| U5 | 桥接层是单文件 | `bridge/client.ts` 3,419 行，类型、契约校验、合并逻辑、导出、轮询放在一起 |
| U6 | 全量快照轮询 | 每个命令返回完整 `Snapshot`，界面按修订号轮询 `runtime.snapshot`；只有行情做了增量。随着模块增加，快照会持续变大 |
| U7 | 设计稿无法用于视觉验收 | 58 项设计都是线框，真实界面从未与设计做过像素级或结构化对照 |

**建议**：先把 R1 涉及的页面做实（数据、回测、评估、交易），其他模块入口保留“未实现”提示；
把行情、自选、合约提升为一级入口或放到“数据”下（需决定，D6）；页面文案去掉内部术语，内部身份放到“详情”折叠区。

## 7. 缺陷清单

“已确认”指本次有日志或源码证据；“推断”指有证据但尚未复现根因。

| 编号 | 状态 | 缺陷 | 证据 | 修复方向 |
| --- | --- | --- | --- | --- |
| B1 | 已确认 | 4 个 `.proto` 未格式化，CI style 失败 | `pnpm format:check` | `pnpm run format` |
| B2 | 已确认 | 测试仍引用已改名程序 `asterion_market_data` | `tests/market/remote_market.py:16`、`tests/acceptance/node_service_control.py:49`；CI Linux `remote_market` 失败 | 改为 `asterion_market`，同步 `architecture-design-review.md:460` |
| B3 | 已确认 | macOS 13.0 部署目标下编译失败 | 本机 `build/Debug` 两处 FAILED；`scripts/desktop/desktop.py:334` 固定 13.0，CI 未设置部署目标 | 决定 D2 后：提高目标，或去掉浮点 `from_chars`/`std::format`；CI 设置与产品相同的部署目标 |
| B4 | 推断 | Linux 4 项 Research 进程测试“IPC timeout”，1 项冷备超时 | CI `40074205360`；测试客户端超时 2 s，每次元数据请求先往返 Agent（H6），再排队等日志线程（H7） | 先做 P0.5，复测；仍失败再查 H7 |
| B5 | 已确认 | sanitizer 作业：插桩后 Research 程序超过 Agent 制品上限 512 MiB，20 余项失败 | ASan 日志 “artifact must be a regular file within the Agent size limit” | 测试改用小型固定程序，或 sanitizer 构建减少调试信息；不提高生产上限 |
| B6 | 已确认 | macOS 测试的 Unix socket 路径超过 104 字节 | CI macOS 7 项 “invalid local socket path”/“invalid test IPC endpoint” | 测试支持库统一用短临时根（如 `/tmp/ast-t-XXXX`） |
| B7 | 待查 | macOS TSan：`TaskProcess.ServiceOwnsConcurrency…` 123 s 失败、`TableRefresh.ADelayed…` 进程中止；macOS `CtpEvents.FullCatalog…` 10 s 截止超时 | CI 日志 | B4—B6 修完后单独复现，可能是真实竞态 |
| B8 | 已确认 | 系统 → 资料与诊断的提示与实际功能不符 | `App.tsx:396` 与 `settings/DataBackup.tsx` | 改为指向设置中的备份页，诊断导出仍标未实现 |
| B9 | 已确认 | 远程交易账户的“已有记录”判断错误 | C7 | 由 Trading 实例回答记录是否存在 |
| B10 | 风险 | 单个请求的意外异常会让 Research 整体进入恢复态直到重启 | C2 | 仅持久化失败降级；其他异常返回内部错误并记录 |

## 8. 需要使用者确认的决定

计划中依赖这些决定的工作包已标注。未决前，执行者只做第 0 阶段。

| 编号 | 决定 | 建议 |
| --- | --- | --- |
| D1 | 首个发布切片 R1 的范围 | 日线与 1 分钟数据（Tushare + CTP 行情）→ 现有规则策略 → 回测 → 绩效与风险评估 → 本地实时模拟 → CTP 仿真柜台。情报、模型训练、批次评估、研究项目/变量/样本/统计在 R1 中冻结：保留现有测试，不再加功能 |
| D2 | macOS 最低版本 | 若只服务自己的机器，直接定为 macOS 26（与已要求的 Xcode 26 一致），删除 13.0 相关脚本参数；若要支持旧系统，保持 13.x 并移除浮点 `from_chars`/`std::format` |
| D3 | 远程节点与远程维护 | R1 期间冻结远程部署、远程升级、联合卸载的新功能，现有能力保持测试通过；R1 完成后再按实际需要评估是否保留 |
| D4 | 文档治理 | 派生 HTML 不入库；`audit-progress.md` 归档，新进度文件只保留未完成项；`status.md` 只保留能力表 |
| D5 | 界面契约生成 | 采用 proto → TS 生成（如 `@bufbuild/protobuf`），Native 用 Protobuf 标准 JSON 映射传领域对象，逐模块删除手写 decode/encode |
| D6 | 行情入口位置 | 行情、自选、合约作为日常入口，不放在“实盘 → 准备与运行”下 |
| D7 | 冻结功能的去留 | R1 验收后逐项评估使用情况，没有实际使用的直接删除（含协议、测试、文档），不长期保留 |

## 9. 重构与改造计划

### 9.1 执行规则（每个工作包都适用）

1. 工作包 ID 即提交主题前缀（如 `P0.3`）。一个提交只做一个工作包，文件数尽量少于 30；需要跨模块重命名时单独提交。
2. **完成以 GitHub CI 全绿为准**（Linux、macOS、sanitizer、style）。本机通过不算完成。CI 红时只允许提交修复 CI 的改动。
3. 修改服务源码时不要同时运行 CTest（会触发 `remote_resources` 指纹不一致）；长测试放后台运行。
4. 每个工作包先写清：要删除什么、保留什么、哪项测试证明行为。删除优先于新增。
5. 同一组件连续两次修正仍不收敛时，按 AGENTS 的“停止并重新设计”处理，不继续打补丁。
6. 工作包结束时：更新本文第 10 节的状态表（一行），不新增按日期的长篇进度记录。

### 9.2 第 0 阶段：恢复可信基线（不依赖决定，立即执行）

| 包 | 内容 | 涉及文件 | 完成判据 |
| --- | --- | --- | --- |
| P0.1 | 格式化 4 个 proto 文件 | `protocol/proto/asterion/{research_statistics,trading,research_evaluation,account_review}.proto` | `pnpm format:check` 通过 |
| P0.2 | 修正已改名程序的引用 | `tests/market/remote_market.py`、`tests/acceptance/node_service_control.py`、`docs/reviews/architecture-design-review.md` | `git grep asterion_market_data` 无结果；`remote_market` 通过 |
| P0.3 | 按 D2 处理 macOS 部署目标；CI 的 macOS 作业设置与产品相同的 `MACOSX_DEPLOYMENT_TARGET` | `scripts/desktop/desktop.py`、`scripts/desktop/analytics_runtime.py`、`.github/workflows/ci.yml`，或 `feature_expression.cpp`、`program_files_test.cpp` | `pnpm desktop:check` 在 macOS 通过；CI 能复现本次失败（先验证 CI 改动会变红） |
| P0.4 | 测试临时目录与 socket 路径统一走短路径 | `tests/support/` 新增一个函数，替换各测试自建路径 | macOS 上 7 项 socket 失败消失 |
| P0.5 | Research 启动时取得 node ID（与 Data 一致，由 Agent 启动参数或启动时一次发现提供），删除请求路径上的 21 次发现 | `research-service/main.cpp`、`research_host.{hpp,cpp}`、`research_data.{hpp,cpp}`、Agent 启动参数 | Linux 4 项 `TaskProcess` 超时消失；新增一项测试：Agent 离线时 Research 读请求仍成功 |
| P0.6 | sanitizer 测试不再上传完整 Research 程序 | 相关测试夹具，或 sanitizer 构建选项 | ASan/UBSan、TSan 作业中“size limit”失败消失；生产上限不变 |
| P0.7 | 复现并修复 B7 | 视复现结果 | CI 全绿；若为真实竞态，补一项回归测试 |
| P0.8 | 修正 B8 文案 | `apps/clients/terminal/src/App.tsx`、语言包 | 组件测试覆盖“资料与诊断”分区内容 |

**阶段出口**：同一提交上 CI 全部作业通过，并在第 10 节记录运行编号。

### 9.3 第 1 阶段：范围与文档收敛（依赖 D1、D3、D4）

| 包 | 内容 | 完成判据 |
| --- | --- | --- |
| P1.1 | 新增 `docs/requirements/release-r1.md`：支持集合（品种、周期、数据源、策略、页面）、冻结项、外部条件、完成判据；`product.md`/`delivery.md` 只加链接 | 文件 ≤200 行；每个 R1 页面能对应到 UI 编号 |
| P1.2 | `audit-progress.md` 原样移到 `docs/reviews/archive/`；新建只含未完成项的进度表 | 新进度表 ≤200 行；AGENTS 中的路径同步更新 |
| P1.3 | `status.md` 只保留能力与限制表，日期条目移入归档 | 无按日期的段落 |
| P1.4 | 派生 HTML 不入库：加入 `.gitignore`，`docs:requirements:check` 在 CI 运行 | 仓库中无 `requirements.html`、`prototype.html`；CI 检查需求链接 |
| P1.5 | 清理上一轮 A01 列出的旧表述，以及 `architecture.md:168` 的“任务服务账本只读打开” | 主题文档中不再出现已删除机制的现行描述 |
| P1.6 | 25 份 `docs/research-*.md` 合并为按代码模块组织的少数文档 | 每份合并文档对应一个源码目录 |

### 9.4 第 2 阶段：后端结构重构（第 0 阶段完成后执行）

按顺序执行，每步独立可提交、CI 全绿。

**P2.1 任务类型描述表。** 在 `task-runtime` 中定义与业务无关的 `TaskKind` 描述（类型枚举、输入/结果消息
的读写函数、提交校验、结果校验、是否允许重试）。Data 和 Research 在各自服务中注册自己的类型。
`Store` 只保存类型、不透明载荷与摘要，删除 13 个 `submission()` 重载和 `verify`/`load_for_claim`/
`confirm_attempt` 中的类型链。
完成判据：新增一种任务类型只需修改所属服务的一个文件；`task_store.cpp` 不再 include 任何 research/data 业务头文件。

**P2.2 研究元数据自有存储。** `research::Projects/Variables/Strategies/Logic/Batches/Models` 自己持有连接（可以是同一 SQLite 文件），
删除 `Store` 的 6 个 `friend` 和 `research_metadata()`。完成判据：`task_store.hpp` 不出现 `research::`。

**P2.3 拆分 `ResearchHost`。** 保留 `ResearchHost` 负责 RPC 通道、接纳、升级静默和生命周期；按模块拆出
`ProjectService`、`LogicService`、`ModelService`、`StrategyService`、`BatchService`、`ResearchTasks`（提交/重试/取消/结果）、`NoticeCustody`。
路由用 `request.operation_case()` 的 `switch`，删除 `resources.kind` 字符串分派和重复的 `has_*` 链；
去掉三连 `admitted()` 与重复 `describe()`。完成判据：每个文件 ≤800 行；`handle()` ≤80 行。

**P2.4 读写分离。** SQLite 使用 WAL，查询在文件池用只读连接执行；日志线程只执行写入。删除 `metadata_work_` 单槽和
`wait_for_store` 对读请求的全局等待。完成判据：一项测试证明慢写入期间元数据查询在 1 s 内返回。

**P2.5 结果只在写入时校验。** 完成任务时由所有者校验并计算摘要（保留，作为固定身份）；普通读取直接返回存储内容，
不再重新哈希、不重跑语义校验；只在冷备恢复这一信任边界重新校验。同步删除对应测试与文档，更新
[哈希清理记录](hash-cleanup-2026-10-19.md)。完成判据：`ResultRead::verify()` 删除或只剩反序列化。

**P2.6 异常分级。** 只有持久化失败让服务进入恢复态；其他意外异常返回 `internal` 错误并记录堆栈，Debug 构建可直接中止；
删除 `std::out_of_range → not_found` 的映射，找不到对象用显式 `Error(not_found)`。逐个检查 `catch (...)`，
只保留“回滚后重抛”和线程边界转交两种用法。

**P2.7 拆分 `asterion_protocol`。** 保留生成代码与边界校验；`factor_observations` 等领域计算移到
`plugins/tools/factor_analysis` 或 Research；界面 JSON 的 `decode_*`/`encode_*` 移到 Terminal native。
完成判据：服务进程不再链接任何界面 JSON 代码。

**P2.8 命名统一。** 目录改为 `apps/services/{market,data,research,intelligence,trading}`；`ServiceKind` 改为
`MARKET/DATA/RESEARCH/INTELLIGENCE/TRADING`（**保持枚举编号不变**；Agent 部署状态按整数保存
`kind`，见 `node-agent/service_configuration.cpp:17,71`，因此改名不改写用户数据；执行前再查一遍其他持久化位置）；`ResearchHost` 移入 `asterion::research`；研究线消息不再放在 `task::proto`。
同步 CMake、打包、脚本、测试与文档。

**P2.9 删除旧注释与无用依赖。** C5 的 worker/租约注释；C8 的 Terminal 链接项。

**P2.10 身份与配置文件原子写。** C6 的 5 处改为 `replace_file_durably`。

**P2.11 交易记录由 Trading 回答。** 删除 C7 的本机文件判断，改为向账户绑定的 Trading 实例查询；实例离线时明确报告“无法确认”并拒绝修改柜台资料。

**P2.12 去掉不必要的定时轮询。** 确认所有等待都由完成回调唤醒后，删除 `service_io.cpp:109` 的 2 ms 定时，
把 `rpc_host.cpp:598` 的固定 10 ms 节拍改为按最近截止时间设置；保留截止时间检查所需的最小定时。
完成判据：空闲服务 CPU 占用接近 0（用 `top` 采样记录）。

### 9.5 第 3 阶段：界面契约与前端（依赖 D5、D6）

| 包 | 内容 | 完成判据 |
| --- | --- | --- |
| P3.1 | 在一个模块（建议因子结果）试点 proto → TS 生成，Native 用标准 JSON 映射；删除该模块的手写 decode 与 TS 类型 | 该模块的 C++ JSON 函数和 TS 手写类型删除；E2E 对应用例通过 |
| P3.2 | 试点通过后逐模块推广；`bridge/client.ts` 按领域拆成多个文件，与插件同目录 | `client.ts` 只剩快照外壳与调用函数，≤500 行 |
| P3.3 | 拆分超过 600 行的组件：状态放进自定义 hook，表单步骤拆为子组件 | 无组件超过 600 行；单元测试仍通过 |
| P3.4 | 按 D6 调整导航；R1 页面去掉内部术语，内部身份放入“详情”折叠区 | 使用者按 R1 旅程走一遍，记录步骤数与卡点 |
| P3.5 | 用 E2E 录制给 R1 每个页面截一张图，放入 `docs/reviews/assets/`，作为视觉基线 | 截图与 `terminal.md` 的规则逐条对照 |

### 9.6 第 4 阶段：R1 纵向闭环（依赖 D1）

1. **数据实证**：按 GATE-DATA 核对首批品种与区间的行情、合约规则、费用、保证金和交易时段（含 H10 的时段生效日期）。
2. **研究闭环**：固定数据 → 规则策略 → 回测 → 绩效/风险评估，结果可从界面逐步核对。
3. **模拟闭环**：同一策略进入本地实时模拟，再进入 CTP 仿真柜台；跨一个交易日运行，覆盖暂停、恢复、未知订单核对。
4. **验收**：按 `delivery.md` 的验收模板记录。

### 9.7 第 5 阶段：真实资金前的安全门

真实账户启用前必须完成上一轮评估的 A02（跨节点唯一执行）、A03（监控失联）、A07（部分成交归属）、A08（风险基准与现金流）。
不在 R1 内启用真实资金交易。

### 9.8 测试体系调整（与第 0、2 阶段并行）

- CTest 加标签：`unit`（纯内存、<1 s）、`process`（启动服务进程）、`acceptance`。日常本机只跑 `unit` + 受影响的 `process`。
- 拆分超大测试文件：`task_execution_test.cpp`（9,420 行）按任务类型拆；与 P2.1 同步。
- 第 2 阶段删除机制时同步删除只验证该机制的测试（如读取时重新校验的测试），不保留。
- 测试名改为短名称加注释说明场景；现有句子式长名称在拆分文件时顺带缩短。

## 10. 执行状态

| 包 | 状态 | 提交 | CI 运行 |
| --- | --- | --- | --- |
| P0.1 | 本机完成：4 个 proto 已格式化 | — | — |
| P0.2 | 本机完成：两处测试与目标设计文档改为 `asterion_market` | — | — |
| P0.3 | 本机完成：D2 未决，保持 13.0。`feature_expression.cpp` 改为经典 locale 流解析数字并新增常量测试；3 处测试比较文件时间改为比较计数；CI 设置 `MACOSX_DEPLOYMENT_TARGET=13.0`。`build/Debug`（13.0）完整构建通过 | — | — |
| P0.4 | 本机完成：`tests/CMakeLists.txt` 在 macOS 上为全部测试设置 `TMPDIR=/private/tmp`（用真实路径，持久写入拒绝 `/tmp` 符号链接），不逐个改 75 处夹具；`terminal_minute_query_test.cpp` 的夹具不再硬编码 `/tmp` | — | — |
| P0.5 | 本机完成：Agent 传 `--node-id`，Research 删除 21 处自我发现与 `ResearchData::node_id`；新增“Agent 发现离线时元数据请求仍成功”测试 | — | — |
| P0.6 | 本机完成：仅 sanitizer 构建的制品上限提高到 2 GiB（插桩构建不发布）；发布与 Debug 上限不变。Linux 插桩程序实际大小未在本机测量 | — | — |
| P0.7 | 部分：本机 macOS 只剩 `market_observation_archive`、`task_agent_recovery`（CI macOS 同样失败，改动前已失败）。线索：Agent 以正确参数启动 Research/Data，进程存活，但 Research 未写日志，Terminal 报 “RPC connection failed”。TSan 与 Linux 结果待 CI | — | — |
| P0.8 | 本机完成：资料与诊断分区改为链接设置中的“数据备份与恢复”，只保留“诊断导出尚未实现”。App 没有组件测试夹具，未为一处文案新建测试 | — | — |
| P2.9 | 本机完成：Terminal 删除未使用的 `asterion_task_store`、`asterion_sqlite` 链接；改写描述已删除 worker 租约的注释（task_store、protocol/task.hpp、data_store） | — | — |
| P2.10 | 本机完成：节点身份文件与证书/私钥改为原子替换写，重新安装判断依据的 `client.key` 最后写入。`service.pending` 机制保留：它是显式的“提交失败需人工恢复”状态，测试以它注入冻结行为 | — | — |
| P2.6 | 本机完成：`Store` 找不到任务时抛 `Error(not_found)`，新增 `find_task()`；Research 21 处、Data 2 处以 `out_of_range` 做控制流的代码改为显式查询；`respond()` 删除 `out_of_range → unknown task` 映射，意外异常返回 `internal_error`，不再让整个服务进入恢复态（持久化失败仍在原处降级） | — | — |
| P2.11 | 本机完成：“已开通交易”以 Terminal 账户记录中的执行绑定为准；删除命令层按本机 `journal.sqlite` 的重复检查（`CtpConnections::save` 已在文件锁内强制同一规则） | — | — |
| P2.5 | 本机完成：普通启动不再哈希每个任务的输入与结果，普通读取不再哈希、不再重跑结果语义校验；完成任务时的校验与冷备检查保留。删除 2 项只验证读取/启动重校验的测试并收窄 2 项 | — | — |
| P2.3 | 本机完成：`handle()` 按 `operation_case()`/`definition_case()` 分派，重试检查合并为一次 `describe`，去掉三连 `admitted()`；4,972 行的 `research_host.cpp` 按模块拆为 7 个文件（最大 957 行）与内部头文件 `research_host_internal.hpp`。改为多个类的拆分未做（成员共享同一状态拥有者，收益不足以抵消改动） |
| P2.2 | 完成：`task-runtime` 改为 `apps/services/common/tasks` 下的纯机制库。新增 `TaskDomain` 接口，Data 与 Research 各自实现（`data_tasks.cpp`、`research_tasks.cpp`）：任务种类表、定义校验与列表摘要、清单附加字段、接纳与派生规则、执行描述、风控捕获、结果校验、历史引用。Research 资源目录与项目任务查询、Data 快照占用与下载发布证据移回服务；`task_events` 从 sqlite 插件移入任务库；Research 元数据经 `Store::database()` 访问，删除友元类。持久格式（清单字段、索引名）不变 |
| P2.12 | 评估后不做：10 ms 节拍还承担宿主存活、停止信号、会话超时与服务内务的兜底推进；线程池完成时已主动唤醒。每秒 100 次唤醒成本很小，改为按截止时间唤醒需梳理所有不主动通知的工作，风险大于收益 |
| 测试 | 默认单测超时 30 → 60 秒：冷备等进程测试单独运行 3—12 秒，并行负载下超过 30 秒导致误报 |
| P2.7 | 暂缓：服务端也把这些 JSON 映射用作持久格式（任务清单、交易日志），不能直接拆成“只给界面用”的库；随 D5（界面契约生成）一起重新设计 |
| P3.3 | 完成：`LivePanel.tsx` 拆为 `LivePanel`/`CreateLive`/`LiveAccount`；`LiveAccount` 再拆出 `OrderTicket`、`AccountRecords`（399 行）；`Backtest.tsx`（1,258 行单组件）拆为结果与进度视图、设置分节与运行摘要，候选策略、固定策略交付、研究计划绑定改为 hook（587 行）；`NodeServices` 拆出 `ManagedService`（555 行）。按 `function` 声明统计已无组件超过 600 行（未统计箭头函数组件）。单元测试 207 项、相关 e2e 通过；`backtest.spec.ts` 留出比较与 `portfolio-analysis.spec.ts` 两项在改动前即失败 |
| A01 | 本机完成：需求正文 `setup.md` 删除实施状态表述；主题文档中旧 Task 服务、worker 等表述已清理 |
| P2.1 | 完成：见 P2.2，任务种类表由各服务的 `TaskDomain` 提供 |
| P2.8 | 本机完成：源码目录改为 `apps/services/{market,data,research}`；`ServiceKind` 枚举改为 `MARKET/DATA/RESEARCH/INTELLIGENCE/TRADING`（整数值不变）。命名空间迁移（`asterion::tasks` → `asterion::research`）涉及十余个文件与测试，收益仅为外观，暂不做；持久的实例 ID（如 `market-data`）与日志程序名不改 |
| 文档 | 本机完成：删除 80 份按日期的评审与 9,185 行整改日志；25 份 research-* 等碎片合并为 `research.md`，情报 3 份合并为 `intelligence.md`，交易与行情交接文档并入主题文档；`data-paths.md`、目标设计移入 `docs/architecture/`；重写 AGENTS.md、README、docs 导航、架构与现状；派生 HTML 不再入库。Markdown 54,859 → 30,491 行，docs 文件 160 → 66，失效链接 0 |
| P2.4 | 暂缓：写连接为 `locking_mode=EXCLUSIVE`（冷备依赖无共享内存旁路文件），并发读连接会改变持久化与冷备语义；P0.5 已去掉主要延迟来源，按“先测量再优化”在有负载数据前不改 | — | — |
| D1/D7 删除 | 完成：情报服务（`dd190094`），模型训练/逐折验证/模型重放与成本批次/稳健性审阅（协议、Research、task-runtime、Native、界面、测试、文案、文档一并删除，约 −2.4 万行）。`backtest.reuse` 的单项 `fee_multiplier`/`slippage_ticks` 已无界面调用方，留待 R1 决定是否作为单个成本变体重新开放或删除 |
| D6 | 完成：行情为独立一级入口 | — | — |
| P1.1 | 完成：`docs/requirements/release-r1.md`（支持集合、R1 页面编号、冻结与删除、外部条件、完成判据），`product.md`/`delivery.md`/需求导航加链接 | — | — |
| 本机测试 | 删除后 `ctest -j 6`：960 项中 952 项通过。失败 8 项：`market_observation_archive`、`task_agent_recovery`（改动前即失败），QuantStats/Portfolio 6 项（`build/electron-resources/analytics` 中仍是改名前的 `asterion-analytics`，需重新 `pnpm desktop --prepare-analytics`）。界面：tsc、lint、205 项单元测试通过 | — | — |
| 本机测试（任务库拆分后） | `ctest -j 6`：960 项中 952 项通过；8 项失败同上（QuantStats/Portfolio 6 项分析助手未重新准备、`market_observation_archive`、`task_agent_recovery`），无新增失败。界面单元测试 205 项通过 | — | — |
| CI 基线 | 修复：两处进程测试在服务就绪前就发请求（改为等 `online`）；恢复期健康检查测试不再要求恰好观察到恢复中（P2.5 后恢复太快）；Linux 包资源大小改用内核的 `max_artifact_bytes`（ASan 程序超过 256 MiB，导致 `node_deployment`/`ssh_bootstrap` 失败）；消毒器下的 Agent 部署时限 60→180 秒、测试 IPC 时限按 `bound()` 放大；表刷新夹具的共享集合加锁（TSan）；最大因子输入容量测试与其他容量测试一样不在消毒器作业运行。本机全套通过 | — | — |
| A（成本变体） | 完成：回测“按此回测创建”在符合条件（单合约日线、单一动量规则、全样本）且沿用原固定数据时，可选滑点跳数或手续费倍数之一 | — | — |
| 决定（2026-10-27） | 观测不足一年不年化收益（卡玛随之不可计算）；R1 去掉本地实时模拟，直接用 CTP 仿真柜台 | — | — |
| e2e 关键链路 | 12 项全部通过：修正 5 项过时断言（文案变化、服务实例名 `research`、重启后等待服务在线、按可访问名选择柜台环境） | — | — |
| D5 | 调整顺序：界面与原生层是自定义 JSON 方言，`ResourceObject`、Decimal、任务等共享类型贯穿全部模块与约 40 个 e2e 规格，按域迁移会牵动全局；冻结的模块可能在 R1 后删除。改为在 R1 页面改造时逐域迁移到生成类型 | — | — |
| CI 基线（续） | TSan 作业通过、Linux core 958/958。修复：本地监听在排空超时退出时不再遗留 socket 文件（下次启动无法绑定）；节点身份重复替换时先等待正在重启的 Agent 回应新旧身份之一，不再二次停止节点；CI 中已删除的 Agent 升级脚本改为运行 `node_manager_services.py`；观测不足一年不年化 | — | — |
| 界面整理（一） | 导航改为两级（模块 → 一行视图标签），删除空子模块与系统管理页；视图标题缩短；研究类页面去掉重复页头，统一留白与字段布局；行情只列已接入分类，登录改为一条栏。e2e：修正 30 余处过时断言（服务实例名、文案、启动页、`owner` 参数），夹具重启服务后等待在线。全量 e2e 待复跑；`contract-contributions`、`research-evaluation`、`research-integration` 在改动前已失败（重启与联合冷备恢复场景），待单独排查 | — | — |
| 测试分层（9.8） | 部分完成：CTest 标签 `unit`（279 项，纯内存、单项 <1 s，`-j 6` 约 22 s）与 `process`（682 项，启动服务进程、套接字或慢存储；脚本测试全部归此）。按测试程序归类，依据最近一次全量运行的实测耗时。`task_execution_test.cpp`（8,532 行）拆为 `task_store_test`、`backtest_execution_test`、`task_process_test` 与共享 `task_execution_support.hpp`，145 项不变。测试名缩短未做 | — | — |
| CI 基线（2026-10-27） | 进行中：Manager/Agent 验收改用现名程序；Terminal 在隔离/开发节点上先建目录再加锁（SSH 验收启动即退出）；`TableRefresh` 并发用例用夹具独立放行固定先后；`DataInputs` 提交读取窗口 5→30 s（ASan 已转绿）；`RpcHost` 请求预算用例为未释放的回复留 2 字节；5 项过时 e2e 修复（重启后按新进程且就绪等待，表格任务属 Data）。剩余：Linux SSH 验收中 `useradd` 在 CI 上 60 s 超时，已加卡住时的进程与锁诊断待下一轮 CI | — | — |
| 哈希收缩（2026-10-27） | 完成：删除服务源码指纹，Linux 服务包按产品版本匹配，逐文件 SHA-256 保留；同一所有者读回自己写入的数据不再重算哈希（任务事件段、评估/统计来源、保存程序的每次使用、表格对象、分钟分段与 MACD 前缀、观测与响应对象、待删除对象、Terminal 解码结果）；Terminal 显示的输入摘要改用 Research 入库时存的值（`TaskResponse.result_input_sha256`）。保留：冷备、远程与插件制品、实盘前风控模块、下载、跨服务取回的固定证据。后续：Terminal 解码结果时调用的完整语义校验器（`validate_*_result`）属同类重复校验，删除前须逐项确认解码对异常数据安全 | — | — |
| D7 删除（2026-10-28） | 完成：研究项目、假设计划与人工结论整块删除——协议（`research_project.proto` 及任务/提交上的 `research_plan`，字段号保留）、Research 项目存储与请求、任务库仅为它存在的清单索引机制、Native 命令、界面三页与计划选择、评估导出的人工结论、相关测试与 e2e、现状文档。需求正文保留为远期目标 | — | — |
| R1 判据 4（旅程覆盖） | 部分完成：`r1-loop.spec.ts`（@journey）一条贯穿：固定 1 分钟数据 → 含费用与 1 跳滑点的规则回测 → 结果页与研究证据导出 → 同一结果的绩效与风险评价（年期数、零无风险、日损失路径、末日敞口）并导出 HTML 报告 → 由该结果冻结策略包 → 仿真柜台账户授权后以同一策略包启动运行。未覆盖：Tushare 真实下载（外部条件）、主力换月 | — | — |
| 第 3—5 阶段 | 下一步：D5 由 Protobuf 生成界面类型（含 P2.7、P3.1/P3.2） | — | — |

## 附录 A：本机 CTest 结果

macOS（Apple Clang 21，部署目标 26.0，构建目录位于较长的 scratchpad 路径），`ctest -j 6`，耗时 674 s：
**1059 项中 1027 项通过，32 项失败**（另有 1 项既有禁用）。按原因归类：

| 原因 | 数量 | 对应缺陷 / 说明 |
| --- | --- | --- |
| Unix socket 或文件路径过长（“invalid local socket path”“invalid test IPC endpoint”“File name too long”“cannot open directory tmp”） | 约 10 | B6；构建目录路径较长时更明显。说明测试夹具依赖短路径，与 CI macOS 失败一致 |
| 本构建目录未准备冻结分析运行时（QuantStats、Portfolio） | 6 | 环境原因；`build/Debug` 中已准备过，不计为缺陷 |
| “artifact is not a native executable”、Linux 服务包缺失（`desktop_resources`）、CTP 打包 ABI | 约 6 | 多为本机未构建 Linux 包与制品夹具；需在 P0.7 中逐项确认 |
| 冷备“requires the domain cold ownership cut”、Notice 备份 | 3 | 待查，可能与前面服务未就绪连带 |
| 已改名程序 `asterion_market_data` | 1 | B2 |
| Research 冷备超时（30 s） | 1 | 与 CI Linux 相同，B4 |
| Node 生命周期 / Agent 恢复 | 若干 | 待查，可能与路径问题连带 |

第 0 阶段改动后（`build/Debug`，部署目标 13.0，`-j 6`）：1063 项中 1040 项通过、23 项失败；
补齐漏改的 3 处 Research 启动参数和 `/tmp` 夹具后重跑失败项，只剩 8 项：6 项因本机未准备冻结分析运行时
（CI 会执行 `analytics_runtime.py`），以及 P0.7 所列 2 项 macOS 隔离节点用例。期间发现 `build/Debug/plugins/`
残留 10 月 16 日旧命名插件库（`asterion-*.dylib`，旧入口符号），会让插件目录扫描整体失败；已移出构建目录，
这是本机构建目录残留，不是源码缺陷。

结论：本机结果与 CI 相互印证。第 0 阶段先修路径（P0.4）与改名（P0.2），在短路径构建目录中重跑，
剩余失败再按 P0.7 逐项归因；不能把“环境原因”整体当作通过。
