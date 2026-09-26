# ASTERION · 星枢

本机优先的期货研究与交易终端，目标是分钟/日线研究、实时行情、稳定模拟与实盘。当前已有数据管理、有限单合约日线研究、实时行情与只读账户切片；正式交易、连续加工和分钟回测尚未完成。详细范围见 [当前进度](docs/ROADMAP.md)。

开发遵守 [AGENTS.md](AGENTS.md)：期货核心概念与定义属于内核，用 Rust 实现、静态编译，不能通过插件或配置替换；扩展点只有内置 Rust 数据源适配、内置 Rust 券商接入和 Python 策略包。后端除研究/回测计算与策略外全部使用 Rust，界面为 TypeScript。[架构](docs/architecture.md)与[工程结构](docs/engineering-structure.md)是目标设计；现有 Python 后端、内置插件宿主与外部数据源扩展点尚待迁移或删除，进度见 [ROADMAP](docs/ROADMAP.md)。不保留旧契约兼容路径。

## 使用

- 安装当前平台的桌面包，打开 Asterion Terminal，按首次设置完成本机环境与账户初始化。
- macOS 使用 DMG 内的安装器，Linux 使用 DEB；Windows 运行支持尚未完成。
- 多窗口共用后台。关闭窗口后，已提交后台任务继续；可在“设置 → 本机服务”管理后台。
- 数据与研究记录位于用户应用数据目录，不随应用文件覆盖删除。

安装、更新、数据位置及故障边界见 [桌面运行](docs/desktop-runtime.md)。数据源、文件导入、行情连接和研究分别见[文档导航](docs/README.md)。

## 开发

先准备 Python 3.12+、uv、Node.js 22+、pnpm 10.32.1 和 Rust stable，在项目根执行：

~~~bash
uv sync --locked --group packaging
pnpm install --frozen-lockfile
uv run python scripts/build_desktop.py --smoke-test
~~~

目标系统依赖、VS Code 调试、隔离测试数据库、类型生成和打包选项统一见 [构建与开发](docs/development.md)。提交要求见 [贡献指南](CONTRIBUTING.md)。

## 文档入口

| 问题 | 入口 |
|---|---|
| 架构、依赖与目录 | [架构](docs/architecture.md) · [工程结构](docs/engineering-structure.md) |
| 现在能做什么、下一步做什么 | [进度](docs/ROADMAP.md) |
| 期货语义与验收门槛 | [领域基线](docs/futures-domain.md) |
| 当前 API、插件与任务协议 | [协议入口](docs/protocols.md) |
| 用法与领域专题 | [文档导航](docs/README.md) |
| 做过哪些实际验证 | [验收索引](docs/VALIDATION.md) |

用户策略包通过“设置 → 插件”安装并启用，只运行信任的代码；独立进程不等于恶意代码沙箱。开发示例见 [策略插件](examples/strategies/close-momentum/README.md)，完整契约见 [策略插件契约](docs/plugin-system.md)。许可证与公众发布准备见 [开源准备](docs/open-source.md)。
