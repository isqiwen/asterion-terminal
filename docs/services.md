# 服务与部署

部署单元有三类：本机的 Terminal 和 Lab，Linux 上独立部署的 Market，以及 Linux 上每个账户一个的 Trading（含策略子进程）。Market 和 Trading 有各自的安装和生命周期，不属于任何客户端。

## 本机 Lab

Lab 是 Terminal 的子进程（`apps/clients/terminal/native/lab_supervisor.*`）：

- 每次启动 Terminal 时在启动画面里启动，没有启停按钮。
- 进程退出或持续心跳失败后自动重启，直到恢复。
- Lab 以 `--owner-pid` 观察 Terminal，Terminal 退出（包括被强制结束）后随之退出。关闭 Terminal 不会停止 Market 或 Trading。
- Lab 用第一次启动时安装的 Python 运行时以 `python3 -I -B -m asterion_lab` 运行；开发和测试用 `ASTERION_PYTHON` 指定解释器。
- 一个根目录同时只能有一个 Terminal（`terminal/data/services.lock`）。

## 远程服务

- **Market**：用 `Asterion-Market-<版本>-linux-x86_64.sh` 安装到 `/opt/asterion/market`，服务 `asterion-market.service`，见[录制与部署](market/deployment.md)。
- **Trading**：用 `Asterion-Trading-<版本>-linux-x86_64.sh` 安装到 `/opt/asterion/trading`，每个账户一个 `asterion-trading@<账户>.service`，见[交易](trading/deployment.md#安装与账户配置)。安装包自带策略用的 Python 运行时。升级程序前必须停止全部账户。
- 两个服务都以执行 `sudo` 的普通账户运行，凭据是 systemd 加密凭据，启动时自动登录柜台。Trading 重启不恢复授权。
- Terminal 只是客户端：不安装、启动、升级或备份远程服务。

## 连接

- 本机：Unix socket，位于每次运行私有的 `/tmp/ast-<实例>/`。
- Market 和 Trading：TLS，双方各用自签证书，只接受事先登记的对方证书指纹（SHA-256），同一台机器上也一样。
- 断线后重新连接并读取服务端的状态；交易命令不自动重发。
- 状态栏显示 Market、Lab 和各 Trading 连接的状态；读取中断或服务处理停滞进入通知中心，账户页保留最后一次读取的状态并标记失联。

## 桌面安装与升级

Terminal 发布包带有 Terminal 和 Lab 的程序：macOS 的 DMG 拖进“应用程序”，Linux 的 `.deb` 用 apt 安装，见[发行](development.md#发行)。升级前先等研究运行结束并关闭 Terminal。卸载只删除程序，用户资料保留。

本机数据目录：macOS `~/Library/Application Support/Asterion`，Linux `~/.local/share/asterion`。

## 备份与恢复

只支持冷备份：先停止资料的拥有者，再复制。

- 本机：等研究运行结束，关闭 Terminal，确认 Lab 已退出，再复制本机根目录。
- Market 主机：停止 `asterion-market.service`，复制安装目录（配置、TLS 身份）和 `market.json` 里 `data` 指定的数据目录（仓库、录制文件、采集任务；默认在安装目录里，放到别的磁盘时要单独复制）。恢复时数据目录回到原来的路径，或者改 `data` 指向新位置。
- Trading 主机：停止全部账户，复制安装目录（账户记录及 `archives/`、策略包和实例记录）和账户归属目录。停止服务不等于撤单。
- Lab 的 `market-cache/` 可以从 Market 重新下载，不必备份。
- 凭据加密绑定原主机，复制安装目录不包含它们；换机器恢复时要在新主机重新配置凭据。
- 恢复到原来的绝对路径，先核对配置和凭据，再启动。程序读不懂数据格式时明确失败，不要改数据库来绕过。恢复后 Trading 要重新授权。

## 运行日志

每个程序默认在安装目录的 `logs/` 下按日写入日志，保留 30 天。输出由各自的 `config/logging.json` 配置，完整默认文件见[配置文件](configuration.md)。

- `level` 可为 debug、info、warning、error；`file.path` 相对 `bin/`，可用 `{program}` 和 `{pid}`；控制台和文件可分别设置 `pattern`。默认 `[%Y-%m-%d %H:%M:%S.%e %z] [%l] [%n] [%s:%#] %v` 输出带方括号的时间、时区、级别、程序名和调用处文件名/行号，最后是消息。`%s` 是文件名，`%#` 是行号；时区偏移用于对齐不同主机的日志。C++ 使用 spdlog 格式，Lab 支持这里列出的格式符。
- 配置有缺键、多余键或无效值时程序启动失败并报告文件和键；没有这个文件时只写到标准错误。修改后重启生效。
- 日志是自由文本，密码、令牌、授权码和凭据不得写进日志。
- 桌面通过通知中心的“打开本机日志目录”打开本机程序根目录，再进入对应程序的 `logs/`。Market 和 Trading 的日志保存在各自服务主机。

### 崩溃诊断

- Linux Market、Trading 崩溃后，由随包提供的独立解析器自动生成安装目录 `logs/*-crash.report.log`，包含版本、提交、信号、函数名和源码行号。现场只保留一份 `*-crash.frames`，函数名和源码行号统一由解析器生成。若解析被系统关停打断，下次服务启动会补齐同次构建的待解析报告。Trading 仍需重新授权。
- Lab 捕获的异常在日常日志中保留 Python traceback；解释器启动错误、线程未捕获异常和原生扩展崩溃写入 `lab/logs/python-*.log`。回测子进程的标准错误保留在 Lab 的失败记录中。
- Trading 策略子进程的 traceback 和解释器崩溃信息写入服务的 `logs/strategy-*.log`，服务日志记录实例与文件的对应关系。
- Terminal 的 JavaScript 异常堆栈及子进程退出原因写入 `terminal/logs/electron-*.log`；主进程未捕获异常后退出。
- macOS 和 Linux 桌面的原生崩溃保存在 `terminal/logs/crashes/`。下次打开 Terminal 时自动生成 `*.dmp.report.log`：先使用随包符号输出全部可恢复栈帧，再后台从 Electron 官方符号服务器补充 Chromium 符号。解析不阻塞工作台，转储不上传。

解析工具随安装包提供，无须手动安装调试器。桌面按模块标识缓存符号，升级后仍保留旧版本符号。网络不可用或闭源库缺少符号时，文本报告标明缺失模块，相关帧保留模块名和地址；下次启动继续尝试补全。Release 保留行号信息，前端携带 source map；原始现场和转储同时保留，排查完成后可删除。

崩溃现场只做安全采集，符号解析在独立进程或下次启动进行。Sanitizer 构建保留自身的错误报告。`SIGKILL`（包括系统强制杀进程）不能被进程捕获，这类退出从宿主进程或系统日志查看。
