# 开发约束

本文件适用于整个仓库，开始工作前阅读。只记录当前有效的规则。

## 范围

- 只开发、测试和交付 **macOS Terminal**，资产只做 **期货**。
- 不新建其他客户端（Web、Mobile、Notebook、独立 CLI）或 Windows/Linux 桌面版本。
- 远程服务部署只支持 Linux x86_64，只为 Terminal 服务。
- 不为未来需求预建空接口、空目录或空实现；接口按实际用例建立。

## 只保留当前实现

- 项目只保留当前有效的代码、契约、配置、脚本、测试和文档；替换实现时同步删除被替代的内容，不保留历史参考副本或备份实现。
- 不保留旧接口、旧协议、兼容层、别名、双实现、迁移入口或降级路径；调用方、测试和文档同步更新到当前契约。
- 不支持的输入和持久化格式明确拒绝；版本校验用于拒绝不支持的格式，不用于恢复旧实现。用户数据保护仍按下文执行。

## 架构

- Core 分层：`asterion_foundation` → `asterion_kernel` → `asterion_domain`，依赖只能向下。
- 供应商、存储后端、具体策略和风险算法写成插件，放在 `plugins/`，不进 Core。
- 每个服务是独立进程，由 Node Agent 托管；Terminal 只连接服务，关闭窗口不停止服务。
- 进程间通信使用 `protocol/proto/` 中的 Protobuf；本机 Unix Socket，远程 TCP + mTLS。
- Terminal 专属 C++ 编排在 `apps/clients/terminal/native/`；UI 插件契约在 `apps/clients/terminal/plugins/contract.ts`。
- 插件是可信的进程内代码，不是安全隔离。

## 编码

- C++20、RAII、明确所有权。异常不得跨越 C ABI 或 Node-API 边界。
- 金额和价格使用 `Decimal`，不使用二进制浮点作为权威账本值。
- 领域对象之间通过类型化接口交互；JSON 只用于协议边界和界面展示。
- Core 与服务只输出英文诊断，跨进程错误携带 `ErrorCode`；面向用户的新诊断登记到 `apps/clients/terminal/src/i18n/locales/diagnostics.*.json`。
- 持久状态与密钥通过 `kernel/durable_file.hpp` 写入。
- 改变撮合、费用、保证金、风控或交易命令语义时，提升 `apps/services/trading/paper_session.cpp` 的日志引擎标识；恢复时拒绝不同标识。
- 依赖由 Conan（C++）和 pnpm（前端）锁定，禁止隐式下载和全局 include/link 路径。日志用 spdlog，命令行用 CLI11，测试用 GoogleTest + CTest。
- 格式：C++ 用 `.clang-format`，前端用 Prettier；提交前运行 `pnpm run format`。

## 数据与交易安全

- 行情、历史数据只来自数据源；不导入本地 CSV/JSON 代替数据源，不生成冒充真实行情的数据。测试夹具仅用于测试。
- 实盘必须经过授权、账户风控和统一执行链；缺少任何一环就拒绝执行。
- 风险配置是模拟会话与回测的必填输入，缺失或插件不可用一律拒绝，不默认放行。
- 交易命令断线后不自动重发。
- 凭据只在用户本机输入或生成，不写进源码、日志或聊天；密码不持久化。
- 保护用户数据：不静默改写、迁移或删除账本、任务和数据集。

## 界面

- 视觉风格：极简、科技感、未来感，规范见 [docs/terminal.md](docs/terminal.md)。
- 行情页布局参照同花顺期货通；缺数据源的区域明确显示未接入，不伪造。
- 界面文案走 `src/i18n`，中英文同步；缺 key 会直接报错。

## 工作流程

- 日常开发直接在 `main` 分支进行；未经明确要求不创建其它开发分支。
- 端到端测试只能用 `pnpm run test:e2e`（隔离的 Agent 与测试 CTP SDK），不要直接运行 `playwright test`，否则会操作真实的本机服务。
- 修改 `core/`、`protocol/`、`plugins/`、`apps/services/`、`bindings/` 后，远程 Linux 服务包的源码指纹会变化；发布前按 [开发指南](docs/development.md) 重建。`pnpm desktop` 开发模式只警告。
- 不经要求不推送代码。
- 交付时说明实际实现、测试结果和未验证的范围。
