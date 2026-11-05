# Terminal 部署与运行

Terminal 支持 macOS Apple Silicon 和 Linux x86_64。应用包包含 Electron 界面、C++ 原生层、Lab 程序和内置研究源码；Market 与 Trading 独立安装在 Linux 主机。

## 安装与启动

| 平台 | 安装方式 |
| --- | --- |
| macOS | 打开 `.dmg`，将 Asterion Terminal 拖入“应用程序” |
| Linux | 使用 `sudo apt install ./Asterion-Terminal-<版本>-linux-amd64.deb` 安装 |
| 从源码运行 | 完成[开发环境准备](../development.md#环境)后执行 `pnpm desktop` |

首次启动安装研究使用的 Python 运行时及依赖，并创建本机客户端证书。安装失败可重试。进入工作台后，Lab 自动启动，已配置的行情服务自动连接。

在设置的“关于”中复制客户端证书指纹，到 Market 或 Trading 主机登记，再在相应服务设置中填写地址、端口和服务端证书指纹。交易连接还需要填写账户实例名。柜台密码和供应商密钥留在服务主机。

## 程序与数据布局

发布程序与用户数据分开。安装包资源在 macOS 的 `Asterion Terminal.app/Contents/Resources/`，Linux 的 `/opt/Asterion Terminal/resources/`；其中包含 `research/` 内置源码和 Lab、原生模块等程序资源。

用户程序根目录在 Linux 为 `~/.local/share/asterion`，在 macOS 为 `~/Library/Application Support/Asterion`：

```text
<根>/
├── terminal/
│   ├── config/logging.json
│   ├── logs/
│   ├── runtime/                    # 首次启动准备的 Python 环境
│   └── data/
│       ├── services.lock           # 本机根目录的使用锁
│       ├── market.json             # Market 连接与自选
│       ├── trading-connections/    # Trading 连接引用
│       └── tls/                    # 客户端证书与私钥
├── lab/                            # 研究缓存和记录
└── research/                       # 内置及用户策略、因子
```

Electron 窗口及界面配置另在 Linux 的 `~/.config/me.asterion.terminal`、macOS 的 `~/Library/Application Support/me.asterion.terminal`。开发启动与安装版共用上述用户目录；隔离测试使用临时根目录及独立 Electron 用户目录。

Lab 内部布局见[Lab 部署](../lab/deployment.md)。业务文件由界面维护，日志默认文件与交易时段表的归属见[配置文件](../configuration.md)。

## 生命周期

- Terminal 只看护 Lab；Market 和 Trading 的启动、停止、升级由服务主机负责。
- 窗口关闭后 Lab 结束，远程录制和已授权交易继续。停止策略、撤销授权、撤单和平仓必须分别执行对应操作。
- 状态栏依次显示研究、行情和交易状态。操作错误显示在表单中，后台错误进入通知中心；诊断可复制，本机日志可从通知中心打开。
- 连接修改与删除在设置页进行，业务页面只在未配置时提供添加入口。

## 升级、卸载与备份

升级前等待研究结束并退出 Terminal，再安装新版本。卸载应用保留用户目录；Linux 使用 `sudo apt remove asterion-terminal`，macOS 删除应用。

备份前关闭 Terminal 并确认 Lab 已退出。复制程序根目录可保留连接身份、研究记录和源码；保留窗口与界面偏好时同时备份 Electron 用户目录。远程服务的数据需在各自主机另行备份，见[备份与恢复](../services.md#备份与恢复)。

发布包构建、签名与验收命令见[发行](../development.md#发行)，桌面启动问题见[运行诊断](../development.md#运行诊断)。
