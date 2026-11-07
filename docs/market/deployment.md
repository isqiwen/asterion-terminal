# Market 部署

## 安装

Market 主机是 Ubuntu 24.04 x86_64，安装在 `/opt/asterion/market/`。发布物是自解压脚本 `Asterion-Market-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_market_setup`；本地用 `scripts/services/build_linux_services.sh` 构建，输出到 `build/linux-bundles/`），包含服务程序、安装脚本、默认日志配置和两个 CTP 库。

```sh
sudo bash "Asterion-Market-<版本>-linux-x86_64.sh"   # 第一次：安装并配置；以后：升级
sudo /opt/asterion/market/bin/setup.sh               # 只修改配置
```

- 安装时写入并启用 `asterion-market.service`：以执行 `sudo` 的账户运行，只能写 `logs/` 和数据目录，`Restart=always`。
- 配置时依次询问监听地址、端口（默认 7451）、数据目录、行情和交易前置、经纪公司代码、投资者代码、AppID，登记客户端证书指纹，最后隐藏输入 CTP 密码和授权码。投资者代码要用单独的行情账户，不得使用实盘交易账户，见[交易规则](../safety.md)。
- 升级只替换程序和库，保留配置、TLS 身份和目录缓存。交易时段内（北京时间工作日 08:45–15:30、20:45–次日 02:45）不重启正在运行的服务，改为收盘后由定时器重启。
- 安装时生成服务端证书，脚本显示它的指纹，供 Terminal 登记。

## 配置

`config/market.json` 只保存非秘密配置：

```json
{
  "bind": "0.0.0.0",
  "port": 7451,
  "data": "/opt/asterion/market/data",
  "clients": [{"fingerprint": "<客户端证书的 SHA-256 指纹>"}],
  "counter": {"market_front": "tcp://…", "trade_front": "tcp://…", "broker": "…", "user": "…", "app_id": "…"}
}
```

CTP 密码和授权码由 `systemd-creds` 加密保存在 `/etc/credstore.encrypted/asterion.ctp.*`，不写入配置；TLS 私钥在 `config/tls/server.key`。缺少配置、TLS 文件或凭据时拒绝启动，并报告路径。

## 运行

- 启动后立即登录 CTP，不等客户端。行情登录被拒时每分钟重试一次。
- 收到 SIGTERM 后报价流发出最后一帧再退出。原始行情队列写满时进程退出，由 systemd 重启。
- 默认日志在 `logs/asterion_market_YYYY-MM-DD.log`，保留 30 天。
