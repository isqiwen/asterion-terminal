# Tushare 每日结算参数验证

日期：2026-09-20。范围为 `fut_settle` 同步、版本证据、规则期间确认和冻结复现。

## 当前实现

- Tushare 1.2.0 新增 `settlement` 能力和 `futures.settlement` 类型。按实际合约和日期分段采集，保留所有声明字段、原始响应和采集时间。标准版本是日期范围快照，无日线图表和累计合并。
- 规则插件通过数据公开版本能力读取固定行；明确确认手续费字段及单位、买投机保证金单位、普通开平仓适用性和生效期间。未确认、缺值、错误换算、证据篡改不能成为可保存的映射规则。
- 期间开始严格晚于来源交易日。历史公布时刻未知；后续期间可用性是显式研究假设，不能当成历史时点认证。
- 每个期间显式提供可空 `settlement_basis`。规则保存再次验证来源；引用统计、研究输入、草稿、导出和离线复现保留冻结依据。没有兼容字段默认值或迁移。
- 实现位于数据、规则及研究功能插件；沿用现有任务、版本、账户、作用域和存储机制。核心没有新增供应商识别分支。

## 验证

- 415 项后端测试通过，使用隔离 PostgreSQL，数据库测试无跳过：`.state/settlement-postgres.log`。
- 23 项前端单元测试通过；5 项研究浏览器场景通过：`.state/settlement-browser.log`。浏览器场景包含读取结算原值、未确认禁用采用、显式单位换算、保存规则及已有研究流程。
- 新增 11 项专项测试覆盖实际发布代码、原始数值及空值、非法/重复/错合约/越界数据、转换一致性、同日/提前生效拒绝、来源篡改、认证、版本引用，以及不访问数据源的冻结复现。
- Ruff、Pyright、TypeScript、OpenAPI 与前端生成类型及差异空白检查通过。
- `scripts/build_desktop.py --no-install --smoke-test` 通过：冻结后台、研究/复现/备份恢复烟测及 macOS 应用构建完成，日志 `.state/settlement-build.log`；`codesign --verify --deep --strict release/Asterion_Terminal.app` 通过。构建仍有 Vite 主包超过 500 kB 的体积提示，未影响构建。

## 真实数据边界

只读核对当前桌面环境：本机 API 和 PostgreSQL 端口可达，`data_provider_configurations` 保存记录为 0，Tushare 命名连接为 0。本轮没有可用的已保存连接，因此没有请求 Tushare 真实接口，也没有验证积分、权限、网络额度或实际合约参数。没有修改用户环境或重启其服务。

接口行为依据 [fut_settle 官方文档](https://tushare.pro/document/2?doc_id=141) 和 [盘后更新时间说明](https://tushare.pro/document/1?doc_id=108)。自动测试响应均为明确离线构造样本，不是官方费率认证。下一步在设置保存 Tushare 连接，用单个实际合约的小日期范围验证日线、合约资料、结算快照和规则研究流程。不要在聊天或文档中粘贴 Token。
