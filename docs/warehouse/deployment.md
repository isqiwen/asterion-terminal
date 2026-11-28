# Warehouse 部署

Warehouse 在 Linux 主机由 systemd 独立看护，负责历史采集与版本化仓库管理，不连接任何交易柜台；Terminal 的关闭、断线或升级均不影响它。

## 安装

Warehouse 运行于 Ubuntu 24.04 x86_64，安装目录为 `/opt/asterion/warehouse/`。发布物为自解压脚本 `Asterion-Warehouse-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_warehouse_setup`；本地通过 `scripts/services/build_linux_services.sh` 构建并输出到 `build/linux-bundles/`），内含服务程序、崩溃解析器（`bin/asterion_crash_reporter`）、冷备脚本（`bin/cold_backup.py`）、安装管理脚本、内置规则表与默认日志配置。服务本身不依赖 Python，安装脚本借助主机 Python 3 读写配置。

```sh
sudo bash "Asterion-Warehouse-<版本>-linux-x86_64.sh"   # 首次安装并配置；后续执行即为升级
sudo asterion-warehouse                                 # 打开管理面板（软链至 /opt/asterion/warehouse/bin/setup.sh）
```

- **时钟同步**：主机必须开启时间同步（如 `systemd-timesyncd`）。理由：各数据源按主机北京日期只采至昨日，时钟超前会把未收盘的当日行情当作整日落盘，之后不再重采。
- **服务单元**：安装时创建并启用 `asterion-warehouse.service`，以执行 `sudo` 的普通账户运行，仅开放 `logs/` 与数据目录写权限，`Restart=always`。
- **首次向导**：依次询问监听地址、端口（默认 `7450`）、数据存储目录（默认 `/opt/asterion/warehouse/data`，可指向独立磁盘）、公开补缺交易所、Tushare 每分钟请求上限（默认 `500`）与每日自动更新时间（北京时间，默认 `06:00`，填 `-` 关闭），随后逐条粘贴 Terminal 证书指纹（见 Terminal“设置 → 关于”，直接回车结束），最后隐藏录入 Tushare token。
- **管理面板**：安装后执行 `sudo asterion-warehouse` 打开面板，展示版本、运行状态、开机自启、监听地址、已授权终端数与本机证书指纹，支持启停重启、开机自启开关、分项修改配置或凭据（亦支持完整向导）、增删终端指纹（至少保留一台）及查看运行日志。修改项均预填当前值，输错仅重问该项；保存前先列出变更清单并确认，无改动不写盘，已有 token 可选择保留。面板内的改动在选 `0` 退出或选 `1` 立即应用时统一重启生效。
- **升级**：替换可执行程序与规则表（保留更晚的本地大商所核验截止日），将 `warehouse.json` 迁移至当前版本结构（保留已有值、新增项填默认值、废弃项移除并逐项列出改动），同时保留 TLS 身份、仓库数据与采集任务，随后进入管理面板；随时可重启，不影响行情与交易。
- **服务端指纹**：安装时生成 `config/tls/server.crt`，每次运行脚本都会输出其 DER 编码的 SHA-256 指纹（并非证书文件本身的哈希），可直接填入 Terminal；不重启查询可执行 `sudo asterion-warehouse fingerprint`。

## 部署目录

默认布局如下，数据目录中的子目录随采集进度生成。`warehouse.json` 中的 `data` 可指向独立磁盘，程序、配置与日志仍留在安装目录。

```text
/opt/asterion/warehouse/
├── bin/
│   ├── asterion_warehouse          # 服务程序
│   ├── asterion_crash_reporter     # 崩溃符号解析器
│   ├── cold_backup.py              # 冷备、校验与恢复脚本
│   └── setup.sh                    # 管理面板；升级请运行新发布安装包
├── config/
│   ├── warehouse.json              # 安装生成的非秘密配置
│   ├── exchange_rules.json         # 经核验的六所最小变动价位与大商所计费规则
│   ├── logging.json                # 日志输出、格式与保留天数
│   └── tls/
│       ├── server.crt              # 服务端证书
│       └── server.key              # 服务端私钥
├── logs/                           # 按日滚动的服务日志
└── data/                           # 可配置为任意绝对路径
    ├── warehouse.lock              # 同一份数据只允许一个 Warehouse 进程
    ├── acquisition/
    │   └── <任务ID>.pb             # 采集请求、进度与结果
    ├── warehouse/                  # Tushare 来源仓库
    └── sources/
        ├── research/warehouse/     # 合成统一研究数据集
        ├── verified/warehouse/     # 内置核验的历史最小变动价位
        ├── gtja/warehouse/         # 国泰君安报表的历史报价单位
        ├── sina/warehouse/         # 新浪财经补缺行情
        ├── shfe/warehouse/
        ├── ine/warehouse/
        ├── czce/warehouse/
        ├── dce/warehouse/
        ├── cffex/warehouse/
        └── gfex/warehouse/         # 各交易所独立来源仓库
```

每份仓库目录内部均包含 Parquet 分区文件、`versions/` 版本清单、`shards/` 清单分片与指向当前版本的 `current` 文件，版本与分片规则见[仓库与固定版本](README.md#仓库与固定版本)。

安装目录之外的文件：

```text
/usr/local/bin/asterion-warehouse        # 软链至 /opt/asterion/warehouse/bin/setup.sh

/etc/systemd/system/
└── asterion-warehouse.service

/etc/credstore.encrypted/
└── asterion.warehouse.tushare.token     # 配置 Tushare token 时生成
```

Tushare token 由 `systemd-creds` 加密保存，不写入 `warehouse.json`；systemd 启动时通过 `CREDENTIALS_DIRECTORY` 提供解密凭据，TLS 私钥单独保存在 `config/tls/server.key`。

## 配置

`config/warehouse.json` 仅包含非秘密配置：

```json
{
  "bind": "0.0.0.0",
  "port": 7450,
  "data": "/opt/asterion/warehouse/data",
  "clients": [{"fingerprint": "<客户端证书的 SHA-256 指纹>"}],
  "acquisition": {"exchanges": ["SHFE", "INE", "CZCE", "DCE", "CFFEX", "GFEX"], "tushare": {"requests_per_minute": 500}, "update_at": "06:00"}
}
```

- `data` 为仓库与采集任务根目录（须为绝对路径）。更换目录请通过管理面板操作：脚本会自动建目录并授予服务写权限，旧目录数据不会自动迁移，如需延续使用请先停服务再手动搬迁。
- `tushare.requests_per_minute` 为每分钟请求上限（1–500，默认 500）；若超出账户积分允许频率，Tushare 拒收请求并使任务报 `rate_limit`，调低后重试。token 读取自主机加密凭据 `asterion.warehouse.tushare.token`。因合约表与交易日历仅由 Tushare 提供，安装脚本默认开启 Tushare；`null` 仅用于离线测试。
- `update_at` 为每日自动更新的北京时间（`null` 关闭，见[历史采集](README.md#历史采集)）。默认 `06:00`，理由：各源只采至前一日，凌晨收盘的夜盘已归入下一交易日，清晨更新即可取得昨日全量数据。
- `exchanges` 启用对应交易所公开采集与[历史最小变动价位](README.md#历史最小变动价位)核验；列表非空时自动启用新浪补缺，含非郑商所交易所时自动启用 `gtja` 核验（无需额外凭据）。
- 缺失配置、TLS 文件、已启用来源的凭据或存在未知键时，服务拒绝启动并输出具体路径。
- 默认日志位于 `logs/asterion_warehouse_YYYY-MM-DD.log`，保留 30 天，默认关闭控制台输出；`journalctl -u asterion-warehouse -f` 用于观察启停与标准输出/错误，业务日志请查看日志文件。

## 运行

- systemd 直接运行 `/opt/asterion/warehouse/bin/asterion_warehouse`，程序自动从 `bin/` 上级定位安装目录，不接受 `--installation`。
- 默认监听 `0.0.0.0:7450`；Terminal 与 Lab 通过双向固定指纹 TLS 连接，指纹登记方式见[连接与协议接入](README.md#连接与协议接入)。
- 收到 `SIGTERM` 后停止接收新请求，待后台采集与合成优雅结束后退出。
