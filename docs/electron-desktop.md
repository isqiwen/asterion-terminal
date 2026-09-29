# Electron 桌面宿主

2026-09-28 起桌面宿主切换为 Electron。React 工作台和 C++ 领域/服务契约继续沿用；Tauri 源码、Cargo 清单、前端包依赖及构建入口已移除。历史验收文档中记录的 Tauri 结果仅代表当时产物，不能作为 Electron 验收证据。

## 调用和权限边界

React → 隔离预加载接口 → Electron IPC → 异步 Node-API → C ABI → C++ Terminal 编排。业务服务仍由 Agent 管理，关闭桌面不停止服务。Node-API 用共享所有权保证异步请求结束前 Runtime 存活；所有窗口共享一份 Runtime。C++ 继续决定命令互斥、状态快照与错误协议。

主进程位于 `apps/clients/terminal/electron/`，绑定位于 `bindings/node/`。渲染进程启用 sandbox、contextIsolation、webSecurity，禁用 nodeIntegration。预加载仅暴露请求、设置窗口、标题、关闭和原生文件对话框；IPC 校验窗口归属、主 frame、来源、参数及请求大小，最多同时排队 32 项原生调用。弹出新页面、外部导航、webview 和权限请求默认拒绝。正式版使用受限 `asterion://app` 协议和 CSP 加载打包资源，不启动 HTTP 服务。

三平台使用系统窗口装饰。设置是主窗口所属的非模态独立窗口：首次按主窗口居中并限制在屏幕工作区内，关闭时隐藏，再次打开保持位置和内部状态，主窗口关闭时销毁。设置状态与工作台偏好通过同源存储事件同步。

## 构建与分发

`pnpm desktop` 构建 C++、Node-API、React 后启动 Electron。桌面 Vite 默认使用 1422 端口，可通过 `ASTERION_DESKTOP_PORT` 指定；浏览器开发仍使用 1420。桌面 Vite 不启动浏览器开发 C++ 桥。

`pnpm desktop:check` 编译并实际加载原生模块、校验 C++ 快照和错误边界，检查主进程/预加载语法。`node tests/electron_desktop.cjs` 启动真实 Electron，使用临时 UI 数据和隔离 Agent，测试首次初始化、工作台草稿、设置窗口及 IPC。文件选择测试替换系统对话框返回值，只验证对话框桥接，不代表原生选择器人工验收。

Node-API SDK 由脚本按 pnpm 锁定的 Electron 版本显式下载并校验。Conan/CMake 管理 C++ 依赖，已经移除 Cargo 链接清单。macOS 使用 Apple Clang，Windows 使用 MSVC Release 动态 CRT 和 Electron 延迟加载钩子，Linux 使用 GCC。

`pnpm desktop:build` 使用 Electron Builder 生成 `build/desktop/` 中的 DMG、NSIS EXE 或 DEB。本机模块、八个服务程序和 CTP 库位于资源目录 `native/`；Linux x86_64 远程程序及初始化脚本位于 `remote-linux/`。正式版根据应用资源目录定位，不读取 PATH 中的业务程序。

macOS 当前使用 ad-hoc 签名，尚未公证。JIT entitlement 支持 Chromium；library-validation entitlement 支持 ad-hoc 原生模块和供应商 SDK。这不是原生插件隔离。三平台 CI 包括原生 Electron 测试，Linux 使用 Xvfb；只有实际运行的结果才作为平台验收。

## 本次实际验收（2026-09-28）

- macOS Apple Silicon：`pnpm desktop` 实际编译和启动，`desktop:check` 通过；最终 DMG 校验、严格签名、内置 Linux 资源摘要、包内交易/研究/策略恢复及 CTP SDK 回环通过。打包后的 Electron 启动、设置、跨窗口语言/草稿及 Agent 独立生命周期通过。
- Linux：隔离 OrbStack Ubuntu 24.04 x86_64 虚拟机（Apple Silicon 上 x86 转译）构建 DEB，通过 apt 实际安装并从 `/usr/bin/asterion-terminal` 运行同一组测试，显式启用 Chromium 沙箱。没有将虚拟机验证称为物理机器验收。
- C++：macOS 235 通过、1 个 Linux 专属用例跳过；Linux 236/236。浏览器 44 通过、1 个平台专属用例跳过。TypeScript/Vite、ESLint、Prettier、C++ 格式与 diff 检查通过。
- 新增 ChildProcess 管道继承回归，防止 Agent 保持 Electron 调试/IPC 通道，验证关闭桌面后 Agent 继续运行且桌面正常退出。

最终日志：`build/electron-final-check.log`、`build/electron-dev-smoke.log`、`build/electron-packaged-final.log`、`build/electron-dmg-final-verify.log`、`build/electron-linux-installed-final.log`、`build/electron-core-final.log`、`build/electron-linux-core.log`、`build/electron-browser-regression.log`。产物路径、大小及 SHA-256 记录于 `build/electron-release-acceptance.json`。

Linux 本次产物要求 glibc ≥ 2.38 和 libstdc++ ≥ 13.1；CI Ubuntu 22.04 构建会使用其自身依赖基线，不以本机包声称支持旧系统。Windows NSIS/Node-API 构建和原生测试已接入 CI，但本次未执行 Windows 原生验收；干净机器 VC++ 运行库安装仍待验证。macOS 未公证，未替换用户 /Applications 或现有 Agent；只在隔离 Linux 验收虚拟机中安装 DEB。原生系统文件选择器没有人工交互验收。

## 桌面与业务流程追加验收（2026-09-28）

`tests/electron_desktop.cjs` 已加入实际快捷键（Cmd/Ctrl+B、Cmd/Ctrl+,、Cmd/Ctrl+W）及整个桌面进程重启后的模拟交易恢复。通过真实界面导入 CSV、创建账户，验证超限委托拒绝且不冻结资金、合规委托成交、持仓和费用。关闭全部桌面后 Agent 继续运行，重新启动并恢复原目录，余额 998、费用 2、一个成交和多头持仓保持；继续平仓后余额 1105、累计费用 5、两笔成交且无持仓。数据明确为测试样例，不是生产行情。macOS 打包版与 Linux 已安装版通过，日志 `build/electron-workflow-macos-final.log`、`build/electron-workflow-linux-release.log`。

macOS 通过真实系统文件选择器完成 CSV 选择和回填、取消后保留原路径。实测发现再次选择总回到下载目录，已修复 CSV、结算表和交易记录目录入口，传入当前路径；新版 macOS 包实测直接进入当前 CSV 所在目录。Linux/Windows 原生选择器、macOS 保存对话框和多屏切换仍未人工验收，自动化对话框检查仍使用替代返回值。

独立临时服务的 launchd/systemd 升级恢复测试通过（`tests/native_agent_upgrade.py`），覆盖重复升级、并发竞争、保留发布异常、检查点恢复和数据保留；日志 `build/electron-managed-upgrade-macos.log`、`build/electron-managed-upgrade-linux.log`。这不是对用户现有 Agent 的修改，也不把 helper 验证称为 Electron 启动页完整升级验收。

组合业务回归首次为 7/8：模拟交易用例假定导入区始终展开，而前一个发布用例留下数据后该区会折叠。现按用户操作显式展开后导入，不调整产品行为、不放宽超时；首次证据保留于 `build/electron-business-workflow-browser.log`，修正后 8/8 通过，结果见 `build/electron-business-workflow-final.log`。安装包已重新生成，摘要仍记录于 `build/electron-release-acceptance.json`。本轮不含 Windows 原生或真实市场账号/交易时段验收。

## 新版界面安装验收（2026-09-28）

新版 UI 已重新生成 macOS arm64 DMG 和 Linux amd64 DEB，安装包路径仍在 `build/desktop/`。`build/electron-release-acceptance.json` 记录当前文件的 SHA-256 与验收范围；必须以摘要确认被测试文件，不能仅凭同为 0.1.0 的名称判断。

新增 `tests/electron_installer.py`，CI 在构建安装包后执行实际安装后的桌面验收，并保存 `build/installed-desktop/` 中的截图与摘要报告：

- macOS：从只读 DMG 复制完整应用到临时目录，卸载镜像，验证签名后启动复制的应用。不会替换 `/Applications` 中的用户安装，也不等同于 notarization/Gatekeeper 验收。
- Linux：在可丢弃机器中通过 apt 安装/重装 DEB，从 `/usr/bin/asterion-terminal` 启动；需要显式传入 `--allow-system-install`。
- Windows：在可丢弃机器中静默运行 NSIS，使用当前用户和专用临时目录，执行桌面测试后运行自带卸载器；同样需要上述参数。已加入 CI，但本轮没有 Windows 运行环境，尚未生成/验收 NSIS。

macOS 与 Linux 已实际通过：启动、渲染隔离、125% Chromium 缩放、快捷键、设置窗口关系/复用/位置、跨窗口语言与草稿、模拟风控拒绝/成交、整个桌面重启后的账户和成交恢复。文件选择器在该测试中替换系统返回值，只验证 IPC 链路，不冒充系统选择器人工验收。所有业务数据使用专用临时目录，退出测试后只清理自身 Agent。

本轮包依旧为 macOS ad-hoc 签名、未公证；Linux 包在 Ubuntu 24.04 构建，其 glibc/libstdc++ 下限由实际 ELF 依赖导出，不宣称该本地产物支持 Ubuntu 22.04。MSVC 运行库在无开发环境 Windows 上的安装体验仍需独立验收。

## 服务源码与桌面资源一致性（2026-09-29）

Linux 服务资源清单使用 version 2，包含 `source_sha256`。`scripts/service_fingerprint.py` 对服务、Core、协议、插件、绑定、依赖锁及构建输入按相对路径和精确内容计算确定性摘要；不依赖 Git 工作区是否干净或 checkout 的绝对路径，纯 Terminal UI 编辑不改变此摘要。CMake 跟踪这些输入的新增、删除和内容变化，将预期摘要编入 Terminal 的资源校验，并写入当前构建目录。

`deployment_bundle.py` 必须在 Linux x86_64 上重新检查并构建全部服务目标，再确认构建目录摘要与当前源码一致后生成清单。打包暂存和 Terminal 导出／部署资源都同时检查源码摘要、产品版本、架构、文件集合和各文件内容；旧清单不兼容。同一产品版本号不能作为同一协议／实现的证明。该摘要用于构建一致性，不替代产物签名和供应链信任。

开发机现有旧包经核实缺少日线因子引擎，已被新版校验拒绝。需取得当前源码构建的 Linux x86_64 服务包，再执行 `pnpm desktop:build`；不通过修改旧清单、跳过检查或删除远程服务资源来生成安装包。
