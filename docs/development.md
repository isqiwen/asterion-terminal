# 开发指南

## 环境

- macOS（Apple Silicon 或 Intel），Xcode Command Line Tools（Apple Clang）
- Node.js 22+、pnpm 10、Python 3、Conan 2、CMake 3.25+、Ninja
- 可选：OrbStack 或 Docker（重建远程 Linux 服务包）

首次准备：

```sh
conan profile detect          # 没有 Conan profile 时
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
```

`build/local-profile` 存在时，桌面脚本会使用它并强制系统 Apple Clang。

## 构建

| 命令 | 说明 |
| --- | --- |
| `pnpm desktop` | Conan → CMake Debug → Node-API → Vite，启动 Electron（Vite 端口 1422） |
| `pnpm desktop:check` | 同上但不打开窗口，验证原生模块与桥接 |
| `pnpm desktop:build` | Release 构建并生成 `.dmg` |
| `pnpm dev` | 仅界面开发：浏览器 + C++ 开发桥（端口 1423），需要已构建的 `build/Debug` |
| `cmake --build build/Debug` | 只重建 C++ |

三个 `desktop` 命令由 `scripts/desktop.mjs` → `scripts/desktop.py` 统一编排。
桌面入口只接受 macOS，安装包只生成 DMG；远程 Linux 服务包由下面的独立构建流程提供。

## 测试

| 命令 | 内容 |
| --- | --- |
| `ctest --test-dir build/Debug -j 8` | C++ 单元测试与进程集成测试 |
| `pnpm run test:e2e [spec...]` | Playwright 端到端测试 |
| `pnpm run lint` | ESLint |
| `pnpm run format:check` / `python3 scripts/format_cpp.py --check` | 格式检查 |
| `npx tsc --noEmit -p apps/clients/terminal/tsconfig.json` | 类型检查 |

端到端测试必须用 `pnpm run test:e2e`。它通过 `tests/ssh_e2e.py` 启动临时目录中的隔离 Agent，并使用测试 CTP SDK；直接运行 `playwright test` 会连接并修改你本机正在使用的服务。运行前确认没有其他进程占用 1423 端口。

## 远程 Linux 服务包

桌面安装包内置 Linux x86_64 服务程序，用于远程部署。包的清单记录服务源码指纹（`scripts/service_fingerprint.py`，覆盖 `core`、`protocol`、`plugins`、`bindings`、`apps/services`、`conan`、`scripts/node` 和构建脚本）。

- `desktop:check` 和 `desktop:build` 要求 `build/linux-bundles/asterion-services-linux-x86_64.zip` 与当前源码指纹一致。
- `pnpm desktop` 在不一致时只警告，本次开发会话中远程 Linux 部署不可用。

重建（Docker 或 OrbStack，首次需编译全部依赖）：

```sh
scripts/build_linux_services.sh
```

脚本把当前源码打包进 Ubuntu 24.04 x86_64 容器，执行 `scripts/linux-services-container.sh`，输出到 `build/linux-bundles/`。可用 `ASTERION_LINUX_IMAGE` 指定已缓存 Conan 依赖的镜像以加速。

## 本机服务

`pnpm desktop` 和 `pnpm dev` 默认使用开发环境；安装的 App 使用日常环境。首次开发启动是独立空环境，需要单独配置数据源和账户，不复制或迁移日常数据。

| 内容 | 安装版 | 开发版 |
| --- | --- | --- |
| Agent、服务、账户、历史数据与任务 | `~/Library/Application Support/Asterion/node` | `~/Library/Application Support/Asterion Development/node` |
| Electron 界面配置 | `~/Library/Application Support/me.asterion.terminal` | `~/Library/Application Support/me.asterion.terminal.dev` |
| launchd 标识 | `me.asterion.node-agent` | `me.asterion.node-agent.dev` |
| SSH 身份与远程节点记录 | `~/.asterion/nodes` | 开发 `node/enrollments` |

开发窗口标题及标题栏显示“开发环境”。Agent 各自持有独立随机 IPC 地址，安装版关闭窗口后继续运行。开发版关闭主窗口或按 Ctrl+C 时，自动依次停止本机任务、交易、行情与 Agent，再退出界面和 Vite；停止成功后移除开发 Agent 的登录启动注册，数据保留。下次启动重新注册 Agent，基础服务自动启动，账户按需打开。升级与维护只作用于当前环境。钥匙串账户按数据源配置目录的摘要区分，两个环境相同连接名称不会读取或覆盖对方凭据。密码仍不持久化。

同一开发环境同时只能由一个开发入口管理，避免退出一个入口时影响另一个。开发中未完成任务按服务现有中断恢复规则保留记录，不声称完成；关闭进程不会自动撤销已经发送到柜台的委托。停止失败会明确报错，可处理后再次关闭重试。开发启动器在 Electron 崩溃时也尝试清理；强制杀死整个启动器或系统掉电无法保证执行退出流程。

浏览器开发入口为 `http://127.0.0.1:1423`，其界面偏好与 Electron 分开，业务服务与开发桌面使用同一开发目录，正常关闭 Vite 也会停止本机开发服务。不要复用旧浏览器入口的连接配置。环境隔离不是权限沙箱：显式选择同一个自定义账本目录、远程服务或柜台账户仍可能操作相同资源；测试应使用专用账户和节点。

`ASTERION_NODE_DIRECTORY` 仍只作为显式临时测试目录覆盖：不注册 launchd、不执行系统 Agent 升级。测试同时使用独立 `--user-data-dir`；不能只改界面目录就声称业务已隔离。自动化测试继续通过隔离包装器运行。

修改服务程序后，已部署的服务不会自动替换：在当前环境的“设置 → 连接与部署”中停止服务后更新。Agent 自身程序变化时，启动流程会自动协调升级；行情登录状态不会保留，需要重新登录。

原生插件的 ABI、独立编译、安装目录和契约测试见 [原生插件 SDK](native-plugins.md)。修改插件后需要重新构建动态库；首次启动时选择数据服务的插件集合；之后在「设置 → 插件」停止服务、保存启用清单，再启动生效。服务程序升级保留原插件集合，不能仅替换桌面文件后让运行中的服务自动切换。
