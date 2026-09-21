"""Built-in trusted data types; no provider imports or dynamic code loading."""

from datetime import date as Date
from decimal import Decimal
from typing import Literal

from pydantic import AwareDatetime, BaseModel, Field, ValidationError, model_validator

from asterion.data.providers.public import ProviderError
from asterion.data.public import Bar
from asterion.data.types.public import DataField, DataType, TypeManifest, TypeRegistry


class Contract(BaseModel):
    contract: str = Field(min_length=1)
    suggested_multiplier: Decimal | None = Field(gt=0, le=10**6, allow_inf_nan=False)
    multiplier_note: str = Field(min_length=1)
    exchange: str = Field(min_length=1)
    symbol: str = Field(min_length=1)
    name: str = Field(min_length=1)
    product: str = Field(pattern=r"^[A-Z]+$")
    currency: str = Field(pattern=r"^[A-Z]{3}$")
    delivery_month: str | None = Field(pattern=r"^[0-9]{4}-(0[1-9]|1[0-2])$")
    last_delivery_on: Date | None
    listed: Date
    delisted: Date | None
    multiplier: Decimal | None = Field(allow_inf_nan=False)
    quote_unit_desc: str | None

    @model_validator(mode="after")
    def dates(self):
        if self.delivery_month is not None:
            Date.fromisoformat(self.delivery_month + "-01")
        if self.last_delivery_on and self.delisted and self.last_delivery_on < self.delisted:
            raise ValueError("Invalid delivery dates")
        if self.delisted and self.delisted < self.listed:
            raise ValueError("Invalid listing dates")
        return self


class CalendarDay(BaseModel):
    exchange: str = Field(min_length=1)
    date: Date
    is_open: int = Field(ge=0, le=1)
    previous_trading_day: Date | None

    @model_validator(mode="after")
    def dates(self):
        if self.previous_trading_day and self.previous_trading_day >= self.date:
            raise ValueError("Invalid previous trading day")
        return self


class RoleMapping(BaseModel):
    exchange: str = Field(min_length=1)
    product_id: str = Field(pattern=r"^(SHFE|DCE|CZCE|CFFEX|INE|GFEX)\.[A-Z]+$")
    symbol: str = Field(min_length=1)
    target_symbol: str = Field(min_length=1)
    role: Literal["main", "secondary"]
    trading_day: Date
    available_at: AwareDatetime | None

    @model_validator(mode="after")
    def venue(self):
        if self.product_id.split(".")[0] != self.exchange:
            raise ValueError("角色映射品种与交易所不一致")
        return self


class Settlement(BaseModel):
    exchange: str = Field(min_length=1)
    symbol: str = Field(min_length=1)
    contract: str = Field(min_length=1)
    trading_day: Date
    settle: Decimal | None = Field(allow_inf_nan=False)
    trading_fee_rate: Decimal | None = Field(allow_inf_nan=False, ge=0)
    trading_fee: Decimal | None = Field(allow_inf_nan=False, ge=0)
    delivery_fee: Decimal | None = Field(allow_inf_nan=False, ge=0)
    b_hedging_margin_rate: Decimal | None = Field(allow_inf_nan=False, ge=0)
    s_hedging_margin_rate: Decimal | None = Field(allow_inf_nan=False, ge=0)
    long_margin_rate: Decimal | None = Field(allow_inf_nan=False, ge=0)
    short_margin_rate: Decimal | None = Field(allow_inf_nan=False, ge=0)
    offset_today_fee: Decimal | None = Field(allow_inf_nan=False, ge=0)


class DailyBar(BaseModel):
    settle: Decimal | None = Field(default=None, allow_inf_nan=False)
    exchange: str = Field(min_length=1)
    symbol: str = Field(min_length=1)
    contract: str = Field(min_length=1)
    trading_day: Date
    open: Decimal = Field(allow_inf_nan=False)
    high: Decimal = Field(allow_inf_nan=False)
    low: Decimal = Field(allow_inf_nan=False)
    close: Decimal = Field(allow_inf_nan=False)
    vol: Decimal = Field(ge=0, decimal_places=0, allow_inf_nan=False)
    amount: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False)
    oi: Decimal | None = Field(default=None, ge=0, allow_inf_nan=False)

    @model_validator(mode="after")
    def bounds(self):
        if self.low > min(self.open, self.close) or self.high < max(self.open, self.close):
            raise ValueError("OHLC bounds")
        return self


def validator(model):
    def validate(rows):
        try:
            for row in rows:
                model.model_validate(row)
        except ValidationError:
            raise ProviderError("数据类型校验失败：字段、数值或时间关系无效") from None

    return validate


def fields(*values):
    return [DataField(name=v[0], label=v[1], unit=v[2] if len(v) > 2 else None) for v in values]


def calendar_coverage(rows, start, end):
    if start and end and len(rows) == (end - start).days + 1:
        return "CALENDAR_COMPLETE"
    raise ProviderError("交易日历缺少日期，未发布")


def daily_chart(rows, available_at):
    import csv
    import io

    from asterion.data.public import encode_csv

    stream = io.StringIO()
    writer = csv.DictWriter(
        stream,
        fieldnames=[
            "contract",
            "event_time",
            "available_at",
            "trading_day",
            "open",
            "high",
            "low",
            "close",
            "volume",
        ],
    )
    writer.writeheader()
    for row in rows:
        writer.writerow(
            {
                "contract": row["contract"],
                "event_time": row["trading_day"] + "T00:00:00+00:00",
                "available_at": row.get("_observed_at", available_at),
                "trading_day": row["trading_day"],
                **{k: row[k] for k in ("open", "high", "low", "close")},
                "volume": int(Decimal(row["vol"])),
            }
        )
    data, manifest = encode_csv(stream.getvalue())
    return data, manifest | {"frequency": "1d", "time_semantics": "trading_day_label"}


def builtin_types():
    return TypeRegistry(
        (
            DataType(
                TypeManifest(
                    id="futures.role_mapping",
                    label="供应商角色映射",
                    domain="reference",
                    domain_label="基础资料",
                    shape="table",
                    frequency="1d",
                    primary_key=["product_id", "symbol", "role", "trading_day"],
                    time_field="trading_day",
                    time_semantics="供应商交易日标签；历史公布时间可未知",
                    fields=fields(
                        ("exchange", "交易所"),
                        ("product_id", "品种"),
                        ("symbol", "角色来源代码"),
                        ("target_symbol", "目标来源代码"),
                        ("role", "角色"),
                        ("trading_day", "交易日"),
                        ("available_at", "历史可知时间"),
                    ),
                    description="供应商映射原始声明；须经固定合约资料解析为实际身份",
                ),
                validator(RoleMapping),
            ),
            DataType(
                TypeManifest(
                    id="futures.contracts",
                    schema_version=4,
                    label="合约资料",
                    domain="reference",
                    domain_label="基础资料",
                    shape="table",
                    frequency="snapshot",
                    primary_key=["exchange", "symbol", "listed"],
                    time_semantics="上市和到期区间；采集时间不代表规则生效时间",
                    fields=fields(
                        ("exchange", "交易所"),
                        ("symbol", "合约代码"),
                        ("name", "名称"),
                        ("product", "品种"),
                        ("currency", "币种"),
                        ("delivery_month", "交割年月"),
                        ("last_delivery_on", "最后交割日"),
                        ("listed", "上市日期"),
                        ("delisted", "最后交易日"),
                        ("trade_unit", "交易单位"),
                        ("per_unit", "每手数量"),
                        ("multiplier", "提供方合约乘数"),
                        ("quote_unit_desc", "最小报价说明"),
                        ("quote_unit", "报价单位"),
                        ("rules_status", "规则完整性"),
                        ("contract", "标准行情代码"),
                        ("suggested_multiplier", "建议每点每手乘数"),
                        ("multiplier_note", "乘数解释依据"),
                    ),
                    description="合约基础信息；不替代完整交易规则",
                ),
                validator(Contract),
            ),
            DataType(
                TypeManifest(
                    id="futures.calendar",
                    label="交易日历",
                    domain="reference",
                    domain_label="基础资料",
                    shape="table",
                    frequency="1d",
                    primary_key=["exchange", "date"],
                    time_field="date",
                    time_semantics="交易所当地日历日期；不包含日内交易时段",
                    fields=fields(
                        ("exchange", "交易所"),
                        ("date", "日期"),
                        ("is_open", "是否交易"),
                        ("previous_trading_day", "上一交易日"),
                    ),
                    description="交易日及休市日",
                ),
                validator(CalendarDay),
                coverage=calendar_coverage,
            ),
            DataType(
                TypeManifest(
                    id="futures.settlement",
                    label="每日结算参数",
                    domain="reference",
                    domain_label="基础资料",
                    shape="table",
                    frequency="1d",
                    primary_key=["contract", "trading_day"],
                    time_field="trading_day",
                    time_semantics="盘后交易日快照；采集时间不是历史公布或规则生效时间",
                    fields=fields(
                        ("contract", "标准合约"),
                        ("symbol", "合约代码"),
                        ("exchange", "交易所"),
                        ("trading_day", "参数交易日"),
                        ("settle", "结算价"),
                        ("trading_fee_rate", "交易费率原值"),
                        ("trading_fee", "交易手续费原值"),
                        ("delivery_fee", "交割手续费原值"),
                        ("b_hedging_margin_rate", "买套保保证金原值"),
                        ("s_hedging_margin_rate", "卖套保保证金原值"),
                        ("long_margin_rate", "买投机保证金原值"),
                        ("short_margin_rate", "卖投机保证金原值"),
                        ("offset_today_fee", "平今费率原值"),
                    ),
                    description="提供方原始参数数值；未推断费率单位和开平仓适用性",
                ),
                validator(Settlement),
            ),
            DataType(
                TypeManifest(
                    id="futures.daily",
                    label="历史日线",
                    domain="market",
                    domain_label="行情",
                    shape="timeseries",
                    frequency="1d",
                    primary_key=["contract", "trading_day"],
                    time_field="trading_day",
                    time_semantics="交易日标签，不是日线可获知时刻",
                    fields=fields(
                        ("contract", "标准合约"),
                        ("exchange", "交易所"),
                        ("symbol", "合约代码"),
                        ("trading_day", "交易日"),
                        ("open", "开"),
                        ("high", "高"),
                        ("low", "低"),
                        ("close", "收"),
                        ("vol", "成交量", "手"),
                        ("settle", "结算"),
                        ("pre_settle", "前结算"),
                        ("pre_close", "前收"),
                        ("oi", "持仓量", "手"),
                        ("oi_chg", "持仓变化", "手"),
                        ("amount", "成交额", "万元"),
                    ),
                    description="实际期货合约日线、结算及持仓",
                ),
                validator(DailyBar),
                chart=daily_chart,
            ),
            DataType(
                TypeManifest(
                    id="futures.bars",
                    label="历史行情 / 文件",
                    domain="market",
                    domain_label="行情",
                    shape="timeseries",
                    frequency="unspecified",
                    primary_key=["contract", "event_time"],
                    time_field="event_time",
                    time_semantics="文件明确提供 event_time 与 available_at；频率未声明",
                    fields=fields(
                        ("contract", "标准合约"),
                        ("event_time", "事件时间"),
                        ("available_at", "可获知时间"),
                        ("trading_day", "交易日"),
                        ("open", "开"),
                        ("high", "高"),
                        ("low", "低"),
                        ("close", "收"),
                        ("volume", "成交量", "手"),
                    ),
                    description="CSV 行情导入；不推断为日线，不与日线类型混合",
                ),
                validator(Bar),
            ),
        )
    )
