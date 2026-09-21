# 参与开发

Asterion 正在从数据终端演进为可组合的期货量化终端。当前功能和缺口见 [ROADMAP](docs/ROADMAP.md)，模块规则见 [架构设计](docs/architecture.md)。项目仍在开源准备期，LICENSE、贡献授权和公开发布政策尚待维护者确定，本文不授予额外的使用或再分发许可。

## 必须遵守的开发原则

开始工作先阅读 [AGENTS.md](AGENTS.md)。**核心提供机制，插件实现功能；官方自研功能同样作为内置插件，通过相同功能契约接入。** 易变规则、算法、数据源、业务配置与面板属于插件；核心只管理通用生命周期、调度、授权、事务和宿主能力。新增功能默认改插件，修改核心必须说明通用机制需求；不得在核心添加具体插件的特殊分支。本项目只按当前最新架构与设计开发，绝不为旧设计、旧代码、旧协议或旧格式保留兼容逻辑。契约改变时同步更新调用方、测试与文档，直接删除被替代的实现。旧代码和旧测试不能凌驾于最新设计；不兼容原则不授权删除用户数据。

## 选择改动入口

| 想做什么 | 当前入口 |
|---|---|
| 数据源接入 | `src/asterion/data/providers/`：Provider 协议、可信内置注册 |
| 数据类型、校验和发布 | `src/asterion/data/types/`、`library.py`、`sync.py` |
| 终端面板和交互 | `apps/terminal/src/plugins/*/plugin.tsx`：业务贡献；`workspace/`、`extensions/`：通用宿主 |
| 桌面与后台生命周期 | `apps/terminal/src-tauri/`、`src/asterion/runtime/desktop.py` |
| 协议及领域设计 | `docs/`、`tests/` 中相应契约与不变量 |

当前内置功能按发行清单装配，接入方式见 [机制核心与内置插件](docs/builtin-contributions.md)。外部开发者可使用随源码提供的 `asterion_plugin_sdk`，开发受信本地 Python 数据源、研究策略和声明式表格，打包 ZIP 后通过终端安装并明确启用，无需重构建宿主。当前协议、限制和打包命令见 [插件契约](docs/plugin-system.md)；可复制 [数据源样例](examples/plugins/calendar-source/README.md) 或 [策略样例](examples/plugins/close-momentum/README.md)。SDK 尚未独立发布到公共包索引，不承诺旧协议兼容；任意 React 面板、原生动态插件和实盘网关的外部接入尚未开放。仅安装一个 Python 包不会自动登记为终端插件。

## 本地开发

准备 Python 3.12+、uv、Node.js 22+ 和仓库声明的 pnpm 10.32.1。普通前后端开发无需先构建桌面安装包；原生构建另需 Rust 与平台依赖，见 [README](README.md)。

```bash
uv sync --locked
pnpm install --frozen-lockfile
uv run ruff check src tests scripts
uv run ruff format --check src tests scripts
uv run pyright
uv run pytest -q
pnpm test
pnpm build
```

Python 测试大部分使用临时 SQLite；真实并发测试只有配置 `ASTERION_TEST_DATABASE_URL` 才运行。请使用可重建的独立测试 PostgreSQL，不能指向个人业务数据库；未配置导致的跳过不代表 PostgreSQL 验证通过。

需要手动启动服务、界面或端到端测试时，按 [README 的开发步骤](README.md#vs-code-开发) 设置本地开发环境，再启动 `uv run asterion init`、`uv run asterion serve`、`uv run asterion worker` 与 `pnpm dev`。示例数据必须明确标注合成；不要求真实数据源 Token 才能复现基础问题。

API 变更须同步生成契约，禁止手工改生成类型：

```bash
uv run python scripts/export_schema.py
pnpm --filter @asterion/terminal exec openapi-typescript ../../docs/openapi.json -o src/api/schema.d.ts
```

浏览器测试运行方式、隔离测试数据库及打包冒烟见 [README 验证](README.md#验证) 和 [桌面运行](docs/desktop-runtime.md)。根据改动运行相关验证，公共协议和任务发布变更需覆盖相应集成测试。

## 保持边界

- 跨业务模块使用公开接口；新数据源负责传输与映射，不直接写目录、任务或凭据文件。
- 长计算在 worker；窗口和渲染组件只提交命令、查询和展示状态。
- 使用不可变输入、命令幂等与租约约束；不以覆盖旧文件修复历史结果。
- UI 复用 [视觉规范](docs/ui-style.md) 的 token；按钮反馈区分提交、执行与完成，关键状态有文字说明。
- 默认日志和测试夹具不得包含真实凭据、账户或受限制数据；设计文档中的未来能力明确标为目标。

## 提交可评审的改动

说明用户遇到的问题、最终行为、验证结果和未覆盖范围。涉及持久化契约、插件权限、公共协议或交易规则时补充简短设计、破坏性变更范围与数据影响说明。测试围绕失败恢复、数据正确性和外部行为，不复制实现细节。

保留无关文件和用户数据；不在功能 PR 中顺手重排全仓库。文档链接应可用，已交付项更新 ROADMAP，未来设计不勾选完成。社区治理、许可证和发布准备见 [开源准备](docs/open-source.md)。

内置数据源、任务、命令和面板的接入方式见 [内置扩展登记与模块边界](docs/builtin-contributions.md)。内置功能使用受信静态装配；本地外部插件使用固定 ZIP、公开 Python SDK 和独立进程，接入步骤见 [当前插件契约](docs/plugin-system.md) 与 [独立样例](examples/plugins/calendar-source/README.md)。该运行方式不构成恶意代码 OS 沙箱。
