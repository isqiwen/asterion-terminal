# Trading 部署与运行

Trading 运行于 Ubuntu 24.04 x86_64，每个账户一个 systemd 服务实例。主机必须开启时间同步（如 `systemd-timesyncd`），理由：Trading 丢弃落后 10 秒的报价，行情时间超前主机 2 秒就结束策略实例。程序和 Python 策略运行时由同一服务包提供，Terminal 只连接服务，不管理其部署。

## 安装与账户配置

```sh
sudo bash "Asterion-Trading-<版本>-linux-x86_64.sh"
sudo asterion-trading
```

第一个命令安装或升级发布包，第二个命令打开已安装服务的账户管理面板（软链到 `/opt/asterion/trading/bin/setup.sh`）；只查看本机证书指纹可运行 `sudo asterion-trading fingerprint`。构建发布包见[发行](../development.md#发行)。

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

- `sudo asterion-trading` 进入账户管理面板：列出本机版本、证书指纹及各账户的运行状态、开机自启、监听地址、柜台标识和待执行重启。可以新增账户，或按编号进入账户面板：启停与重启实例、开机自启开关、分项修改监听端口 / 前置与 AppID / 密码与授权码 / 告警 Webhook（也可运行完整配置向导）、增删 Terminal（至少保留一个）及查看账户日志。修改时每项预先填好当前值，输错只重问该项；保存前列出改动并确认，没有改动不保存，也不重启。密码、授权码与告警 Webhook 在主机上隐藏输入，按账户存为 systemd 加密凭据，不进入配置或 Terminal。
- `environment` 只能是 `real` 或 `simulation`，写入交易记录后不能再改；已有交易记录的实例不能更换账户。
- 账户面板内保存的改动在选 `0` 返回上级时一起生效，只重启一次；交易时段内延到收盘后重启。有待执行的重启时，可在账户面板选 `1` 确认后立即重启。重启会结束授权，柜台上的挂单保留，也不重发。
- Terminal 第一次启动时生成本机客户端证书（Warehouse 和交易主机登记同一个），在“设置 → 关于”复制它的指纹登记到账户的 Terminal，再在 Terminal 填写主机地址、端口、账户和交易服务证书指纹。连接是双向固定指纹 TLS。Terminal 不部署、启动或备份交易服务。

账户 ID 是本地实例名；执行归属由 CTP 的 `broker_id` 与 `user_id` 共同确定。同一安装目录内的所有实例共用 `data/owners/`，开通或恢复账户时先取得独占锁，再打开记录和创建交易 SDK；换实例名、端口、前置或 AppID 都不能取得第二份执行权。

同一柜台账户永久绑定首次使用的交易记录目录，进程退出也不能换目录建立空账本；拒绝时报告原目录，须恢复原实例和记录。启动时只核对记录里的账户 ID、`environment`、`broker_id` 和 `user_id`：改交易前置或 AppID 后重启，接着使用原记录，每次登录用配置里的当前值，授权不恢复；改了经纪商代码或账号则拒绝启动并报告配置文件路径。`environment` 是使用者填写的标签，不参与去重，因此不同仿真柜台若使用相同的经纪商代码和账号，也共用这份归属。

## 部署目录

```text
/opt/asterion/trading/
├── bin/
│   ├── asterion_trading
│   ├── asterion_crash_reporter
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
        ├── journal.sqlite          # 风控设置、命令与柜台回报
        ├── archives/               # 已封存的交易记录
        ├── ctp-flow/               # CTP SDK 流文件与最近同步快照
        ├── owner/                  # 交易记录目录的所有权
        └── strategies/
            ├── bars.sqlite         # 本地录制的实时 K 线
            ├── catalog.pb          # 柜台合约目录缓存
            ├── packages/<名称>/<版本>/
            └── runs/<实例ID>.sqlite
```

目录按运行进度生成。策略包和实例记录由 Trading 管理；发布与恢复规则见[策略实例](README.md#策略实例)。默认日志文件为 `logs/asterion_trading-<账户>_YYYY-MM-DD.log`，配置字段见[运行日志](../services.md#运行日志)。

安装目录之外：

```text
/usr/local/bin/asterion-trading               # 指向 /opt/asterion/trading/bin/setup.sh
/etc/systemd/system/asterion-trading@.service
/etc/credstore.encrypted/asterion-trading-<账户>/
├── password
└── auth_code
/etc/credstore.encrypted/asterion.trading.<账户>.webhook   # 可选脱机告警 Webhook URL
```

systemd 解密凭据后通过 `CREDENTIALS_DIRECTORY` 提供给服务。服务端 TLS 私钥位于安装目录，柜台密码、授权码与可选告警 Webhook URL 均位于加密凭据目录，不进入配置文件或运行日志。

## 运行与停止

systemd 执行 `bin/asterion_trading --account <账户>`。程序根据启动路径定位安装目录，不接受 `--installation`；账户文件决定监听端口、柜台身份和允许连接的 Terminal 指纹。

```sh
systemctl status asterion-trading@<账户>.service
journalctl -u asterion-trading@<账户>.service
```

服务状态和启停诊断使用上述命令，业务事件读取安装目录内的日志。账户同步、授权和策略状态在 Terminal 交易页查看。服务启动后自动连接柜台，交易授权仍须由使用者确认。

正常停止会关闭新操作接纳、结束策略并处理撤单；持仓不会因此平仓。进程被强制终止后的委托须按柜台事实核实。重新启动不恢复交易授权，操作要求见[交易规则](../safety.md)。

进程卡死时由 systemd 重启：账户循环、待处理的命令或记录写入超过 30 秒没有进展，Trading 就停止向 systemd 报活，看门狗 60 秒超时后 systemd 以 `SIGABRT` 结束进程并重新启动，现场写入崩溃报告。带着已有交易记录启动时推送 `service_restarted` 告警，提醒授权已经失效。

## 运行处置

| 情况 | 先做什么 | 然后 |
| --- | --- | --- |
| 需要立刻停止交易 | 点“停止下单”：服务接收命令时就关闭发送，策略实例随之结束并撤掉它的挂单 | 手动挂单另行撤单；要减仓时重新“允许下单”，再手动平仓 |
| 告警 `service_restarted` | 授权已经失效，核对柜台上的挂单和持仓 | 需要继续时重新允许下单，用新实例 ID 从本地录制的 K 线恢复 |
| 告警 `unconfirmed_order` | 到柜台核实这笔委托 | 在交易页标记“已核实券商无此委托”；标记前策略暂停全部规划，包括减仓和平仓 |
| 告警 `recovery_required` | 账本写入或完整性异常，停止新增并保留现场目录 | 检查磁盘空间、权限与日志，按原记录核验或冷备恢复后再启动 |
| 告警 `risk_limit_breached` | 触发资金或撤单硬风控，新增开仓已拦截（减仓仍可用） | 核对盈亏、保证金占用或当日撤单次数，决定是否减仓或调整策略 |
| 告警 `order_rejected` / `batch_abandoned` | 柜台拒单或单合约执行批次超时/被拒，实例继续运行 | 查看实例事件与原因（资金、价差、涨跌停或流控），按需处理剩余敞口 |
| 告警 `strategy_failed` | 看实例事件和 `logs/strategy-*.log` | 修复后用新实例 ID 启动；持仓保留，不会自动平仓 |
| 告警 `broker_disconnected` | SDK 自动重连；前置维护时段之外长时间未恢复时检查网络和前置地址 | 同步完成前不发送，也不重发 |
| Terminal 显示交易服务停滞或失联 | `systemctl status`、`journalctl` 查看；卡死会被看门狗重启 | 重启后按 `service_restarted` 处理 |
| 策略因“行情时间超前主机时钟”失败 | `timedatectl` 核对时间同步 | 修复后用新实例 ID 启动 |

脱机告警通过 HTTP `POST` 发送 JSON 载荷：`{"service": "asterion-trading", "account": "<账户>", "kind": "<事件类型>", "time_ms": 1730000000000, "text": "...", "details": {...}}`。

冷备份要停服务，会结束授权并中断策略实例：浸泡和实盘期间放在周末做，交易日以柜台结算单对账。

升级说明交易记录格式已变化时，旧记录不能继续使用，按以下步骤为账户重新开始：

1. 在 Terminal 停止策略并撤单，确认柜台没有挂单（持仓可以保留），点“停止下单”。
2. 停止账户实例，把 `data/<账户>/` 整个移到归档位置，不要删除。
3. 删除 `data/owners/` 里 `directory` 指向该账户目录的 `.json` 绑定文件。
4. 启动实例，在 Terminal 重新开通账户、发布策略包并授权。

## 升级与备份

- 在收盘后或没有持仓和挂单时升级，Terminal 同时装同一版本：常规协议哈希不一致时 Terminal 会禁用下单、授权、修改政策和启动策略等全部常规业务，仅保留固化在独立紧急控制契约上的停止策略、撤单和撤销授权。
- 升级前停止全部账户，升级脚本替换程序、库和运行时，把每个账户的配置改写为新版本的格式（保留现有值，新增项取默认值，不再使用的项删除，逐项列出改动），保留身份及记录；升级完成后由使用者启动账户并核对状态。
- 安装不完整时服务拒绝启动，重新运行完整发布包修复，不能通过删除标记绕过。
- 冷备份前停止全部账户，复制安装目录及其数据；备份范围包括 `owners/`、`archives/`、策略包和实例记录。
- 跨主机迁移前停止全部旧实例并保持停用，在新主机的相同标准路径 `/opt/asterion/trading/` 恢复完整 `data/`（含绑定规范路径的 `owners/`、账户记录、策略包与实例记录）、生成或配置新主机 TLS 证书与加密凭据，核对账户、证书指纹与柜台状态后重新授权；联系不上旧主机时不接管。

## CTP SDK 溯源与测评版 / 生产版切换

`conan/ctp/sources.json` 按 `prod`（生产版）与 `eval`（看穿式监管测评版）分别记录头文件与动态库的 SHA-256 及来源标记。CI 与日常开发默认使用 `prod` 中带摘要核验的镜像条目；穿透式监管认证与实盘发布前必须切换为上期技术（`www.sfit.com.cn`）或期货公司官方提供的 SDK 包：

```sh
# 1. 导入官方生产版（或测评版）压缩包 / 解压目录，写入官方 SHA-256 并移除第三方镜像 URL
python3 scripts/prepare_ctp.py --profile prod --local-dir /path/to/official_prod_linux64.tar.gz --record-origin "SFIT official v6.7.7 prod"
python3 scripts/prepare_ctp.py --profile eval --local-dir /path/to/official_eval_linux64.tar.gz --record-origin "Broker official v6.7.7 eval"

# 2. 在 prod 与 eval 之间切换（仅校验已登记的官方 SHA-256，不联网）
python3 scripts/prepare_ctp.py --profile eval --local-dir /path/to/official_eval_linux64.tar.gz --require-official
python3 scripts/prepare_ctp.py --profile prod --local-dir /path/to/official_prod_linux64.tar.gz --require-official

# 3. 容器化构建 Linux 发布包并强制要求官方来源（同时输出 build/linux-bundles/ctp-provenance.json）
ASTERION_CTP_PROFILE=eval ASTERION_CTP_LOCAL_DIR=/path/to/official_eval_linux64.tar.gz ASTERION_CTP_REQUIRE_OFFICIAL=1 scripts/services/build_linux_services.sh
```

每次准备都会在 `build/ctp-sdk/<os>-<arch>/provenance.json`（服务打包同步输出到 `build/linux-bundles/ctp-provenance.json`）记录当前使用的 `profile`、来源说明、是否官方源及 6 个文件的 SHA-256，避免测评版与生产版安装包混用。

完整备份规则见[备份与恢复](../services.md#备份与恢复)，部署后按[CTP 仿真验收](../acceptance/ctp-simulation.md)核实柜台行为。
