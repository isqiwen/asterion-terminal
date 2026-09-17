# ASTERION · 星枢

期货研究与交易综合终端。遵循 [架构设计 v1.1](docs/architecture.md)，目前交付的是 **M0 基础与 M1 数据切片（CSV / Tushare）**，不是完成版交易系统。

已打通：React 终端提交 CSV → PostgreSQL 持久化任务 → 独立 Python worker 领取租约并在子进程校验 → API 确认不可变 Parquet 快照 → 市场工作区查看 K 线与来源。关闭页面不停止后台任务，重开后可查看任务结果。

## 日常启动（macOS）

打开“应用程序”中的 **Asterion Terminal.app**，或用 Spotlight 搜索 Asterion。应用直接打开原生桌面窗口。

- 首次启动自动初始化专用 PostgreSQL、数据目录及后台服务。
- Python、PostgreSQL 和界面都包含在应用包内，使用时无需安装开发工具。
- 自动连接本机服务，无需网页、命令行或令牌输入。
- 新窗口复用同一个后台。关闭窗口或退出应用后，已经开始的任务继续运行。
- 在独立“设置 → 本机服务”窗口中可停止或启动后台服务。
- 数据保存在 `~/Library/Application Support/me.asterion.terminal/`，不写进应用包或代码仓库。

桌面包支持 macOS 和 Linux 本机架构构建，仍是数据功能基础版；多屏完整布局、研究和交易能力按 ROADMAP 继续实现。

## 工作台与设置

业务区采用左侧业务导航和顶部业务内标签、合约资料、主面板、检查器和底部任务面板。任务面板可收起或拖动调整高度；各面板独立滚动。页面不展示部署信息或开发里程碑。

`⌘1`–`⌘6` 切换业务工作区，`⌘J` 显示或收起任务面板。`⌘,` 或应用菜单“设置…”打开独立设置窗口，管理信息密度、涨跌颜色和本机服务。设置窗口不会更改当前业务工作区，重复打开会聚焦已有窗口。

## 构建桌面应用（开发者）

支持在 **macOS 构建 `.app`**、在 **Linux（Debian/Ubuntu）构建 `.deb`**，均内置 Python 后台和 PostgreSQL 17。必须在目标系统和目标 CPU 架构上构建，不支持 macOS/Linux 交叉编译。

通用构建依赖：Python 3.12+、uv、Node.js 22+、pnpm 10.32.1 和 Rust stable。这些开发工具准备好后，在项目根目录执行：

```bash
uv sync --locked --group packaging
pnpm install --frozen-lockfile
uv run python scripts/build_desktop.py --smoke-test
```

构建入口会检查系统依赖。Debian/Ubuntu 缺包时列出安装内容并询问一次，确认后通过 `sudo apt-get` 安装；macOS 可通过 Homebrew 补齐 PostgreSQL 17。安装成功并复查依赖后继续构建，无需重新执行任务。依赖齐全时不会询问。

仅系统包安装使用 `sudo`，密码直接在终端输入；整个构建以普通用户运行。脚本不会自动添加软件源：若现有源不提供 PostgreSQL 17，会停止并提示配置 PGDG。`CI=true`、非交互终端或 `--no-install` 模式均不会询问或自动安装，缺失依赖时直接失败。uv、Node.js/pnpm、Rust、Homebrew 和 Apple Command Line Tools 需要预先安装。

构建成功后，最终 `.app` 或 `.deb` 会自动复制到项目根目录的 `release/`，文件名中的空格统一替换为下划线（如 `Asterion_Terminal_0.1.0_amd64.deb`），终端会显示完整路径；编译缓存仍保留在 Tauri 默认目录，`release/` 不纳入 Git。构建失败时保留打包现场，且不会更新 `release/` 中已有的产物。构建期间请勿删除 `target/` 或运行时目录。构建完成后可删除 `apps/terminal/src-tauri/target/` 释放空间，不影响 `release/` 中的产物；下次构建会重新编译。

`--smoke-test` 在临时数据库中验证冻结后的后台、worker、CSV 导入和 Parquet 查询，成功后才生成安装包。不要以 root 身份运行构建或应用（PostgreSQL 拒绝 root 初始化）。

### macOS

```bash
xcode-select --install
brew install postgresql@17
uv run python scripts/build_desktop.py --platform macos --smoke-test
```

自动通过 `brew --prefix` 定位 PostgreSQL 17，兼容 Apple Silicon 和 Intel Mac；自定义安装路径可设置 `ASTERION_PG_SOURCE`。产物：

```text
release/Asterion_Terminal.app
```

应用使用本地 ad-hoc 签名；面向公众分发仍需 Developer ID 签名及公证。

### Linux（Debian/Ubuntu）

可直接运行构建并按提示安装依赖；也可手动安装（以下命令适用于 Debian 13；Ubuntu 若仓库没有 PostgreSQL 17，需先配置 PostgreSQL 官方 PGDG 软件源）：

```bash
sudo apt update
sudo apt install build-essential pkg-config libwebkit2gtk-4.1-dev \
  libayatana-appindicator3-dev librsvg2-dev libssl-dev \
  patchelf postgresql-17 libpq-dev python3-dev
uv run python scripts/build_desktop.py --platform linux --smoke-test
```

系统依赖说明参考 [Tauri 官方文档](https://v2.tauri.app/start/prerequisites/)，PostgreSQL 软件源参考 [官方 Ubuntu 安装说明](https://www.postgresql.org/download/linux/ubuntu/)。

多个 PostgreSQL 版本共存时，用 `--pg-config /path/to/pg_config` 或 `PG_CONFIG` 指定 PostgreSQL 17 的配置工具。打包会检查版本、复制服务器和动态库，并保留 PostgreSQL 所需的相对目录结构；用户无需另行安装 PostgreSQL 或 Python。

```text
release/*.deb
```

```bash
sudo apt install ./release/*.deb
```

安装后从应用菜单打开 Asterion Terminal。运行环境需提供 WebKitGTK 4.1 和可用的 **systemd 用户会话**（`systemctl --user`）；这些通常由常规 Linux 桌面提供。服务在首次打开应用时启动，退出窗口后继续工作，可在设置中停止；不主动启用登录自启动或用户 linger。日志为应用数据目录的 `service.log`，数据默认在 `~/.local/share/me.asterion.terminal/`（遵循 `XDG_DATA_HOME`）。

Linux 启动时若检测到 NVIDIA 内核驱动，应用会自动启用 WebKitGTK 的 DMABUF 兼容设置，避免原生窗口已打开但内容白屏；显式设置的 `WEBKIT_DISABLE_DMABUF_RENDERER` 环境变量会优先保留。该处理参考 [Tauri Linux 图形兼容说明](https://v2.tauri.app/develop/debug/linux-graphics/)。

Linux 主窗口、设置和独立窗口统一使用深色标题栏，支持拖动、双击最大化、边缘缩放及最小化/最大化/关闭，不显示窗口内的默认菜单。macOS 保留原生标题栏和系统菜单。

Linux 包与构建机的 CPU 架构、glibc 版本相关，应在计划支持的最旧系统上构建；本项目不承诺跨所有发行版通用。当前交付 `.deb`，不包含 AppImage/RPM。

### VS Code 开发

项目已提供 `.vscode/tasks.json`。先安装基础开发工具，并确保 VS Code 的 PATH 能找到 `uv`、`pnpm`、`cargo`；其余系统包由构建入口检查并引导安装。

仅保留一个默认任务 `Asterion: Build`。按 **Ctrl+Shift+B**（macOS 为 **Cmd+Shift+B**），依次安装锁定依赖、检查并引导安装系统依赖、验证后台并构建当前系统的桌面包；自动识别 macOS/Linux，无需选择平台。

需要单独验证后台时，在终端运行 `uv run python scripts/smoke_desktop_runtime.py`。构建脚本的 `--prepare-only` 仅生成运行时；`--skip-runtime` 复用同平台、同架构的运行时，仅在未修改 Python 后台时使用。

开发配置包括 `settings.json`（Python/Ruff、TypeScript、Rust 和 pytest）、`extensions.json`（推荐扩展）及 `launch.json`（调试入口）。首次打开工作区时安装推荐扩展，并运行 `uv sync --locked`、`pnpm install --frozen-lockfile`。

调试前，按 `config/local.env.example` 创建 `.env`，配置开发数据库和令牌，再执行 `uv run asterion init`。在“运行和调试”中选择 `Asterion: Backend` 同时调试 API 与 worker，也可分别启动。前端先在终端执行 `pnpm dev`，再选择 `Asterion: Web` 使用 Chrome 调试。测试可从 VS Code 的测试面板运行。

代码注释和文档字符串统一使用英文；产品界面文案按对应语言保留。

原生启动路径不依赖工作目录、`.env`、全局开发工具或代码仓库。

仅调试 Web 渲染层时可按 `config/local.env.example` 配置开发数据库和令牌，然后运行 `asterion init`、`asterion serve`、`asterion worker` 和 `pnpm dev`；此路径不属于产品启动流程。

## Tushare 数据同步

在「设置 → 数据源」保存自己的 Tushare Pro Token，再进入「数据 → 数据同步」选择期货合约资料、交易日历或实际合约历史日线。任务中心显示进度，可取消或重新同步；成功后在「数据集」查看所有来源的版本化结果，日线也可打开历史图表。本地文件导入位于数据源选择中的「本地文件」，合约基础信息统一在「合约资料」查看。

当前是插件化的手动历史数据同步，尚未交付自动增量、实时行情及完整交易规则。Token 加密保存在本机，不写入任务或数据文件；接口访问取决于账号权限。设计、数据语义与边界详见 [数据源插件](docs/data-providers.md)。

## CSV 数据契约

```csv
contract,event_time,available_at,trading_day,open,high,low,close,volume
SHFE.rb2610,2026-09-14T13:00:00Z,2026-09-14T13:01:00Z,2026-09-15,3200,3220,3190,3210,100
```

**以上是合成格式示例，不是实际市场数据。** CSV 入口支持明确时间粒度的 OHLCV 文件；Tushare 在线同步通过独立的数据源插件接入。来源字段建议填写提供方、频率和文件版本。最多 2 MB；交易日由输入提供，不以自然日推算夜盘归属。

合约标识必须符合交易所和合约代码格式，拒绝主连标识；尚未与权威合约目录核对是否真实上市。价格以 Decimal/Parquet decimal128(20,8) 保存；校验 OHLC、重复主键、时区及 available_at；v1 的 event_time 须为整秒。图表使用中国市场红涨绿跌，目前显示快照前 1,000 行。

## 验证

```bash
uv run ruff check src tests scripts
uv run ruff format --check src tests scripts
uv run pyright
uv run pytest -q
pnpm build
pnpm test
```

真实 PostgreSQL 并发测试使用**隔离的测试数据库**，勿指定已有业务库：

```bash
ASTERION_TEST_DATABASE_URL=postgresql+psycopg://USER:PASS@localhost/asterion_test uv run pytest tests/test_postgres.py -q
```

启动 serve/worker 后，可用 `pnpm --filter @asterion/terminal exec playwright install chromium` 安装测试浏览器，再设置与后台相同的 `ASTERION_TOKEN` 运行 `pnpm --filter @asterion/terminal test:e2e`。测试导入标注为合成数据的两根 K 线，验证发布、图表与刷新后任务恢复。

更新 API 类型：

```bash
uv run python scripts/export_schema.py
pnpm --filter @asterion/terminal exec openapi-typescript ../../docs/openapi.json -o src/api/schema.d.ts
```

## 工程边界与进度

业务代码位于一个 `asterion` distribution 内。`serve` 持有任务及发布目录，worker 只通过 HTTP 领取和提交；worker 不直连目录数据库。SQLite 仅用于单元测试，生产配置默认 PostgreSQL。当前尚未实现交易节点，因此没有生产节点 SQLite。

[实施进度及未完成项](docs/ROADMAP.md) · [当前接口与发布协议](docs/protocols.md)
