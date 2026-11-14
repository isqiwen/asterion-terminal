# Market 部署

## 安装

Market 主机是 Ubuntu 24.04 x86_64，安装在 `/opt/asterion/market/`。发布物是自解压脚本 `Asterion-Market-<版本>-linux-x86_64.sh`（CMake 目标 `asterion_market_setup`；本地用 `scripts/services/build_linux_services.sh` 构建，输出到 `build/linux-bundles/`），包含服务程序、安装脚本、默认日志配置和两个 CTP 库。

```sh
sudo bash "Asterion-Market-<版本>-linux-x86_64.sh"   # 第一次：安装并配置；以后：升级
sudo /opt/asterion/market/bin/setup.sh               # 菜单：增删 Terminal、修改配置
```

- 主机开启时间同步（如 `systemd-timesyncd`）。理由：报价的接收时间用于行情页的新鲜度和“来源时间超前”判断。
- 安装时写入并启用 `asterion-market.service`：以执行 `sudo` 的账户运行，只能写 `logs/` 和数据目录，`Restart=always`。
- 安装时依次询问监听地址、端口（默认 7451）、数据目录、行情和交易前置、经纪公司代码、投资者代码、AppID，再逐个粘贴 Terminal 证书指纹（Terminal 的“设置 → 关于”，直接回车结束），最后隐藏输入 CTP 密码和授权码。
- 已安装后运行脚本进入菜单：显示服务状态、监听地址、本机证书指纹和已登记的 Terminal，可以添加或按编号删除 Terminal（至少保留一个），或修改其他配置。修改时每项预先填好当前值，输错只重问该项；保存前列出改动并确认，没有改动不保存。柜台账户不变时可以保留已保存的密码和授权码。菜单里保存的改动在选“Done”时一起生效，交易时段内延到收盘后重启；有待执行的重启时，菜单可以确认后立即重启。
- 投资者代码要用单独的行情账户，不得使用实盘交易账户，见[交易规则](../safety.md)。
- 升级替换程序和库，把 `market.json` 改写为新版本的格式（保留现有值，新增项取默认值，不再使用的项删除，逐项列出改动），保留 TLS 身份和目录缓存，然后进入菜单。理由：服务只认当前格式，缺项或多项都拒绝启动。交易时段内（北京时间工作日 08:45–15:30、20:45–次日 02:45）不重启正在运行的服务，改为收盘后由定时器重启。
- 安装时生成服务端证书 `config/tls/server.crt`，每次运行脚本都显示它的指纹，可直接粘贴到 Terminal。指纹是证书 DER 编码的 SHA-256，不是文件本身的哈希；不重启查看：`sudo /opt/asterion/market/bin/setup.sh fingerprint`。

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
- 默认日志在 `logs/asterion_market_YYYY-MM-DD.log`，保留 30 天。行情连接每次变化（登录、断线及 CTP 原因码、重连）记一行。
