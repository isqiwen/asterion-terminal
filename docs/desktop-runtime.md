# macOS 桌面运行协议

## 点击启动

原生应用使用打包后的 React 资源，不启动 Vite 或浏览器。Tauri 通过受限本机命令启动内置 `asterion-backend desktop-bootstrap`，后台就绪后将当前会话通过原生 IPC 交给界面。浏览器访问不能调用此命令。

首次运行生成应用专用本地配置（目录权限 0700、会话文件 0600），注册 `~/Library/LaunchAgents/me.asterion.terminal.backend.plist`。plist 保存受控可执行文件路径和状态路径，不保存口令或会话令牌。

launchd 启动 supervisor；supervisor 初始化内置 PostgreSQL 的独立集群，建立目录，再启动 serve/worker。PostgreSQL 使用本机回环 TCP 和 SCRAM 密码认证。每个业务进程的配置通过环境传入，无需读取项目 `.env`。

## 生命周期

bootstrap 文件锁防止窗口同时执行初始化；supervisor 锁与固定 launchd label 保证同一用户只有一个本机环境。多个窗口只获取已有会话，不重复创建环境。初始化使用暂存集群，成功后改名；初始化被中断后下一次可重新建立尚未发布的暂存集群。

关闭窗口和退出桌面进程不卸载 launchd 服务。用户在独立“设置 → 本机服务”窗口点击“停止后台服务”时移除已加载服务，等待 supervisor 停止子进程和 PostgreSQL；数据保留。“启动后台服务”再次注册并附着环境。

用户数据不在 `.app` 内，替换应用不会清空数据。数据库升级和迁移尚未实现；当前打包固定 PostgreSQL 17。

## 打包与验证

`scripts/build_desktop.py` 是开发构建入口。PyInstaller 生成固定平台的独立 Python 目录，PostgreSQL 二进制与必需扩展放在相邻运行包；依赖动态库使用相对 loader 路径，并检查没有剩余 Homebrew 绝对依赖。

`scripts/smoke_desktop_runtime.py` 在新目录及仅含系统命令的 PATH 中运行实际打包程序，验证新数据库初始化、worker 子进程、快照发布与固定精度查询。不会依赖开发 venv 或使用已有开发数据库。

当前仅构建 macOS arm64 本机应用，使用本地签名。面向其他机器分发的 Developer ID 签名、公证，以及 Windows/Linux 安装包仍待交付。

## 独立设置窗口

设置使用固定窗口标识 `settings`，可从 macOS 菜单、⌘, 或全局设置图标打开；重复调用聚焦现有窗口。设置窗口只读取已有本机会话，不因为查看设置而启动已停止的服务。窗口不挂载业务工作区或任务目录。外观偏好通过同源存储通知其他窗口，业务对象和工作区布局不变。旧版“机器与服务”工作区记录迁移为市场工作区。
