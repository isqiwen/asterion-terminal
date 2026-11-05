# 文档导航

| 问题 | 文档 |
| --- | --- |
| 产品是什么、给谁用、不做什么 | [产品说明](product.md) |
| 交易必须遵守哪些规则 | [交易规则](safety.md) |
| 先做什么、做到哪了 | [开发计划](plan.md) |
| 现在能做什么、有哪些限制 | [现状](status.md) |
| 系统怎样组成、状态归谁 | [架构](architecture.md) · [服务与部署](services.md) |
| 服务之间怎样通信 | [通信协议](protocol.md) |
| 怎样构建、运行与测试 | [开发指南](development.md) |
| 配置归属与默认文件 | [配置文件](configuration.md) |
| 联合仿真验收 | [CTP 仿真验收](acceptance/ctp-simulation.md) |

应用文档：

| 应用 | 阅读入口 |
| --- | --- |
| Market | [功能与使用](market/README.md) → [架构与通信图解](market/architecture.md) → [录制与部署](market/deployment.md) |
| Trading | [交易、风控与部署](trading/README.md) |
| Lab | [固定版本读取、策略、因子与回测](lab/README.md) |
| Terminal | [界面与交互](terminal/README.md) |

约定：

- 应用文档放在对应应用目录；跨应用的产品、计划、架构、协议和开发部署文档留在根目录，联合验收放在 `acceptance/`。
- 产品说明定义目标，交易规则定义下单必须遵守的规则，开发计划定义顺序和状态；主题文档描述当前代码。
- 主题文档跟代码走：替换或删除代码的同一提交里更新或删除对应文档，不保留旧版本或归档副本。
- 过程记录写在提交说明里，不写进文档。
