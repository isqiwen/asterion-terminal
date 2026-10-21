# Asterion 与 NMR 插件机制比较

日期：2026-10-16。按用户要求比较两个本地项目的当前实现，不修改生产代码。

## 基线与方法

- Asterion：`/home/qiwen/Workspace/asterion-terminal`，`main`，`723f4184ea7a5c699ea4039f89a7b157c3a98bee`，评审前工作区干净。
- NMR：`/mnt/Code/upgrade/nmr`，HEAD `d26361fed0756c8b975378b4419cdf02f4a37418`，存在未提交修改和新增文件。本文以磁盘实际代码为准，包含已修改的 CBundle/CAssembler 和新增 SQLite 插件，不将其都归于 HEAD。
- 对照 Asterion 架构设计、实施和审计进度，检查公共契约、加载器、实例所有权、注册查询、装配流程、插件示例及相关测试源码。
- 本次为静态比较，未启动两个应用、编译 NMR 或运行测试。测试文件存在不代表本轮测试通过；不引用其他 checkout 的运行结果。

## 结论

NMR 是以统一 Bundle、服务注册表和通知机制装配整套程序的微内核框架，扩展覆盖面更广。
Asterion 是面向期货 Terminal 的分层扩展：内置 UI 工作区、服务内原生能力插件，以及尚未动态化的仓库内模块。
Asterion 当前对动态插件的 ABI、所有权和任务产物身份约束更明确；NMR 对界面与基础设施的运行时替换更灵活，但部分框架机制仍未完成。

不能把“更多功能放在插件目录”直接等同于“更成熟的插件系统”，也不能把 Asterion 的六类能力标签当作六套已经开放的动态 SDK。

## 实现对照

| 维度 | Asterion | NMR 当前工作树 |
| --- | --- | --- |
| 装配范围 | UI 与原生服务扩展分开；服务是 Agent 管理的独立进程 | 日志、配置、运行时、主窗口、布局、数据库、业务 UI/WF 等均按 Bundle 装配 |
| UI 扩展 | `src/plugins.ts` 静态导入并注册，React 管理组件生命周期；不能安装外部 UI 包后自动加载 | UI 动态库通过上下文查询主布局，再用命令插入或移除 QWidget |
| 原生入口 | `asterion_plugin_entry_v1` 返回 C 结构和函数表，检查 ABI、结构大小、平台、能力 | `GetActivator` 导出名采用 C 链接，但返回 C++ `IBundleActivator*`，后续走虚函数接口 |
| 插件调用 | 宿主按声明的领域能力创建实例、取得指定版本的函数表 | `IBundleContext::Register/Query/Unregister` 提供服务定位；常见业务入口为字符串命令和 `void*` 参数 |
| 描述信息 | 动态库内描述 ID、实现版本、平台及能力 | `.plugin` 描述 Symbol、Version、Activator、ActivationPolicy、Dependency 等；配置另有 `.cfg` |
| 依赖 | 当前不解析跨插件依赖，要求自包含非系统依赖，宿主显式编排 | 清单有 Dependency，但激活依赖仍是 TODO；启动器扫描 Platform 后 Plugins，未见依赖图排序 |
| 生命周期 | create/start/stop/destroy；RAII 保证实例持有库句柄，实例释放前不卸载 | Install/Resolve/Start/Stop/Uninstall 状态机；Activator、Plugin、Controller 分层管理与手动注册注销 |
| 更新 | 停止服务及工作进程后修改集合，再显式启动；无热替换 | 存在进程内动态启停与卸载接口，但不足以证明任意插件可以安全热替换 |
| 版本选择 | 同 ID 多实现可在目录并存，每个服务显式选择一个；任务固定动态库摘要 | 扫描版本子目录；服务按版本字符串登记，未指定版本时取字符串排序最后一项 |
| 身份与部署 | SHA-256 校验、服务专属插件目录、修订冲突检查、任务固定副本；Agent 用子进程检查库 | 已检查的加载路径主要依赖目录、清单和入口；未见对应任务摘要绑定、原子部署集合或同等 ABI 握手 |
| 信任 | 原生插件可信、进程内运行；UI 命令声明用于限制误用，不构成沙箱 | 同样是可信进程内代码；服务查询鉴权不是内存或进程隔离 |

### Asterion 的实际开放程度

`CMakeLists.txt` 当前构建 Tushare、SHFE 和订单限额三个正式原生插件动态库。
历史分钟、市场表、连接配置、原始响应可选观测和交易前风控具有对应 C 能力契约。
策略、CTP/执行、SQLite/Parquet、指标和若干分析工具仍采用仓库内静态目标。
QuantStats 是随桌面分发的独立分析助手，经 Protobuf 调用，不是 `AstPluginV1` 动态库。

因此：新增符合现有能力的数据供应商可以只实现动态插件；新增第三方工作区、全新交易策略或执行通道，仍需要修改并重新构建宿主相关代码。

主要依据：

- [原生 ABI](../../bindings/plugin/include/asterion/plugin/abi.h)
- [加载器和实例所有权](../../core/src/kernel/native_plugin.cpp)
- [UI 契约](../../apps/clients/terminal/plugins/contract.ts)、[内置注册](../../apps/clients/terminal/src/plugins.ts)、[命令范围](../../apps/clients/terminal/src/host/plugin-registry.ts)
- [部署产物](../../apps/management/node-agent/plugin_artifacts.cpp)、[本机插件目录](../../apps/common/control/plugin_catalog.cpp)
- [当前 SDK 边界](../native-plugins.md)、[构建目标](../../CMakeLists.txt)

### NMR 的实际开放程度与限制

下列路径均相对 `/mnt/Code/upgrade/nmr/`。

1. **整套程序通过服务组合。** `Src/Code/Platform/Starter/Module/Kernel/Platform/CRunWizard.cpp:211` 先装配 Platform、再装配 Plugins；停止时反向处理。`Src/Include/MK/Core/IBundleContext.h:35` 定义注册和查询接口。`Src/Code/Plugins/CUI/NMR.DC.Analysis.UI/Module/CController.cpp:248` 查询布局并动态插入分析面板，展示了实际 UI 扩展能力。
2. **C 导出名不等于 C ABI。** `Src/Code/Plugins/CUI/NMR.DC.Analysis.UI/CActivator.cpp:27` 返回 C++ 接口；`Src/Include/MK/Core/IBundleControl.h:35` 使用虚方法和无类型参数，SQLite 公共参数又包含 QString/QVariant 等 Qt 类型。部署双方需要匹配的 C++/Qt ABI；不像 Asterion 的 SDK 仅以 C 结构、函数指针和借用数据为边界。
3. **Dependency 尚未兑现为依赖解析。** `CBundle.cpp:597` 仍是启动依赖的 TODO。`CRunWizard.cpp:294` 扫描目录逐个安装启动。不能声称当前有依赖图、版本约束求解或循环依赖检测。激活策略有分支，但正常 CAssembler 调用 Start()，不能仅凭清单字段宣称按首次服务查询完成懒加载。
4. **动态卸载仍依赖插件自行清理。** `CBundle.cpp:487` 自动注销启动时注册服务仍是 TODO。当前工作树 `CBundle.cpp:711` 已在 Stop 失败时保留动态库，`CAssembler.cpp` 也在 Uninstall 失败时保留管理入口；`Starter/Test/CBundleLifecycleTest.cpp:94` 有对应测试源码。这是有价值的约束，但服务 Query 返回裸指针，框架未在该路径建立借用引用计数和调用排空，不能据此证明全局热卸载安全。
5. **“最新版本”目前是字符串排序。** `Src/Code/Platform/Starter/Module/Kernel/Common/CContext.cpp:115` 的 ServiceSet 为 `std::map<std::string, ...>`，未指定版本时在 717/791 行使用 `rbegin()`。版本字符串 `1.9` 与 `1.10` 的顺序不等于语义版本顺序。是否实际触发取决于版本命名，不将其写作已经发生的运行事故。
6. **权限体系含占位实现。** `Src/Code/Platform/Starter/Module/Kernel/Platform/CSecurity.cpp:43` 的令牌生成仍是 TODO；服务权限校验部分分支允许继续。不能把现有鉴权接口视为已完成的第三方插件安全边界。本文不复制源码中的令牌字面量。

另有两个可直接从源码确认的失败语义问题，留作比较依据，不在本次修改 NMR：

- `CBundle.cpp:338` 检测入口不存在后没有退出当前解析块，后面的成功赋值会覆盖该状态；后续激活仍会再次检查入口，不能夸大为任意库可成功激活。
- `NMR.DC.Analysis.UI/CPlugin.cpp:53` 的 Start 初值为成功，catch 只记录日志，异常路径可能返回成功。这说明状态机的正确性仍依赖每个插件准确报告失败。

## 对本项目的取舍建议

保留 Asterion 的服务进程边界、C ABI、实例所有权、明确失败和任务摘要绑定。
这些约束直接服务于期货执行、风险控制和实验可复验，不适合为统一插件形式而放松。

可以借鉴 NMR 的贡献点思想：当出现明确需求时，将具体的菜单、面板、设置页或工具入口作为类型化贡献加入 UI 契约，让插件拥有自己的贡献。
现有 workspace/headerTools/toolbar/tasks 已体现同一方向，无需先引入全局服务定位器或复制 Activator→Plugin→Controller 的固定层级。

后续是否动态化按实际用户需求决定：

- 若需求是第三方增加数据源，沿现有历史能力 SDK 扩展即可。
- 若需求是无需重编译安装第三方工作区，需要另行定义 UI 包交付和装载契约，当前内置注册器并不提供该能力。
- 若需求是独立交付交易策略，应先解决策略状态、事务与回放身份，再定义对应动态 ABI，不能仅导出工厂函数。
- 不为当前固定产品范围新增通用依赖求解、任意热卸载或全局事件总线。

本次仅形成评审与审计进度记录；不修改 NMR 工作树，不提交、推送或实施上述建议。

## 后续讨论：适合 Asterion 的目标架构

用户进一步询问本项目的最佳架构。结合当前产品范围，建议维持“分层核心 + 按状态与故障边界划分的服务进程 + 类型化插件”的主干。
这与现有架构设计大体一致，以下是目标约束和演进建议，不是新增实现或已验收声明。

- Terminal UI 管交互和展示，Native 管服务连接、应用编排及只读投影。它们不能成为账户、历史数据或任务结果的第二个事实拥有者；允许明确归属的 UI 草稿和缓存。
- 行情服务管理实时观测；Data 管历史版本、来源证据及供应商授权和共享额度；Task 管任务定义、尝试和结果；每账户 Trading 管执行许可、风险占用与本地账本投影。柜台仍是实际成交事实的来源。
- Node Agent 只负责部署、进程生命周期与资源准入。worker 一次执行一个任务尝试，承担耗时或易失败计算。保留本机 Unix Socket、远程 TCP+mTLS 和 Protobuf，不新增通用服务总线。
- Core 依赖保持 Domain → Kernel → Foundation。算法和供应商实现位于 plugins，服务负责装配；仅对实际存在多实现或独立交付需求的能力定义接口。
- UI 插件继续采用类型化、构建期贡献；原生供应商和风控采用 C ABI 动态插件；策略、存储及内部工具可以继续静态模块化。独立交付需求出现后才扩展动态 ABI，插件和进程没有一一对应关系。
- 账户和任务状态分别由明确的单写入者裁决。持久完成、外部调用和 UI 反馈按定义顺序交接；资源有界，SDK 回调只交付事件，慢存储或插件调用不占用控制路径。
- 策略产出意图，风险插件产出明确裁决，交易服务拥有最终发送许可；正常调用链不能绕过授权、宿主强制控制与持久记录。可信进程内插件契约不提供对恶意代码的隔离。
- 实盘与回测共享真实适用的策略、订单和风险语义，供应商执行与历史撮合分别实现；不强行合并两种不同的时间和故障模型。
- 固定输入、参数、费用和实际使用的算法产物身份；新版本形成新执行依据，不能静默改变已有实验、账本或数据。进程重启、插件更换与业务状态恢复分别定义。

建议近期优先核对现有边界的实现和实际故障证据，避免增加平台概念：确认启动和设置对插件能力的理解一致，明确内置 UI 模块与可安装原生插件的产品表述，按需要完善具体贡献点和 SDK 示例。
第三方 UI 包、动态交易策略、依赖求解和热替换均不作为本轮默认建设目标。
架构验收应以慢盘、撤销授权、未知委托、worker 崩溃、版本切换后任务重试等行为证据为准；本轮没有运行这些验收。
