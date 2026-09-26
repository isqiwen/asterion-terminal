# 桌面运行协议

2026-09-22 整理。本文描述当前 macOS/Linux 安装、生命周期与数据保护；构建命令和开发环境见 [开发指南](development.md)，验证记录见 [验证索引](VALIDATION.md)。

## 启动与后台生命周期

原生应用加载打包后的 React 资源，不启动 Vite 或外部浏览器。Tauri 通过受限 IPC 调用已安装环境的 `python -I -m asterion.runtime.cli desktop-bootstrap`，后台就绪后附着当前会话；普通浏览器不能调用原生命令。该路径不依赖工作目录、项目 `.env` 或全局 Python。

macOS 的 launchd、Linux 的 systemd 用户服务启动 supervisor，再由它管理 PostgreSQL、Rust 入口 `asterion-server`、内部 Python API 和 worker。界面与 worker 只访问 Rust 入口所在的固定端口；Rust 入口负责 CORS，并以内核 authority 对每个请求鉴权；Python API 每次启动使用临时回环端口，只处理尚未迁移的接口，只接受入口转发的已授权请求（附转发凭据与身份），直接访问即使持有令牌也被拒绝（迁移期例外，见[进度](ROADMAP.md#工程替换顺序)）。入口二进制随 wheel 安装在运行环境的 `bin/` 中，缺失时 supervisor 明确报错。数据库为应用专用 PostgreSQL 17 集群，使用回环 TCP 与 SCRAM 认证，不使用或修改包管理器的默认集群。应用配置目录权限 0700、会话文件 0600；服务配置只包含受控路径和无秘密运行身份。

bootstrap 锁、supervisor 锁与固定服务名称防止多窗口重复创建环境。数据库先在暂存目录初始化，成功后发布；被中断的未发布暂存集群可重建。

关闭窗口或退出桌面进程不会停止后台。在“设置 → 本机服务”停止时，等待 supervisor 关闭 Rust 入口、API、worker 和数据库；数据保留。再次启动应用或显式启动服务可重新附着环境。

## 首次设置

800×640 布局支持中英文，只有点击“开始设置”才下载/安装。五个阶段为准备系统数据库与安装工具、安装 Python、创建隔离环境、安装依赖、验证环境；成功后进入工作台。界面仅显示简短数据根路径，不提供打开/复制路径按钮或悬停交互。

进度遵循实际工作量：下载按字节，依赖按实际已安装数量，最终验证按三个完成检查项；未知总量显示不定进度。uv 并行下载、批量安装，数量可一次跳到总数，不能人为平滑。依赖数量不含 Python、uv、PostgreSQL 或项目 wheel，项目安装另行显示。完成行使用绿色“完成”/`DONE`，进度条橙色，进行中文字使用 `...`。

点击开始后显示本次尝试耗时、上传/下载速度及最近 40 秒下载曲线；结束后移除，重试重新计时。Linux 读取 `/sys/class/net` 物理网卡收发计数，以单调时钟差计算，排除回环、网桥及虚拟接口；该值包含其他应用流量，不是安装器独占流量。初次采样、接口变化、计数下降或读取失败显示 `—`。macOS 尚无系统网卡统计，不用其他口径替代。

安装机制位于 `apps/terminal/src-tauri/src/runtime_setup.rs`，发行构建生成清单。它承担 Python 可用之前的下载、校验、锁和进度，不识别业务插件或替插件采集数据。

## 依赖与平台

轻量安装包携带清单、带哈希的依赖文件及项目 wheel。Python、uv 来自 Astral 官方固定地址；下载按大小与 SHA-256 校验，依赖使用 `uv pip sync --require-hashes --only-binary :all:`，项目 wheel 校验包内摘要。不执行网络 shell 安装脚本，不发布数据库附件或随包冻结后台。

| 平台 | 当前数据库来源与后台 | 安装包与边界 |
|---|---|---|
| macOS | 标准 Homebrew 路径的 `postgresql@17`；缺失时由开始设置执行 `brew install`，无 Homebrew 则提示官方安装指引；launchd 用户服务 | 本机架构 DMG 内含 PKG，固定安装到 `/Applications/Asterion Terminal.app`；目前本地签名，Developer ID 与公证未交付 |
| Linux | apt 的 `postgresql-17`；程序 `/usr/lib/postgresql/17/bin`、共享资源 `/usr/share/postgresql/17`；systemd 用户服务 | 本机架构 DEB，声明数据库依赖；当前实测 Debian 13，其他发行版未据此验收 |
| Windows | 规划由 WinGet 提供数据库，具体安装和服务适配未完成 | 约定机器级 NSIS EXE；构建入口拒绝 Windows，配置目标不代表可运行 |

macOS 不提权、不运行 `brew services`，安装时禁用附带自动更新、清理和依赖方更新。Linux 由用户通过 apt 安装 DEB；仓库缺少 PostgreSQL 17 时需先配置 PGDG 来源，应用不添加软件源或提权运行 apt。原生外壳仍依赖系统 WebKitGTK 等库。AppImage/RPM 未支持。

数据库检查先于撤销已发布 Python 环境，包管理器失败不得清除可用程序或业务数据。只接受当前平台声明、固定配方和合法路径，并检查启动/备份/恢复所需程序；缺少系统程序时保留就绪标记，但禁止调用后台，修复后重试。

## 重复安装与失败保护

应用标识为 `me.asterion.terminal`，Linux 包名为 `asterion-terminal`，安装位置不含版本号。macOS PKG 关闭标准位置的桌面进程后整包覆盖，等待退出失败则中止；最近成功安装的包生效，包括较低版本号，不提供“保留两者”。此规则不搜索或删除用户手工复制的应用、下载文件或备份。

运行程序固定在应用数据目录的 `runtime/`，缓存为 `cache/`，清单摘要仅存内部记录：

1. 相同合法摘要复用环境，每次启动仍校验一致性。
2. 合法摘要变化且已发布环境完整时，取得独占安装锁，经当前生命周期接口停止后台，再撤销就绪记录、清理安装器拥有的程序目录并原位安装。
3. 原生后台调用持有共享锁，安装期间不能启动旧程序；安装子进程继承锁，桌面退出不提前释放保护。
4. 停止失败保留原程序和记录；下载/安装中断保持未就绪，可重试，不回退上一版。损坏标记或缺失的已发布程序明确拒绝，保留原文件，不自动修复。
5. 仅全部验证通过后写就绪记录。重试复用已校验缓存、重建未发布环境；部分下载重新获取，不声称 HTTP 断点续传。

用户数据、账户环境、数据库和历史结果位于程序之外，替换程序不删除它们。当前固定 PostgreSQL 17；不支持的数据库或数据契约明确拒绝，不自动转换。备份/恢复操作见 [备份与恢复](backup-and-restore.md)。

## 独立设置与工作区

设置使用固定窗口 ID `settings`，通过全局图标、macOS 菜单或快捷键打开，重复调用聚焦已有窗口。它只读取已有会话，不因查看设置而启动已停止的后台，也不挂载业务工作区或任务目录。外观偏好通过同源存储通知其他窗口。

原生工作区以 `workspace.json` 为唯一持久化来源，不导入浏览器布局。不支持格式保留原文件并报错。窗口持久化、修订和拆出/归并机制位于 L4 `presentation/desktop-bridge/native`；`presentation/panels/terminal-workspace/native` 拥有布局规则，`products/terminal/native` 选择产品配置。工作台、共享 UI 与薄 Rust UI 桥均属 L4；进程监督已由 L1 Rust supervisor 执行，安装器仍待独立，见[模块与层级](architecture.md#层级与模块)和[当前代码归属](engineering-structure.md#当前代码归属)。

Linux unit 位于 `$XDG_CONFIG_HOME/systemd/user/me.asterion.terminal.backend.service`，默认 `~/.config/systemd/user/`；参数对空格、美元符号与百分号转义。`Restart=always` 恢复异常退出，`KillMode=mixed` 先通知 supervisor 清理子进程。未启用登录自启动，注销后的行为遵循系统用户服务生命周期；没有真实用户 systemd 会话时返回诊断错误。

Linux 数据目录默认 `~/.local/share/me.asterion.terminal/`，日志为其下 `service.log`。macOS 服务文件为 `~/Library/LaunchAgents/me.asterion.terminal.backend.plist`。目录由 Tauri `app_data_dir` 提供，不位于应用包内。

## 应用更新后的后台版本核对

打开工作台时按运行文件内容 SHA-256 核对实际后台身份，不以产品版本号、安装路径或修改时间判断。安装环境覆盖独立 Python、虚拟环境模块/策略源码及 PostgreSQL 运行目录；Linux 只覆盖数据库版本目录与共享资源。开发环境覆盖 `asterion` 和公开 SDK 源码；排除 `.pyc`、`__pycache__`、用户数据及外部插件。

服务配置写入无秘密的 `ASTERION_RUNTIME_BUILD`。内容变化时先停当前服务，等待数据库所有权释放，再登记并启动；停止失败保留原配置。相同内容复用，多窗口由维护锁与启动锁串行化。supervisor 独立计算指纹，与声明不一致则拒绝初始化数据库；状态报告使用启动时捕获的 `build_id`。bootstrap 要求 API 健康、worker 就绪、状态未过期且运行身份一致。

该机制位于 `runtime/build_identity.py` 与 `runtime/desktop.py`，属于宿主生命周期。它不自动下载安装更新或持续监控目录，也不承诺无中断热升级。切换可能中断任务；输入、状态、结果保留，未完成任务按既有租约规则回收，不支持的输入明确失败。本机切换不能代表在线交易节点升级协议。

已有记录覆盖 macOS 实际服务切换及同内容复用、Linux 配置与错误处理；Linux 实际服务升级、运行中任务跨版本切换和交易节点升级仍需专项验收。本次文档整理未重新安装、重启或操作用户环境。构建与隔离冒烟方法集中见 [开发指南](development.md)。
