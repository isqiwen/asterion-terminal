# macOS / Linux 桌面运行协议

## 点击启动

原生应用使用打包后的 React 资源，不启动 Vite 或浏览器。Tauri 通过受限本机命令启动内置 `asterion-backend desktop-bootstrap`，后台就绪后将当前会话通过原生 IPC 交给界面。浏览器访问不能调用此命令。

首次运行生成应用专用本地配置（目录权限 0700、会话文件 0600）。macOS 注册 `~/Library/LaunchAgents/me.asterion.terminal.backend.plist`。plist 保存受控可执行文件路径和状态路径，不保存口令或会话令牌。

macOS 由 launchd、Linux 由 systemd 用户服务启动 supervisor；supervisor 初始化内置 PostgreSQL 的独立集群，建立目录，再启动 serve/worker。PostgreSQL 使用本机回环 TCP 和 SCRAM 密码认证。每个业务进程的配置通过环境传入，无需读取项目 `.env`。

## 生命周期

bootstrap 文件锁防止窗口同时执行初始化；supervisor 锁与固定服务名称 保证同一用户只有一个本机环境。多个窗口只获取已有会话，不重复创建环境。初始化使用暂存集群，成功后改名；初始化被中断后下一次可重新建立尚未发布的暂存集群。

关闭窗口和退出桌面进程不会停止后台服务。用户在独立“设置 → 本机服务”窗口点击“停止后台服务”时停止后台服务（macOS 卸载 launchd 服务，Linux 停止 systemd unit），等待 supervisor 停止子进程和 PostgreSQL；数据保留。“启动后台服务”再次注册并附着环境。

用户数据不在 `.app` 内，替换应用不会清空数据。当前打包固定 PostgreSQL 17；不自动转换旧数据库格式，不受支持的目录明确拒绝。

## 打包与验证

`scripts/build_desktop.py` 是开发构建入口。构建成功后将最终安装包或应用复制到根目录 `release/`，并输出产物路径；Tauri 原始产物和编译缓存保留在默认目录。PyInstaller 生成固定平台的独立 Python 目录，PostgreSQL 二进制与必需扩展放在相邻运行包；macOS 依赖动态库使用相对 loader 路径，并检查没有剩余 Homebrew 绝对依赖；Linux 使用 ELF RUNPATH 重定位。

`scripts/smoke_desktop_runtime.py` 在新目录及仅含系统命令的 PATH 中运行实际打包程序，验证新数据库初始化、worker 子进程、快照发布与固定精度查询。不会依赖开发 venv 或使用已有开发数据库。

macOS 生成本机架构 `.app` 并使用本地签名；Linux 生成本机架构 `.deb`。构建命令、依赖和 VS Code 任务见 README。Developer ID 签名、公证、Windows、AppImage 和 RPM 尚未交付。

## 独立设置窗口

设置使用固定窗口标识 `settings`，可从 macOS 菜单、⌘, 或全局设置图标打开；重复调用聚焦现有窗口。设置窗口只读取已有本机会话，不因为查看设置而启动已停止的服务。窗口不挂载业务工作区或任务目录。外观偏好通过同源存储通知其他窗口，业务对象和工作区布局不变。原生工作区以宿主 `workspace.json` 为唯一持久化来源；不从浏览器旧布局导入。只接受当前布局规范，格式不支持时保留原文件并显示错误。

## Linux 服务与打包

Linux 使用 `$XDG_CONFIG_HOME/systemd/user/me.asterion.terminal.backend.service`（默认 `~/.config/systemd/user/`）。unit 不包含秘密；ExecStart 对空格、美元符号和百分号转义。后台通过用户级 systemd 启动，`Restart=always` 恢复异常退出，`KillMode=mixed` 先通知 supervisor，再由它停止 API、worker 和 PostgreSQL。停止操作等待服务退出；下次打开应用可再次启动。未启用登录自启动，退出登录后的行为遵循系统的用户服务生命周期。

数据由 Tauri 的 app_data_dir 提供，默认 `~/.local/share/me.asterion.terminal/`；日志写入该目录下的 `service.log`。启动/停止需要真实用户 systemd 会话，无会话时返回诊断错误。没有 systemd 的环境仍可手动运行开发服务。

Linux PostgreSQL 打包从 PostgreSQL 17 的 `pg_config` 获取路径，将 bin/share/扩展目录的相对层级写入 `layout.json`，使数据库能在安装目录中重新定位 `$libdir`。递归复制非 glibc 的 ELF 依赖并用 `$ORIGIN` 重写 RUNPATH；构建后拒绝任何剩余的非系统依赖。glibc 和 ELF loader 使用目标系统版本，需在不高于目标系统 glibc 的环境中构建。PyInstaller 私有动态库搜索路径不会传给 PostgreSQL。

Linux 采用安装位置稳定的 `.deb`；当前不支持从 AppImage 临时挂载目录运行持久后台。运行时带平台/架构标记，防止 `--skip-runtime` 误用另一平台产物。数据库主版本仍固定为 17，不自动迁移已有集群。

## 应用更新后的后台版本核对

启动工作台时按实际运行文件的内容 SHA-256 识别运行版本，不以产品版本号、安装路径或修改时间判断。冻结应用的指纹覆盖后端可执行文件、随包 Python/模块/策略源码和 PostgreSQL 目录；开发模式覆盖 `asterion` 与公共插件 SDK 源目录。临时 `__pycache__` 和 `.pyc` 不参与；用户数据与安装在数据目录中的外部插件不纳入应用指纹。

这是运行宿主的通用生命周期机制需求：既有插件扩展点不能替换已运行的宿主进程。实现位于 `runtime/build_identity.py` 与 `runtime/desktop.py`，不识别业务插件 ID、不导入功能实现、不修改数据或插件契约。

macOS LaunchAgent 和 Linux 用户 unit 写入无秘密的 `ASTERION_RUNTIME_BUILD`。内容变化时配置随之变化，先停止当前服务，等待原 supervisor 释放数据库所有权，再登记并启动当前运行包；停止失败时保留原配置并报错。相同内容复用现有服务，多窗口调用仍由维护锁和启动锁串行化。

Supervisor 启动时独立计算内容指纹；与服务配置声明不一致时拒绝初始化数据库，提示重新打开应用。运行状态报告保存该次启动捕获的 `build_id`，不会因磁盘文件后来改变而冒充新进程。桌面 bootstrap 只有在 API 健康、Worker 就绪、状态时间有效且实际运行身份等于当前包时才返回会话。

切换沿用现有进程停止和任务租约机制，可能中断正在执行的任务；不承诺无中断热升级。任务输入、状态和已发布结果不删除，未完成任务按现有租约到期规则回收，当前执行契约不支持的输入明确失败，不迁移或降级运行。数据库主版本或数据契约不匹配仍按当前规则拒绝启动。本机自动重启不等于实现在线交易节点升级协议。

核对在打开工作台时发生，不是后台自动下载安装更新，也不持续监控安装目录。Linux 的配置与错误处理已通过测试，本轮实际服务切换验收在 macOS 完成。
