# Tushare 合约资料映射验证

日期：2026-09-20。范围为 `fut_basic` 合约资料到研究规则草稿的映射；不是完整交易规则或真实账户联网验收。

## 本轮变更

- Tushare 适配器 1.1.0 保留 `multiplier` 和 `quote_unit_desc`；当前标准合约资料结构为 schema 2。
- 规则插件声明依赖数据公开读取能力，通过固定版本的文件校验、来源/类型/结构校验及唯一合约匹配生成预览。
- 单位明确时建议商品每手乘数或股指乘数。未知单位、缺值、非正值、国债计价关系均不猜测；最小报价文字只展示。
- 界面支持选择本机资料版本或填写历史 ID，采用预览后补齐规则。未把上市区间映射为规则生效区间。
- 规则必填可空 `basis` 保存资料版本、文件指纹、连接 ID 和字段快照；手动规则明确为 null。保存时再核对原资料；任何伪造字段均拒绝。
- 来源引用进入规则、运行、草稿/模板和复现包引用统计。附带行情的复现不重新访问来源或规则目录。
- 核心机制未新增供应商分支、资源类型或调度逻辑；实现均在功能插件与发行装配已有授权范围内。

## 检查

- 404 项完整后端测试通过，使用临时 PostgreSQL，数据库测试未跳过：`.state/tushare-rules-postgres.log`。
- 23 项前端单测通过；5 个研究浏览器场景通过，包含 Tushare 映射预览、带入乘数、缺失字段留空、补齐保存、复制修改、草稿/模板及回测流程：`.state/tushare-rules-browser.log`。
- 专项覆盖真实数据发布代码到映射 API、文件损坏、错误来源/结构、缺失字段、重复/缺失合约、数据截断、来源证据篡改、单位和国债边界、引用统计，以及不读取原来源的精确复现。
- Pyright、Ruff、TypeScript、生成 OpenAPI/前端类型及 `git diff --check` 通过。
- 冻结后台与 macOS 应用通过 `scripts/build_desktop.py --no-install --smoke-test` 验证；详见 `.state/tushare-rules-build.log`。发布位置 `release/Asterion_Terminal.app`；`codesign --verify --deep --strict` 已通过。

## 证据边界

官方字段依据为 [fut_basic 文档](https://tushare.pro/document/2?doc_id=135)，核对日期同上。提供方响应在自动测试中采用明确离线构造样本；没有使用用户 Token 发起联网采集，也没有验证用户积分、限流额度或实际合约数据覆盖。未修改用户数据库或重启现有服务。

[fut_settle 文档](https://tushare.pro/document/2?doc_id=141) 提供每日结算参数字段，但本轮没有接入费用/保证金自动映射；仍要求用户补充这些规则以及生效依据。打包烟测证明冻结进程加载与现有研究链路工作，不等于 Tushare 在线数据准确性认证。
