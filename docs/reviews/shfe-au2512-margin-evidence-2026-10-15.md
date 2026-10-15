# F02：SHFE au2512 四事件官方证据闭环（2026-10-15）

实际仓库 `/home/qiwen/Code/quant/asterion-terminal`，`main`，HEAD `bacf29d33e8748fbf4c4a8585f3bfaa0926ada41`。本批独立从公开官网取证；没有真实供应商 API 请求、凭据读取、真实账本/旧实验写入、生产来源注册、提交推送或部署。

## 已核验事实

仅限 `SHFE/au/2025-12`，并列以下标注事件，不扩展为全历史规则或连续生效区间。

| 官方文件 | 标注日／阶段 | 一般买／卖保证金率 | 套保买／卖 | 文件内 update_date |
| --- | --- | --- | --- | --- |
| Settlement20251020.dat | 2025-10-20结算 | 0.14 / 0.14（14%） | 0.13 / 0.13 | 20251020 16:00:00 |
| ContractDailyTradeArgument20251021.dat | 2025-10-21交易参数 | 0.14 / 0.14（14%） | 0.13 / 0.13 | 20251021 15:50:12 |
| Settlement20251021.dat | 2025-10-21结算 | 0.16 / 0.16（16%） | 0.15 / 0.15 | 20251021 15:50:00 |
| ContractDailyTradeArgument20251022.dat | 2025-10-22交易参数 | 0.16 / 0.16（16%） | 0.15 / 0.15 | 20251022 15:55:12 |
| js20251021.dat | 2025-10-21结算交叉核对 | 0.16 / 0.16 | 0.15 / 0.15 | 20251021 15:25:02 |

前四份的行 `TRADINGDAY` 与文件 `report_date` 均一致。第五份以 `report_date` 标记日期，不虚构不存在的行 TRADINGDAY。两份21日结算文件的 au2512 结算价均为 **991.44**；一般字段分别为 `SPEC_LONGMARGINRATIO/SPEC_SHORTMARGINRATIO` 和 `SPECLONGMARGINRATIO/SPECSHORTMARGINRATIO`。一般与套保字段不能混用。

## 单位、阶段与来源边界

- 官网业务参数汇总页的 `settlement-f-component` 模板把 `SPEC_LONGMARGINRATIO` 和 `SPEC_SHORTMARGINRATIO` 乘100，并以一般持仓买/卖保证金率(%)显示。这直接支持这些 **SHFE字段** 的小数比例单位；不是靠数值大小猜测。
- `api.js` 的 `api_option_settlement_f(date)` 指向 `Settlement{date}.dat`，`generateData_settlement_f.js` 由当前所选日期调用并消费 `json.Settlement`；页面与日期数据有直接关联。
- `report_config_cn.dat` 中适用日期覆盖20251021的 `shfe_settlement` 备注，区分交易时段所展示的上一交易日结算参数和日终结算完成后发布的当日参数。它支持解释报告阶段，不把历史日期URL在盘中可读取当成已证明。
- 上期发〔2025〕300号明确黄金一般持仓16%从2025-10-21收盘结算起调整，并保留风控例外。此处官方实际 au2512 行与公告一致，无需把公告基础率泛化至所有合约。该公告不意味着当日盘中已为16%。
- 指定风险管理办法及黄金细则历史页均载明2025-08-08起施行，用于核对适用版本和交易所规则背景。黄金交易单位1000克/手、报价元/克；本批未据此改历史规格或撮合。
- `report_date`、`TRADINGDAY`、文件内 `update_date`、本次UTC读取时刻和HTTP Last-Modified均分别保留。两份同日结算文件 `update_date` 不同，不选其中一个当作首次发布时间或宣称两者为相同修订。

这些是 **现在官网提供的历史内容**。没有当时抓取记录或可验证发布历史，因此不能证明2025年当时首次发布时刻、从未修订、任一盘中事件前已知；会员/券商额外加收及客户实际保证金仍未知。不能把交易参数文件内日终update_date当作盘前已经公开的证据，也不据四个点推断未观察日期的适用值或夜盘时刻。

## 实现与验证

新增 `tests/data/fixtures/shfe_au2512_margin.json`：五条au2512精确提取行、原字段名、标注日、独立更新时间、URL、获取UTC时间、完整原文SHA/大小及语义引用。仅为公开证据回归夹具，`fixture.shfe.au2512.official.v1`不是生产提供者。原文完整保存在本次证据目录raw，未进入行情仓库。

新增离线验收探针 `tests/acceptance/verify_shfe_au2512_margin.py`：读取上述原文快照，核验12份文件摘要/大小、5条精确行、日期、单位模板与脚本关联、配置备注、双结算文件差异。不联网、不导入、不改文件。

新增已有 HistoricalMargin 测试中的官方样本回归，沿原临时TableStore→settlement_margin_fact→pin_margin路径验证结算14%/16%的精确准备值及来源绑定；确认当日交易14%旧快照不被结算16%覆盖，结算阶段被trading_day_start_margin拒绝、公开时点仍未知、套保15%不冒充一般16%、Tushare定义仍未知。交易参数行只作为独立标注事实，不自动生成日初费用表、不自动顺延结算值。

验证结果：

- 离线证据复核：12份文件、5条au2512行全部匹配；重放网络请求和仓库写入均0。
- 定向构建 `asterion_tushare_tests` 成功；`ctest --test-dir build/Debug -R "^HistoricalMargin\." --output-on-failure` **3/3通过**，其中1项为新官方样本。
- C++格式及限定改动空白检查通过；Python探针实际执行成功。
- 仅测试/文档改动，服务源指纹仍为 `541f85d316b8bcc1c2331d3dff8a0aed3bc710b01448942d3b2824bd8b0ef301`。不重建服务包、不重复跑无关全集，也不声称本批重新验过桌面或macOS。

重放命令：

```sh
python3 tests/acceptance/verify_shfe_au2512_margin.py build/reviews/f02-shfe-au2512-2026-10-15
ctest --test-dir build/Debug -R '^HistoricalMargin\.' --output-on-failure
```

证据目录 `build/reviews/f02-shfe-au2512-2026-10-15/` 包含 raw、sources.json、verified-events.json、evidence-check.json、core-tests.log、test-build.log及verification.json。

## 支持与仍不支持的路径

**支持：**只读核验上述官方历史事件；以明确结算阶段准备SHFE交易所一般持仓率并解释来源；在隔离契约测试中拒绝将21日结算16%前移到21日盘中；证明旧固定值保持。

**不支持：**Tushare字段单位/日期映射、SHFE自动导入、生产来源/Task成本集成、历史当时可知性、任意合约/日期推广、自动顺延为次日日初费率、结算阶段实际引擎执行、会员/券商客户保证金。当前费用日程仍按日初切换，结算阶段真实执行须另立范围并提升相关引擎标识，不能借取证静默改变。下一步若继续真实Tushare路线，仍需供应商对自身字段和日期/阶段的明确说明；若走SHFE来源，应单独确定来源契约和固定范围，不能让测试夹具成为生产导入口。

## 本次独立读取清单

读取时间为实际UTC；文件内时间未擅自转换为UTC。每项SHA对应完整响应体，参考脚本/配置/历史规则亦同。

| 文件／官方URL | 读取UTC | SHA-256 |
| --- | --- | --- |
| [raw/settlement-20251020.dat](https://www.shfe.com.cn/data/busiparamdata/future/Settlement20251020.dat) | 2026-10-15T06:42:25.660246+00:00 | `77b4794d58bcf5cb26f22af9781af1317091d38b3a4f259ce31307c690cbc099` |
| [raw/settlement-20251021.dat](https://www.shfe.com.cn/data/busiparamdata/future/Settlement20251021.dat) | 2026-10-15T06:42:29.086780+00:00 | `08bddb1b2d32f43610a87ff88a08564e1b43b085be853c8a0f00bbd15cb6636d` |
| [raw/daily-20251021.dat](https://www.shfe.com.cn/data/tradedata/future/dailydata/js20251021.dat) | 2026-10-15T06:42:29.399810+00:00 | `02cba5c4adb49335cddafea69bab8a365b6013953c7e27e6e56d7ee868bbc08a` |
| [raw/trade-20251021.dat](https://www.shfe.com.cn/data/busiparamdata/future/ContractDailyTradeArgument20251021.dat) | 2026-10-15T06:42:29.705973+00:00 | `dc1f5ed6f81c8a2e1a719d90fe9d514f1241c6748d8b9f9f7dbe82b6425c139e` |
| [raw/trade-20251022.dat](https://www.shfe.com.cn/data/busiparamdata/future/ContractDailyTradeArgument20251022.dat) | 2026-10-15T06:42:29.973294+00:00 | `448a7c808f399d3b17fb20f472e5b53f6f2176d124b901f743137bc7d00b880d` |
| [raw/report-config.dat](https://www.shfe.com.cn/data/config/report_config_cn.dat) | 2026-10-15T06:42:30.213412+00:00 | `92d3c4527c37a250a11d0773e7ae1fc06305e833799b586b53505625c134f49c` |
| [raw/prmsummary.html](https://www.shfe.com.cn/reports/businessdata/prmsummary/) | 2026-10-15T06:42:30.561441+00:00 | `a99d4a660ff2681526a4b5e1fc22c7d1015500e8f73e994221aff8b4cd870e54` |
| [raw/notice.html](https://www.shfe.com.cn/publicnotice/notice/202510/t20251017_829279.html) | 2026-10-15T06:42:30.958318+00:00 | `e81e03e3171edd52be3e2da65019c92ec17ff6cb84134cdb51a1fc1f96278bfb` |
| [raw/risk-rules.html](https://www.shfe.com.cn/regulation/exchangerules/historicalversion/202508/t20250807_828524.html) | 2026-10-15T06:42:31.253890+00:00 | `9722a7401caf4cc9492e008782cad311f71fbb26d36cce6ec407c9fdcc81b3d1` |
| [raw/gold-rules.html](https://www.shfe.com.cn/regulation/exchangerules/historicalversion/202508/t20250807_828540.html) | 2026-10-15T06:42:31.778636+00:00 | `2ffa14cea9097c82469b44e76436466d5e2b316052082ae4c1c5f9f1b1ce5a3c` |
| [raw/api.js](https://www.shfe.com.cn/images/api.js) | 2026-10-15T06:43:43.430042+00:00 | `888923e303b7178af9b3bfac83621c60f2f6ae0871894bf1ebe75a3a2708f86b` |
| [raw/generateData_settlement_f.js](https://www.shfe.com.cn/images/generateData_settlement_f.js) | 2026-10-15T06:43:43.616583+00:00 | `e3d65c2bde9eab0ed57931b90601ec9fd386d0d0b99915473a0ef906d719953c` |
