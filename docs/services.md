# 服务与部署

系统包含三类部署单元：本机的 Terminal 与 Lab、Linux 主机上独立运行的 Market 与 Warehouse，以及 Linux 主机上每账户独立的 Trading（含策略子进程）。三类远程服务各自独立安装和运行，不隶属于任何客户端。

```mermaid
flowchart LR
  subgraph Local["本机 (macOS / Linux)"]
    Terminal["Terminal"] <-->|Unix socket| Lab["Lab (子进程)"]
  end
  subgraph Linux["Linux 服务主机 (systemd)"]
    Market["Market"]
    Warehouse["Warehouse"]
    Trading["Trading@账户 + 策略子进程"]
  end
  Terminal <-->|双向指纹 mTLS| Market
  Terminal <-->|双向指纹 mTLS| Warehouse
  Lab -->|双向指纹 mTLS 只读| Warehouse
  Terminal <-->|双向指纹 mTLS| Trading
```

## 本机 Lab

Lab 是 Terminal 看护的本机子进程（`apps/clients/terminal/native/lab_supervisor.*`）：

- 随 Terminal 启动，在启动画面阶段完成就绪检查，不设手动启停按钮；意外退出或持续心跳失败后自动重启。
- 通过 `--owner-pid` 监听 Terminal 存活状态，Terminal 退出（包括被强制终止）后 Lab 随之退出；关闭 Terminal 不影响远程 Market、Warehouse 或 Trading。
- 使用首次启动安装的独立 Python 运行时，以 `python3 -I -B -m asterion_lab` 运行；开发与测试通过 `ASTERION_PYTHON` 指定解释器。
- 同一本机根目录同时只允许运行一个 Terminal（通过 `terminal/data/services.lock` 互斥）。

## 远程服务

| 服务 | 安装包与目录 | systemd 单元 | 说明 |
| --- | --- | --- | --- |
| **Market** | `Asterion-Market-<版本>-linux-x86_64.sh` → `/opt/asterion/market` | `asterion-market.service` | 独立行情账户，不含下单代码，见[部署](market/deployment.md) |
| **Warehouse** | `Asterion-Warehouse-<版本>-linux-x86_64.sh` → `/opt/asterion/warehouse` | `asterion-warehouse.service` | 历史采集与统一仓库，不连接柜台，见[部署](warehouse/deployment.md) |
| **Trading** | `Asterion-Trading-<版本>-linux-x86_64.sh` → `/opt/asterion/trading` | `asterion-trading@<账户>.service` | 每账户一进程，随包携带离线 Python 运行时；升级前须停止全部账户，见[部署](trading/deployment.md#安装与账户配置) |

- 三个服务均以执行 `sudo` 的普通用户运行，凭据以 systemd 加密凭据保存在主机；Market 和 Trading 启动后自动连接柜台，Warehouse 不连接柜台，Trading 重启不恢复授权。
- 安装与升级前先检查主机时钟已同步（`timedatectl` 的 `NTPSynchronized` 为 `yes`），未同步时拒绝继续；各服务对时钟同步的依赖原因见各自的部署文档。
- Terminal 仅作为客户端，不负责安装、启动、升级或备份远程服务。

## 连接

- **本机**：Unix socket，位于每次运行私有的 `/tmp/ast-<实例>/`。
- **远程服务**：双向固定证书指纹（SHA-256）TLS 1.3，双方使用自签证书，仅接受事先登记的对方指纹（同机部署亦然）。
- 断线后自动重连并读取服务端最新状态；交易命令绝不自动重发。
- 状态栏依次展示行情、研究、数据与各 Trading 账户状态；读取中断或服务停滞写入通知中心，账户页保留上次快照并标记失联。

## 桌面安装与升级

Terminal 发布包同时包含桌面应用与 Lab：macOS 将 `.dmg` 拖入“应用程序”，Linux 使用 `apt` 安装 `.deb`，见[发行](development.md#发行)。升级前等待研究任务结束并关闭 Terminal；卸载仅删除程序，保留用户数据。

本机根目录：macOS 为 `~/Library/Application Support/Asterion`，Linux 为 `~/.local/share/asterion`。

## 备份与恢复

系统仅支持**冷备份**：先停止状态拥有者，再复制或归档数据。

| 目标 | 备份前停止 | 需备份路径 | 注意事项 |
| --- | --- | --- | --- |
| 本机 | 等待研究结束，关闭 Terminal 并确认 Lab 已退出 | 本机根目录 | `lab/data/lab/warehouse-cache/` 可从 Warehouse 重建，不必备份 |
| Market 主机 | `asterion-market.service` | `/opt/asterion/market/`（配置、TLS 身份） | — |
| Warehouse 主机 | `asterion-warehouse.service` | `/opt/asterion/warehouse/` 及 `warehouse.json` 指向的 `data` 目录 | `data` 位于独立磁盘时须单独备份；恢复至原路径或更新 `data` 配置 |
| Trading 主机 | 全部账户实例 | `/opt/asterion/trading/`（账本及 `archives/`、策略包、实例记录、账户归属目录 `owners/`） | 停止服务不撤单；恢复后须重新授权 |

- systemd 加密凭据绑定原主机，不包含在安装目录内；换机恢复时须在新主机重新录入凭据。遇到无法识别的记录格式直接报错，不静默改写数据库。

三个 Linux 服务安装包均内置统一冷备脚本 `/opt/asterion/<服务>/bin/cold_backup.py`（`scripts/services/cold_backup.py`，仅用 Python 3 标准库）：

| 子命令 | 行为与校验 |
| --- | --- |
| `backup --service <market\|warehouse\|trading> --output <归档.tar.gz>` | 确认 systemd 单元已停并持独占锁；对 SQLite 做 `PRAGMA integrity_check`；自动纳入外部 `data` 目录、保留硬链接与 TLS 私钥权限，生成含逐文件 SHA-256 清单的归档及 `.sha256` 文件 |
| `verify --archive <归档.tar.gz>` | 校验归档整体与逐文件 SHA-256、SQLite 完整性，以及 Warehouse 全部保留版本、分片与分区摘要 |
| `restore --archive <归档.tar.gz> [--target-install-dir ...] [--target-data-dir ...] [--owner <用户>]` | 先完整 `verify` 再落盘并复核 SHA-256，还原属主并收紧 `config/`、`data/` 为 `0700`；非空目录默认拒绝（`--force` 先清旧配置与残留 `-wal`/`-shm`）；Trading 迁至新路径须显式 `--allow-relocate` 同步更新 `owners/*.json` 绑定 |

## 运行日志

各程序默认按日将日志写入安装目录 `logs/`，保留 30 天，由 `config/logging.json` 控制（文件清单见[配置文件](configuration.md)）。

- `level` 支持 `debug` / `info` / `warning` / `error`，`file.path` 相对 `bin/` 解析并支持 `{program}` / `{pid}`；默认格式 `[%Y-%m-%d %H:%M:%S.%e %z] [%l] [%n] [%s:%#] %v`（带时区偏移便于跨主机对齐），C++ 与 Lab 支持同等格式符。
- 配置文件缺键、多余键或非法值时启动即失败，缺失时退回标准错误；凭据严禁写入日志。桌面端可从通知中心直达本机日志目录，远程服务日志保留在服务主机。

### 崩溃诊断

崩溃现场仅做信号安全采集，符号解析放在独立进程或下次启动执行（无需手动装调试器，Release 保留行号、前端带 source map）；`SIGKILL` 须通过宿主或系统日志排查。

| 组件 | 落盘产物 | 解析与保留行为 |
| --- | --- | --- |
| **Linux 远程服务** | `logs/*-crash.frames` → `*-crash.report.log` | 随包独立解析器自动还原版本、提交号、信号、函数名与源码行号；若关停中断，下次启动自动补齐（Trading 仍须重授权） |
| **Lab** | 日常日志 + `lab/logs/python-*.log` | 捕获异常记 traceback，解释器/原生崩溃写 `python-*.log`，回测子进程标准错误随失败记录保存 |
| **Trading 策略子进程** | `logs/strategy-*.log` | 记录 Python traceback 与解释器崩溃，服务日志保存实例 ID 映射 |
| **Terminal** | `terminal/logs/electron-*.log` 与 `crashes/*.dmp` | JS 堆栈写 `electron-*.log`；原生转储下次启动后台先用随包符号生成 `*.dmp.report.log`，再异步补全 Electron 官方符号（不上传转储文件） |
