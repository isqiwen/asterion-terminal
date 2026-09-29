# 重构后的桌面验收

桌面宿主已于 2026-09-28 切换 Electron；本文既有 Tauri/Cargo 日志为历史证据，不代表当前安装包。当前宿主与验收见 [Electron 桌面宿主](electron-desktop.md)。

本轮环境：2026-09-28，macOS Apple Silicon，以及隔离 OrbStack Ubuntu 24.04 x86_64（Apple Silicon 上运行 x86 转译）。后者运行实际 Linux 二进制和 systemd，不代表物理跨机或 Windows 验收；未提交、推送或替换已安装应用。

## 实际修复

- Agent 程序已与目标一致且没有待恢复的更新记录时，重复更新直接返回。不会因重复请求重新排空、停止或重启服务；首个客户端完成后，第二个客户端可用原摘要安全重试。并发锁争用仍明确返回，不重发业务命令。
- 启动页以最后一次完整快照检查行情与研究连接，拒绝 stale 状态；先前启动调用成功不能代替最终健康检查。失败不保存首次完成标记，重试后可正常进入。
- 总览空状态测试在工作台出现后才应用空数据，避免用空行情状态绕过启动验收。
- Windows 分支语法检查改用显式准备、摘要校验过的 Windows CTP 头文件，覆盖 `__APPLE__` 平台分支，并以编译器退出码判断成功。此前拿 macOS SDK 检查 Windows 登录接口会产生误报。
- 缺少必需的 Linux 发行材料时，打包入口直接给出缺失路径与 CI 材料要求，不再输出层层 Python 异常，也不跳过资源校验。
- macOS 桌面构建显式使用系统 Apple Clang，并从 Tauri 配置统一读取 macOS 13.0 最低目标；重置 CMake 编译/链接参数缓存并移除 shell 全局编译/链接参数，避免引入 Homebrew LLVM 运行库。包内 Mach-O 最低系统版本逐个检查。
- CTP 价格边界的浮点字符转换改用固定 classic locale 的标准流，保留八位小数精度；消除浮点 `to_chars` 要求 macOS 13.3、`from_chars` 要求 macOS 26.0 的编译阻断。补充精度、舍入、负值、越界和模拟 SDK 小数委托回报验证；未改变模拟撮合或账本规则。
- Linux 打包在 Rust 编译后使用 `dpkg-shlibdeps` 分析本机程序和供应商库，写入并核对实际最低运行库版本，避免旧系统接受安装后才因缺少符号而启动失败。本次 Ubuntu 24.04 包要求 glibc 2.39、libstdc++ 13.1；不能据此宣称兼容 Ubuntu 22.04。CI 基线上的构建会生成其自身依赖。
- SimNow 验收脚本现在等待本次临时目录中的 Agent 退出后再清理目录；干运行同时覆盖成功和登录拒绝，确认密码不泄漏、Agent 不遗留。仅使用测试 SDK，无真实柜台登录。macOS/Linux 日志为 `build/acceptance-process-cleanup.log`、`build/linux-acceptance-process-cleanup.log`。

## 验收结果与证据

后续状态读取修复：`runtime.snapshot` 在选择实时/缓存读取之前统一校验参数，避免空闲时接受非法字段、忙碌时却拒绝的锁时序差异。空参数对象或无符号 `since` 为当前契约，不自动改写非法请求。回归先复现旧行为失败，再在 macOS/Linux 验证空闲与并发读取；日志 `build/snapshot-validation-reproduction.log`、`build/snapshot-validation-tests.log`、`build/linux-snapshot-validation-tests.log`。

后续并发修复：已有命令运行期间，新命令立即返回 `conflict`，不排队后延迟执行；状态读取仍可读取完整缓存。macOS/Linux C ABI 回归通过（`build/command-contention-tests.log`、`build/linux-command-contention-tests.log`），修复前排队复现见 `build/command-contention-reproduction.log`。这保留现有操作串行边界，尚未实现独立业务域同时执行。浏览器全量 40 通过、1 个 Linux 专用跳过（`build/command-contention-e2e.log`），新用例验证本地化忙碌提示、手动重试和跨窗口草稿保留。

高级设置的更新入口已与协调协议一致：不再根据 `desired_running` 或 PID 禁用更新，不再要求用户逐个停止/手动恢复服务。Agent 继续负责安全维护条件和恢复原运行意愿；不能安全排空时仍拒绝。设置/启动/语言定向 13/13 通过，含两个新增 UI 场景（`build/settings-upgrade-e2e.log`）；这两个场景模拟升级响应，真正的 OS 更新证据仍以上述原生脚本为准。

| 范围 | 结果 | 日志 |
| --- | --- | --- |
| Debug C++ 全量 | 234 项中 233 通过，Linux 专用 node_deployment 跳过 | `build/acceptance-ctest.log` |
| 更新修复后的 C++ 定向复验 | 5/5 通过 | `build/acceptance-upgrade-regression.log` |
| macOS 独立 launchd 更新 | 同版本 PID 不变、两个并发请求、完成后重试、pending 文件拒绝且保留、停止/发布检查点恢复、数据与运行意愿保留 | `build/acceptance-native-upgrade-fixed.log` |
| 浏览器第一轮全量 | 36 通过，远程 Linux 部署 1 项跳过 | `build/acceptance-e2e.log` |
| 新增启动检查后的全量 | 38 通过，1 项空状态夹具冲突，1 项 Linux 专用跳过；夹具随后修复 | `build/acceptance-final-e2e.log` |
| 夹具修复后总览与启动复验 | 7/7 通过；最终共 39 个浏览器场景验证通过 | `build/acceptance-overview-startup.log` |
| 前端及原生桌面编译 | `pnpm build`、`pnpm desktop:check` 通过 | `build/acceptance-ui-build.log`、`build/acceptance-desktop-check.log` |
| 风格与静态检查 | `pnpm format`、lint、format:check、git diff --check 通过 | `build/acceptance-format.log`、`build/acceptance-lint.log`、`build/acceptance-format-check.log` |
| Windows 条件代码 | MinGW 语法检查通过，使用锁定的 Windows SDK；不是 MSVC 构建或 Windows 实机测试 | `build/acceptance-windows-syntax.log` |

浏览器业务用例使用真实 C++ 开发桥与隔离节点。覆盖数据校验与发布、删除原 CSV 后继续研究、交易日历发布、多日回测、因子结果恢复、模拟委托风控、账本中断恢复、策略服务重启与结算、mTLS 连接、任务结果与状态顺序。测试数据仅为夹具，不代表生产行情或实盘验收。

原生验收使用最新调试二进制的独立测试应用：实际从启动检查进入工作台；Cmd+B 收起/展开导航；Cmd+, 恢复设置分类；Cmd+W 关闭设置后返回工作台；关闭主窗口后测试进程退出，既有本机 Agent 仍运行。未关闭用户原有开发窗口或服务。窗口位置、草稿和分类的前次验收见 [设置窗口](terminal-settings.md)。

本轮浏览器测试使用 `build/navigation-playwright.config.ts` / `build/navigation-vite.config.ts` 的隔离 1421 端口；它们是本机验收产物，开发端口 1420 保持运行。普通环境使用仓库 `pnpm test:e2e`，不应与占用同一端口的开发服务并行。

## Release 与安装包验收

先前缺少同源码 Linux 服务包的阻塞已解除。将当前工作树复制到隔离 Linux，逐文件 SHA-256 核对后构建；记录位于 `build/linux-source-current.json`。从该环境生成 `build/linux-bundles/asterion-services-linux-x86_64.zip`，通过版本、ELF 架构、文件摘要及初始化脚本一致性校验后用于本次桌面包。没有采用旧安装包中的服务。

| 范围 | 结果 | 日志 |
| --- | --- | --- |
| macOS Apple Clang Release | 全量编译通过，235 项测试中 234 通过、Linux 专用 1 项跳过 | `build/macos-release-clean.log`、`build/macos-release-ctest.log` |
| Linux GCC Release | 全量编译、235/235 测试通过，包括远程部署集成 | `build/linux-release-build.log`、`build/linux-release-ctest.log` |
| Release launchd 升级 | 同版本、并发、发布拒绝、检查点恢复与数据保留通过 | `build/macos-release-native-upgrade.log` |
| Linux systemd 与 SSH | 用户服务升级；专用账户初始化、真实 SSH/SFTP、系统服务安装、mTLS、业务部署、Terminal 退出、Agent 重启与停止意愿持久化均通过 | `build/linux-native-service-acceptance.log` |
| macOS DMG | `pnpm desktop:build` 成功，包含快照校验修复，签名、最低系统版本、包内资源与业务恢复验收通过 | `build/macos-desktop-verified.log` |
| Linux DEB | Release 桌面构建、依赖生成/核对、安装与内置远程资源校验通过；重复构建不重编 C++ | `build/linux-desktop-verified.log`、`build/linux-deb-final-verification.log` |
| 已安装 Linux 服务 | 用当前本机 API 驱动包内 Agent/行情/研究程序及供应商 SDK；systemd 托管与客户端退出后存活通过 | `build/linux-installed-services-smoke.log` |

产物：`apps/clients/terminal/src-tauri/target/release/bundle/dmg/Asterion Terminal_0.1.0_aarch64.dmg`。只读挂载后检查包内程序，并完成交易进程中断恢复、研究结果恢复、策略与模拟账户执行链及供应商行情 SDK 加载。签名为本机 ad-hoc，未做 Developer ID 公证。

本次 DMG 与 Linux 服务包的 SHA-256 和大小保存于 `build/release-acceptance-artifacts.json`。包内材料的版本一致性不代表物理目标机或最低 macOS 版本设备已经验收。

Linux 包已复制到本机 `build/linux-installers/Asterion Terminal_0.1.0_amd64.deb`。已在隔离 Ubuntu 中实际安装，原生程序在 Xvfb 中启动并创建 Core/WebKit 进程。首次设置按设计需要点击按钮；最初自动等待服务的测试因此失败（`build/linux-installed-desktop-smoke.log`），不能把该脚本当成完整 UI 通过。随后通过本机 API 单独验证安装后的服务初始化，未绕过 WebKit 沙箱，也未修改首次设置交互。

从本次 DMG 提取的应用在隔离 Agent 下实际进入工作台；Cmd+B 切换导航、Cmd+, 打开设置、Cmd+W 返回主窗口、重开保留关于分类均通过，关闭主窗口后测试进程退出。既有系统 Agent PID 不变，未覆盖 `/Applications`。

仍未验证 Windows/MSVC 原生、Linux 原生文件选择器交互、物理跨机、macOS 13.0 设备实际运行及多屏热插拔。Linux SSH 验收在隔离机器内部 loopback 上完成。现有 CI 的三平台流水线本轮未触发。活跃行情、交易和策略的自动维护恢复仍明确拒绝或等待，详见 [Agent 升级边界](agent-upgrades.md)。


### Linux 原生窗口后续验收

通过仅监听 loopback 的 noVNC 查看隔离 Ubuntu 的 Xvfb/Openbox 会话，实际操作已安装 DEB。首次设置三步完成、进入工作台、Ctrl+B 收起/展开侧栏、Ctrl+, 打开设置、Ctrl+W 返回主窗口、分类复用均通过。关闭主窗口后 Agent PID 32402 不变且 systemd 状态 active，随后只停止本次隔离机器的测试服务，保留配置和数据；记录 `build/linux-native-ui-service-survival.log`。

此验收发现 GTK 隐藏窗口首次返回的外框尺寸不可靠，设置窗口偏到屏幕右下角。定位改用首次请求的 client size，外框不小于 client size，并在缩小窗口后使用计算结果，避免立即读取异步尺寸。重建安装后，首次窗口居中且完整可见，拖动后关闭/重开保留位置与分类；截图 `build/linux-native-settings-centered.png`，会话日志 `build/linux-native-ui-position-fixed.log`。macOS/Linux 安装包均完成重建；此修改后的 macOS 窗口交互尚未重复验收。

快捷键提示统一补充 Ctrl 用法；前端构建与 lint 通过，设置窗口 4/4、语言 3/3 通过（`build/settings-position-ui-regression.log`、`build/shortcut-hints-i18n.log`）。macOS 最新包已提取并验证签名，交互复验因宿主锁屏未执行，已结束本次测试进程并保留既有用户服务。


### 不同系统间的直接 mTLS

macOS Apple Clang Release 的真实 Terminal C++ 客户端直接连接隔离 Linux 虚拟机地址（非 loopback、非 SSH 端口转发），服务使用已安装 DEB 中的交易程序。错误 CA、陌生客户端证书、错误会话和实盘模式被拒绝；模拟会话初始化、客户端退出后存活、服务正常退出/重新启动后的持久账本恢复，以及相同请求幂等均通过。证据 `build/cross-os-tls-acceptance.log`，测试代码 `build/cross_os_tls_acceptance.py`。临时证书、测试进程与专用账本目录均已清理。两端在同一物理宿主机上，因此仍不代表物理跨机、防火墙/NAT 或 Windows 验收。


### Agent 拒绝与连接故障的区别

已验证、关联到当前请求的非状态操作错误不再把 NodeClient 标为不可达，也不刷新缓存健康时间。业务拒绝仍以原 ErrorCode 和完整诊断返回给调用者；传输失败、非法响应和状态读取失败仍标为不可达。先复现维护拒绝导致状态误报，再在 macOS/Linux 各运行维护用例 4/4 通过（`build/node-rejection-reproduction.log`、`build/node-rejection-tests.log`、`build/linux-node-rejection-tests.log`）。

启动页针对安全排空等待和运行服务不支持自动恢复给出中英文摘要，原始服务证据保留在详情；旧 Agent 不支持协调、另一更新占用和更新记录恢复也有诊断登记。两个新增 UI 用例确认不自动重发、不启动后续服务、不保存首次完成标记，并在切换语言后保留详情；初次用例的英文按钮期望写错，修正后 2/2 通过（`build/startup-update-diagnostics-fixed.log`），原有启动 6 项同时通过。这里仅模拟响应验证展示，不能代替上述原生维护用例。
