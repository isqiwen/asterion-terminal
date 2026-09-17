# ASTERION · 星衡

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

当前运行包为本机 macOS Apple Silicon 版本，仍是数据功能基础版；多屏完整布局、研究和交易能力按 ROADMAP 继续实现。

## 工作台与设置

业务区采用左侧业务导航和顶部业务内标签、合约资料、主面板、检查器和底部任务面板。任务面板可收起或拖动调整高度；各面板独立滚动。页面不展示部署信息或开发里程碑。

`⌘1`–`⌘6` 切换业务工作区，`⌘J` 显示或收起任务面板。`⌘,` 或应用菜单“设置…”打开独立设置窗口，管理信息密度、涨跌颜色和本机服务。设置窗口不会更改当前业务工作区，重复打开会聚焦已有窗口。

## 构建桌面应用（开发者）

构建需要 Python 3.12+、uv、Node.js/pnpm、Rust、Xcode Command Line Tools，以及用于制作运行包的 Homebrew PostgreSQL 17。**这些是构建依赖，不是用户启动依赖。**

```bash
uv sync --locked --group packaging
pnpm install --frozen-lockfile
uv run python scripts/build_desktop.py
```

产物位于 `apps/terminal/src-tauri/target/release/bundle/macos/Asterion Terminal.app`。构建脚本冻结 Python 后台、复制 PostgreSQL 并重写动态库引用，再打包原生窗口和前端。原生启动路径不依赖工作目录、`.env`、全局 PATH 或代码仓库。

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
