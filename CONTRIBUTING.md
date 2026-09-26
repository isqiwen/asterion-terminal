# 参与开发

Asterion 正在从数据终端演进为可组合的期货量化终端。当前功能和缺口见 [ROADMAP](docs/ROADMAP.md)，模块规则见 [架构设计](docs/architecture.md)。项目仍在开源准备期，LICENSE、贡献授权和公开发布政策尚待维护者确定，本文不授予额外的使用或再分发许可。

## 必须遵守的开发原则

开始工作先阅读 [AGENTS.md](AGENTS.md)。**固定内核，有限扩展。** 期货核心概念与定义属于内核（Rust，静态编译，不可替换）；扩展点只有内置 Rust 数据源适配、内置 Rust 券商接入和 Python 策略包。后端除研究/回测计算与策略外全部用 Rust。易变的是带版本和来源证据的事实数据，不是期货定义。本项目只按当前最新架构与设计开发，绝不为旧设计、旧代码、旧协议或旧格式保留兼容逻辑。契约改变时同步更新调用方、测试与文档，直接删除被替代的实现。旧代码和旧测试不能凌驾于最新设计；不兼容原则不授权删除用户数据。

## 选择改动入口

| 想做什么 | 当前入口 |
|---|---|
| 数据源接入 | 内置 Rust 适配（目标 `services/adapters/`）；现有 `src/asterion/data/providers/` 待迁移 |
| 数据类型、校验和发布 | `src/asterion/data/types/`、`library.py`、`sync.py` |
| 终端面板和交互 | `presentation/panels/*/src/module.tsx`：业务贡献；`presentation/`：共享表现基础 |
| 桌面与后台生命周期 | `apps/terminal/src-tauri/`、`src/asterion/runtime/desktop.py` |
| 协议及领域设计 | `docs/`、`tests/` 中相应契约与不变量 |

现有内置功能仍经 Python 内置插件宿主装配（[待移除的现状](docs/builtin-contributions.md)），迁移中不要在其上新增扩展点。外部开发者只能开发 Python 策略包：使用随源码提供的 `asterion_plugin_sdk` 打包 ZIP，通过终端安装并明确启用，协议见 [策略插件契约](docs/plugin-system.md)，样例见 [策略样例](examples/plugins/close-momentum/README.md)。新增数据源或券商接入是新增内置 Rust 模块，不接受外部包。SDK 尚未发布到公共包索引，不承诺旧协议兼容。

## 本地开发

准备 Python 3.12+、uv、Node.js 22+ 和仓库声明的 pnpm 10.32.1。后台开发也需要 Rust stable，以构建当前 PyO3 绑定；不必先构建桌面安装包，原生桌面另需平台依赖，见 [README](README.md)。

```bash
uv sync --locked
pnpm install --frozen-lockfile
uv run ruff check src tests scripts bindings/python/asterion_bindings
uv run ruff format --check src tests scripts bindings/python/asterion_bindings
cargo test --workspace --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
uv run python scripts/generate_domain_models.py --check
uv run python scripts/generate_task_contract.py --check
uv run pyright
uv run pytest -q
pnpm test
pnpm build
```

Python 测试大部分使用临时 SQLite；真实并发测试只有配置 `ASTERION_TEST_DATABASE_URL` 才运行。请使用可重建的独立测试 PostgreSQL，不能指向个人业务数据库；未配置导致的跳过不代表 PostgreSQL 验证通过。

需要手动启动服务、界面或端到端测试时，按[开发步骤](docs/development.md#vs-code-开发)设置本地开发环境，再启动 `uv run asterion init`、`uv run asterion serve --port 8001`、`.venv/bin/asterion-server --listen 127.0.0.1:8000 --upstream http://127.0.0.1:8001`、`uv run asterion worker` 与 `pnpm dev`（VS Code 的“Asterion: Backend”一并启动前三者）。示例数据必须明确标注合成；不要求真实数据源 Token 才能复现基础问题。

API 变更须同步生成契约，禁止手工改生成类型：

```bash
uv run python scripts/export_schema.py
pnpm --filter @asterion/terminal exec openapi-typescript ../../docs/openapi.json -o ../../bindings/typescript/src/schema.d.ts
```

浏览器测试运行方式、隔离测试数据库及打包冒烟见[开发验证](docs/development.md#验证)和[桌面运行](docs/desktop-runtime.md)。根据改动运行相关验证，公共协议和任务发布变更需覆盖相应集成测试。

## 保持边界

- 跨业务模块使用公开接口；新数据源负责传输与映射，不直接写目录、任务或凭据文件。
- 长计算在 worker；窗口和渲染组件只提交命令、查询和展示状态。
- 使用不可变输入、命令幂等与租约约束；不以覆盖旧文件修复历史结果。
- UI 复用 [视觉规范](docs/ui-style.md) 的 token；按钮反馈区分提交、执行与完成，关键状态有文字说明。
- 默认日志和测试夹具不得包含真实凭据、账户或受限制数据；设计文档中的未来能力明确标为目标。

## 提交可评审的改动

说明用户遇到的问题、最终行为、验证结果和未覆盖范围。涉及持久化契约、插件权限、公共协议或交易规则时补充简短设计、破坏性变更范围与数据影响说明。测试围绕失败恢复、数据正确性和外部行为，不复制实现细节。

保留无关文件和用户数据；不在功能 PR 中顺手重排全仓库。文档链接应可用，已交付项更新 ROADMAP，未来设计不勾选完成。社区治理、许可证和发布准备见 [开源准备](docs/open-source.md)。

外部策略包使用固定 ZIP、公开 Python SDK 和独立进程，接入步骤见 [策略插件契约](docs/plugin-system.md)。该运行方式不构成恶意代码 OS 沙箱。
