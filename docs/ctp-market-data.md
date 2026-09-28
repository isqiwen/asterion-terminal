# CTP 实时期货行情

Terminal 的市场工作区提供“实时行情 / 历史行情”。实时页启动本机行情服务后，填写服务方提供的行情前置、经纪商代码、用户代码与密码，添加实际月份合约，再连接。没有默认前置地址，也不把历史 CSV 当作实时行情。远程 Linux 在设置的服务管理中部署“实时行情”，随后连接该服务。

## 实现归属

- `plugins/data/ctp/`：CTP 6.7.7 MdApi 数据插件；只包含行情，未链接交易接口。
- `core/include/asterion/domain/live_market.hpp`：报价、订阅状态与实时数据端口。
- `apps/market-data/`：CLI11 独立宿主，管理插件生命周期及请求路由。
- `protocol/proto/asterion/v1/market.proto`：连接、订阅、快照推送、合并前事件分页与心跳。
- `apps/terminal/native/market_client.*`：本机 IPC / TCP+mTLS 客户端。
- `apps/terminal/plugins/futures-market/`：行情工作区与中英文资源。

Agent 显式区分 paper / market 服务类型，负责部署、启动、私有健康探测与有限重启。Terminal 关闭只断开观察连接。SDK 网络重连后重新登录并恢复订阅；服务进程重启后需用户重新输入密码，不能因进程存活就显示行情正常。

## 数据与凭据

密码仅存在于本机输入、认证通道请求及服务内存，服务内存保留至断开以支持自动重登录；不会进入快照、日志或持久配置。远程服务通过双向 TLS 接收密码。前置、经纪商、用户代码和自选保存在 Terminal 本机。

订阅只接受六家期货交易所的实际月份合约，最多 50 个，不支持主力/连续合约别名。订阅成功与收到首笔分开；单合约订阅失败可见。买卖一档、价格、成交量、持仓量显示空值而不是伪造零价格。报价时间使用 ActionDay + UpdateTime + UpdateMillisec（北京时间），保留 TradingDay，不将交易日当作自然日。缺失时间、超前时间、30 秒未更新、断线旧报价分别标识；这不是交易日历判断，休市也会显示未更新。

终端每 250ms 接收合并快照，中间 tick 可能被合并，不能用作逐笔历史归档或无损策略行情源。旧时间戳报价不覆盖界面最新值，并单独计数；合并前事件流仍保留该报价及乱序标记。CTP 原始浮点报价只在供应商边界转为八位 Decimal，非权威账本输入。

## SDK 与构建

首次构建显式运行 `python3 scripts/prepare_ctp.py`，再运行 Conan / CMake 或桌面脚本。脚本按照 `conan/ctp-md/sources.json` 的固定提交和 SHA256 获取 SDK，并导入本机 Conan；CMake 和 Conan 配方不会隐式下载 SDK。供应商 SDK 镜像来源为 [ctp2rs 固定提交](https://github.com/pseudocodes/ctp2rs/tree/2bad971d48d30ee5923f75e82f39dd58b8fe0af9)，只有 MdApi 头文件与动态库。发布方需遵守供应商 SDK 的使用和分发条款。

当前 6.7.7 材料提供 macOS arm64/x86_64、Windows x86_64、Linux x86_64。Linux 暂时仅支持 x86_64，Terminal、Agent 和服务均不支持 Linux ARM64。桌面包包含本机 SDK 和 Linux x86_64 SDK，不需要用户另行下载。

## 验收边界

单元测试覆盖时间、缺失报价、实际合约和 Protobuf 表示；独立进程测试使用仅测试构建的 SDK 替身，覆盖登录、订阅拒绝、推送、乱序、断线恢复和 Agent 健康。浏览器测试经过真实 C++ 服务与测试 SDK，不代表实际 SimNow 验证。真实账号登录、交易时段新鲜行情及 Windows 原生运行仍需独立验收。

macOS 保留 hardened runtime，打包签名授予加载外部原生库的 library-validation entitlement，以支持没有相同 Team ID 的供应商 SDK（本机构建为 ad-hoc 签名）。Tauri 将此 entitlement 用于包内可执行程序；这不是插件安全隔离。Agent 仍只加载平台、摘要校验通过的显式 SDK 材料。

TCP/mTLS 集成测试另行覆盖匿名客户端拒绝、原生 Terminal 客户端接收报价，以及断开客户端后服务继续运行。SDK 的回环测试只访问 `127.0.0.1`，不使用真实账号或外部前置。


CTP SDK 的三平台准备入口均为 `python scripts/prepare_ctp.py`（macOS/Linux 也可用 `python3`），从脚本位置定位仓库，不依赖当前工作目录。Conan 环境脚本输出到忽略的 `build/ctp-sdk/<os>-<arch>/conan/`，它们包含本机绝对路径，不能提交或跨机器复用。`conan/ctp-md/` 只保留配方 `conanfile.py` 和固定来源/摘要清单 `sources.json`；清单中的 URL 是可复现依赖定位，不是开发者本机路径。macOS 的 `lipo` 和 `codesign` 仅在 macOS 分支运行，Linux/Windows 使用各自 SDK 库。

## 合并前事件流

`LiveMarketDataPort.events_after(stream_id, cursor, limit)` 读取供应商边界标准化后的事件；CTP 插件在更新界面快照之前记录报价，按回调接收顺序分配独立序号。订阅状态、连接、断线、重连、错误和显式断开也记录为状态事件，状态事件不重复携带旧报价。记录中没有供应商配置或凭据。

每个 Feed 实例有独立流身份，连接重建保留同一实例的事件和连续序号；服务进程重启或创建新 Feed 会生成新身份。首次读取可使用空身份与零游标，之后必须使用返回的身份；旧实例身份、未来游标和非零匿名游标一律拒绝。此序号不是交易所序号，连续只表示插件记录入口的顺序，不能证明上游网络没有丢包。

默认保留最近 4096 个事件，插件可配置容量 1..65536；每页 1..1024 个。读取不消费事件，调用者只有在自己的持久提交成功后才推进游标。返回最早/最新序号和 `gap`：游标落后于已淘汰记录时明确报告缺口，同时提供尚保留的后缀，不能把后缀冒充完整数据。分配或记录异常使 `failed` 永久置位；已有前缀仍可读，该流不能再作为完整记录来源。状态与报价共享这一容量，消费端必须处理两类事件。

行情宿主通过 `market.proto` 的 `ReadEvents` / `EventBatch` 暴露相同分页，支持本机 IPC 和 TCP/mTLS；私有健康通道不接受行情读取。原 250ms `Watch` 保持界面合并快照语义。当前未提供持久归档、历史补取、Terminal 录制按钮或实时策略驱动。

这些是 **标准化一档报价观察**，不是供应商原始报文或逐笔成交。保留 CTP ActionDay、TradingDay、来源与接收时间及缺失价格；价格沿用八位 Decimal 转换，数量沿用供应商适配层的非负规范化。`volume` 是累计成交量，不转换为逐笔成交数量，不能直接送入当前基于成交事件的回测和模拟撮合。

本阶段 macOS GoogleTest/真实子进程验收覆盖：界面丢弃但事件保留的乱序报价、SDK 断线重连、无凭据状态、非破坏分页重读、缓冲溢出与缺口、旧流身份及游标拒绝。测试使用仅测试构建的 SDK；不代表真实 CTP 账号或交易所数据完整性验收。

补充验证：macOS Debug 与 Linux x86_64 Release 均通过 8/8 项相关测试，包含 CTP/事件流 GoogleTest、本机真实进程、TCP/mTLS 和 Agent 行情管理回归。证据为 `build/market-events-integration-tests.log`、`build/market-events-linux-tests.log`；Linux 使用 Ubuntu 24 x86_64 容器。Windows 原生未执行，本阶段代码尚未重新打入 DMG。
