# Terminal 部署与运行

Terminal 支持 macOS（Apple Silicon）与 Linux x86_64。桌面安装包自带 Electron 界面、C++ 原生层、Lab 研究服务及内置策略/因子源码；Market、Warehouse 与 Trading 则独立部署于 Linux 服务主机。

## 安装与启动

| 平台 | 安装与启动方式 |
| --- | --- |
| macOS | 打开 `.dmg`，将 `Asterion Terminal` 拖入“应用程序” |
| Linux | 执行 `sudo apt install ./Asterion-Terminal-<版本>-linux-amd64.deb` |
| 源码启动 | 完成[环境准备](../development.md#环境)后执行 `pnpm desktop` |

首次启动时自动准备研究用 Python 运行时及锁定依赖，并生成本机客户端证书（安装失败可重试）。进入工作台后，Lab 自动启动，已配置的行情、数据与交易服务自动连接。

在“设置 → 关于”复制本机客户端证书指纹并登记至对应 Market、Warehouse 或 Trading 主机，再在相应服务设置中填写主机地址、端口与服务端证书指纹（交易连接另填账户实例名）。柜台密码与数据源凭据始终保留在服务主机。

## 程序与数据布局

应用程序与用户数据严格分离。安装包只读资源在 macOS 位于 `Asterion Terminal.app/Contents/Resources/`，在 Linux 位于 `/opt/Asterion Terminal/resources/`，内含内置 `research/` 源码、Lab 程序与原生模块。

用户程序根目录在 Linux 为 `~/.local/share/asterion`，在 macOS 为 `~/Library/Application Support/Asterion`：

```text
<根>/
├── terminal/
│   ├── config/logging.json
│   ├── logs/
│   ├── runtime/                    # 首次启动准备的 Python 运行时
│   └── data/
│       ├── services.lock           # 本机根目录单实例互斥锁
│       ├── market.json             # Market 连接与自选列表
│       ├── warehouse.json          # Warehouse 连接
│       ├── trading-connections/    # Trading 连接配置
│       └── tls/                    # 本机客户端证书与私钥
├── lab/                            # 研究数据缓存与运行记录
└── research/                       # 内置只读与用户自建策略、因子
```

Electron 窗口状态与界面偏好另存于 Linux 的 `~/.config/me.asterion.terminal`、macOS 的 `~/Library/Application Support/me.asterion.terminal`。源码启动与安装版共用同一套用户目录；隔离测试通过临时根目录与独立 Electron 用户目录运行。

Lab 目录细节见[Lab 部署与运行](../lab/deployment.md)，默认日志与交易时段表归属见[配置文件](../configuration.md)。

## 生命周期

- Terminal 仅看护本机 Lab；Market、Warehouse 与 Trading 的启停和升级完全由服务主机管理。
- 关闭 Terminal 会随之结束 Lab，但远程 Market、Warehouse 与 Trading 照常运行，已授权交易不受任何影响。停止策略、撤销授权、撤单与平仓均须显式分别执行。
- 底部状态栏依次展示行情、研究、数据与各交易账户状态；表单错误就地显示，后台故障汇总至通知中心（支持复制诊断与一键打开本机日志目录）。
- 远程连接的编辑与删除统一在设置窗口完成，业务页仅在未配置连接时提供添加入口。

## 升级、卸载与备份

- **升级与卸载**：升级前等待需要保留的研究任务结束并关闭 Terminal，再直接安装新版。卸载仅删除应用程序，用户目录完整保留（Linux 执行 `sudo apt remove asterion-terminal`，macOS 直接删除应用）。
- **备份**：备份前先关闭 Terminal 并确认 Lab 已退出；备份程序根目录可保留客户端证书、连接配置、策略源码与研究记录，如需一并保留窗口与界面偏好则同时备份 Electron 用户目录。远程服务数据须在各服务主机冷备，见[备份与恢复](../services.md#备份与恢复)。

发行包构建、签名与公证见[发行](../development.md#发行)，运行诊断见[运行诊断](../development.md#运行诊断)。
