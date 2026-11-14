# Trading 部署与运行

Trading 运行于 Ubuntu 24.04 x86_64，每个账户一个 systemd 服务实例。主机必须开启时间同步（如 `systemd-timesyncd`），理由：Trading 丢弃落后 10 秒的报价，行情时间超前主机 2 秒就结束策略实例。程序和 Python 策略运行时由同一服务包提供，Terminal 只连接服务，不管理其部署。

## 安装与账户配置

```sh
sudo bash "Asterion-Trading-<版本>-linux-x86_64.sh"
sudo /opt/asterion/trading/bin/setup.sh
```

第一个命令安装或升级发布包，第二个命令管理已安装服务的账户配置。构建发布包见[发行](../development.md#发行)。

正式环境只支持一台 Trading 主机和一套安装目录 `/opt/asterion/trading/`，每个账户一个 systemd 实例 `asterion-trading@<账户>.service`，配置是 `config/accounts/<账户>.json`，交易记录在 `data/<账户>/`。

```json
{
  "bind": "0.0.0.0",
  "port": 7460,
  "terminals": ["客户端证书的 SHA-256 指纹"],
  "environment": "simulation",
  "market_front": "tcp://行情前置:端口",
  "broker": {"front": "tcp://交易前置:端口", "broker_id": "经纪商代码", "user_id": "投资者账号", "app_id": "AppID"}
}
```

- `sudo /opt/asterion/trading/bin/setup.sh` 进入菜单：列出账户及其监听地址、运行状态、开机自启和待执行的重启，以及本机证书指纹。可以新增账户，或按编号打开一个账户：添加或按编号删除 Terminal（至少保留一个）、修改配置、停止并停用实例，或启动已停止的实例。修改时每项预先填好当前值，输错只重问该项；保存前列出改动并确认，没有改动不保存，也不重启。密码和授权码在主机上隐藏输入，按账户存为 systemd 加密凭据，不进入配置或 Terminal。
- `environment` 只能是 `real` 或 `simulation`，写入交易记录后不能再改；已有交易记录的实例不能更换账户。
- 账户菜单里保存的改动在选“Back”时一起生效，只重启一次；交易时段内延到收盘后重启。有待执行的重启时，可以确认后立即重启。重启会结束授权，柜台上的挂单保留，也不重发。
- Terminal 第一次启动时生成本机客户端证书（Warehouse 和交易主机登记同一个），在“设置 → 关于”复制它的指纹登记到账户的 Terminal，再在 Terminal 填写主机地址、端口、账户和交易服务证书指纹。连接是双向固定指纹 TLS。Terminal 不部署、启动或备份交易服务。

账户 ID 是本地实例名；执行归属由 CTP 的 `broker_id` 与 `user_id` 共同确定。同一安装目录内的所有实例共用 `data/owners/`，开通或恢复账户时先取得独占锁，再打开记录和创建交易 SDK；换实例名、端口、前置或 AppID 都不能取得第二份执行权。

同一柜台账户永久绑定首次使用的交易记录目录，进程退出也不能换目录建立空账本；拒绝时报告原目录，须恢复原实例和记录。启动时只核对记录里的账户 ID、`environment`、`broker_id` 和 `user_id`：改交易前置或 AppID 后重启，接着使用原记录，每次登录用配置里的当前值，授权不恢复；改了经纪商代码或账号则拒绝启动并报告配置文件路径。`environment` 是使用者填写的标签，不参与去重，因此不同仿真柜台若使用相同的经纪商代码和账号，也共用这份归属。

## 部署目录

```text
/opt/asterion/trading/
├── bin/
│   ├── asterion_trading
│   └── setup.sh
├── lib/
│   ├── ctp-trader.so
│   ├── ctp-md.so
│   └── strategy/                   # 策略运行器、共享 API 与协议代码
├── runtime/python/                 # 随包提供的 Python 和依赖
├── config/
│   ├── logging.json
│   ├── accounts/<账户>.json
│   └── tls/
│       ├── server.crt
│       └── server.key
├── logs/
└── data/
    ├── owners/                     # 本机账户执行归属
    └── <账户>/
        ├── journal.sqlite          # 账户政策、命令与柜台回报
        ├── archives/               # 已封存的交易记录
        ├── ctp-flow/               # CTP SDK 流文件
        ├── owner/                  # 交易记录目录的所有权
        └── strategies/
            ├── packages/<名称>/<版本>/
            └── runs/<实例ID>.sqlite
```

目录按运行进度生成。策略包和实例记录由 Trading 管理；发布与恢复规则见[策略实例](README.md#策略实例)。默认日志文件为 `logs/asterion_trading-<账户>_YYYY-MM-DD.log`，配置字段见[运行日志](../services.md#运行日志)。

安装目录之外：

```text
/etc/systemd/system/asterion-trading@.service
/etc/credstore.encrypted/asterion-trading-<账户>/
├── password
└── auth_code
```

systemd 解密凭据后通过 `CREDENTIALS_DIRECTORY` 提供给服务。服务端 TLS 私钥位于安装目录，柜台密码与授权码位于加密凭据目录，两者用途不同。

## 运行与停止

systemd 执行 `bin/asterion_trading --account <账户>`。程序根据启动路径定位安装目录，不接受 `--installation`；账户文件决定监听端口、柜台身份和允许连接的 Terminal 指纹。

```sh
systemctl status asterion-trading@<账户>.service
journalctl -u asterion-trading@<账户>.service
```

服务状态和启停诊断使用上述命令，业务事件读取安装目录内的日志。账户同步、授权和策略状态在 Terminal 交易页查看。服务启动后自动连接柜台，交易授权仍须由使用者确认。

正常停止会关闭新操作接纳、结束策略并处理撤单；持仓不会因此平仓。进程被强制终止后的委托须按柜台事实核实。重新启动不恢复交易授权，操作要求见[交易规则](../safety.md)。

## 升级与备份

- 在收盘后或没有持仓和挂单时升级，Terminal 同时装同一版本：协议不一致时 Terminal 的一切操作都会被拒绝，包括撤单和平仓。
- 升级前停止全部账户，升级脚本只替换程序、库和运行时，保留账户配置、身份及记录；升级完成后由使用者启动账户并核对状态。
- 安装不完整时服务拒绝启动，重新运行完整发布包修复，不能通过删除标记绕过。
- 冷备份前停止全部账户，复制安装目录及其数据；备份范围包括 `owners/`、`archives/`、策略包和实例记录。
- 迁移前停止全部旧实例并保持停用，再在新主机恢复完整记录、配置柜台加密凭据，核对账户、证书与柜台状态后重新授权；联系不上旧主机时不接管。

完整备份规则见[备份与恢复](../services.md#备份与恢复)，部署后按[CTP 仿真验收](../acceptance/ctp-simulation.md)核实柜台行为。
