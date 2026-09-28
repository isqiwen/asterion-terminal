# 上线前验收清单

CI 已覆盖三平台构建、单元/集成测试、sanitizer 与界面测试。以下各项依赖真实账号、物理机器或干净系统，CI 无法代替，需要维护者在本机执行。每项给出命令、预期结果和记录方式；凭据只在本机输入，不进入聊天、仓库或日志。

## 1. 实时行情（SimNow 或券商前置）

前置地址与经纪商代码以 SimNow 官网或券商提供为准。

```bash
python3 scripts/acceptance/simnow_market.py --build build/Debug \
  --front tcp://<行情前置> --broker <经纪商代码> --user <账号> \
  --instrument SHFE:rb2610 --instrument DCE:m2609 \
  --report acceptance-simnow.json
```

- 密码以不回显方式输入，只经本机私有管道传给行情服务，不写入报告。
- 预期：每个合约输出 `first quote`，最后 `PASSED`，退出码 0。
- 在交易时段内执行；非交易时段可能登录成功但没有报价。
- 记录：保留 `acceptance-simnow.json`（不含凭据），附上执行时间与前置类型（7×24 / 交易时段）。
- `acceptance_dry_run` 测试用测试替身 SDK 保证脚本本身随代码持续可用。

## 2. 远程 Linux 节点（物理机或云主机，x86_64）

1. Terminal「设置 → 连接与部署 → 远程 Linux」生成本机公钥，导出初始化脚本。
2. 在目标机以管理员执行：`sudo python3 initialize-linux.py --public-key <公钥文件>`。预期输出 `Initialized asterion ...`，且 `/etc/sudoers.d` 权限正确（脚本会用 `visudo -c` 校验）。
3. 回到 Terminal，填写地址、SSH 端口、已核验的 known_hosts，点击安装。预期：节点在线，显示平台、版本与心跳。
4. 部署一个模拟交易服务，创建会话、下单、成交；在 Terminal 关闭后确认服务仍在运行（`systemctl status` 对应单元），重新打开 Terminal 后能重新附着并看到同一账本。
5. 在目标机 `sudo reboot`，确认 Agent 与服务随系统恢复。
6. 证书角色：把本机节点目录中的 `trading-client.crt/.key` 配置到另一台 Terminal，确认可以连接交易服务，但部署或更新操作返回“需要节点管理员证书”。
7. 防火墙：执行防火墙预览与确认，核对 UFW 规则仅包含预览中的来源 IP 与端口，撤销后规则消失。

记录：每步截图或命令输出，注明发行版与内核版本。

## 3. 安装包（每个平台一台干净机器或虚拟机）

| 平台 | 产物 | 检查 |
| --- | --- | --- |
| macOS | `.dmg` | 拖入“应用程序”后首次打开无“已损坏”提示；启动页完成本机初始化；登录项中出现 Agent 用户任务 |
| Windows | NSIS `.exe` | 安装与卸载无管理员以外的提权；任务计划程序中出现当前用户的 Agent 任务；非 ASCII 用户名（如 `C:\Users\张三`）下正常运行 |
| Linux | `.deb` | `sudo apt install ./asterion*.deb` 无缺失依赖；`systemctl --user status` 可见 Agent |

每个平台还需验证：
- 安装新版本覆盖旧版本后，已有模拟账本、节点注册与界面偏好保留；启动流程自动完成 Agent 升级（见 [Agent 升级](agent-upgrades.md)）。
- 引擎 v2 之前的模拟账本会被明确拒绝并提示原因（预期行为，不迁移）。

## 4. 交易所规则

平仓规则已按公开资料核对（见 [期货模拟交易](paper-trading.md)）。每季度或交易所发布规则变更后复核一次；规则变化需要修改 `ClosePolicy` 并提升引擎标识。

## 记录与结论

验收结果汇总到 `docs/terminal-validation.md` 对应章节：日期、执行人、环境、结论、遗留问题。任何一项失败都不应宣称该能力“已可用”。
