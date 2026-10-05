# 发布候选版推进记录

当前版本说明：2026-10-05 后续修复了旧开发节点导致的启动阻断和诊断问题，详见
[整改进度](audit-progress.md#开发节点启动阻断修复2026-10-05)。下列包摘要和完整回归
对应修复前的固定源码；新修复已验证开发入口，尚未重打发布包。

开始日期：2026-10-05（Asia/Shanghai）。用户已批准按五阶段计划持续推进。
目录为主 checkout `main`，起始 HEAD 为 `92280bda6cefc7e755e136d89771914ac2f5e429`；
该提交不包含全部架构改造，必须同时记录工作树源码清单，不能以 HEAD 冒充验证基线。
前序架构实施与分阶段证据见 [实施记录](architecture-implementation.md)。

## 范围与完成条件

| 阶段 | 当前状态 | 完成条件 |
| --- | --- | --- |
| 1 代码与构建基线 | 独立源码全新构建通过 | 当前源码完整、职责明确，独立源码副本可重建，无遗漏新文件或依赖旧构建产物 |
| 2 固定版本统一验收 | 本机 macOS arm64 与 Linux 通过；Intel/托管 CI 未执行 | 同一源码的 macOS/Linux 回归、关键并发、隔离 E2E、桌面及安装包证据相互对应 |
| 3 真实业务闭环 | 隔离跨系统闭环通过；真实数据源与 CTP 仿真条件尚未提供 | 真实数据版本、回测/因子结果可追溯，专用 CTP 仿真账户与柜台对账通过 |
| 4 持续运行与负载 | 10 分钟隔离负载及最大输入通过；真实市场长时运行未执行 | 有界并发负载下控制仍可用，记录资源趋势；真实夜盘/日盘/交易日切换单独验收 |
| 5 正式交付 | Linux 实际虚拟机、macOS TEST 包通过；正式签名条件缺失 | 实际 Linux 节点部署与恢复、macOS Developer ID 签名/公证/首次安装验收通过 |

不操作日常账户或假定任何账户属于仿真；密码和令牌只在本机输入。
不推送，不新增客户端或交易接管机制，不把缺少外部条件改写成验收通过。

## 基线核对

- 已读取当前 AGENTS 和架构实施/整改记录。AGENTS 引用的 `technical-audit.md`、
  `connection-conflict.md`、`worktree-gap.md` 当前不存在；不重建或沿用其旧结论。
- 当前工作树包括 128 个未跟踪源码/文档文件，必须纳入源码副本；两个 tmux 运行日志
  已保留到 `build/release-candidate/baseline/diagnostics/`，不混入发布源码。
- 本机未配置 `CSC_NAME`、`APPLE_KEYCHAIN_PROFILE`，可用 Developer ID Application
  身份数量为 0。只读取身份存在性，没有导出证书、私钥或密码。
- 未发现日常 `~/.asterion/nodes` 或开发节点的 enrollment 目录；这不证明外部主机不存在。
  已请求用户只说明专用仿真/数据源/节点的可用情况及配置位置，不在聊天收集秘密材料。

## 构建责任修正

Terminal 的 CMake 依赖手写四个 worker/Task 目标，遗漏独立 Data 可执行文件。
全量构建会顺带生成 Data，掩盖仅构建 Terminal 目标时缺少运行组件的问题。
改为引用已有 `ASTERION_SERVICE_TARGETS` 清单，删除第二份服务列表；现有 CLI 入口
契约测试一并纳入 Data，不增加新测试框架。

Linux CI 先手写构建列表，再调用会自行构建的 `deployment_bundle.py`，有重复步骤且
前一列表遗漏 Data。改为由现有打包脚本唯一负责构建服务，随后检查版本，补上 Data
和 Market 的版本检查。没有改变服务业务语义、持久格式或引擎身份。

修正后的服务源码指纹为
`cc6065d16fd56a3d03315137e2e1351fe9efd8817a8830aa19b370ef375ab08e`。
前序 b9be938b…1158 安装包/服务包仍保留原证据，不作为此次基线的交付产物。

## 固定源码验证

全部 613 个现存受管及非忽略新文件纳入 `build/release-candidate/baseline/source.tar`；
逐文件摘要和模式见同目录 `source.json`，归档 SHA-256 为
`d097523ad9a183573852ff7d05ea49abf0ac851c80621b6a90724b6cfc067be9`。
该副本包含未提交架构代码；后续更新本进度文档不改变服务源码指纹。

macOS 从空构建目录使用锁定依赖缓存离线安装（`--build=never --no-remote`），先仅构建
Terminal 目标，确认八个服务/worker 及 keychain helper 齐全，再构建全部测试目标。
构建目录为 `build/release-candidate/conan/build/Release`。环境沿用桌面入口的 Apple
Clang 和清理 shell 编译参数约定，原生模块没有 Homebrew 动态链接或 RPATH。
独立构建初次手动命令继承了 shell 链接参数，已清空重建；一次辅助探针误将内部
keychain helper 当成公共 `--version` 命令，已修正探针而未扩展其接口。原日志保留。

| 验证 | 当前结果 | 证据（`build/release-candidate/` 下） |
| --- | --- | --- |
| 独立 macOS 完整构建 | 通过 | `clean-conan.log`、`clean-rebuild.log`、`clean-all-build.log`、`baseline/single-target.json` |
| macOS 完整 CTest | 504 项：503 通过，1 平台跳过；405.73 秒 | `macos-tests.log`、`macos-tests.xml` |
| Linux x86_64 完整 CTest | 500 项：498 通过，2 平台跳过；637.93 秒 | `../architecture-linux-regression/output/rc-current-tests.log`、同名 XML |
| Node-API 实际加载/调用 | 通过，含独立队列与退出生命周期 | `node-bridge.log` |
| C++/UI 格式、TypeScript、ESLint、前端构建 | 通过 | `format-cpp.log`、`format-ui.log`、`types.log`、`lint.log`、`frontend-build.log` |

macOS 跳过 `node_deployment`，该场景在 Linux 实际执行通过；Linux 跳过两个仅适用
macOS 的插件目录场景，它们在 macOS 实际执行通过。没有把跳过计为执行成功。
Linux 使用同一源码归档，在已有锁定依赖/构建缓存上编译；不是第二次全新依赖安装，
也不是 GitHub 托管 CI。工具链为 Ubuntu 24.04 x86_64 / GCC 13.3 / glibc 2.39。

新 Linux 服务包已按当前指纹构建、暂存并逐文件验证；大小 103,305,789 字节，SHA-256
`367eee9c425004057b9b8a2e3d668a47cb6bbd6558de6da124845b1ba234d1ec`，
证据为 `linux-bundle.json`。旧包保留在 `baseline/previous-linux-bundle.zip`，只作历史
构建证据，不被当前桌面资源引用。

## 恢复、桌面与持续负载

- 8 个恢复关卡各执行三次全部通过（123.16 秒），日志 `recovery-gate.log`；逐轮证据为
  独立构建目录内 `recovery-repeat.log`，没有只凭最终 JUnit 代替三轮结果。
- 原生 Electron 两个验收入口通过：`desktop.log` 与 `desktop-screenshots/`，覆盖启动、
  Agent 独立存活、独立 Data/Task、版本数据、因子结果、行情深度和窗口状态。
- 当前源码重建 ThreadSanitizer 后，Agent 监督、RPC、ServiceHost 与持久交易请求去重
  共 21 项通过（70.25 秒，无 TSan 报告）；`tsan-build.log`、`tsan-tests.log/xml`。
- 隔离节点停服务后停 Agent，冷备份并按原绝对路径恢复 48 个文件；字节、权限、属主、
  服务身份、固定结果和历史版本一致。`cold-backup.log/json` 不代表异机或物理掉电恢复。
- 同一组进程持续 600.2 秒，完成 60 个各含 20,000 条夹具数据的因子任务；6,180 次
  状态查询 P50/P95/最大为 36.47/44.67/143.71 毫秒；服务 PID 不变，最早结果的摘要
  再次读取一致。293 次 RSS 采样见 `sustained-load.json` 和 `sustained-resources.json`。
  Data/Task/Native/Agent 峰值分别为 67.33/54.67/78.78/25.44 MiB。Data 在 30–60 秒与
  末 30 秒窗口的 RSS 中位数为 49.45/64.66 MiB；这次短时观测不能证明长时无泄漏。
  同期存在其他隔离验收负载，数值不是独占硬件性能上限。
- 持续负载探针第一次以多个 lookback 请求 full_sample，被现行“参数比较需要留出集”
  契约正确拒绝。改用单个 lookback 后执行上述运行；`initial-sustained-load.*` 保留原
  结果，没有为探针修改生产规则，也没有新增永久测试框架。

## 完整 E2E 暴露的问题

首轮固定源码 91 项：87 通过、4 失败（15.3 分钟），`e2e.log`、`e2e-results/` 保留。
这些失败未折算成通过，修正后先定向复验，再重新执行完整套件。

1. 图表夹具仍把历史来源绑定到 Task 的 service 字段，违反启动时选中服务与 Agent
   健康状态的一致性；改为 Data 连接与 `data.datasets` 当前契约。顺着数据流发现
   ContractHistory 只在合约/连接变化时刷新目录，发布新版本后不更新；现沿用已有
   下载任务的 `history_dataset_id` 完成通知重读目录。数据仍由 Data 返回，任务列表
   只提供刷新信号，不作为数据仓库。不增加线程、定时器、重试或兼容数据分支。
2. 导航栏固定宽度但标签禁止换行，中英长标签均可能越界。比较扩大整栏与在现有窄栏
   内换行，采用后者；显式安排两行文字高度，删除重复宽度覆盖，保留完整名称与提示。
3. 行情布局场景业务断言已全部完成，但异步路由拦截器在上下文销毁后继续读响应；
   该场景清理先等待已接纳的路由完成，不吞异常，也不调整生产请求生命周期。
4. 失败任务夹具只改列表，没有设置新架构的 `failed_count` 权威汇总；同步当前计数、
   来源与合约标识，状态栏不改回从分页列表推算总数。

上一版 TEST DMG 和解包应用已整体保留在
`baseline/previous-desktop-test/`，前序报告记录的是其原生成位置。

## 当前原生服务的操作系统与跨系统验收

进一步核对发现本机已有 `asterion-release-x64` Ubuntu 24.04 x86_64 虚拟机。此前“未发现
节点 enrollment”的结论只描述 Terminal 配置，不意味着没有可用 Linux 环境。
以下全部使用唯一命名的测试目录、服务或账户，未接管已有安装：

- `macos-native-upgrade.log`：macOS launchd 下的首次安装、无变化升级、并发升级、
  未完成发布拒绝、检查点恢复及数据保留通过。
- `linux-native-upgrade.log/json`：Linux systemd 用户服务执行同一组原生升级恢复通过；
  上传的当前二进制和临时目录已清理。
- `linux-ssh-system.log/json`：既有 `ssh_system_service.py` 在启动了 systemd 的虚拟机
  上实际通过 SSH/SFTP 初始化、系统服务安装、mTLS、子服务部署、Terminal 退出、
  Agent 重启和停止意图持久化。测试前确认专用账户及安装策略路径不存在；测试后
  再次确认这些账户、路径和唯一 systemd 单元均已清理。
- `cross-os.log/json`：macOS Native 使用实际发布包，通过 TCP/mTLS 上传并部署独立
  Linux Data/Task 和 workers；夹具历史数据通过 Data 读取，配置风险的回测与因子
  由 Linux worker 完成。Terminal 重启保留远端服务 PID 和精确结果；Agent 重启后
  重新连接，原结果完全相同，没有重新提交任务。

跨系统探针最初只复制部分可执行文件，运行历史夹具时缺少原生插件目录；两次失败
分别保留在 `initial-cross-os.*` 与 `partial-package-cross-os.*`。最终改为传送完整
已校验服务包，从其目录运行现有夹具，并使用夹具声明的 Task 身份。没有为一次性
探针扩展生产接口或放宽目录/身份检查。正式服务包文件摘要与源指纹保持不变。

上述结果覆盖实际操作系统服务管理和两端网络协议；仍不是第二台物理主机、操作系统
重启、真实数据源下载或仿真柜台交易的证明。

修正后的客户端源码归档位于 `baseline-final/source.tar`（仍为 613 个文件），SHA-256
`2cc85737ca9f28373be327f20f73230b1e488c8e2a031bb6a979daf032e284c0`。
服务源码与独立 C++ 构建未变化，差异限于客户端 UI、E2E 夹具及进度文档。
定向 8 项首次复验为 7 通过、1 失败；剩余图表场景因跨连接重新建目录使原生 details
折叠，测试增加显式打开操作，并删除绑定旧主题色值的实现细节断言。该场景随后完整
通过（25.0 秒），原始结果分别保留在 `ui-fix-*` 和 `chart-current-*`。

最终前端生产构建通过后，原生 Electron 桌面再次通过（`desktop-final.log`），截图
`desktop-final-screenshots/` 可见中英文完整导航文字位于栏内。完整 E2E 的两个 Electron
归档场景先前使用修正前 dist；因此在最新生产 dist 上额外复验这两个原生场景，结果
单独记录于 `native-final-e2e.log`，不将 Vite 源码测试与旧 dist 的证据混用。

## 本轮最终结果与交付物

- 最终完整 E2E：**91/91 通过，15.9 分钟**，`e2e-final.log`；最新生产 dist 的原生
  归档补验 **2/2 通过，1.3 分钟**，`native-final-e2e.log`。
- 最大 200,000 条因子输入、结果读取与独立控制查询通过，7.78 秒；采样 RSS 峰值
  Data 149.7 MiB、Task 306.8 MiB、factor worker 92.7 MiB。测试夹具进程本身为
  470.2 MiB，单独列出，不算作服务开销。`maximum-factor-resources.json/log/xml`。
- 打包入口的 Node-API 烟测改为调用已有 `tests/isolated_node.py`，默认使用临时节点
  并清理其 Agent；不再依赖调用者额外设置隔离环境。此次实际打包已执行通过，没有
  新增包装框架或永久用例。最终打包源码归档为 `baseline-package/source.tar`，摘要
  `cc1d917b690e38141e16250294acc20c434fb7aaa2e4b6dd8971dfcc1f52867f`。
- `python3 scripts/desktop.py package-test` 通过，生成
  `build/desktop-test/Asterion-Terminal-TEST-0.1.0-mac-arm64.dmg`，331,109,818 字节，
  SHA-256 `89b8a1ab55a016e3977975a3fdbc808072cb58c35b8c257f8863652a9dc6166e`。
  DMG 校验、完整资源、ad-hoc 签名、包内任务恢复和实际厂商 SDK 本地生命周期均通过，
  `package-test.log`、`installer.json` 保存证据。
- 复制 DMG 应用到独立临时目录并卸载镜像后，安装副本的启动、六工作区、125% 缩放、
  数据/因子/回测、账户记录恢复、风控和 Agent 独立存活全部通过；证据为
  `installed-desktop.log` 与 `installed-desktop/acceptance.json`。验收报告 DMG 摘要
  与实际产物一致。使用隔离档案和测试数据，文件选择对话框仍采用 IPC 适配器，
  不是手工原生文件选择器验收。

本轮没有推送或创建分支；所有改动仍在主 checkout 的 `main` 未提交工作树。归档覆盖
现存受管及非忽略新文件，HEAD 单独不能代表本次验证版本。后续进度文档更新不改变
已归档的程序源码或产物。索引与各项证据摘要见 `build/release-candidate/acceptance.json`。

剩余边界：专用真实数据源授权、CTP 仿真账户及对应实际交易时间未提供；真实日夜盘/
交易日切换与柜台对账尚未执行。macOS Developer ID 身份与公证配置缺失，当前 DMG
明确为 TEST 包，不是正式发行通过。macOS Intel、托管 CI、另一台物理主机、操作系统
重启或掉电恢复也不由本轮本机与虚拟机结果代替。既有券商仿真步骤见
[券商仿真验收](../acceptance/ctp-simulation.md)，补充环境时只说明配置位置，秘密材料
仍在本机输入。

## 原生文件选择器补验（2026-10-05）

同一 TEST DMG 再次复制到独立临时目录，卸载镜像后以隔离节点和用户档案启动。
通过 macOS 原生界面实际打开插件文件选择器，取消后正常返回且没有插件预览；
再次选择应用自带的 `asterion-order-limits.dylib`，返回界面正确显示
`asterion.risk.order-limits`、`1.0.0`、`asterion.risk.pre-trade.v1` 及文件摘要。
预览摘要与所选文件的 SHA-256 相符。没有执行安装操作。

此项补齐原自动化安装验收使用对话框适配器留下的插件选文件边界，不覆盖保存或
文件夹对话框。证据为 `build/release-candidate/native-dialog.json`，已纳入汇总摘要；
隔离应用退出、临时目录清理均已确认。程序源码和安装包未改变，外部验收限制仍然有效。
