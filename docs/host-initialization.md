# 目标机器初始化

管理员在目标机器执行独立脚本，授权 SSH 公钥并准备受限管理入口。脚本不安装 Agent；Agent 仍由 Terminal 通过 SSH 上传和安装。私钥默认由 Terminal 本机生成和保存，不能复制到目标机器。

## Linux / systemd

每种桌面安装包都内置 `scripts/node/initialize-linux.py`。在设置 → 连接 → 远程 Linux 点击“导出 Linux 初始化脚本”，将导出的脚本交给目标机器管理员。审阅可信来源的脚本后，在目标机器运行：

```bash
sudo /usr/bin/python3 -I initialize-linux.py
```

脚本会提示粘贴 Terminal 中复制的一行 SSH **公钥**。自动化时也可以传入 `--public-key /path/to/terminal-key.pub`；该文件必须是公钥，不是私钥。可在持有密钥的本机通过 `ssh-keygen -y -f /path/to/private-key > terminal-key.pub` 导出公钥，只将 `.pub` 文件交给管理员。

前置条件：Linux 已启动 systemd；安装 Python 3、OpenSSH Server/SFTP、sudo、useradd；SSH 服务已运行，主配置加载 `/etc/ssh/sshd_config.d/`，启用 PAM。脚本不安装系统软件、不启动 SSH、不改变全局防火墙。自定义 AllowUsers / AllowGroups、条件 Match、PAM 访问策略仍由管理员审核；脚本的配置检查不能替代从真实客户端验证登录。

脚本执行：

1. 创建普通账户 `asterion`，独立主组，home 为 `/var/lib/asterion`，不加入管理员或其他附加组，不设置可用密码。
2. 将公钥放入 root 持有的 `/etc/ssh/asterion_authorized_keys`；只对该账户启用公钥认证，禁用密码、交互认证、转发、PTY 和用户 rc，保留命令与 SFTP 以便上传及安装。
3. 安装 root 持有的 `/usr/local/sbin/asterion-host`，以及仅允许其 `--manage` 入口和 UFW 状态查询的 sudoers 规则。助手严格解析 `check` / `install NODE PORT BIND`，不提供任意 root shell、任意服务名称或任意文件写入。
4. 检查 sshd / sudoers 配置并重新加载 SSH 服务，输出主机 ED25519 公钥指纹，供客户端经可信渠道核对。

现有同名账户必须满足专用账户契约；已有配置和脚本必须逐字一致，否则拒绝覆盖。符号链接、不安全的管理员目录、不同内容的同名服务均拒绝。重复执行同一版本、同一公钥可以重试；不提供自动升级或密钥轮换。失败可能留下已经创建的账户和匹配配置，会显示错误；不静默删除用户文件或自动回滚账户。更换脚本版本、公钥和已有服务配置需要管理员显式处理。

## Terminal 安装与运行权限

Linux 登录用户名为 `asterion`（表单默认值），管理端口至少为 1024。上传目录、Agent 程序和身份文件由该账户处理；上传的安装脚本以该账户执行。唯一的提权步骤是调用固定助手生成、注册并启动 `asterion-node-agent-<节点>.service`，服务的 `User` / `Group` 固定为 `asterion`。

系统服务启用 `NoNewPrivileges`、清空 capability 集合、只读系统目录和私有临时目录。Agent 以及它启动的业务进程不以 root 运行。它们仍共用同一账户及数据目录，这不是不可信策略沙箱，也没有跨节点文件隔离。SSH 持有者可以运行普通账户命令，属于受信任的机器部署管理者。

此版本只授权防火墙查询，不授权修改。Terminal 对专用账户显示“仅有查询权限”，不提供可执行的放行确认。管理员根据预览中的来源 IP、TCP 端口设置规则。Agent 的 `NoNewPrivileges` 不允许它通过 sudo 提权。后续若增加受限防火墙管理，需另行建立来源、端口、规则所有权白名单；不能直接给 `/usr/sbin/ufw *` 或 `NOPASSWD: ALL`。

Terminal 目前仍展示机器名称、用户名、known_hosts 等高级字段，尚未完成只填写 IP / SSH 端口 / 私钥的最终界面。主机身份校验不能省略。

## 平台与验收边界

此初始化脚本仅适用于 Linux / systemd，在其他系统拒绝执行。Terminal、Node Agent 和交易服务的 Linux / Windows / macOS 支持目标不变；不提供 Windows、macOS 的远程初始化和安装流程。

`tests/host_initialization.py` 检查权限边界和拒绝路径；`tests/ssh_bootstrap.py` 验证应用编排。`tests/ssh_system_service.py --allow-system-service` 在一次性 Linux 环境执行真实初始化、SSH/SFTP 安装、非 root systemd 服务及清理，已纳入 CI，但未在当前真实 Linux 机器上提权执行。不要把替身测试通过表述为服务器初始化完成。

依据：[OpenSSH 认证与 Match 配置](https://man.openbsd.org/sshd_config)、[authorized_keys 限制](https://man.openbsd.org/sshd)、[systemd 执行身份和权限约束](https://www.freedesktop.org/software/systemd/man/latest/systemd.exec.html)。


## 部署范围（2026-09-27 最新决定）

本机部署支持 Linux、Windows、macOS，使用当前用户的系统托管机制和本机 IPC，不需要 SSH、私钥或目标机器初始化。远程部署目标仅支持 Linux，使用专用 asterion 账户、独立初始化脚本及 SSH 引导，后续通过 mTLS 管理。不再提供远程 macOS/Windows 安装流程；不影响三平台 Terminal、Agent 与业务服务的本机运行。设置中的“本机部署”和“远程 Linux”分开显示，默认本机。


## Terminal 本机生成登录密钥

设置 → 连接 → 远程 Linux → 通过 SSH 添加机器，先填写机器名称，默认选择“本机生成（推荐）”，点击“生成或查看本机公钥”。C++ 调用系统 OpenSSH `ssh-keygen` 生成 Ed25519 密钥对，页面仅获得公钥，可复制后交给管理员初始化机器。公钥授权的是远端 asterion 账户，主机身份仍需独立核验。

生成的私钥没有口令，保存在独立目录：Linux/macOS 为 `~/.asterion/nodes/.ssh-keys/<机器名称>/identity`，Windows 为 `%LOCALAPPDATA%/.asterion/nodes/.ssh-keys/<机器名称>/identity`。Unix 目录 0700、文件 0600；Windows 新目录配置仅当前用户的继承 DACL。此版本是文件权限保护，没有系统凭据库或额外的静态加密。系统管理员仍可通过其权限访问文件。

机器配置只保存连接字段。`node.key.prepare` 和运行状态只返回机器名称与公钥，不返回私钥或私钥路径。重启后使用同一机器名称再次查看公钥，会校验和复用原密钥，不自动重新生成；文件不匹配、缺失或不安全路径明确失败。尚无密钥删除、轮换、导出或跨电脑同步入口，删除节点监控也不删除密钥。

SSH 操作明确携带 `key_source=managed` 或 `provided`。前者由原生端加载对应机器的本地密钥，`private_key` 必须为空；后者只临时使用本机粘贴的未加密私钥，不持久保存。OpenSSH 操作完成仍会清理临时身份文件。安装完成后的日常节点管理使用 mTLS，不反复要求 SSH 登录。

依据：[OpenSSH ssh-keygen](https://man.openbsd.org/ssh-keygen)。本机部署不展示此流程，也不生成 SSH 密钥。
