# macOS 编译与运行修复 — 2026-10-16

基线：主 checkout `/Users/qiwen/Code/quant/asterion-terminal`，`main`，
`0d52b8f3dbd022b95b3db8508b47f4b70c7a86c0`，开始时工作树清洁。
本轮在 macOS 27.0（26A428）、Apple Silicon、Apple Clang 21.0.0 上验证，
CMake 的最低部署目标保持 macOS 13.0。此前 Linux 验收记录不作为本轮 macOS 证据。

## 复现与修复

1. `pnpm desktop --prepare-quantstats` 首先因本机缺少锁定的 CTP 配方修订而退出。
   显式运行 `scripts/prepare_ctp.py` 注册当前 CTP 与 DuckDB 配方，按锁文件安装 pnpm
   依赖、准备 Electron 和 QuantStats；没有更新依赖锁或改用全局库。
2. Apple Clang 拒绝月度贡献的部分聚合初始化。现在先值初始化整条记录，再填写月首日与
   期初权益，其余字段由原循环计算。月度分组、固定精度金额和统计公式保持原义。
3. 合约乘数错误诊断的浮点 `std::format` 间接依赖 macOS 13.3 才提供的 `to_chars`。
   改为 classic locale 的标准流，保留六位有效数字；现有精确错误文本断言通过。
   月度测试中的日期比较改为日差，避免 GoogleTest 的 chrono 打印器触发同一限制。
4. 冻结 QuantStats 的首次三项测试均超时。采样定位到 Matplotlib 字体扫描：PyInstaller
   为每次进程创建空配置目录，导致反复扫描系统字体。显式准备阶段生成字体缓存，筛选为
   101 个库自带字体，以相对路径打包；助手在导入 QuantStats 前复制到本次临时目录。
   资源清单包含缓存摘要，不携带用户字体、绝对路径或共享运行状态。
5. 消除字体扫描后，首次两项并行测试仍超时，第三项通过。第二次采样定位到 macOS
   动态库映射与校验；独立全新目录探针测得 IPC 端点就绪需 37.87 秒。
   对比修改第三方库初始化与分开启动/分析期限，采用后者：启动最多 60 秒，端点就绪后
   分析仍最多 30 秒。保留一个短生命周期子进程、失败回收与不重放请求的行为。
   没有修改第三方库源码、增加常驻 Python 服务或重试。

## 当前验证

证据目录：`build/reviews/macos-build-run-2026-10-16/`。

| 验证 | 结果与证据 |
| --- | --- |
| 完整 macOS Debug 构建、TypeScript、Vite | 通过；`desktop-final.log`，不是仅重编译出错文件 |
| 相关 C++ 与进程回归 | 32/32；`ctest.log`，覆盖月度贡献、乘数拒绝、数据集、架构检查、Node-API 生命周期、资源校验、真实 CTP SDK 回环、行情进程和任务恢复 |
| QuantStats 新目录完整回归 | 3/3；`quantstats-cold-test.log`。复制冻结资源到全新临时目录并校验全部摘要后，通过 `ASTERION_QUANTSTATS_EXECUTABLE` 运行现有 C++ 测试；两个并行冷启动用例各 48.83 秒，后续无效输入用例 2.66 秒 |
| 冻结资源与可重定位字体 | 全部资源摘要通过，101 个字体路径均为相对路径且存在于包内 |
| 两组 Electron 桌面验收 | `pnpm test:desktop` 通过；`desktop-tests.log` 和 `screenshots/`，覆盖真实宿主/原生模块、隔离 Agent、设置与窗口重启、行情、历史数据和图表链路 |
| 正常开发入口生命周期 | 首次准备启动、正常退出并停止本机开发服务、再次不带准备参数运行 `pnpm desktop` 均完成；退出记录在 `desktop-fixed.log` |
| 实际运行来源 | `runtime-verification.json` 记录 Electron 的主 checkout 工作目录、Vite 源码入口、实际加载的原生模块及 15 个 staging/build 文件一致的摘要 |
| 格式与 diff | `pnpm run format:check`、`git diff --check` 通过；`format-final.log` |

实际开发窗口已进入工作台；本机节点已连接，数据和任务服务正常。行情等待配置或连接，
没有打开交易账户。隔离桌面测试使用测试 SDK，普通开发窗口没有配置或连接真实柜台。

原生模块 SHA-256：`a7dca7e37c2ccb3703fd6a479fe4466cd314f25946c0f02e5cd92f959d6b1031`。
当前服务源码指纹：`595ab80a002297191ec4f4800c02e04e9fa8ff34d590216d2864aefff5054687`。
首轮依赖、编译及两轮 QuantStats 失败日志均保留，不将修复后的结果表述为首次全绿。
两组桌面验收在启动/分析期限分离前完成；该期限调整后重新构建并运行普通开发入口，
另以全新副本通过上述三项 QuantStats 完整回归，没有重复统计无关桌面场景。

## 未验证范围

没有重跑全量 CTest、Vitest 或全量隔离 E2E；没有验证 macOS 13 实机、真实柜台、长时负载、
Release、DMG、签名与公证。现有远程 Linux 服务包已过期，本次未重建；开发入口明确警告
远程部署不可用，因此不宣称 `desktop:check` 或发行打包通过，发行前仍须重建服务包。

保留用户账本、数据集、任务和凭据；正常开发入口仅创建/运行独立开发节点。未提交或推送。
