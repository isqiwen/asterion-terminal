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
| `pnpm dev` | 仅界面开发：浏览器 + C++ 开发桥（端口 1420），需要已构建的 `build/Debug` |
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

端到端测试必须用 `pnpm run test:e2e`。它通过 `tests/ssh_e2e.py` 启动临时目录中的隔离 Agent，并使用测试 CTP SDK；直接运行 `playwright test` 会连接并修改你本机正在使用的服务。运行前确认没有其他进程占用 1420 端口。

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

开发时 Terminal 使用 `~/Library/Application Support/Asterion/node` 下的本机 Agent。修改服务程序后，已部署的服务不会自动替换：在“设置 → 连接与部署”中停止服务后更新，或调用 `node.update`。Agent 自身程序变化时，启动流程会自动协调升级；行情登录状态不会保留，需要重新登录。

原生插件的 ABI、独立编译、安装目录和契约测试见 [原生插件 SDK](native-plugins.md)。修改插件后需要重新构建动态库；首次启动时选择研究服务的插件集合；之后在「设置 → 插件」停止服务、保存启用清单，再启动生效。服务程序升级保留原插件集合，不能仅替换桌面文件后让运行中的服务自动切换。
