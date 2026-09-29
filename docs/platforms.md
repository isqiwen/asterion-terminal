# Linux、Windows、macOS 支持

三平台共用 C++20 / Conan / CMake 核心、React 界面和 Electron / Node-API 薄桥。`bindings/` 保留顶层，统一管理跨语言接口。平台差异限于工具链、系统能力和安装包，不复制业务逻辑。

## 构建基线

| 平台 | 工具链与系统依赖 | 安装包 | CI |
| --- | --- | --- | --- |
| Linux | GCC / libstdc++、GTK3 / Chromium 运行库 | `.deb` | Ubuntu 22.04 x64 |
| Windows | Visual Studio C++ Build Tools / MSVC、Windows SDK | NSIS `.exe` | windows-latest x64 |
| macOS | Xcode Command Line Tools / Apple Clang / libc++ | `.dmg` | macos-latest |

这些是当前构建基线，不表示所有 Linux 发行版或所有 CPU 架构均已验收。macOS 最低版本配置为 13.0；Windows 用户运行环境需要匹配架构的 Visual C++ 运行库，Chromium 随 Electron 分发。Windows 安装包中的运行库部署仍需干净机器安装验收。

桌面依赖通过 pnpm 锁定 Electron / Electron Builder，构建脚本显式下载对应版本 Node-API SDK。Ubuntu 构建需要 build-essential、dpkg-dev、GTK3/NSS/ALSA/GBM/XSS/secret 运行库，详见 CI。DEB 结合本机服务与 CTP 库的 dpkg-shlibdeps 结果及 Chromium 运行库声明依赖。Windows 使用 Visual Studio MSVC 开发者终端和动态 Release CRT，不与 MinGW 混用。

## 统一入口

安装 Python 3、Conan 2.32.0、CMake 3.25+、Ninja、Node.js 22.22.2+、pnpm 10.32.1 后执行：

```sh
conan profile detect
python3 scripts/prepare_ctp.py
pnpm install --frozen-lockfile
pnpm desktop:check
pnpm test:e2e
pnpm desktop:build
```

首次运行界面测试前执行 `pnpm exec playwright install --with-deps chromium`。`check` 会编译并加载 Node-API 模块，真实调用 C++ 快照接口并构建前端。`build` 在当前原生平台生成唯一对应格式，不提供跨系统打包。

`desktop:build` 还需要同源码的 `remote-linux` 服务材料：将 `asterion-services-linux-x86_64.zip` 放入 `build/linux-bundles/`，或用 `ASTERION_LINUX_BUNDLES` 指定目录。CI 自动汇集该产物；本地缺失时明确报错，不使用旧安装包代替。macOS 入口固定使用系统 Apple Clang，固定最低 macOS 13.0，清除 shell 全局编译/链接参数并重置对应的 CMake 参数缓存，防止 Homebrew LLVM 路径进入分发程序。

Windows 的 C++ 桌面构建使用动态 Release CRT，与 Electron 原生模块一致，包括 Debug 构建。Python 构建脚本按 PATH 解析工具入口（包括 Windows pnpm.cmd）；开发桥接按平台选择 `.exe` 后缀。Electron Builder 按原生平台选择 DMG、NSIS 或 DEB，输出 build/desktop。

## 验证边界

CI 配置在三个系统分别执行核心测试、原生桌面链接、浏览器端到端测试、Release 安装包构建与测试，并归档安装包。macOS 还校验挂载 DMG 内的应用签名，Linux 检查 DEB 元数据。浏览器测试不替代原生 WebView、文件对话框、安装和卸载验收。

2026-09-28：macOS Apple Clang Release 全量 235 项中 234 通过、Linux 专用 1 项跳过；隔离 OrbStack Ubuntu 24.04 x86_64 全量 235/235 通过，并完成真实 systemd 用户服务升级和 loopback SSH 系统安装验收。macOS 新 DMG 与原生窗口交互通过，详见 [桌面验收](desktop-acceptance.md)。Linux x86 转译环境不代表物理跨机验收，Windows/MSVC 仍无本轮原生结果，远端 CI 未触发。macOS 当前使用本地 ad-hoc 签名；正式发行的签名、公证及 Windows 签名另行配置。Mach-O 最低版本 13.0 检查不代替 macOS 13.0 设备运行验收。

历史模拟交易的文件日志使用本地系统锁与持久同步：Linux/macOS flock/fsync，Windows 独占 CreateFileW/FlushFileBuffers/MoveFileExW。Python 进程崩溃恢复测试纳入 CTest（测试构建需 Python 3，CI 已安装）。本轮 macOS/Linux 测试通过，Windows 原生与网络盘语义仍待实际执行。

独立交易程序采用 CLI11（Windows ensure_utf8 与 shell32），本机 IPC 的 Windows 用户 DACL 使用 advapi32；Unix 检查 socket 对端 UID。Protobuf/Abseil 由 Conan 静态构建并通过 CMake target 链接 Node-API 模块，CMake 生成协议代码。Electron Builder 将本机交易程序打入 resources/native。Mac DMG 验收检查包内交易程序的版本、签名及进程强杀恢复。

跨机器通信新增 Conan 管理的 Asio 1.30.2、OpenSSL 3.6.2（含 Zlib 依赖）。网络差异由 Asio 封装，TLS 与 Protobuf 契约三平台共用。Node-API 模块通过 CMake 传递依赖链接。测试证书由仅用于测试的 C++ 程序生成，不依赖系统 openssl 命令；不随产品打包。跨机器网络、防火墙、证书部署仍需目标环境联调。

Node Agent 和交易程序由各原生平台编译。CI 先在 Ubuntu 24.04 的 x86_64 runner 构建远程 Linux 服务，随后三个桌面构建任务均嵌入该产物及初始化脚本；用户只下载一个 DEB / NSIS EXE / DMG。Linux 远程产物以 Ubuntu 24.04 / glibc 2.39 为构建基线，目标还需兼容的 libstdc++ 等系统运行库；这不代表支持任意 Linux 发行版。打包时校验同产品版本、ELF x86_64 架构、SHA-256 和初始化脚本内容，缺失或不一致直接失败。初始化脚本通过 `.gitattributes` 固定 LF 换行，避免 Windows checkout 改变嵌入文件摘要。默认本机随桌面包提供当前平台 Agent 和交易程序，按需注册 launchd/systemd 用户服务或 Windows 当前用户计划任务；远端由 Terminal 自动选择内置 Linux 程序，经 SSH 安装 Agent、经 mTLS 部署交易服务。


## 部署范围（2026-09-27 最新决定）

本机部署支持 Linux、Windows、macOS，使用当前用户的系统托管机制和本机 IPC，不需要 SSH、私钥或目标机器初始化。远程部署目标仅支持 Linux，使用专用 asterion 账户、独立初始化脚本及 SSH 引导，后续通过 mTLS 管理。不再提供远程 macOS/Windows 安装流程；不影响三平台 Terminal、Agent 与业务服务的本机运行。设置中的“本机部署”和“远程 Linux”分开显示，默认本机。

当前 Linux 仅支持 x86_64，包含 Terminal、Agent、行情和交易服务；Linux ARM64 暂停构建、验收和分发。macOS Apple Silicon 不受影响。


策略分发增量：本机 sidecar 和内置 Linux x86_64 资源包均包含 `asterion-strategy`。当前完整 Linux 资源为 Agent、Trading、Market Data、Task Service、Backtest、Factor、Data Pipeline、Strategy 八个程序，加 CTP 插件和初始化脚本。macOS DMG 已重新生成并通过包内策略/交易/研究恢复及资源校验；Windows/DEB 本轮未构建，已有部署显式升级仍待实现。证据见 `build/strategy-desktop-build.log`。

## 非 Windows 机器上的 Windows 分支检查

`scripts/check_windows_syntax.py` 使用 mingw-w64 头文件（如 `brew install mingw-w64`）和本机构建的 `compile_commands.json`（配置时加 `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`），对所有含 `_WIN32` 分支的源文件做 Windows 目标的语法检查，可在推送前发现缺失声明、头文件顺序和类型错误。它近似 MSVC，不能代替 Windows 原生构建与 CI 验收。

## Windows 桌面构建的 C++ 配置

Electron 原生模块使用 Release 动态 CRT（`/MD`），而 protobuf 的 CMake 会在 Debug 配置下强制使用 Debug CRT（`/MDd`），两者无法链接。因此 Windows 上的桌面构建（`pnpm desktop`、`desktop:check`、`desktop:build`）一律使用 Release 配置的 C++（`build/Release`）。开发桥、界面测试与 CI 通过 `ASTERION_CPP_BUILD` 指定 C++ 构建目录，未设置时默认 `build/Debug`。纯 C++ 的 core 测试仍在 Windows Debug 下运行。
