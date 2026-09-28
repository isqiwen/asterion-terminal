# 交易日与结算表

当前实现 CSV 数据插件、共享交易日契约、带内容版本的不可变发布，以及 Data Pipeline 独立命令行导入/检查。发布的交易日可直接用于多日回测，不需要再解析原文件。Task Service 已持久化结算表任务，由 Agent 派发 Data Pipeline 工作进程；Terminal 数据工作区可导入、查看、取消和重试。回测可选择已完成的结算表发布任务，也可显式选择手工日程。

## CSV 契约

UTF-8 文本，表头严格为：

```csv
trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source
```

每行是一段交易时段；同一交易日的多行必须连续，结算价、时段来源和结算来源逐字一致。最多 64 个交易日、每个交易日 16 个时段、总计 1024 行，原文件不超过 1 MiB。

- trading_day 为 YYYY-MM-DD 标签，严格递增；来源提供夜盘归属，系统不推断节假日。
- session_begin/end 为 `YYYY-MM-DDTHH:MM:SSZ` 或带明确 `+HH:MM` / `-HH:MM` 偏移的整秒时刻。支持最大 ±14:00，拒绝没有时区、无效日期、闰秒及超出非负 int64 纳秒范围的值。转成 UTC 半开区间后，时段及交易日间均不得重叠或倒序。
- settlement_price 为固定精度的规范十进制数；发布层保留负值及不对齐交易价格步长的数值，不猜测交易所价格规则、不用成交末值补全。当前模拟回测只支持正且对齐价格步长的结算价，在回测校验阶段明确拒绝不适配的数据，不在导入时删改。
- 两个来源字段均需 1–256 字节有效 UTF-8，且不全为空格、不包含控制字节。它们是输入者说明，不是系统对交易所来源的认证。
- 支持 LF、CRLF，以及用双引号包裹含逗号的字段；字段内部双引号写成两个双引号。不支持多行字段、BOM、空行或多余列。解析失败报告行号，不排序、截断、去重或跳过错误行。

下面仅演示格式，不是实际行情或交易所结算资料：

```csv
trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source
2026-09-25,2026-09-24T21:00:00+08:00,2026-09-24T23:00:00+08:00,105,"示例,时段","示例结算"
2026-09-25,2026-09-25T09:00:00+08:00,2026-09-25T15:00:00+08:00,105,"示例,时段","示例结算"
```

## 工程与发布身份

- `core/include/asterion/domain/settlement_calendar_port.hpp`：结算日记录及数据插件读取契约，组合既有 TradingDaySchedule 与精确 Decimal，不包含 CSV 或交易所推断。
- `plugins/data/csv/csv_settlement_calendar.*`：官方 CSV 结算表插件，复用插件生命周期与取消语义。
- `protocol/proto/asterion/v1/data.proto`：共享 SettlementDay / TradingSession、SettlementCalendar、CalendarCsvSnapshot、CalendarPublication。BacktestInput 版本 5 使用同一个 SettlementDay 字段模型，并保存可选的完整 CalendarPublication 证据；不添加旧字段别名或迁移路径。
- `protocol/src/calendar.cpp`：共享交易日校验、规范表示、内容摘要与发布身份；研究输入与数据发布调用同一个日期/区间校验入口。
- `apps/data-pipeline/calendar.*`：本地文件捕获、插件装配、来源重算、持久化发布和读取。输入保持在当前应用层，Core 不读取供应商文件。

内容版本对规范合约及完整交易日记录求 SHA-256，包含各日时段、结算价和来源说明。不同但等价的时区字符串或 LF/CRLF 可以对应相同内容版本。发布 ID 另外绑定文件名、原始字节 SHA-256、字节数和导入器 `asterion.csv.settlement.v1`，因此保留原文件差异。摘要是内容身份，不是外部数字签名。工作结果可根据原始快照重新解析核对；原始文件路径不进入发布物。

专用发布目录中只保存一个提交记录。相同输入重复发布幂等；已有不同内容、损坏记录或 pending.tmp 都明确拒绝，不覆盖或自动修复。源文件删除后仍可读取并校验发布内容和来源记录；没有恢复原始 CSV 字节、跨服务目录、权限共享或远端复制的承诺。

## 独立命令行

```sh
asterion-data-pipeline --settlement-calendar --input /absolute/import.pb --directory /absolute/empty-publication
asterion-data-pipeline --settlement-calendar --inspect --directory /absolute/publication
```

import.pb 是版本 1 的 data.v1.CsvImport，包含完整期货合约、本机源文件绝对路径和预期 SHA-256。捕获前后核对输入摘要，拒绝符号链接、超限或变动源文件。目录必须预先存在；检查时不需要原文件或输入规格。Agent 工作模式使用 `--settlement-calendar` 选择 CALENDAR_IMPORT 类型，领取任务后读取持久化的原始快照；不接受命令行文件路径或发布目录混入工作模式。

## 由交易时段模板生成

手工写每个时段容易出错。`config/futures-sessions.json` 按交易所/品种记录日盘与夜盘模板（北京时间），来源为期货公司汇编的《交易规则——各交易所交易时段汇总（2024年8月）》，2026-09-28 复核；交易所公告优先，交易时间调整后需更新该文件。

```sh
asterion-data-pipeline calendar-generate --sessions config/futures-sessions.json \
  --venue SHFE --product rb --settlements settlements.csv \
  --previous-trading-day 2026-09-25 --output calendar.csv
```

settlements.csv 表头为 `trading_day,settlement_price,settlement_source`，交易日严格递增、不得为周末。交易日列表和结算价由使用者依据交易所公告提供，工具不推断节假日。夜盘规则：交易日 D 的夜盘在上一交易日 P 的晚上进行，且仅当 P 与 D 之间只隔周六、周日；因此法定长假前最后一个交易日晚上没有夜盘。跨零点的夜盘结束于下一自然日（如周五夜盘结束于周六凌晨）。第一行的夜盘需要 `--previous-trading-day` 才能判定，未提供时不生成。

schedule_source 写为 `template <交易所>/<品种>: <来源, 复核日期>`。输出文件已存在时拒绝，不覆盖；生成的 CSV 仍需通过上面的导入流程校验和发布。临时停夜盘、交易所调整时段等例外不在模板中，需要手工修改生成结果。

## 后续产品接入

结算表已作为 CALENDAR_IMPORT 持久化任务进入 Task Service，由现有 Data Pipeline 程序执行；Agent 按任务类型派发。Terminal 使用当前历史数据的完整合约捕获最多 1 MiB 的本机 CSV 快照，提交后源文件删除不影响执行和恢复。Task Service 根据原始快照重新解析核对工作结果，拒绝不匹配来源；重试必须取得新执行令牌。Terminal 的“日程来源”可选择已完成发布任务。后端读取 Task Service 的结果，绑定完整发布物并严格比较合约与全部 days；发布身份、内容版本和来源保存在实验中，重启不需要原 CSV。选择发布日程时不允许同时传入手工 days；切换回手工日程解除关联，恢复此前手工表单，不自动复制/修改发布物。哈希证明内容一致，不证明外部来源权威性。旧 BacktestInput 版本 1–4 明确拒绝，既有文件不迁移、不删除。
