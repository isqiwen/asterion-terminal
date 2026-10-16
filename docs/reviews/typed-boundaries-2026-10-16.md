# 类型化接口与架构约束

> 后续实盘观察出口的收敛见[专项记录](live-observation-2026-10-16.md)。本文下方未完成项和安装包证据描述本轮结束时的基线。

## 基线与设计

2026-10-16，用户授权继续推进架构约束自动检查、命令参数类型和完整验收，并要求复核直接
返回 JSON 的接口。基线为主 checkout `main` 的 `723f418` 加上尚未提交的
[插件边界收敛](architecture-convergence-2026-10-16.md)，本轮不覆盖或撤销此前修改。

根问题是领域状态与展示格式混在一起，以及命令名虽有类型、参数却可任意组合。
JSON 是序列化格式，不能代替账户、订单、资金及计算定义的领域模型。

比较两种方案：

- 所有接口统一使用 JSON，加运行时 schema：仍使内部调用依赖字符串字段和金额解析，
  且把已知业务结构的错误延迟到运行时，不采用。
- 领域使用 C++ 类型，服务传输使用已有 Protobuf，UI/Native 边界保留 JSON：在现有边界
  转换，前端用命令到参数的类型映射约束调用，采用此方案。不新建协议或通用 RPC 框架。

前端比较泛型参数映射和命令/参数联合元组：后者可保持方法和参数的关联，避免一个联合
方法名接受另一个方法的参数；组件、宿主和 bridge 都传递同一元组。交易面板绑定账户的
适配器只补入该面板拥有的账户 ID，仍由账户服务裁决授权、政策修订和发送许可。

不改变服务进程、状态所有权、交易行为、持久数据结构、用户文件或原生插件 ABI。
不增加热卸载、服务定位器、事件总线、数据迁移或业务版本计数。

## 实现

- `FuturesAccount::snapshot()` 返回 `AccountSnapshot`，价格和资金保留 `Decimal`，
  持仓、订单、成交和标记价均有明确类型；账户不再包含 JSON 序列化头文件。
- `PaperExecution::snapshot()` 返回包含账户观察和回放进度的结构。
  回测 worker 直接从账户与固定输入编码 Protobuf，删除
  “领域状态 → JSON → Protobuf”以及成本模型“Protobuf → JSON → Protobuf”的往返。
  固定输入身份与账户合约不匹配属于内部不变量错误，明确失败；不猜测映射。
- 账户原有计算是金额的唯一来源。观察对象是脱离可变账户的值，不拥有执行权，也不回写账本。
  原有成交、结算、部分成交、回滚和费用测试改为类型化断言。
- `src/bridge/commands.ts` 保存当前 UI 命令参数映射；命令名称从映射导出。
  宿主、设置、工作区和共享组件不再用任意字典传递命令参数。
  `commands.typecheck.ts` 以正常 TypeScript 构建检查缺账户、参数错配、数字类型及查询权限等反例。
  Native 对外部输入的实际校验继续有效，编译期类型不充当运行时安全校验。
- CMake 配置时遍历真实的本地链接依赖，检查 Domain → Kernel → Foundation 的向下依赖方向，
  拒绝 Task/Agent 直接或间接链接回测、因子、模拟执行和策略引擎。
  构建顺序依赖与链接依赖分开，随包资源不会被误判为执行所有权。
- `Architecture.SourceBoundaries` 通过 CTest 检查核心源文件的向上 include 与领域 JSON 使用。
  Kernel 对公开原生插件 C ABI 的依赖是已有装载机制的必要边界。
  CMake 规则文件纳入服务源码指纹；新增构建规则也能使旧服务包失效。

## JSON 使用结论

| 位置 | 判断与当前处理 |
| --- | --- |
| 核心领域、模拟账户与回测计算 | 使用类型化观察与已有 Protobuf，不返回任意 JSON；本轮已处理上述链路 |
| 服务间 IPC | 保持 Protobuf、Unix Socket 或 TCP+mTLS；不换成 JSON RPC |
| Native 给 UI 的快照、列表、导出内容 | 属于展示与协议边界，可以返回 JSON；前端以已有 `Snapshot` 等类型消费 |
| 供应商 HTTP 响应 | JSON 是外部协议；供应商插件解析后向内部提交类型化数据 |
| 日志、配置与持久记录编解码 | JSON 可以作为边界表示；不能以反复读取动态字段替代内部业务状态 |
| 现有实盘服务观察出口 | `LiveAccountState::snapshot()`/`LiveSession::snapshot()` 仍组装 JSON，再由协议层编码 `LiveSnapshot`；这是尚未消除的中间表示，不能宣称整个仓库已无 JSON 往返 |

实盘柜台适配器、风险裁决和账户记录已有类型接口。本轮没有同时重写实盘观察/日志全链路，
避免将一次接口收敛扩展为新的交易状态机改造。后续若继续清理，应从观察出口直接生成既有
`LiveSnapshot` 或领域观察类型，并同步调用者与测试，不能再保留一套兼容出口。

## 验证与验收边界

本 checkout 的记录在 `build/reviews/typed-boundaries-2026-10-16/`。
一次 CTest 执行被会话中断终止，已保留 `ctest-interrupted.log`；不把部分通过计作全量通过。
已完成：Debug 与 Release 全量编译、683/683 CTest（409 秒）、41 文件 111/111 Vitest、
前端生产构建、TypeScript 正反例检查、ESLint、统一格式与 diff 检查；真实桌面开发入口两组
验收均通过。CMake 负例验证拒绝核心向上依赖、Task 间接依赖回测引擎、Agent 依赖因子引擎。
Debug 暂存的 15 项原生资源摘要与本次构建一致。

桌面首轮失败来自旧测试仍调用 SVG `getBBox()`/`getPointAtLength()`，而当前图表已使用
Lightweight Charts Canvas。断言已改为可见画布、单根 K 线数量和 Home/End 的日期游标；
没有为旧测试修改产品图表。旧失败日志保留为 `desktop-stale-assertion.log`，修复后
`pnpm test:desktop` 两组均通过，见 `desktop.log`。

全量隔离 E2E 首轮 98/99 通过（20.9 分钟），唯一失败为 `i18n.spec.ts` 仍期望旧英文
插件文案 `Registered · Loaded on Demand`。已改为当前 `Built in · Shipped with the App`，
与“内置 · 随应用发布”的产品契约一致；首轮 `e2e.json` 和失败 trace 保留；修正后的语言测试定向复验 2/2 通过（13.9 秒），
见 `e2e-i18n.json`。没有把首轮结果改写成单次 99/99 通过。

Ubuntu 24.04 x86_64 容器已从当前源码及锁定依赖重新生成 Linux 服务包，包内文件、ELF
架构、初始化程序和源码指纹校验通过。Linux 桌面 `package-test` 已生成约 377 MiB 的测试
Debian 包，包身份、声明依赖、包内任务恢复和厂商 CTP 回环生命周期检查通过。
随后将该包解压到临时目录，用包内程序及插件执行桌面验收，通过；没有系统安装或覆盖现有应用。
证据为 `installed/acceptance.json`，安装包 SHA-256 为
`c6a68e90b40f1f2303c8c064c13382851d2d0afd638cb63c976612b1a167b76e`。
这是 Debian 13 本机的隔离解包验收，不能推断其他发行版桌面安装通过。

打包首次因本机 Conan 缺少锁定 CTP 配方失败，按开发指南执行 `scripts/prepare_ctp.py`
导出当前本地配方后通过；保留 `package-unprepared-cache.log`。未修改依赖锁或使用替代 SDK。
统一格式检查和文档本地链接/锚点检查通过，完整命令结果见 `verification.json`。
录像默认关闭。

macOS 实机、签名/公证、真实柜台和持续跨交易日负载需要对应平台与环境，不能从 Linux
隔离测试推断通过。没有提交、推送或部署，没有使用真实账户下单。
