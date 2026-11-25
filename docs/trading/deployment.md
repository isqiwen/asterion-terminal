# Trading 部署与运行

Trading 运行于 Ubuntu 24.04 x86_64，每个账户对应一个独立的 systemd 服务实例。主机必须开启时间同步（如 `systemd-timesyncd`），理由：Trading 会丢弃落后 10 秒的报价，且行情时间超前主机时钟 2 秒即终止策略实例。服务程序与离线 Python 策略运行时均随同一安装包分发，Terminal 只作为客户端连接，不参与部署管理。

## 安装与账户配置

```sh
sudo bash "Asterion-Trading-<版本>-linux-x86_64.sh"
sudo asterion-trading
```

第一条命令用于首次安装或升级发布包，第二条命令打开账户管理面板（软链至 `/opt/asterion/trading/bin/setup.sh`）；仅查看本机服务端证书指纹可执行 `sudo asterion-trading fingerprint`。安装包构建方式见[发行](../development.md#发行)。

正式环境固定使用单一安装目录 `/opt/asterion/trading/`，每个账户对应一个 systemd 实例 `asterion-trading@<账户>.service`、配置文件 `config/accounts/<账户>.json` 与数据目录 `data/<账户>/`：

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

- **账户管理面板**：执行 `sudo asterion-trading` 后，总览页列出本机版本、证书指纹及各账户的监听地址、运行状态、开机自启与待执行重启；可新增账户，或按编号进入单个账户面板查看柜台标识（经纪商代码、投资者账号与 AppID）与已授权终端数，并执行启停重启、开机自启开关、分项修改监听端口/柜台前置与 AppID/密码与授权码/脱机告警 Webhook（亦支持完整向导）、增删终端指纹（至少保留一台）及查看日志。所有修改项预填当前值，输错仅重问该项；保存前列出变更清单并确认，无改动不写盘也不重启。密码、授权码与告警 Webhook 在主机隐藏输入，按账户存入 systemd 加密凭据，不进入配置文件或 Terminal。
- **环境与身份不可变更**：`environment` 仅限 `real` 或 `simulation`，一旦生成交易记录即不可更改，已有记录的实例亦不能更换柜台账户。
- **变更生效与重启**：账户面板内的改动在选 `0` 返回上级时统一生效（仅重启一次；交易时段内延至收盘后自动重启）；有待执行重启时，可在账户面板选 `1` 确认后立即重启。重启会使交易授权失效，柜台挂单保持不变且绝不重发。
- **Terminal 配对**：Terminal 首次启动生成本机客户端证书（Market、Warehouse、Trading 登记同一指纹），从“设置 → 关于”复制指纹登记至账户的 `terminals`，再在 Terminal“设置 → 交易服务”填写主机地址、端口、账户名与本机服务端证书指纹（TLS 1.3 双向固定指纹）。
- **账户执行唯一归属**：账户 ID 为本地实例名，执行归属由 CTP 的 `broker_id` 与 `user_id` 唯一确定。安装目录内全部实例共用 `data/owners/`，开通或恢复时先获取独占锁，再打开账本与交易 SDK；更改实例名、端口、前置或 AppID 均无法绕过归属锁取得第二份执行权。同一柜台账户永久绑定首次使用的交易记录目录，进程退出亦不可另建空账本（拒绝时报告原目录路径）。启动仅校验账本中的账户 ID、`environment`、`broker_id` 与 `user_id`：变更交易前置或 AppID 重启后可沿用原账本（授权不恢复）；变更经纪商代码或投资者账号则拒绝启动并输出配置文件路径。`environment` 为用户标签，不参与去重，因此不同仿真柜台若复用相同的经纪商代码与账号也会共享同一归属。

## 部署目录

```text
/opt/asterion/trading/
├── bin/
│   ├── asterion_trading
│   ├── asterion_crash_reporter
│   ├── cold_backup.py
│   └── setup.sh
├── lib/
│   ├── ctp-trader.so
│   ├── ctp-md.so
│   └── strategy/                   # 策略运行器、共享 API 与协议代码
├── runtime/python/                 # 随包分发的离线 Python 与数值依赖
├── config/
│   ├── logging.json
│   ├── accounts/<账户>.json
│   └── tls/
│       ├── server.crt
│       └── server.key
├── logs/
└── data/
    ├── owners/                     # 本机账户执行归属绑定与独占锁
    └── <账户>/
        ├── journal.sqlite          # 风控政策、命令与柜台回报
        ├── archives/               # 已封存的历史交易记录
        ├── ctp-flow/               # CTP SDK 流文件与最近同步快照
        ├── owner/                  # 账户数据目录所有权锁
        └── strategies/
            ├── bars.sqlite         # 本地录制的实时 K 线
            ├── catalog.pb          # 柜台合约目录缓存
            ├── instance.pb         # 当前或最近策略实例状态
            ├── packages/<名称>/<版本>/
            └── runs/<实例ID>.sqlite
```

数据子目录按运行进度自动创建，策略包与实例记录均由 Trading 管理（规则见[策略实例](README.md#策略实例)）。默认日志文件为 `logs/asterion_trading-<账户>_YYYY-MM-DD.log`，配置字段见[运行日志](../services.md#运行日志)。

安装目录之外的文件：

```text
/usr/local/bin/asterion-trading               # 软链至 /opt/asterion/trading/bin/setup.sh
/etc/systemd/system/asterion-trading@.service
/etc/credstore.encrypted/asterion-trading-<账户>/
├── password
└── auth_code
/etc/credstore.encrypted/asterion.trading.<账户>.webhook   # 可选脱机告警 Webhook URL
```

systemd 启动时解密凭据并通过 `CREDENTIALS_DIRECTORY` 注入进程；服务端 TLS 私钥保留在安装目录，柜台密码、授权码与可选告警 Webhook URL 均保存在加密凭据目录，不进入配置文件或运行日志。

## 运行与停止

systemd 通过 `bin/asterion_trading --account <账户>` 启动各账户实例。程序自动依据自身路径定位安装目录，不接受 `--installation`。

```sh
systemctl status asterion-trading@<账户>.service
journalctl -u asterion-trading@<账户>.service
```

上述命令用于查看服务状态与启停日志，业务详情查看安装目录 `logs/`，账户同步、授权与策略运行态在 Terminal 交易页观察。服务启动后自动连接柜台，交易授权始终须由使用者显式开启。

正常停止会先关闭新操作接纳、结束运行中的策略并处理撤单，但**不会平仓**；进程若被强杀，未终结委托须到柜台核实。重启后授权一律重置，详见[交易规则](../safety.md)。

进程卡死由 systemd 看门狗自动拉起：启动初始化、账户主循环、待处理命令或账本写入任意环节超过 30 秒无进展时，Trading 停止向 systemd 报活，60 秒超时后 systemd 以 `SIGABRT` 终止并重启进程，现场生成崩溃报告；带着已有交易记录启动时推送 `service_restarted` 告警，提示授权已失效。

## 运行处置

| 情况 | 第一步处置 | 后续动作 |
| --- | --- | --- |
| 需要立即制动 | 点击“停止下单”：服务收令即关闭发送，策略实例随之终止并撤其挂单 | 手动挂单另行撤单；如需减仓，重新“允许下单”后手动平仓 |
| 告警 `service_restarted` | 授权已失效，先到柜台核对挂单与持仓 | 需继续运行时重新允许下单，并以新实例 ID 从本地录制 K 线热重启 |
| 告警 `unconfirmed_order` | 到柜台核实该笔委托是否存在 | 确认不存在后在交易页标记“已核实券商无此委托”；标记前策略暂停全部规划（含减仓与平仓） |
| 告警 `recovery_required` | 账本写入或完整性异常，已停止新增并保留现场 | 排查磁盘空间、权限与日志，按原记录核验或冷备恢复后再启动 |
| 告警 `risk_limit_breached` | 触发资金或撤单硬风控，开仓已拦截（减仓不受限） | 核对盈亏、保证金占用或当日撤单数，决定是否减仓或调整策略 |
| 告警 `order_rejected` / `batch_abandoned` | 柜台拒单或单合约批次超时/被拒，实例继续运行 | 查看实例事件与拒因（资金、价差、涨跌停或流控），按需处理残余敞口 |
| 告警 `strategy_failed` | 查看实例事件与 `logs/strategy-*.log` | 修复后用新实例 ID 重启；既有持仓保留，不会自动平仓 |
| 告警 `broker_disconnected` | SDK 自动重连；非维护时段持续未恢复时检查网络与前置地址 | 重连同步完成前不会发送，也不自动重发 |
| Terminal 提示停滞或失联 | 执行 `systemctl status` 与 `journalctl` 排查；卡死会触发看门狗重启 | 若已重启，按 `service_restarted` 处置 |
| 策略报“行情时间超前主机时钟”失败 | 执行 `timedatectl` 核对时钟同步状态 | 时钟恢复正常后用新实例 ID 重启 |

脱机告警以 HTTP `POST` 发送 JSON 载荷：

```json
{"service": "asterion-trading", "account": "<账户>", "kind": "<事件类型>", "time_ms": 1730000000000, "text": "...", "details": {...}}
```

冷备份需停止服务（会重置授权并中断策略实例），浸泡与实盘阶段宜安排在周末执行，交易日以柜台结算单逐日对账。

若版本升级说明交易记录格式已变更，旧账本不可复用，按以下步骤重置账户：

1. 在 Terminal 停止策略并撤单，确认柜台无挂单（持仓可保留），点击“停止下单”。
2. 停止该账户 systemd 实例，将 `data/<账户>/` 完整移至归档目录（勿直接删除）。
3. 删除 `data/owners/` 中 `directory` 指向该账户目录的 `.json` 归属文件。
4. 重启实例，在 Terminal 重新开通账户、发布策略包并授权。

## 升级与备份

- **配套升级**：须在收盘后或无持仓、无挂单时将 Trading 与 Terminal 升级至同一版本。常规协议哈希不匹配时，Terminal 会禁用下单、授权、修改政策与启动策略等全部常规业务，仅保留基于独立紧急契约的停止策略、撤单与撤销授权。
- **升级步骤**：先停止本机全部账户实例再执行新安装包；脚本替换程序、库与离线运行时，并将各账户配置迁移至当前格式（保留已有值、新增项填默认值、废弃项移除并逐项列出改动），完整保留 TLS 身份与业务记录；升级结束后进入账户管理面板，由使用者手动启动账户并核对状态。
- **完整性保护**：安装未完成时存在残留标记，服务会拒绝启动；必须重新运行完整发布包修复，严禁手动删标记绕过。
- **冷备与跨主机迁移**：冷备前停止全部账户，使用内置 `cold_backup.py` 或完整复制安装目录（含 `owners/`、`archives/`、策略包与实例记录）。跨主机冷备接管前须先停止旧主机全部实例并确保不再拉起，在新主机同一标准路径 `/opt/asterion/trading/` 恢复 `data/`、配置 TLS 证书与加密凭据，核对账户身份与柜台状态后再授权；联系不上旧主机时严禁接管。

## CTP SDK 溯源与测评版 / 生产版切换

`conan/ctp/sources.json` 区分 `prod`（生产版）与 `eval`（看穿式监管测评版），记录各头文件与动态库的 SHA-256 及来源标记。CI 与日常开发默认使用 `prod` 中带摘要校验的镜像条目；穿透式监管认证与实盘发布前，必须切换为上期技术（`www.sfit.com.cn`）或期货公司提供的官方 SDK 包：

```sh
# 1. 导入官方生产版 / 测评版压缩包或解压目录，写入官方 SHA-256 并移除第三方镜像 URL
python3 scripts/prepare_ctp.py --profile prod --local-dir /path/to/official_prod_linux64.tar.gz --record-origin "SFIT official v6.7.7 prod"
python3 scripts/prepare_ctp.py --profile eval --local-dir /path/to/official_eval_linux64.tar.gz --record-origin "Broker official v6.7.7 eval"

# 2. 离线切换 prod 与 eval（仅校验已登记的官方 SHA-256，不联网）
python3 scripts/prepare_ctp.py --profile eval --local-dir /path/to/official_eval_linux64.tar.gz --require-official
python3 scripts/prepare_ctp.py --profile prod --local-dir /path/to/official_prod_linux64.tar.gz --require-official

# 3. 容器化构建 Linux 发布包并强制校验官方来源（同步输出 build/linux-bundles/ctp-provenance.json）
ASTERION_CTP_PROFILE=eval ASTERION_CTP_LOCAL_DIR=/path/to/official_eval_linux64.tar.gz ASTERION_CTP_REQUIRE_OFFICIAL=1 scripts/services/build_linux_services.sh
```

每次准备均会在 `build/ctp-sdk/<os>-<arch>/provenance.json`（服务打包同步输出至 `build/linux-bundles/ctp-provenance.json`）记录当前 `profile`、来源说明、官方标识与 6 个文件的 SHA-256，防止测评版与生产版混用。

完整冷备与恢复流程见[备份与恢复](../services.md#备份与恢复)，部署完成后按[CTP 仿真验收](../acceptance/ctp-simulation.md)核实柜台行为。
