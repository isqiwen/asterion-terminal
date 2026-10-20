# 开发节点插件选择修复

日期：2026-10-20。主 checkout `/home/qiwen/Code/quant/asterion-terminal`。

## 原因

命名清理后，本机开发启动报告 `development program synchronization failed`。
节点最新失败日志为
`~/.local/share/asterion-development/node/logs/development-programs-49da0407930ac8302c1bd03fc96ae9cd.log`，
具体原因是 `native plugin artifact is missing or corrupted`。

配置仍选择三个此前构建的插件，它们导出旧入口；当前 Agent 通过新入口检查这些产物，
在取得用于匹配内置插件的稳定ID之前失败。文件实际存在，已安装的旧 Agent 能正常读取其
ID：`asterion.data.tushare`、`asterion.data.shfe` 和 `asterion.risk.order-limits`。
因此这是入口改名后保留的旧开发产物引用，不是数据损坏或文件缺失。

此前全量测试使用当前SDK同时构建宿主与插件，证明当前契约可用，不证明旧开发节点能
直接通过当前加载器识别旧插件。当前项目不提供旧插件入口兼容。

## 本机处理

比较了增加旧入口识别路径与直接维护当前开发配置。选择后者：稳定ID已经核实，新插件
已经由失败前的正常准备步骤暂存，无需增加兼容加载器、迁移入口或新的持久元数据。

- 用已安装 Agent 只读核对旧插件ID；用当前 Agent 检查新插件及已暂存产物。
- 确认没有开发进程运行，持有 development、bootstrap 和 Agent 三个锁。
- 仅更新 `task`、`historical-data` 的 `plugin_artifacts`，按相同插件ID换成当前产物。
  使用 `kernel/durable_file.hpp` 持久替换；其余配置字段保持。一次性维护程序执行后移除，
  不加入项目入口。
- 保留旧插件产物、任务、数据集、账户配置和运行意愿，没有删除或重建节点。

## 验证

使用与 Electron 开发入口相同的资源路径和环境设置：
`build/electron-resources/native/asterion_terminal.node`、当前程序与插件目录，
通过真实 Native `node.local` 连接默认开发节点，未设置测试节点目录或测试容量。

同步日志 `development-programs-575308853a427d6a298631b3a1d632d2.log` 确认：
3个服务程序同步成功，正常清理6个已无服务引用的旧程序副本。

Agent为 online/ready；`historical-data` 和 `task` 为 running/ready；
`market-data` 为 running/awaiting_input，等待用户行情登录，没有连接真实柜台。
随后 `development.shutdown` 成功停止本机环境。验证日志：
`build/reviews/development-plugin-repair-2026-10-20/startup.log`。

本次没有修改生产代码，不新增测试或重复全量套件。未打开交互桌面，未验证远程节点。
未提交推送。
