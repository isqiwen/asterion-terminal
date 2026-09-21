"""Offline demonstration adapter; its instruments and calendar are entirely fictional."""

import hashlib
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation

from asterion.data.providers.public import (
    Capability,
    ConfigurationField,
    ConfigurationSpec,
    Partition,
    ProviderError,
    ProviderManifest,
    SyncRequest,
)
from asterion.data.types import builtin_types

START = date(2024, 1, 1)
END = date(2024, 1, 31)
EXCHANGE = "SIM"
SYMBOL = "DEMO001.SIM"
CONTRACT = "SIM.DEMO001"
DEFAULTS = {"start": "2024-01-02", "end": "2024-01-12"}
FIELDS = {
    "contracts": ["venue", "instrument", "label", "product", "listed", "delisted"],
    "calendar": ["venue", "day", "open", "previous_day"],
    "daily": [
        "venue",
        "instrument",
        "day",
        "open",
        "high",
        "low",
        "close",
        "volume",
        "turnover",
        "open_interest",
    ],
}


def previous_day(value: date) -> date | None:
    cursor = value - timedelta(days=1)
    while cursor >= START:
        if cursor.weekday() < 5:
            return cursor
        cursor -= timedelta(days=1)
    return None


def seed_value(configuration: dict) -> int:
    seed = configuration.get("seed", 7)
    if set(configuration) - {"seed"} or type(seed) is not int or not 0 <= seed <= 10000:
        raise ProviderError("合成示例的种子须为 0 至 10000 的整数")
    return seed


def number(value, *, integral=False, positive=False) -> str:
    try:
        result = Decimal(str(value))
        if not result.is_finite() or result < 0 or positive and result == 0:
            raise InvalidOperation
        if integral and result != result.to_integral_value():
            raise InvalidOperation
        # Match the terminal's lossless decimal representation without rounding values.
        if result != result.quantize(Decimal("0.00000001")) or result >= Decimal("1e12"):
            raise InvalidOperation
        return format(result, "f")
    except (InvalidOperation, ValueError, TypeError):
        raise ProviderError("合成示例校验失败：数值或精度非法") from None


class Synthetic:
    manifest = ProviderManifest(
        id="synthetic",
        name="合成示例（非真实行情）",
        version="1.0.0",
        description="无需网络或凭据；仅用于体验终端流程，不代表真实市场、交易日历或可交易合约。",
        demo=True,
        configuration=ConfigurationSpec(
            schema_version=1,
            fields=[
                ConfigurationField(
                    id="seed",
                    label="合成种子",
                    type="integer",
                    default=7,
                    minimum=0,
                    maximum=10000,
                    description="相同种子和日期始终生成相同示例行情；修改种子会改变价格。",
                )
            ],
        ),
        capabilities=[
            Capability(
                id="contracts",
                label="示例合约资料",
                type_id="futures.contracts",
                exchanges=[EXCHANGE],
                description="虚构合约 DEMO001.SIM；有效区间为 2024 年 1 月，不可用于真实交易。",
            ),
            Capability(
                id="calendar",
                label="示例交易日历",
                type_id="futures.calendar",
                exchanges=[EXCHANGE],
                date_range=True,
                defaults=DEFAULTS,
                description="仅 2024 年 1 月；周一至周五开放，周末关闭，不对应真实市场节假日。",
            ),
            Capability(
                id="daily",
                label="示例历史日线",
                type_id="testing.synthetic",
                exchanges=[EXCHANGE],
                date_range=True,
                symbol_required=True,
                defaults=DEFAULTS | {"symbol": SYMBOL},
                description="仅 DEMO001.SIM、2024 年 1 月；确定性生成的虚构日线。",
            ),
        ],
    )

    def plan(self, request: SyncRequest) -> list[Partition]:
        if (
            request.provider != self.manifest.id
            or request.dataset not in FIELDS
            or request.exchange != EXCHANGE
        ):
            raise ProviderError("合成示例仅支持 SIM 交易所的示例合约、日历与日线")
        params = {"exchange": EXCHANGE}
        if request.dataset == "contracts":
            if request.symbol or request.start or request.end:
                raise ProviderError("示例合约资料不接受合约或日期过滤")
        else:
            if (
                not request.start
                or not request.end
                or not START <= request.start <= request.end <= END
            ):
                raise ProviderError("合成示例日期范围仅限 2024-01-01 至 2024-01-31")
            params |= {"start_date": request.start.isoformat(), "end_date": request.end.isoformat()}
            if request.dataset == "daily":
                if request.symbol != SYMBOL:
                    raise ProviderError("合成日线仅支持虚构合约 DEMO001.SIM")
                params["symbol"] = SYMBOL
            elif request.symbol:
                raise ProviderError("示例日历不接受合约过滤")
        return [
            Partition(
                api=f"synthetic_{request.dataset}",
                params=params,
                fields=FIELDS[request.dataset],
                limit=32,
                start=request.start.isoformat() if request.start else None,
                end=request.end.isoformat() if request.end else None,
            )
        ]

    def probe(self, configuration: dict) -> str:
        seed_value(configuration)
        return "离线合成示例已就绪；无需网络或凭据，数据不代表真实市场"

    def fetch(self, partition: Partition, configuration: dict) -> list[dict]:
        seed = seed_value(configuration)
        try:
            request = SyncRequest.model_validate(
                {
                    "command_id": "synthetic-fetch",
                    "provider": self.manifest.id,
                    "dataset": partition.api.removeprefix("synthetic_"),
                    "exchange": partition.params["exchange"],
                    "symbol": partition.params.get("symbol", ""),
                    "start": partition.params.get("start_date"),
                    "end": partition.params.get("end_date"),
                }
            )
            if self.plan(request) != [partition]:
                raise ProviderError("合成示例分段计划无效")
        except (KeyError, ValueError) as exc:
            if isinstance(exc, ProviderError):
                raise
            raise ProviderError("合成示例分段计划无效") from None
        if request.dataset == "contracts":
            return [
                {
                    "venue": EXCHANGE,
                    "instrument": SYMBOL,
                    "label": "虚构示例合约（不可交易）",
                    "product": "DEMO",
                    "listed": START.isoformat(),
                    "delisted": END.isoformat(),
                }
            ]
        assert request.start is not None and request.end is not None
        result = []
        cursor = request.start
        while cursor <= request.end:
            if request.dataset == "calendar":
                prior = previous_day(cursor)
                result.append(
                    {
                        "venue": EXCHANGE,
                        "day": cursor.isoformat(),
                        "open": int(cursor.weekday() < 5),
                        "previous_day": prior.isoformat() if prior else None,
                    }
                )
            elif cursor.weekday() < 5:
                # A per-date digest makes partition boundaries and fetch order irrelevant.
                digest = hashlib.sha256(f"synthetic-v1:{seed}:{cursor}".encode()).digest()
                opening = 10000 + seed * 3 + (cursor - START).days * 11 + digest[0]
                closing = opening + digest[1] - 128
                high = max(opening, closing) + 1 + digest[2]
                low = min(opening, closing) - 1 - digest[3]
                volume = 100 + int.from_bytes(digest[4:6], "big")
                result.append(
                    {
                        "venue": EXCHANGE,
                        "instrument": SYMBOL,
                        "day": cursor.isoformat(),
                        **{
                            field: format(Decimal(value) / 100, ".2f")
                            for field, value in zip(
                                ("open", "high", "low", "close"),
                                (opening, high, low, closing),
                                strict=True,
                            )
                        },
                        "volume": str(volume),
                        # Synthetic multiplier is 1; amount uses the shared ten-thousand unit.
                        "turnover": format(Decimal(closing * volume) / 1000000, ".6f"),
                        "open_interest": str(1000 + int.from_bytes(digest[6:8], "big")),
                    }
                )
            cursor += timedelta(days=1)
        return result

    def normalize(self, request: SyncRequest, rows: list[dict]) -> list[dict]:
        self.plan(request)
        result = []
        try:
            for row in rows:
                if set(row) != set(FIELDS[request.dataset]) or row["venue"] != EXCHANGE:
                    raise ProviderError("合成示例校验失败：字段或交易所不符合请求")
                if request.dataset == "contracts":
                    if (
                        row["instrument"] != SYMBOL
                        or row["product"] != "DEMO"
                        or row["listed"] != START.isoformat()
                        or row["delisted"] != END.isoformat()
                        or row["label"] != "虚构示例合约（不可交易）"
                    ):
                        raise ProviderError("合成示例校验失败：示例合约资料不匹配")
                    normalized = {
                        "exchange": EXCHANGE,
                        "symbol": SYMBOL,
                        "name": row["label"],
                        "product": "DEMO",
                        "currency": "XXX",
                        "delivery_month": None,
                        "last_delivery_on": None,
                        "listed": row["listed"],
                        "delisted": row["delisted"],
                        "trade_unit": "示例单位/手",
                        "per_unit": "1",
                        "multiplier": None,
                        "quote_unit_desc": None,
                        "quote_unit": "示例价格/单位",
                        "rules_status": "INCOMPLETE",
                        "contract": EXCHANGE + "." + SYMBOL.split(".")[0],
                        "suggested_multiplier": None,
                        "multiplier_note": "Test-only unspecified quotation",
                    }
                else:
                    value = date.fromisoformat(row["day"])
                    if (
                        value.isoformat() != row["day"]
                        or not request.start
                        or not request.end
                        or not request.start <= value <= request.end
                    ):
                        raise ProviderError("合成示例校验失败：日期超出请求范围")
                    if request.dataset == "calendar":
                        prior = previous_day(value)
                        if (
                            type(row["open"]) is not int
                            or row["open"] != int(value.weekday() < 5)
                            or row["previous_day"] != (prior.isoformat() if prior else None)
                        ):
                            raise ProviderError("合成示例校验失败：日历不符合示例规则")
                        normalized = {
                            "exchange": EXCHANGE,
                            "date": value.isoformat(),
                            "is_open": row["open"],
                            "previous_trading_day": row["previous_day"],
                        }
                    else:
                        if row["instrument"] != SYMBOL or value.weekday() >= 5:
                            raise ProviderError("合成示例校验失败：合约或交易日不符合示例规则")
                        normalized = {
                            "exchange": EXCHANGE,
                            "symbol": SYMBOL,
                            "contract": CONTRACT,
                            "trading_day": value.isoformat(),
                            **{
                                key: number(row[key], positive=True)
                                for key in ("open", "high", "low", "close")
                            },
                            "vol": number(row["volume"], integral=True),
                            "amount": number(row["turnover"]),
                            "oi": number(row["open_interest"], integral=True),
                            "settle": None,
                            "pre_settle": None,
                            "pre_close": None,
                            "oi_chg": None,
                        }
                result.append(normalized)
        except (KeyError, TypeError, ValueError) as exc:
            if isinstance(exc, ProviderError):
                raise
            raise ProviderError("合成示例校验失败：字段缺失或格式非法") from None
        builtin_types().get(f"futures.{request.dataset}").validate(result)
        return sorted(
            result, key=lambda row: row.get("trading_day", row.get("date", row.get("symbol")))
        )
