"""Tushare futures adapter. Exact provider values remain in retrieval evidence."""

import re
import time
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation

import httpx

from asterion.data.providers.public import (
    Capability,
    Partition,
    ProviderError,
    ProviderManifest,
    SyncRequest,
)

EXCHANGES = ["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]
SUFFIXES = {"SHFE": "SHF", "DCE": "DCE", "CZCE": "ZCE", "CFFEX": "CFX", "INE": "INE", "GFEX": "GFE"}
FIELDS = {
    "contracts": "ts_code,symbol,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate",
    "calendar": "exchange,cal_date,is_open,pretrade_date",
    "daily": "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi,oi_chg",
}


def day(value) -> str:
    try:
        return date.fromisoformat(str(value)).isoformat()
    except (ValueError, TypeError):
        raise ProviderError("数据校验失败：日期格式不正确") from None


def number(value, *, optional=False, nonnegative=False):
    if optional and value is None:
        return None
    try:
        result = Decimal(str(value))
        if not result.is_finite() or (nonnegative and result < 0):
            raise InvalidOperation
        return str(result)
    except (InvalidOperation, ValueError):
        raise ProviderError("数据校验失败：数值缺失或非法") from None


class Tushare:
    manifest = ProviderManifest(
        id="tushare",
        name="Tushare Pro",
        version="1.0.0",
        capabilities=[
            Capability(
                id="contracts",
                type_id="futures.contracts",
                label="期货合约资料",
                exchanges=EXCHANGES,
                description="普通合约基础资料；不包含完整交易规则",
            ),
            Capability(
                id="calendar",
                type_id="futures.calendar",
                label="期货交易日历",
                exchanges=EXCHANGES[:-1],
                date_range=True,
                description="交易日与休市日；不包含日内交易时段",
            ),
            Capability(
                id="daily",
                type_id="futures.daily",
                label="期货历史日线",
                exchanges=EXCHANGES,
                date_range=True,
                symbol_required=True,
                description="单个实际合约的日线、结算价、成交量与持仓量",
            ),
        ],
    )

    def probe(self, credential: str) -> str:
        request = SyncRequest(
            command_id="probe",
            provider="tushare",
            dataset="calendar",
            exchange="SHFE",
            start=date(2024, 1, 2),
            end=date(2024, 1, 2),
        )
        rows = self.fetch(self.plan(request)[0], credential)
        if len(self.normalize(request, rows)) != 1:
            raise ProviderError("连接返回空数据，无法确认接口权限")
        return "交易日历接口验证通过；其他接口权限以实际同步结果为准"

    def plan(self, request: SyncRequest) -> list[Partition]:
        capability = next((c for c in self.manifest.capabilities if c.id == request.dataset), None)
        if not capability or request.exchange not in capability.exchanges:
            raise ProviderError("该插件不支持所选数据类型或交易所")
        params = {"exchange": request.exchange}
        if request.dataset == "contracts":
            if request.symbol or request.start or request.end:
                raise ProviderError("合约资料按交易所同步，不接受合约或日期过滤")
            return [
                Partition(
                    api="fut_basic",
                    params=params | {"fut_type": "1"},
                    fields=FIELDS[request.dataset].split(","),
                    limit=10000,
                )
            ]
        if not request.start or not request.end:
            raise ProviderError("请选择同步日期范围")
        if request.dataset == "daily":
            if not re.fullmatch(r"[A-Z]+[0-9]{3,4}\." + SUFFIXES[request.exchange], request.symbol):
                raise ProviderError(
                    "请输入该交易所的实际合约代码，例如 RB2610.SHF；暂不支持连续合约"
                )
            params["ts_code"] = request.symbol
        elif request.symbol:
            raise ProviderError("交易日历不接受合约过滤")
        partitions = []
        cursor = request.start
        while cursor <= request.end:
            end = min(cursor + timedelta(days=30), request.end)
            partitions.append(
                Partition(
                    api="fut_daily" if request.dataset == "daily" else "fut_trade_cal",
                    params=params
                    | {"start_date": cursor.strftime("%Y%m%d"), "end_date": end.strftime("%Y%m%d")},
                    fields=FIELDS[request.dataset].split(","),
                    limit=2000,
                )
            )
            cursor = end + timedelta(days=1)
        return partitions

    def fetch(self, partition: Partition, credential: str) -> list[dict]:
        if not credential:
            raise ProviderError("请先在设置 → 数据源中保存 Tushare Token")
        for attempt in range(3):
            try:
                with httpx.Client(timeout=30, follow_redirects=False) as client:
                    response = client.post(
                        "https://api.tushare.pro",
                        json={
                            "api_name": partition.api,
                            "token": credential,
                            "params": partition.params,
                            "fields": ",".join(partition.fields),
                        },
                    )
                if response.status_code == 429 or response.status_code >= 500:
                    if attempt < 2:
                        time.sleep(2**attempt)
                        continue
                    raise ProviderError("Tushare 限流或服务暂不可用，请稍后重试")
                if response.status_code != 200:
                    raise ProviderError("Tushare 请求被拒绝，请检查 Token 和接口权限")
                body = response.json()
                if body.get("code") != 0:
                    # Never echo msg: remote errors can contain request credentials.
                    message = str(body.get("msg", "")).lower()
                    if "token" in message:
                        raise ProviderError("AUTH_FAILED：Tushare Token 无效或已失效")
                    if "权限" in message or "积分" in message:
                        raise ProviderError(
                            "PERMISSION_DENIED：Tushare 账号没有该接口的积分或访问权限"
                        )
                    if "频" in message or "每分钟" in message or "每天" in message:
                        raise ProviderError("RATE_LIMITED：Tushare 调用额度或频率受限，请稍后重试")
                    raise ProviderError("PROVIDER_REJECTED：Tushare 拒绝请求，请检查账号权限或额度")
                data = body["data"]
                fields, items = data["fields"], data["items"]
                if (
                    not isinstance(fields, list)
                    or not all(isinstance(f, str) for f in fields)
                    or len(set(fields)) != len(fields)
                    or not isinstance(items, list)
                    or not set(partition.fields) <= set(fields)
                    or any(not isinstance(r, list) or len(r) != len(fields) for r in items)
                ):
                    raise ProviderError("Tushare 返回字段结构异常")
                if len(items) >= partition.limit:
                    raise ProviderError("返回数据触及接口上限，无法确认完整性；未发布")
                return [dict(zip(fields, row, strict=True)) for row in items]
            except httpx.HTTPError:
                if attempt < 2:
                    time.sleep(2**attempt)
                    continue
                raise ProviderError("无法连接 Tushare，请检查网络后重试") from None
            except (KeyError, TypeError, AttributeError, ValueError) as exc:
                if isinstance(exc, ProviderError):
                    raise
                raise ProviderError("Tushare 返回格式异常") from None
        raise ProviderError("Tushare 请求失败")

    def normalize(self, request: SyncRequest, rows: list[dict]) -> list[dict]:
        result, keys = [], set()
        try:
            for row in rows:
                if request.dataset == "calendar":
                    value = day(row["cal_date"])
                    if row["exchange"] != request.exchange or row["is_open"] not in (
                        0,
                        1,
                        "0",
                        "1",
                    ):
                        raise ProviderError("交易日历字段不符合请求")
                    normalized = {
                        "exchange": request.exchange,
                        "date": value,
                        "is_open": int(row["is_open"]),
                        "previous_trading_day": day(row["pretrade_date"])
                        if row.get("pretrade_date")
                        else None,
                    }
                    if (
                        normalized["previous_trading_day"]
                        and normalized["previous_trading_day"] >= value
                    ):
                        raise ProviderError("上一交易日必须早于当前日期")
                    key = value
                elif request.dataset == "contracts":
                    if row["exchange"] != request.exchange:
                        raise ProviderError("返回合约的交易所与请求不一致")
                    symbol = row["ts_code"]
                    if not re.fullmatch(r"[A-Z]+[0-9]{3,4}\." + SUFFIXES[request.exchange], symbol):
                        raise ProviderError("返回了非实际合约或未知代码")
                    normalized = {
                        "exchange": request.exchange,
                        "symbol": symbol,
                        "name": row["name"],
                        "product": row["fut_code"],
                        "listed": day(row["list_date"]),
                        "delisted": day(row["delist_date"]) if row.get("delist_date") else None,
                        "trade_unit": row.get("trade_unit"),
                        "per_unit": number(row.get("per_unit"), optional=True),
                        "quote_unit": row.get("quote_unit"),
                        "rules_status": "INCOMPLETE",
                    }
                    if (
                        not isinstance(row["name"], str)
                        or not row["name"]
                        or not isinstance(row["fut_code"], str)
                        or not row["fut_code"]
                        or normalized["delisted"]
                        and normalized["listed"] > normalized["delisted"]
                    ):
                        raise ProviderError("合约名称、品种或上市区间无效")
                    key = symbol
                else:
                    value = day(row["trade_date"])
                    if row["ts_code"] != request.symbol:
                        raise ProviderError("返回行情的合约与请求不一致")
                    normalized = {
                        "symbol": request.symbol,
                        "contract": request.exchange + "." + request.symbol.split(".")[0],
                        "exchange": request.exchange,
                        "trading_day": value,
                        **{k: number(row[k]) for k in ("open", "high", "low", "close")},
                        **{
                            k: number(row.get(k), optional=True)
                            for k in ("settle", "pre_settle", "pre_close", "oi_chg")
                        },
                        **{
                            k: number(row.get(k), optional=k != "vol", nonnegative=True)
                            for k in ("vol", "amount", "oi")
                        },
                    }
                    if Decimal(normalized["low"]) > min(
                        Decimal(normalized["open"]), Decimal(normalized["close"])
                    ) or Decimal(normalized["high"]) < max(
                        Decimal(normalized["open"]), Decimal(normalized["close"])
                    ):
                        raise ProviderError("日线 OHLC 价格边界不一致")
                    key = value
                if request.dataset != "contracts" and not (
                    str(request.start) <= key <= str(request.end)
                ):
                    raise ProviderError("返回日期超出请求范围")
                if key in keys:
                    raise ProviderError("返回了重复数据键，未发布")
                keys.add(key)
                result.append(normalized)
        except (KeyError, TypeError, InvalidOperation):
            raise ProviderError("数据校验失败：缺失必需字段") from None
        return sorted(
            result, key=lambda r: str(r.get("trading_day", r.get("date", r.get("symbol"))))
        )
