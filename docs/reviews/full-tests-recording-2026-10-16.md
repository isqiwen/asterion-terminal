# 2026-10-16 本机全量测试与界面录像

当前状态：下述三类问题及格式工具阻塞已修复，定向复验通过，见文末“后续修复与定向复验”。
前文为修复前的全量测试记录。

本轮已执行本机全部常规自动化测试，结果未全通过。C++ 678 项和前端 106 项全部通过；
99 个不同 E2E 场景中 96 项通过、3 项失败；两项独立 Electron 宿主测试均在启动阶段失败。
没有修改产品实现或失败断言，本轮新增内容仅为可选录像配置及验证文档。

## 基线与输入

- 主 checkout：`/home/qiwen/Workspace/asterion-terminal`，分支 `main`，
  HEAD `0a040bd77e099576ea63aec83ce972132f4b6389`；开始时工作树清洁。
- Linux x86_64，内核 `6.12.94+deb13-amd64`，glibc 2.41；Node 24.14.1、pnpm 10.32.1。
- 重建当前 `build/Debug`；前端来自本仓库 `apps/clients/terminal`。
  浏览器开发桥为当前 Debug 的 `asterion_terminal_dev_bridge`。
- 通过既有 `stage_native_resources` 同步 15 个原生资源到
  `build/electron-resources/native`，逐文件 SHA-256 与 Debug 构建一致。
  Electron 加载这里的 `asterion_terminal.node`，前端使用本轮新构建的 `dist`。
- 基线、原生摘要和结果汇总分别保存在证据目录的 `baseline.json`、
  `native-sha256.json`、`summary.json`。历史审计中的旧验证结果不计入本轮。

## 执行结果

| 项目 | 本轮结果 | 证据文件 |
| --- | --- | --- |
| `cmake --build build/Debug -j 8` | 通过 | `build-cpp.log` |
| `ctest --test-dir build/Debug -j 8 --output-on-failure` | 678/678，零跳过，298.33 秒 | `ctest.log`、`ctest.xml` |
| `pnpm test:unit` | 38 文件、106/106 | `unit.log` |
| `pnpm build` | 类型检查与 Vite 构建通过 | `frontend-build.log` |
| `pnpm run lint` | 通过 | `lint.log` |
| 全量 E2E | 两段合计 96/99，无重复计数 | `e2e.log`、`e2e-resume.log`、`summary.json` |
| `pnpm test:desktop` | 首个 Electron 宿主脚本启动失败 | `desktop-host.log` |
| 补跑 `node tests/desktop/electron_depth.cjs` | 同样启动失败 | `desktop-depth.log` |
| Node-API bridge 验证 | 通过 | `native-bridge.log` |
| Prettier 全范围检查 | 通过 | `prettier-final.log` |
| `pnpm run format:check` | C++ 工具版本检查受阻：要求 23.1.1，本机及已缓存版本为 19.1.7 | `format.log` |

E2E 始终通过仓库的 `pnpm run test:e2e` 隔离入口，使用临时节点与测试 CTP SDK。
第一次运行完成 71 项（68 通过、3 失败）后，聊天中断终止了测试进程；当时研究综合场景
已开始但尚无结论。续跑从该场景所在文件开始，执行剩余 28 项并全部通过，用时 362.71 秒。
第一段没有生成最终 Playwright JSON，因此以原始日志、失败 trace 和录像保留证据；
`e2e-interrupted-summary.json` 是日志提取结果。第二段原始结果为 `e2e-resume.json`，
HTML 报告仅覆盖第二段；`summary.json` 合并 99 个不同场景，不能称为一次连续全绿测试。

`test:desktop` 的两个脚本由 `&&` 连接，首个失败后不会执行深度测试；因此本轮单独运行
第二个脚本，避免把未执行计作通过。其后续工作台断言因启动失败未得到验证。

## 失败及原因

1. **默认插件选择阻塞新桌面启动。**
   `electron_desktop.cjs`、`electron_depth.cjs` 和 E2E 的原生历史档案场景均停在数据服务步骤，
   界面显示“应用自带的插件必须保持启用”，随后等待工作台超时。
   [前端插件筛选](../../apps/clients/terminal/src/host/native-plugins.ts) 只接受 history.v3 和 risk，
   遗漏 history.tables.v1；[SHFE 插件](../../plugins/data/shfe/native_plugin.cpp) 仅提供表格及原文能力，
   而[原生选择边界](../../apps/clients/terminal/native/plugin_catalog.cpp) 要求启用所有随包数据插件。
   浏览器 E2E 的初始化夹具提前选择全部可用内置插件，不能用这些浏览器场景通过来否定桌面故障。
   录像截图 `desktop-failure.png`、`native-archive-startup.png` 和深度测试日志记录了相同提示。
2. **截面成员列的旧断言。**
   `factor.spec.ts:216` 将最后一列当作合约数量，预期 `3`，实际为新增的“查看成员”入口。
   场景已经执行到因子结果表格；失败不能被解释为因子计算失败，也不能据此宣称该场景全通过。
3. **市场数据来源数量的旧断言。**
   `market-tables.spec.ts:16` 预期覆盖表 36 行，实际 37 行；当前已增加 SHFE 历史保证金来源，
   该场景仍假定只有原有六类表。后续固定来源清单断言也需要按当前契约审查。

三项 E2E 的原始失败消息及堆栈提取到 `failure-details.json`，各自保留 `trace.zip`。
上述问题本轮均未修复；格式工具不匹配是独立的环境阻塞。

## 录像与复现

证据目录：[`build/reviews/full-tests-2026-10-16`](../../build/reviews/full-tests-2026-10-16/)。
最终视频：[`terminal-ui-full-test.mp4`](../../build/reviews/full-tests-2026-10-16/terminal-ui-full-test.mp4)。

127 段实际窗口录像按落盘顺序合成为约 15 分 07 秒的视频，1440 × 900、25 fps、H.264，
包含成功与失败场景，保持原始速度，无音轨。多窗口录像依次展示；不是对整个桌面或终端的
连续屏幕录制，服务重置及无窗口阶段不在视频内。中断时未完成的研究场景和临时录像保留在
原目录，最终视频使用该场景续跑后的完整录像。章节和片段对应关系见 `video-manifest.json`。
原始 WebM 分别在 `e2e-results`、`e2e-results-resume`、`electron-videos`；未被合成文件替代。
最终 MP4 为 33,712,798 字节，时长 906.6 秒；127 个章节、全片解码及成功/失败画面抽查
通过。SHA-256 与核验信息保存在 `video-verification.json`，解码日志无错误。

可选录像入口已记录在[开发指南](../development.md)：`ASTERION_E2E_VIDEO=1` 保留浏览器
成功及失败录像；`ASTERION_UI_VIDEO_DIR` 指定 Electron 录像绝对目录。默认不录制。
本轮复现命令、运行环境和每阶段退出码均保存在证据目录；合成脚本为 `assemble_video.py`。

## 未验证范围

本轮是 Linux 本机 Debug 与隔离自动化验证；没有运行 macOS、sanitizer、发行安装包验收、
真实远程主机部署、真实供应商或 CTP 柜台验收。未重建 Linux 发行服务包，原有发行边界保持。
未操作真实交易账户、改写用户数据、提交或推送。视频及报告包含失败证据，不构成发布通过结论。

## 后续修复与定向复验

用户随后授权修复上述问题。本节是修复后的新验证，前文保留原始失败结果。

- 共用 `availableDataTaskPlugins` 补齐 `asterion.history.tables.v1`，使启动及设置页都能
  选择仅提供表格能力的 SHFE 插件。原生层必需插件约束保留；没有新增恢复、重试或绕过路径。
- 新增一个单元回归，验证随包表格插件默认启用且不可取消，用户安装的表格插件仍可显式
  选用或取消。该测试在修复前失败，修复后通过。
- 因子截面测试按“参与的合约数”表头定位单元格，避免把新增的成员操作列误当数量。
- 市场数据测试明确验证包含 SHFE 历史保证金的 37 行及完整七类表身份；来源身份集合按排序
  比较，不依赖插件枚举顺序。
- 使用 `uv tool install --python /usr/bin/python3 clang-format==23.1.1` 安装 CI 指定版本，
  `~/.local/bin/clang-format` 已解析为 23.1.1；仓库版本检查规则没有放宽。

修复后的验证全部通过：

| 检查 | 结果 |
| --- | --- |
| Vitest 全量 | 39 文件、107/107 |
| TypeScript / Vite 构建 | 通过，Electron 使用本轮新建的 `dist` |
| ESLint | 通过 |
| `pnpm run format:check` | Prettier 与 400 个 C/C++/Protobuf 文件全部通过 |
| 原失败 E2E | 因子截面、原生历史档案、市场数据来源 3/3，73.42 秒 |
| `pnpm test:desktop` | 两个宿主脚本完整通过，60.19 秒 |
| C++ 构建状态 | `ninja: no work to do`，没有 C++ 或服务源码修改 |

原生档案已经完成发布、读取和重启；Electron 宿主进入工作台后继续验证窗口、语言、重启恢复
及行情深度。复验仍使用隔离节点和测试 SDK。没有再次运行全量 CTest 或全部 99 项 E2E，
不能将前后两轮拼成“修复后全量一次通过”。macOS、真实柜台、发行安装包仍未验证。

新日志与原始录像位于 [`build/reviews/test-fixes-2026-10-16`](../../build/reviews/test-fixes-2026-10-16/)，
复验视频为 [`terminal-ui-fixes.mp4`](../../build/reviews/test-fixes-2026-10-16/terminal-ui-fixes.mp4)。
本次保留此前全部测试与录像文件，未提交或推送。
