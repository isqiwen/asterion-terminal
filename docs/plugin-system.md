# 策略插件安装与执行契约

本文描述用户可安装的 Python 策略包，这是星枢唯一可安装的扩展（见[扩展点](architecture.md#扩展点)）。内核、数据源适配、券商接入与界面面板都不可安装或替换。

## 支持范围与信任

支持固定 ZIP 工件中的本地 Python 策略包，每包只有一个 `research.strategy` 扩展点，不携带 UI、数据源、券商接入或内核代码。策略契约见 [策略插件](strategy-plugins.md)。

数据源只允许内置 Rust 实现（2026-09-24 决定并已实施）。声明 `data.provider` 或其他扩展点的包在检查与安装时即被拒绝；安装记录中若存在此类包，插件管理明确报告“外部数据源插件已不再支持”，不执行、不迁移。策略包不能依赖其他已安装的包，清单没有依赖字段。

当前实现差距：最新架构要求策略不能访问任意文件；本轮目录整理尚未实现该隔离要求，不能据此宣布策略隔离验收通过。现有本地插件是用户明确认可的受信代码，具有当前 OS 用户的文件和网络权限。独立进程、作用域权限和 hash 校验不构成恶意代码沙箱。不支持任意原生依赖、任意 React/脚本界面、在线市场或实盘交易扩展；Windows 打包与进程约束未验收。

## 包格式

ZIP 包含 `manifest.json`、`plugin.py` 和可选纯 Python 源码/文本资源。不执行安装脚本或在线依赖安装；压缩包最多 16 MB、展开最多 32 MB、500 个文件、单文件最多 8 MB，拒绝路径穿越、绝对路径、重复项、目录项、符号链接和原生二进制。

| 字段 | 当前契约 |
|---|---|
| `api_version` | 必须为 1 |
| `id` | 点分命名空间；asterion.* 保留给内置发行版 |
| `version` | 精确“数字.数字.数字”，无范围匹配或兼容承诺 |
| `title / description` | 用户可读名称与说明 |
| `layer / runtime / entry` | L3 / python / plugin.py |
| `trust` | local-code |
| `contributions` | 恰好一个已批准扩展点 → 对应领域声明 |

未知字段或扩展点拒绝。宿主计算包 SHA-256；同 ID/版本即使卸载后也不能指向不同内容。

工件位于数据根 `.extensions/objects/<sha256>/`，安装回执为 `installed.json`。Rust archive/artifacts 负责 ZIP 解析、预算、CRC、文件头一致性及不重叠检查，并以目录描述符约束文件访问、落盘同步和不可覆盖的目录发布；已有对象必须完整校验，不自动修补。支持 Stored/Deflate/BZIP2/LZMA，LZMA 字典最多 32 MiB。原 ZIP 和源码不可变，文件锁与原子替换保护回执；每次执行前重验 hash、文件清单和内容。中断留下的未登记工件不执行。

## 安装、停用与移除

在“设置 → 插件”选择或拖入单个非空 ZIP，先预检名称、贡献、依赖、摘要和本机访问权限。预检不执行插件，取消不登记；点击“信任并安装启用”后，宿主在同一锁内重验并提交启用回执。

- `POST /api/v1/extensions/inspect` 接收 archive。
- `POST /api/v1/extensions/install` 接收 archive、预检 digest，且 trust_local_code 必须为 true；相同工件重复提交幂等。
- 停用、重新信任启用和移除使用当前回执摘要，拒绝过期窗口操作；移除保留工件、任务输入和已发布数据。
- 当前更新需停用、移除安装记录后选择新包；无在线下载或隐式信任依赖。
- 停用或移除会撤销执行授权；恢复任务须显式启用原工件或提交新任务，不能换实现执行历史输入。

安装管理受账户解锁和服务端请求范围保护。拖放不绕过确认，扩展名仅作输入提示，服务端负责格式与完整性检查。真实原生文件拖入仍需单独验收，浏览器测试不代替该验证。

## SDK 与进程协议

当前 SDK 为 `src/asterion_plugin_sdk`，上下文和进程通信使用 `asterion_bindings` 接入固定 Rust 内核；公开的 `asterion_plugin_sdk.packages.PackageManifest` 使用 Pydantic 定义唯一包清单，SDK 打包与安装器共用清单及 ZIP 格式校验。开发环境需安装终端 wheel 及其依赖，以获得原生绑定和 Pydantic；不能只复制 SDK 目录。独立插件使用公开 SDK，不导入 asterion 内部实现。可复制 [策略样例](../examples/strategies/close-momentum/README.md)，以确定性 ZIP 打包：

```sh
uv run python -m asterion_plugin_sdk pack examples/strategies/close-momentum /tmp/close-momentum.zip
```

打包命令在创建输出前校验完整工件；层级、贡献数量、入口、路径或容量不符合当前包契约时退出报错，不生成 ZIP。策略贡献的业务语义和本机依赖状态仍由安装器核验。输出文件必须不存在。

通信使用 [统一 Call/Reply 契约](internal-communication.md)：上下文包含版本、请求/关联/因果 ID 和截止时间，响应必须回传相同上下文。SDK 只对接业务 dispatch，Rust 负责有界帧、固定脱敏错误和调用生命周期。每请求帧最多 8,000,000 字节，单次调用 stdout/stderr 合计最多 8,000,000 字节；长会话输出按整个会话累计。默认 30 秒会话寿命及每请求父截止时间均受检查；子进程 CPU 30 秒、单文件 8MB，并在错误/撤销/关闭时回收所属进程组。策略会话可连续处理逐根请求，单会话不允许并发调用。

环境不传递主令牌、数据库 URL、账户会话或 PYTHONPATH。策略只获得方法所需 JSON，不获得凭据或连接配置；这不等于同用户进程无法访问其他文件。额外原生依赖不向宿主隐式安装。

## 运行诊断与验证

“设置 → 插件 → 运行诊断”显示当前工件最近 30 条完成调用：时间、单调耗时、阶段、调用数和固定失败类别。策略会话聚合为一条；调用完成不代表业务发布成功。

诊断位于 `.extensions/diagnostics.sqlite`，API/Worker 调用同一 Rust 日志机制，经 SQLite 事务协调，总计保留最近 200 条。只保存摘要和宿主指标，不保存参数、返回正文、环境、stderr 或异常原文；写入故障不改变原操作结果，读取故障明确报错。它不是完整审计或正在运行进程清单。

内核实现见 [进程传输](../kernel/mechanisms/src/transport.rs)、[日志](../kernel/mechanisms/src/diagnostics.rs)；宿主适配见 [包管理](../src/asterion/platform/extensions/packages.py)、[进程调用](../src/asterion/platform/extensions/process.py)、[扩展路由](../src/asterion/extensions/plugin.py)。机制测试见 [test_extensions.py](../tests/test_extensions.py)、[test_authorization.py](../tests/test_authorization.py)，冻结环境证据见 [核心与插件验收](validation/core-plugins.md)；当前能力及未验收范围见 [进度](ROADMAP.md)。
