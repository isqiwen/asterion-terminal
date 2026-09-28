# asterion-backtest

单合约、单日、长仓/空仓 SMA 期货回测宿主。复用 `plugins/strategy/cta` 策略与 PaperExecution/FuturesAccount，不连接实盘。

支持两种明确模式：`--input INPUT.pb --directory RESULT_DIRECTORY` 运行独立类型化输入；或 `--session SERVICE --endpoint ENDPOINT --task ID` 作为 Task Service 的独立工作进程。远程模式使用 `--host / --port` 与完整 mTLS 身份。帮助和版本使用 CLI11，构建目标 `asterion-backtest`。

业务限制、撮合时序、研究数据版本与验收见 [研究任务说明](../../docs/research-tasks.md)。Agent 本机自动分配与 Terminal 页面已接入，macOS DMG 和 Linux x86_64 容器部署已验证，Windows 原生待验收。
