# 目标工程结构

更新：2026-09-24。以[分层架构](architecture.md#层级与模块)为唯一层级与语言约束。下图为目标；现有代码与目标的差距在“当前代码归属”逐项列出，不为尚无实现的功能建立空目录。

## 顶层目录

目录直接表达层级。每个可构建模块只有一个层级；内核目录没有插件清单、安装或替换入口。

~~~text
asterion-terminal/
├── foundation/                    # L0 内核 · Rust 基础
├── kernel/                        # L1 内核 · Rust 通用机制
│   └── src/                       # authority / security / tasks / database / storage
│                                  # files / directories / archive / artifacts / recovery
│                                  # communication / events / transport / diagnostics
│                                  # environments / processes / supervisor / strategy host
├── domain/                        # L2 内核 · Rust 期货领域，每个子目录一个 crate
│   ├── instrument-catalog/
│   ├── trading-calendar/
│   ├── market-rules/
│   ├── connections/
│   ├── data-store/
│   ├── market-feed/
│   ├── role-registry/
│   ├── continuous/
│   ├── execution/
│   └── rollover/
├── services/                      # L3 应用服务 · Rust
│   ├── server/                    # API、用例编排、任务调度与 OpenAPI 生成
│   ├── store/                     # 应用表访问（内核连接池、占位符、表结构核对）
│   ├── data/                      # 数据应用服务库：目录、导入、内置来源、连接与配置凭据（迁移期经绑定供 Python 调用）
│   └── adapters/                  # 内置数据源与券商适配，随发行版编译
│       ├── provider-tushare/
│       └── connector-ctp/
├── compute/                       # L3 计算 · Python
│   ├── research/                  # 研究与回测计算、参数优化、统计与报告
│   ├── strategies/                # 内置策略
│   └── strategy-sdk/              # 策略包开发与打包
├── presentation/                  # L4 · TypeScript，构建期组合
│   ├── workbench/                 # 导航、布局、面板生命周期
│   ├── ui-kit/                    # 控件、主题和反馈
│   ├── runtime-client/            # 服务调用与通信
│   ├── desktop-bridge/            # Rust：薄 Tauri 视图/原生桥接
│   └── panels/                    # 业务面板，每个子目录一个包
├── bindings/
│   ├── python/                    # Rust→Python 计算接口与生成类型
│   └── typescript/                # 服务 API 客户端与生成类型
├── products/terminal/             # L5 · 产品清单与默认工作区
├── apps/desktop/                  # L5 · 桌面启动入口
├── contracts/                     # 通用线协议与领域 schema 快照
├── tests/                         # 架构、跨模块集成、端到端、性能
├── tools/                         # 构建、代码生成、打包、验证
└── docs/
~~~

services 内按用例划分模块，确有独立编译或故障边界再拆 crate；不为每个用例建服务或进程。目录数量不决定用户导航数量，产品在 L5 组合少量完整工作区。contracts、tests、tools 是开发设施，产品运行代码不反向依赖它们。

## 模块结构

L2 crate 示例：

~~~text
domain/execution/
├── Cargo.toml                    # package.metadata.asterion.layer = "L2"
├── src/
│   ├── lib.rs                    # 公开入口
│   ├── api.rs                    # 稳定公开类型与操作
│   ├── model.rs                  # 私有状态与不变量
│   ├── engine.rs                 # 单账户状态转换
│   └── storage.rs                # 使用 L1 受限事务/日志接口
├── tests/
└── README.md                     # 职责、契约与验收
~~~

内核 crate 不带 manifest，不登记、激活或停用；装配方式是普通的 Rust 依赖。内置适配器同样静态依赖，按用户配置的连接选择使用哪个，而不是按插件清单加载。唯一有安装清单的是用户策略包，格式见[策略插件契约](plugin-system.md)。

## 单一定义源

| 对象 | 权威源 | 生成或验证结果 |
|---|---|---|
| 通用通信数据 | contracts 的 schema | Rust/Python/TS 类型与共同合法/非法用例 |
| L1 机制接口 | kernel 公开 Rust API | 宿主绑定与契约检查 |
| L2 领域接口 | 对应 crate 公开 API 与 schemars 定义 | Python 计算接口类型（generate_domain_models.py）与 contracts 快照 |
| L3 服务 API | services 的 Rust 操作定义 | OpenAPI 与 TypeScript 客户端 |
| L4 布局与面板状态 | workbench 与各面板自己的状态 schema | 视图恢复校验 |
| 策略包 | 策略包清单 schema | 安装校验与打包工具 |

同一字段不在 Rust、Python 和 JSON 中手工维护多份定义；语义校验和计算只在 Rust 执行。领域接口不暴露数据库表、物理路径、框架对象或 Rust 内存布局。

## 当前代码归属

这是职责替换映射，不是文件移动。每个切片同时完成 Rust 实现、全部调用方、类型生成、打包、测试、备份和复现检查，并删除被替代的 Python 实现；不部署同一能力的双实现。

| 当前来源 | 目标归属与现状 |
|---|---|
| foundation、kernel | 已实现：基础摘要/通信校验、宿主、授权、凭据/加密、任务仓储/租约、数据库池/事务/游标、存储授权、文件/归档/工件一次写入、恢复、环境切换、计算子进程/监督、事务事件、通信与诊断。`kernel/src/plugins.rs` 的通用插件宿主将收缩为只服务策略包 |
| domain/*（原 plugins/domain） | 已迁移并去掉插件清单（2026-09-24）。已实现身份、日历、规则、角色解析、连接状态、报价缓存、日线扫描、累积分区合并/读取与日线账户切片；continuous、rollover 尚无实现 |
| src/asterion/api、runtime、platform、distribution*.py | 迁至 `services/server`（Rust）：HTTP/API、调度、进程入口、装配；Python 运行时插件宿主（`plugin_host.py`、各 `plugin.py`）随之删除 |
| src/asterion/data（目录、同步、导入、覆盖、准备、历史计划） | 采集编排与登记迁至 services；版本、覆盖与质量语义迁至 `domain/data-store`。目录查询（最新版本、历史、业务目录）已在 `services/data`（2026-09-24），入口直接提供，Python 的 `DataLibrary.list/history/hierarchy` 经绑定调用同一实现 |
| src/asterion/data/providers/tushare*.py | 已迁至 `services/adapters/provider-tushare`（Rust，2026-09-25）：计划、HTTPS 拉取、字段映射全部由 Rust 实现，来源端口在 `domain/data-store`（`provider`），内置来源表与计划准入在 `services/data`（`providers`）；Python `Tushare` 只是尚在 Python 的同步流水线调用绑定的薄封装，随流水线迁移删除 |
| 外部 `data.provider` 扩展点、`examples/plugins/calendar-source` | 已删除（2026-09-24）；策略包清单同时去掉了包间依赖 |
| src/asterion/connector_ctp | 迁至 `services/adapters/connector-ctp`（Rust，经 FFI 调用 CTP SDK） |
| src/asterion/contract_roles | 排名与角色计算规则迁至 `domain/role-registry`；发布编排迁至 services |
| src/asterion/contract_rules、trading_time | 语义已在 L2；来源验证、持久化与 HTTP 用例迁至 services |
| src/asterion/identity、connections、market、trading、extensions | 迁至 services（Rust）；extensions 只保留策略包管理 |
| src/asterion/research | 特征、信号、统计与报告计算迁至 `compute/research`（Python）；任务编排、固定输入与发布迁至 services；撮合与记账使用 `domain/execution` |
| src/asterion/strategies、src/asterion_plugin_sdk | 迁至 `compute/strategies`、`compute/strategy-sdk`，只支持策略包 |
| bindings/python | 收缩为计算接口：固定输入读取、领域类型与执行状态机调用；不再承载服务端存储/任务适配 |
| presentation/panels/*（原 plugins/ui） | 已迁移（2026-09-24）；每个包导出一个 `UiModule`，由产品以 `UiComposition` 在构建期组合，无运行时依赖声明或启用顺序 |
| apps/terminal | 前端入口迁至 `apps/desktop`；`src-tauri` 的原生桥部分归 `presentation/desktop-bridge` |

目录名 data/trading/research 只用于定位旧代码，不能成为保留跨层结构的理由。未知旧格式保留并报错，不清库、不覆盖历史结果。

## 工程检查

- 从 Cargo 与包元数据获得唯一层级和语言；目录与声明不符、未分类模块均失败。
- L0–L2 与 L3 应用服务只允许 Rust；Python 只出现在 `compute/` 与 `bindings/python`；L4 为 TypeScript（桥为 Rust）。
- L0/L1 的传递依赖不得到达 L2 及以上；L2 按同层白名单无环，不导入 L3/L4。
- 内核 crate 不带插件清单；不存在按清单选择或替换内核、适配器的入口。策略包清单只接受策略扩展点。
- 零策略包时内核可初始化、诊断并通过机制测试；策略包拒绝、失败或缺失不导致授权/租约/事务检查被跳过。
- Python 计算不直接访问数据库、工件、凭据或券商；策略拿不到发单端口。
- L0–L2 接口变更列出全量调用方，校验生成无漂移，执行契约/故障/性能回归；删除被替代入口。
- Rust/Python/TS 边界验证金额精度、时间/交易日、错误、取消、内存释放和工件身份；跨语言一致不能只靠类型生成。

当前 Rust workspace 已检查唯一层级、无向上依赖、L2 同层白名单和循环；TS 已检查包依赖、公开导出和面板声明，CI 检查生成漂移。其余规则随迁移逐项落地，替换顺序见[进度](ROADMAP.md)。
