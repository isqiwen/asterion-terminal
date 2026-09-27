# Linux、Windows、macOS 支持

三平台共用 C++20 / Conan / CMake 核心、React 界面和 Tauri 薄桥。`bindings/` 保留顶层，统一管理跨语言接口。平台差异限于工具链、系统能力和安装包，不复制业务逻辑。

## 构建基线

| 平台 | 工具链与系统依赖 | 安装包 | CI |
| --- | --- | --- | --- |
| Linux | GCC / libstdc++、GTK / WebKitGTK 4.1 | `.deb` | Ubuntu 22.04 x64 |
| Windows | Visual Studio C++ Build Tools / MSVC、Windows SDK、WebView2 | NSIS `.exe` | windows-latest x64 |
| macOS | Xcode Command Line Tools / Apple Clang / libc++ | `.dmg` | macos-latest |

这些是当前构建基线，不表示所有 Linux 发行版或所有 CPU 架构均已验收。macOS 最低版本配置为 13.0；Windows 用户运行环境需要 WebView2 和匹配架构的 Visual C++ 运行库。Windows 安装包中的运行库部署仍需干净机器安装验收。

平台依赖安装以 [Tauri 官方前置要求](https://v2.tauri.app/start/prerequisites/) 为准。Ubuntu 构建需要 `build-essential libwebkit2gtk-4.1-dev libxdo-dev libssl-dev libayatana-appindicator3-dev librsvg2-dev patchelf`。Windows 在 Visual Studio 开发者终端中构建；Rust 使用 MSVC 工具链，不与 MinGW 混用。

## 统一入口

安装 Python 3、Conan 2.32.0、CMake 3.25+、Ninja、Node.js 22、pnpm 10.32.1 和 Rust stable 后执行：

```sh
conan profile detect
pnpm install --frozen-lockfile
pnpm desktop:check
pnpm test:e2e
pnpm desktop:build
```

首次运行界面测试前执行 `pnpm exec playwright install --with-deps chromium`。`check` 会编译并链接原生桌面程序，而不只是 cargo check，能够发现 C++ 与 Rust 链接问题。`build` 在当前原生平台生成唯一对应格式，不提供跨系统打包。

Windows 的 C++ 桌面构建使用动态 Release CRT，与 Rust 默认 MSVC 运行库一致，包括 Debug 构建。Python 构建脚本按 PATH 解析工具入口（包括 Windows pnpm.cmd）；开发桥接按平台选择 `.exe` 后缀。Tauri 使用各平台配置文件选择安装包格式。

## 验证边界

CI 配置在三个系统分别执行核心测试、原生桌面链接、浏览器端到端测试、Release 安装包构建与测试，并归档安装包。macOS 还校验挂载 DMG 内的应用签名，Linux 检查 DEB 元数据。浏览器测试不替代原生 WebView、文件对话框、安装和卸载验收。

本轮仅有本机 macOS 实测条件。Linux / Windows CI 配置已补齐，但尚未执行远端流水线，也未完成这两个系统的干净机器安装和原生交互验收。macOS 当前使用本地 ad-hoc 签名；正式发行的签名、公证及 Windows 签名另行配置。

本轮 macOS 本机验证已通过：Debug 原生编译链接、Debug/Release CTest 各 6/6、前端构建、Playwright 5/5、Release DMG 构建及挂载后的签名校验。未进行本轮安装包的原生 UI 交互复测。

历史模拟交易的文件日志使用本地系统锁与持久同步：Linux/macOS flock/fsync，Windows 独占 CreateFileW/FlushFileBuffers/MoveFileExW。新增 Python 进程崩溃恢复测试纳入 CTest（测试构建需 Python 3，CI 已安装）。本轮只在 macOS 验证，Windows/Linux 与网络盘语义仍待实际执行。

独立交易程序采用 CLI11（Windows ensure_utf8 与 shell32），本机 IPC 的 Windows 用户 DACL 使用 advapi32；Unix 检查 socket 对端 UID。Protobuf/Abseil 由 Conan 静态构建并导出 Rust 链接信息，CMake 生成协议代码。Tauri externalBin 按本机 target triple 打包交易程序。Mac DMG 验收增加对包内交易程序的版本、签名及进程强杀恢复检查；尚未做 Linux/Windows 或交叉编译验收。

跨机器通信新增 Conan 管理的 Asio 1.30.2、OpenSSL 3.6.2（含 Zlib 依赖）。网络差异由 Asio 封装，TLS 与 Protobuf 契约三平台共用。Rust 链接清单同时导出 OpenSSL/Zlib 静态库和 Asio/Windows 系统依赖。测试证书由仅用于测试的 C++ 程序生成，不依赖系统 openssl 命令；不随产品打包。跨机器网络、防火墙、证书部署仍需目标环境联调。

Node Agent 和交易程序由各原生平台编译。CI 先在 Ubuntu 24.04 的 x86_64 runner 构建远程 Linux 服务，随后三个桌面构建任务均嵌入该产物及初始化脚本；用户只下载一个 DEB / NSIS EXE / DMG。Linux 远程产物以 Ubuntu 24.04 / glibc 2.39 为构建基线，目标还需兼容的 libstdc++ 等系统运行库；这不代表支持任意 Linux 发行版。打包时校验同产品版本、ELF x86_64 架构、SHA-256 和初始化脚本内容，缺失或不一致直接失败。初始化脚本通过 `.gitattributes` 固定 LF 换行，避免 Windows checkout 改变嵌入文件摘要。默认本机随桌面包提供当前平台 Agent 和交易程序，按需注册 launchd/systemd 用户服务或 Windows 当前用户计划任务；远端由 Terminal 自动选择内置 Linux 程序，经 SSH 安装 Agent、经 mTLS 部署交易服务。


## 部署范围（2026-09-27 最新决定）

本机部署支持 Linux、Windows、macOS，使用当前用户的系统托管机制和本机 IPC，不需要 SSH、私钥或目标机器初始化。远程部署目标仅支持 Linux，使用专用 asterion 账户、独立初始化脚本及 SSH 引导，后续通过 mTLS 管理。不再提供远程 macOS/Windows 安装流程；不影响三平台 Terminal、Agent 与业务服务的本机运行。设置中的“本机部署”和“远程 Linux”分开显示，默认本机。

当前 Linux 仅支持 x86_64，包含 Terminal、Agent、行情和交易服务；Linux ARM64 暂停构建、验收和分发。macOS Apple Silicon 不受影响。


策略分发增量：本机 sidecar 和内置 Linux x86_64 资源包均包含 `asterion-strategy`。当前完整 Linux 资源为 Agent、Trading、Market Data、Task Service、Backtest、Factor、Data Pipeline、Strategy 八个程序，加 CTP 插件和初始化脚本。macOS DMG 已重新生成并通过包内策略/交易/研究恢复及资源校验；Windows/DEB 本轮未构建，已有部署显式升级仍待实现。证据见 `build/strategy-desktop-build.log`。
