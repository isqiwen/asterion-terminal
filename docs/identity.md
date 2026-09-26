# 本地身份、会话与终端锁

账号、会话与终端锁由 Rust 入口 `asterion-server` 的身份服务（L3 应用服务）实现，目标模块边界见 [架构设计](architecture.md)。账号保存在本机 PostgreSQL；本地账号模式不依赖云端账号中心，也不证明邮箱所有权。

## 注册与验证

注册必须设置 12–128 字符密码及独立的 6 位数字 PIN。固定 Rust 内核负责随机盐、scrypt 派生与常数时间摘要比较；密码长度和 PIN 规则由身份服务决定。未确认账号不能登录；验证和密码重置都要求存在的账号、有效挑战、未过期且一次性消费。

验证策略由服务端 `ASTERION_ACCOUNT_VERIFICATION` 选择，客户端从 `/api/v1/account/capabilities` 读取，不能自行降级：

| 策略 | 当前行为 |
|---|---|
| `local`（默认） | 不发信，接受任意 6 位数字；界面标记本地验证，重发无需等待 |
| `email` | SMTP 发送随机验证码；HMAC 摘要保存，10 分钟有效、5 次尝试、60 秒重发间隔 |

邮件发送失败使注册回滚，不自动切换本地验证。用户界面和接口都没有邮件配置入口：发信服务器由运维在数据目录写入 `identity-mail.json`（`sender`、`host`、`port`、`security` 为 `tls`/`starttls`/`none`，可选 `username`、`password`），服务只读取；缺失时注册返回 503 `MAIL_NOT_CONFIGURED`。邮件策略及适配器的存在不表示已交付托管账号服务。

## 会话与访问

登录校验密码，连续 5 次失败限制尝试 15 分钟。会话有效期为 12 小时，退出及密码重置撤销相应会话。桌面后台要求账号/PIN 授权；worker 使用独立运行凭据和任务租约，终端锁定不停止已接受的后台任务。

身份服务以运行密钥构造内核会话机制，产生 32 字节安全随机令牌，校验令牌格式、摘要与精确到期边界，并持久化摘要、账号归属与期限；退出和密码重置在同一数据库事务内删除相应会话，不存在的记录明确拒绝。写事务在 SQLite 上先取得写锁，在 PostgreSQL 上锁定所读行，失败计数与锁定状态先提交再返回拒绝。

同一桌面进程在原生宿主内存集中保存账号会话与修订号，新窗口读取并向账号 API 校验。窗口约每秒同步宿主状态、每分钟重新确认后台会话；比较修订号防止晚到响应覆盖新登录/退出。退出后其他窗口清理已加载内容，关闭单窗不退出其他窗口，退出应用释放宿主会话。

会话原文不写入 localStorage、磁盘或布局文件。当前本机工作区总体共享；研究草稿等指定资源另按认证账户隔离，不能据此宣称完整多租户隔离或云端同步。服务端作用域权限见 [接口协议](protocols.md)。

## PIN 与锁定

PIN 与邮箱验证码独立，本地验证模式也必须使用正确 PIN 解锁。缺失注册安全记录或 PIN 哈希的账号明确拒绝，保留记录，不自动补建。

- 默认空闲 5 分钟锁定，可在“设置 → 安全”选择 1、5、10、15、30 分钟。
- 已解锁且有焦点窗口的真实鼠标/键盘活动更新活跃时间；轮询、后台任务不续期。最小化、切换应用和睡眠期间继续计时，恢复或重新聚焦立即核对期限。
- 同账号所有窗口约每秒同步锁定，也可从侧栏“锁定终端”或快捷键 ⌘⇧L / Ctrl+Shift+L 立即遮蔽。工作区与布局保留，内容隐藏且禁止交互，受保护业务 API 返回 423。
- 正确 PIN 恢复视图并刷新数据；解锁携带锁状态修订，旧请求不能覆盖新锁定。5 次失败后限制尝试 5 分钟。
- 重设 PIN 必须验证账号密码，邮箱验证码不能直接修改 PIN。重新登录验证密码后建立新的已认证会话。

`identity_pin_security` 保存账号 ID、盐化 scrypt 哈希、空闲期限、锁状态、失败计数及修订。服务端时间和校验结果是权威，前端期限只用于及时遮蔽；跨窗口同步不传递 PIN。

## 入口判定账户状态

账号接口 `/api/v1/account/*` 由入口直接处理。其余仍转发给 Python 进程的接口，由入口按请求的 `X-Account-Session` 判定账户状态，随转发附上 `X-Asterion-Account-Status`（`unlocked`、`locked`、`expired`、`unsupported`、`unavailable`）；已登录时附上 `X-Asterion-Account`（账号邮箱）。判定与空闲锁定使用同一状态机；客户端自带的同名头一律丢弃。

Python 身份插件只读取这两个头：要求账号时，`locked` 返回 423 `TERMINAL_LOCKED`，`expired` 返回 401 `SESSION_EXPIRED`，`unsupported` 返回 409 `UNSUPPORTED_ACCOUNT_SECURITY`，`unavailable` 或未知值返回 503 `ACCOUNT_UNAVAILABLE`。研究等资源的归属账户取入口给出的邮箱。未要求账号的开发模式归属固定为 `local-development`。

## 实现与验证

接口见 [account.rs](../services/server/src/account.rs)，账号、验证、会话和锁定由 [identity.rs](../services/server/src/identity.rs) 实现，邮件发送见 [mail.rs](../services/server/src/mail.rs)，表经 [store.rs](../services/server/src/store.rs) 以当前声明校验后创建，结构不符时拒绝启动、不改表。Python 侧只剩 [identity/plugin.py](../src/asterion/identity/plugin.py) 读取入口判定。

凭据机制见 [kernel/security.rs](../kernel/src/security.rs)。唯一现行哈希格式是 16 字节盐的十六进制、冒号及 64 字节派生值的十六进制，scrypt 参数固定为 `N=131072, r=8, p=1`。内核限制输入大小并在每个进程内串行执行派生，避免同时分配多个约 128 MiB 工作区；入口在阻塞线程池执行账号操作，不占用异步工作线程。校验拒绝未知结构，保留原记录，不自动重写或补建。

[身份测试](../services/server/tests/identity.rs)、[入口转发测试](../services/server/tests/forwarding.rs)、[账号页面测试](../apps/terminal/e2e/account.spec.ts) 和 [锁屏页面测试](../apps/terminal/e2e/pin.spec.ts) 覆盖当前策略及故障。冻结环境冒烟见 [smoke_desktop_runtime.py](../scripts/smoke_desktop_runtime.py)，在隔离目录通过真实 API 验证本地注册、登录及受保护导入；不发送外部邮件、不修改用户账号。

剩余差距：离线备份与恢复仍在 Python 中统计账号数并清空 `identity_sessions`（[identity/backup.py](../src/asterion/identity/backup.py)），待恢复机制迁入 Rust 服务时移出。
