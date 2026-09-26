# 构建与开发

本文记录当前仓库的构建、调试和验证入口；目标目录已部分落地，见[工程结构](engineering-structure.md)。日常安装和后台行为见[桌面运行](desktop-runtime.md)。

## 构建桌面应用

支持在 **macOS 构建单个 `.dmg`**、在 **Linux（Debian/Ubuntu）构建单个 `.deb`**，均为轻量安装包，仅携带界面、项目代码 wheel 和固定下载清单；首次点击“开始设置”后联网下载 Python 和依赖库；Linux PostgreSQL 17 由 apt 在安装应用时提供，macOS 在首次设置中复用或通过 Homebrew 安装 `postgresql@17`。必须在目标系统和目标 CPU 架构上构建，不支持 macOS/Linux 交叉编译。

通用构建依赖：Python 3.12+、uv、Node.js 22+、pnpm 10.32.1 和 Rust stable（至少 1.88）。这些开发工具准备好后，在项目根目录执行：

```bash
uv sync --locked --group packaging
pnpm install --frozen-lockfile
uv run python scripts/build_desktop.py --smoke-test
```

构建入口会检查系统依赖。Debian/Ubuntu 缺包时列出安装内容并询问一次，确认后通过 `sudo apt-get` 安装；macOS 可通过 Homebrew 补齐 PostgreSQL 17。安装成功并复查依赖后继续构建，无需重新执行任务。依赖齐全时不会询问。

仅系统包安装使用 `sudo`，密码直接在终端输入；整个构建以普通用户运行。脚本不会自动添加软件源：若现有源不提供 PostgreSQL 17，会停止并提示配置 PGDG。`CI=true`、非交互终端或 `--no-install` 模式均不会询问或自动安装，缺失依赖时直接失败。uv、Node.js/pnpm、Rust、Homebrew 和 Apple Command Line Tools 需要预先安装。

各平台只交付一种用户安装包：Linux `.deb`、macOS `.dmg`、Windows NSIS `.exe`（不是裸应用可执行文件）。Windows 目前仅约定打包格式，运行时安装、后台服务和实机验证尚未实现，构建入口明确拒绝 Windows。

构建成功后，最终 `.dmg` 或 `.deb` 会自动复制到项目根目录的 `release/`，文件名中的空格统一替换为下划线（如 `Asterion_Terminal_0.1.0_amd64.deb`），终端会显示完整路径。macOS DMG 内只提供系统安装器 `.pkg`，将应用固定安装到 `/Applications/Asterion Terminal.app`；相同包标识始终覆盖该路径，不按版本创建副本。`.app` 仅为构建中间产物，不导出到 `release/`；制作安装器前签名，导出前解开镜像内安装器并验证应用签名与镜像完整性。编译缓存仍保留在 Tauri 默认目录，`release/` 不纳入 Git。构建失败时保留打包现场，且不会更新 `release/` 中已有的产物。构建期间请勿删除 `target/` 或运行时目录。构建完成后可删除 `apps/terminal/src-tauri/target/` 释放空间，不影响 `release/` 中的产物；下次构建会重新编译。

`--smoke-test` 在临时数据库中验证首次安装后的后台、worker、CSV 导入和 Parquet 查询，成功后才生成安装包。不要以 root 身份运行构建或应用（PostgreSQL 拒绝 root 初始化）。

### macOS

```bash
xcode-select --install
brew install postgresql@17
uv run python scripts/build_desktop.py --platform macos --smoke-test
```

使用当前架构的标准 Homebrew 安装位置：Apple Silicon 为 `/opt/homebrew`，Intel 为 `/usr/local`；PostgreSQL 固定读取 `opt/postgresql@17`，不将构建机器上的 Cellar 小版本路径写入安装包。只有冒烟验证需要构建机安装 PostgreSQL，生成安装输入不打包数据库。产物：

```text
release/Asterion_Terminal_0.1.0_aarch64.dmg
```

上例为 Apple Silicon，Intel 文件名中的架构为 `x64`。打开 DMG，双击其中的安装器。安装器会请求系统安装权限，关闭标准安装路径下正在运行的应用，并覆盖同一位置；不会提供“保留两者”或另选版本目录。最近一次成功安装的包生效，包括重新安装较低版本号的包；不会按版本号保留多套应用。应用使用本地 ad-hoc 签名；面向公众分发仍需 Developer ID 签名及公证。

### Linux（Debian/Ubuntu）

可直接运行构建并按提示安装依赖；也可手动安装（以下命令适用于 Debian 13；Ubuntu 若仓库没有 PostgreSQL 17，需先配置 PostgreSQL 官方 PGDG 软件源）：

```bash
sudo apt update
sudo apt install build-essential pkg-config libwebkit2gtk-4.1-dev \
  libayatana-appindicator3-dev librsvg2-dev libssl-dev \
  postgresql-17 python3-dev
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

首次设置使用原生 Rust 安装器下载固定版本 uv、Python，并用带哈希的锁定清单安装应用专用依赖。数据库由系统包管理器提供：Linux 为 apt 的 `postgresql-17`；macOS 为 Homebrew 的 `postgresql@17`。macOS 缺少 Homebrew 时显示 https://brew.sh 的官方安装指引，用户完成后重试；已有 Homebrew 时由“开始设置”触发数据库安装，不运行 `brew services start`，也不调用 `sudo`。`--prepare-only` 只生成轻量包输入，`--reuse-setup` 仅在代码、依赖与目标平台未改变时复用已有输入。`--smoke-test` 通过与桌面相同的安装器在 `.state/setup-smoke/` 建立独立环境，再以临时数据库验证后台。单独验证已安装环境：`uv run python scripts/smoke_desktop_runtime.py --runtime <运行环境目录>`。

不生成 PostgreSQL 发布附件；Python 与 uv 从官方上游获取，数据库由包管理工具校验和维护。Homebrew 安装及系统授权由用户按官方步骤完成，不自动执行远程 shell 安装脚本。Windows 获取数据库和后台适配仍待实现。

开发配置包括 `settings.json`（Python/Ruff、TypeScript、Rust 和 pytest）、`extensions.json`（推荐扩展）及 `launch.json`（调试入口）。首次打开工作区时安装推荐扩展，并运行 `uv sync --locked`、`pnpm install --frozen-lockfile`。

调试前，按 `config/local.env.example` 创建 `.env`，配置开发数据库和令牌，再执行 `uv run asterion init`。在“运行和调试”中选择 `Asterion: Backend` 同时启动 Rust 入口（需 CodeLLDB 扩展）、内部 API 与 worker，也可分别启动。前端先在终端执行 `pnpm dev`，再选择 `Asterion: Web` 使用 Chrome 调试。测试可从 VS Code 的测试面板运行。

代码注释和文档字符串统一使用英文；产品界面文案按对应语言保留。

原生启动路径不依赖工作目录、`.env`、全局开发工具或代码仓库。

仅调试 Web 渲染层时可按 `config/local.env.example` 配置开发数据库和令牌，然后运行 `asterion init`、`asterion serve --port 8001`、`.venv/bin/asterion-server --listen 127.0.0.1:8000 --upstream http://127.0.0.1:8001`（入口不读取 `.env`，需在环境中设置与 Python 进程相同的 `ASTERION_TOKEN`、`ASTERION_DATABASE_URL`、`ASTERION_DATA_ROOT`；可选 `ASTERION_ACCOUNT_VERIFICATION`（默认 `local`）、`ASTERION_REQUIRE_ACCOUNT`（`true`/`false`，默认 `false`，须与 Python 进程一致）和 `ASTERION_LEASE_SECONDS`（默认 60）。入口使用 Python 初始化创建的任务与事件表，先执行 `asterion init`）、`asterion worker` 和 `pnpm dev`；此路径不属于产品启动流程。界面和 worker 只访问 Rust 入口（8000）：入口负责 CORS、请求鉴权、账号（注册、登录、会话和终端锁）、任务队列操作、事件重放与作用域凭证，只把其余已授权请求连同转发凭据、调用方身份和账户状态转给内部端口上的 Python 进程；Python 不再接受客户端令牌，直接访问内部端口会被拒绝。


## 验证

```bash
uv run ruff check src tests scripts bindings/python/asterion_bindings
uv run ruff format --check src tests scripts bindings/python/asterion_bindings
cargo test --workspace --release --locked
cargo clippy --workspace --all-targets --locked -- -D warnings
uv run python scripts/generate_domain_models.py --check
uv run python scripts/generate_task_contract.py --check
uv run python scripts/generate_communication.py --check
uv run pyright
uv run pytest -q
pnpm build
pnpm test
```

macOS 上内核文件测试要求临时目录的各级路径都不经过符号链接。系统默认的 `/var/...` 经过 `/var` 符号链接时，可使用 `TMPDIR=/private/tmp cargo test --workspace --release --locked` 和 `TMPDIR=/private/tmp uv run pytest -q`；不要为测试放宽文件访问保护。

真实 PostgreSQL 并发测试使用**隔离的测试数据库**，勿指定已有业务库：

```bash
ASTERION_TEST_DATABASE_URL=postgresql://USER:PASS@localhost/asterion_test uv run pytest tests/test_postgres.py -q
```

按上述步骤启动 Rust 入口、serve 与 worker 后，可用 `pnpm --filter @asterion/terminal exec playwright install chromium` 安装测试浏览器，再设置与后台相同的 `ASTERION_TOKEN` 运行 `pnpm --filter @asterion/terminal test:e2e`。测试导入标注为合成数据的两根 K 线，验证发布、图表与刷新后任务恢复。

更新 API 类型：

```bash
uv run python scripts/export_schema.py
pnpm --filter @asterion/terminal exec openapi-typescript ../../docs/openapi.json -o ../../bindings/typescript/src/schema.d.ts
```
