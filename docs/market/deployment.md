# Market 部署

## 安装

Market 运行于 Ubuntu 24.04 x86_64，安装目录为 `/opt/asterion/market/`。发布物为自解压脚本 `Asterion-Market-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_market_setup`；本地通过 `scripts/services/build_linux_services.sh` 构建并输出到 `build/linux-bundles/`），内含服务程序、崩溃解析器（`bin/asterion_crash_reporter`）、冷备脚本（`bin/cold_backup.py`）、安装管理脚本（借助主机 Python 3 读写配置）、默认日志配置及两个 CTP 动态库。

```sh
sudo bash "Asterion-Market-<版本>-linux-x86_64.sh"   # 首次安装并配置；后续执行即为升级
sudo asterion-market                                 # 打开管理面板（软链至 /opt/asterion/market/bin/setup.sh）
```

- **时钟同步**：主机必须开启时间同步（如 `systemd-timesyncd`）。理由：行情页依据接收时间判断报价新鲜度与“来源时间超前”。
- **服务单元**：安装时创建并启用 `asterion-market.service`，以执行 `sudo` 的普通账户运行，仅开放 `logs/` 与数据目录写权限，`Restart=always`。
- **首次向导**：依次询问监听地址、端口（默认 `7451`）、数据缓存目录、CTP 行情与交易前置、经纪商代码、投资者账号、AppID，随后逐条粘贴 Terminal 证书指纹（见 Terminal“设置 → 关于”，直接回车结束），最后隐藏录入 CTP 密码与授权码。
- **管理面板**：安装后执行 `sudo asterion-market` 打开面板，展示版本、运行状态、开机自启、监听地址、已授权终端数与本机证书指纹，支持启停重启、开机自启开关、分项修改配置或凭据（亦支持完整向导）、增删终端指纹（至少保留一台）、查看运行日志及单次测试 CTP 凭据连通性。修改项均预填当前值，输错仅重问该项；保存前先列出变更清单并确认，无改动不写盘。柜台身份不变时可保留已有密码与授权码，变更经纪商或投资者账号时须重新录入。面板内的改动在选 `0` 退出时统一生效（交易时段内延至收盘后自动重启）；有待执行重启时，可选 `1` 确认后立即重启。
- **凭据校验与启动核验**：首次配置或修改 CTP 前置、账号、AppID、密码与授权码后，脚本提示通过 `systemd-run` 解密凭据单次调用 `asterion_market --check-login` 校验柜台认证与登录（不重试，也不与运行中的 Market 抢占锁；亦可随时在管理面板选 `7` 触发）。启动或重启后，脚本自动核验 systemd 运行态、监听端口与本次新增日志中的 CTP 登录结果，凭据被拒或进程异常退出时直接显示原因，非交易时段或柜台未就绪则提示而不阻塞部署。
- **行情账户隔离**：投资者账号必须为独立行情账户，严禁使用实盘交易账户，见[交易规则](../safety.md)。
- **升级**：替换可执行程序与库文件，将 `market.json` 迁移至当前版本结构（保留已有值、新增项填默认值、废弃项移除并逐项列出改动），同时保留 TLS 身份与目录缓存，随后进入管理面板。理由：服务严格校验当前配置结构，缺项或多余项均拒绝启动。交易时段内（北京时间工作日 `08:45–15:30`、`20:45–次日 02:45`）不中断运行中的服务，改为收盘后通过定时器自动重启。
- **服务端指纹**：安装时生成 `config/tls/server.crt`，每次运行脚本都会输出其 DER 编码的 SHA-256 指纹（并非证书文件本身的哈希），可直接填入 Terminal；不重启查询可执行 `sudo asterion-market fingerprint`。

## 配置

`config/market.json` 仅包含非秘密配置：

```json
{
  "bind": "0.0.0.0",
  "port": 7451,
  "data": "/opt/asterion/market/data",
  "clients": [{"fingerprint": "<客户端证书的 SHA-256 指纹>"}],
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

CTP 密码与授权码经 `systemd-creds` 加密存放在 `/etc/credstore.encrypted/asterion.ctp.*`，不写入配置文件；TLS 私钥位于 `config/tls/server.key`。缺少配置、TLS 文件或凭据时服务拒绝启动并输出缺失路径。

## 运行

- 启动后立即连接 CTP，不等待客户端接入；超时或柜台临时不可用时自动恢复，策略与错误边界见[实时行情](README.md#实时行情)。
- 收到 `SIGTERM` 后，报价流发出最后一帧即正常退出；原始行情事件队列溢出时主动退出并由 systemd 重启。
- 默认日志位于 `logs/asterion_market_YYYY-MM-DD.log`，保留 30 天；行情连接的每次状态跃迁（登录、断线及 CTP 原因码、重连）均记入日志。
