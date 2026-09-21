# ASTERION · 星枢

期货研究与交易综合终端，面向可组合、可扩展、默认本机可用的量化工作台。遵循 [架构设计 v2.1](docs/architecture.md)，目前已交付本机运行基础、数据管理（CSV / Tushare）与[固定版本单合约日线回测](docs/local-backtesting.md)。现有功能按[内置插件架构](docs/builtin-contributions.md)装配；已支持受信本地 Python 插件安装、独立进程和声明式视图；完整研究能力和交易仍在建设范围内。

已打通：React 终端提交 CSV → PostgreSQL 持久化任务 → 独立 Python worker 领取租约并在子进程校验 → API 确认不可变 Parquet 快照 → 市场工作区查看 K 线与来源。关闭页面不停止后台任务，重开后可查看任务结果。

开发原则：**核心提供机制，插件实现功能；官方功能也作为内置插件交付。只按当前最新架构编码，绝不为旧设计或旧代码做兼容。** AI 与贡献者开始工作须遵循 [AGENTS.md](AGENTS.md)。

## 日常启动（macOS）

打开“应用程序”中的 **Asterion Terminal.app**，或用 Spotlight 搜索 Asterion。应用直接打开原生桌面窗口。

- 首次启动自动初始化专用 PostgreSQL、数据目录及后台服务。
- 轻量包携带界面与项目代码；首次设置下载 Python 和依赖，Linux PostgreSQL 由 apt 安装，使用时无需开发工具。
- 自动连接本机服务，无需网页、命令行或令牌输入。
- 新窗口复用同一个后台。关闭窗口或退出应用后，已经开始的任务继续运行。
- 在独立“设置 → 本机服务”窗口中可停止或启动后台服务。
- 数据保存在 `~/Library/Application Support/me.asterion.terminal/`，不写进应用包或代码仓库。

桌面包支持 macOS 和 Linux 本机架构构建，包含数据管理和受限日线研究模型；多屏完整布局、完整研究和交易能力按 ROADMAP 继续实现。

## 工作台与设置

业务区采用左侧业务导航和顶部业务内标签、合约资料、主面板、检查器和底部任务面板。任务面板可收起或拖动调整高度；各面板独立滚动。页面不展示部署信息或开发里程碑。

`⌘1`–`⌘6` 切换业务工作区，`⌘J` 显示或收起任务面板。`⌘,` 或应用菜单“设置…”打开独立设置窗口，管理信息密度、涨跌颜色和本机服务。设置窗口不会更改当前业务工作区，重复打开会聚焦已有窗口。

## 构建桌面应用（开发者）

支持在 **macOS 构建 `.app`**、在 **Linux（Debian/Ubuntu）构建 `.deb`**，均为轻量安装包，仅携带界面、项目代码 wheel 和固定下载清单；首次点击“开始设置”后联网下载 Python 和依赖库；Linux PostgreSQL 17 由 apt 在安装应用时提供，macOS 使用单独下载的运行包。必须在目标系统和目标 CPU 架构上构建，不支持 macOS/Linux 交叉编译。

通用构建依赖：Python 3.12+、uv、Node.js 22+、pnpm 10.32.1 和 Rust stable。这些开发工具准备好后，在项目根目录执行：

```bash
uv sync --locked --group packaging
pnpm install --frozen-lockfile
uv run python scripts/build_desktop.py --smoke-test
```

构建入口会检查系统依赖。Debian/Ubuntu 缺包时列出安装内容并询问一次，确认后通过 `sudo apt-get` 安装；macOS 可通过 Homebrew 补齐 PostgreSQL 17。安装成功并复查依赖后继续构建，无需重新执行任务。依赖齐全时不会询问。

仅系统包安装使用 `sudo`，密码直接在终端输入；整个构建以普通用户运行。脚本不会自动添加软件源：若现有源不提供 PostgreSQL 17，会停止并提示配置 PGDG。`CI=true`、非交互终端或 `--no-install` 模式均不会询问或自动安装，缺失依赖时直接失败。uv、Node.js/pnpm、Rust、Homebrew 和 Apple Command Line Tools 需要预先安装。

构建成功后，最终 `.app` 或 `.deb` 会自动复制到项目根目录的 `release/`，文件名中的空格统一替换为下划线（如 `Asterion_Terminal_0.1.0_amd64.deb`），终端会显示完整路径；编译缓存仍保留在 Tauri 默认目录，`release/` 不纳入 Git。构建失败时保留打包现场，且不会更新 `release/` 中已有的产物。构建期间请勿删除 `target/` 或运行时目录。构建完成后可删除 `apps/terminal/src-tauri/target/` 释放空间，不影响 `release/` 中的产物；下次构建会重新编译。

`--smoke-test` 在临时数据库中验证首次安装后的后台、worker、CSV 导入和 Parquet 查询，成功后才生成安装包。不要以 root 身份运行构建或应用（PostgreSQL 拒绝 root 初始化）。

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
  postgresql-17 libpq-dev python3-dev
uv run python scripts/build_desktop.py --platform linux --smoke-test
```

系统依赖说明参考 [Tauri 官方文档](https://v2.tauri.app/start/prerequisites/)，PostgreSQL 软件源参考 [官方 Ubuntu 安装说明](https://www.postgresql.org/download/linux/ubuntu/)。

Linux `.deb` 声明 `postgresql-17` 依赖，使用 `sudo apt install ./release/Asterion_Terminal_0.1.0_amd64.deb` 安装时一并获取数据库及系统依赖。当前验证平台为 Debian 13；其他 Debian/Ubuntu 版本须有提供 PostgreSQL 17 的软件源，并满足桌面包的系统库要求。不要仅用 `dpkg -i` 安装而忽略依赖。应用使用 `/usr/lib/postgresql/17/bin` 中的程序，始终初始化自己的数据目录和端口，不接入系统默认集群。Linux 不再生成或下载自托管 PostgreSQL 附件。

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

首次设置使用原生 Rust 安装器下载固定版本 uv、Python（macOS 另下载 PostgreSQL），并用带哈希的锁定清单安装依赖。`--prepare-only` 生成轻量包输入；仅 macOS 另生成 `release/runtime/` 下的 PostgreSQL 附件；`--reuse-setup` 仅在代码、依赖与目标平台未改变时复用已有输入。`--smoke-test` 通过与桌面相同的安装器在 `.state/setup-smoke/` 建立独立环境，再以临时数据库验证后台。单独验证已安装环境：`uv run python scripts/smoke_desktop_runtime.py --runtime <运行环境目录>`。

macOS 发布时须把本次 `release/runtime/` 附件上传到本仓库对应 `v<版本号>` 的 GitHub Release，再分发桌面包；下载 URL 与 SHA-256 已写入包内清单。也可通过 `--runtime-release-url https://...` 指定发布目录。未上传附件的包不能完成首次联网安装，构建时的本地缓存验证不等于公开下载验收。

开发配置包括 `settings.json`（Python/Ruff、TypeScript、Rust 和 pytest）、`extensions.json`（推荐扩展）及 `launch.json`（调试入口）。首次打开工作区时安装推荐扩展，并运行 `uv sync --locked`、`pnpm install --frozen-lockfile`。

调试前，按 `config/local.env.example` 创建 `.env`，配置开发数据库和令牌，再执行 `uv run asterion init`。在“运行和调试”中选择 `Asterion: Backend` 同时调试 API 与 worker，也可分别启动。前端先在终端执行 `pnpm dev`，再选择 `Asterion: Web` 使用 Chrome 调试。测试可从 VS Code 的测试面板运行。

代码注释和文档字符串统一使用英文；产品界面文案按对应语言保留。

原生启动路径不依赖工作目录、`.env`、全局开发工具或代码仓库。

仅调试 Web 渲染层时可按 `config/local.env.example` 配置开发数据库和令牌，然后运行 `asterion init`、`asterion serve`、`asterion worker` 和 `pnpm dev`；此路径不属于产品启动流程。

## 数据源配置

正式产品仅提供真实数据源与本地文件导入。合成数据生成器仅位于测试目录，用于开发与自动化验证，不随桌面应用发布，也没有产品启用开关。

「设置 → 数据源」根据各插件声明显示字段：Tushare 使用 Token。可以先测试草稿，再保存；测试不会应用配置。保存有修订检查，其他窗口已修改时须刷新后重新应用。每个任务固定提交时配置，后续修改或移除凭据只影响新任务；已有任务如需停止，使用任务取消。秘密不回显，也不写入任务或数据文件。

### Tushare 数据同步

在「设置 → 数据源」填写自己的 Tushare Pro Token 并保存配置，再进入「数据 → 数据同步」选择期货合约资料、交易日历或实际合约历史日线。任务中心显示进度，可取消或重新同步；成功后在「数据集」查看所有来源的版本化结果，日线也可打开历史图表。本地文件导入使用数据同步工具栏的独立「导入数据」按钮，合约基础信息统一在「合约资料」查看。

当前支持插件化手动同步、分段证据查询和断点续传。日线与日历的标准数据跨任务累积为固定版本，重叠数据按采集时间处理修订，旧版本可继续查询；合约资料与文件数据保留各次采集范围。日线版本详情可固定日历与合约资料核对覆盖，并手动提交确认缺口的补齐任务。尚未交付自动补齐调度、实时行情及完整交易规则。Token 加密保存在本机，不写入任务或数据文件；接口访问取决于账号权限。设计、数据语义与边界详见 [数据源插件](docs/data-providers.md)。

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

## 架构与参与开发

| 文档 | 阅读目的 |
|---|---|
| [期货领域基线](docs/futures-domain.md) | 最高优先级：实际合约、主力、连续、规则、持仓结算与换月的职责和验收门槛 |
| [总体架构](docs/architecture.md) | 产品定位、模块边界、状态归属、运行角色与实施取舍 |
| [插件系统](docs/plugin-system.md) | 扩展点、版本协议、权限、依赖隔离、固定包安装、依赖、停用与 SDK 契约 |
| [终端体验](docs/terminal-experience.md) | 可组合面板、对象与命令、简单配置、布局恢复与验收 |
| [数据生命周期](docs/data-lifecycle-design.md) | 固定版本、来源证据、加工、引用保护与回收 |
| [开源准备](docs/open-source.md) | SDK、示例、当前契约、社区贡献和发布门槛 |
| [贡献指南](CONTRIBUTING.md) | 当前可用的开发入口、检查命令和改动要求 |

设计文档明确区分当前实现与未来目标。当前支持内置受信插件与显式信任的本地 Python 插件安装、独立进程、数据源和声明式表格。源码包含公开 SDK 与独立样例；尚未提供恶意代码 OS 沙箱或在线市场。开源许可证与正式分发政策仍待确定。

本机数据与研究记录的备份范围、隔离恢复、保护备份后切换与回滚步骤及限制见 [备份与恢复](docs/backup-and-restore.md)。

研究数据准备：在「数据 → 数据同步」选择实际合约日线，可同时准备同连接的交易日历与合约资料；完成后按该批固定版本核对覆盖。用法与边界见 [数据源](docs/data-providers.md)。


## 本地插件开发

在“设置 → 插件”安装 ZIP，核对内容后启用。来源插件使用现有“数据源”配置流程，声明式表格显示在“扩展”工作区。插件以当前用户权限运行，仅启用信任的代码。

```sh
uv run python -m asterion_plugin_sdk pack examples/plugins/calendar-source /tmp/calendar-source.zip
```

[独立样例](examples/plugins/calendar-source/README.md)不生成行情，数据由你配置的真实 HTTPS 接口提供。可将该目录复制到独立仓库；开发插件无需修改终端核心。完整契约见[插件系统](docs/plugin-system.md)。
