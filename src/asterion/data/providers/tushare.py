"""Tushare futures adapter. Exact provider values remain in retrieval evidence."""

import re
import time
from datetime import date, timedelta
from decimal import Decimal, InvalidOperation

import httpx

from asterion.data.providers.public import (
    Capability,
    ConfigurationField,
    ConfigurationSpec,
    Partition,
    ProviderError,
    ProviderManifest,
    SyncRequest,
)

EXCHANGES = ["SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"]
SUFFIXES = {"SHFE": "SHF", "DCE": "DCE", "CZCE": "ZCE", "CFFEX": "CFX", "INE": "INE", "GFEX": "GFE"}
FIELDS = {
    "mapping": "ts_code,trade_date,mapping_ts_code",
    "contracts": "ts_code,symbol,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate",
    "settlement": "ts_code,trade_date,settle,trading_fee_rate,trading_fee,delivery_fee,b_hedging_margin_rate,s_hedging_margin_rate,long_margin_rate,short_margin_rate,offset_today_fee,exchange",
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
        version="1.5.0",
        description="真实历史数据；接口积分与访问权限由 Tushare 账号决定。",
        configuration=ConfigurationSpec(
            fields=[
                ConfigurationField(
                    id="token",
                    label="Tushare Token",
                    secret=True,
                    required=True,
                    min_length=1,
                    max_length=256,
                    placeholder="粘贴 Tushare Pro Token",
                    description="仅在本机加密保存；不会回显或写入数据集。",
                )
            ]
        ),
        capabilities=[
            Capability(
                id="mapping",
                type_id="futures.role_mapping",
                label="主力合约映射",
                exchanges=EXCHANGES,
                date_range=True,
                symbol_required=True,
                description="来源代码例如 RB.SHF；供应商每日主力对应月合约，历史公布时刻未知",
            ),
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
                id="settlement",
                type_id="futures.settlement",
                label="每日结算参数",
                exchanges=EXCHANGES,
                date_range=True,
                symbol_required=True,
                description="盘后参数快照；费率单位与历史生效时间须另行确认",
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

    def probe(self, configuration: dict) -> str:
        request = SyncRequest(
            command_id="probe",
            provider="tushare",
            dataset="calendar",
            exchange="SHFE",
            start=date(2024, 1, 2),
            end=date(2024, 1, 2),
        )
        rows = self.fetch(self.plan(request)[0], configuration)
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
        if request.dataset in {"daily", "settlement"}:
            if not re.fullmatch(r"[A-Z]+[0-9]{3,4}\." + SUFFIXES[request.exchange], request.symbol):
                raise ProviderError(
                    "请输入该交易所的实际合约代码，例如 RB2610.SHF；暂不支持连续合约"
                )
            params["ts_code"] = request.symbol
        elif request.dataset == "mapping":
            if not re.fullmatch(r"[A-Z]+\." + SUFFIXES[request.exchange], request.symbol):
                raise ProviderError("请输入主力代码，例如 RB.SHF；不接受实际合约或其他连续代码")
            params = {"ts_code": request.symbol}
        elif request.symbol:
            raise ProviderError("交易日历不接受合约过滤")
        partitions = []
        cursor = request.start
        while cursor <= request.end:
            end = min(cursor + timedelta(days=30), request.end)
            partitions.append(
                Partition(
                    api={
                        "mapping": "fut_mapping",
                        "daily": "fut_daily",
                        "settlement": "fut_settle",
                        "calendar": "fut_trade_cal",
                    }[request.dataset],
                    params=params
                    | {"start_date": cursor.strftime("%Y%m%d"), "end_date": end.strftime("%Y%m%d")},
                    fields=FIELDS[request.dataset].split(","),
                    limit=1600 if request.dataset == "settlement" else 2000,
                    start=cursor.isoformat(),
                    end=end.isoformat(),
                )
            )
            cursor = end + timedelta(days=1)
        return partitions

    def fetch(self, partition: Partition, configuration: dict) -> list[dict]:
        credential = configuration.get("token", "")
        if not credential:
            raise ProviderError("请先在设置 → 数据源中保存 Tushare Token")
        if any(c.isspace() for c in credential):
            raise ProviderError("Token 格式不正确")
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
                        "contract": request.exchange + "." + symbol.split(".")[0],
                        "name": row["name"],
                        "product": row["fut_code"],
                        "currency": "CNY",
                        "delivery_month": delivery_month(row["d_month"]),
                        "last_delivery_on": day(row["last_ddate"]) if row["last_ddate"] else None,
                        "listed": day(row["list_date"]),
                        "delisted": day(row["delist_date"]) if row.get("delist_date") else None,
                        "trade_unit": row.get("trade_unit"),
                        "per_unit": number(row.get("per_unit"), optional=True),
                        "multiplier": number(row.get("multiplier"), optional=True),
                        "quote_unit_desc": row.get("quote_unit_desc"),
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
                    code = re.fullmatch(r"([A-Z]+)([0-9]{3,4})\.[A-Z]+", symbol)
                    if code is None or code[1] != normalized["product"]:
                        raise ProviderError("合约代码与来源品种不一致")
                    month = normalized["delivery_month"]
                    if month is not None and month.replace("-", "")[-len(code[2]) :] != code[2]:
                        raise ProviderError("合约代码与来源完整交割年月不一致")
                    normalized.update(contract_multiplier(normalized))
                    key = (symbol, normalized["listed"])
                elif request.dataset == "mapping":
                    value = day(row["trade_date"])
                    product = request.symbol.split(".")[0]
                    target = row["mapping_ts_code"]
                    if row["ts_code"] != request.symbol or not re.fullmatch(
                        re.escape(product) + r"[0-9]{3,4}\." + SUFFIXES[request.exchange], target
                    ):
                        raise ProviderError("主力映射代码或目标合约与请求品种不一致")
                    normalized = {
                        "exchange": request.exchange,
                        "product_id": request.exchange + "." + product,
                        "symbol": request.symbol,
                        "target_symbol": target,
                        "role": "main",
                        "trading_day": value,
                        "available_at": None,
                    }
                    key = value
                elif request.dataset == "settlement":
                    value = day(row["trade_date"])
                    if row["ts_code"] != request.symbol or row["exchange"] != request.exchange:
                        raise ProviderError("结算参数的合约或交易所与请求不一致")
                    normalized = {
                        "symbol": request.symbol,
                        "contract": request.exchange + "." + request.symbol.split(".")[0],
                        "exchange": request.exchange,
                        "trading_day": value,
                        **{
                            key: number(row[key], optional=True, nonnegative=key != "settle")
                            for key in FIELDS["settlement"].split(",")
                            if key not in {"ts_code", "trade_date", "exchange"}
                        },
                    }
                    key = value
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
                    isinstance(key, str) and str(request.start) <= key <= str(request.end)
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


def positive(value):
    try:
        number = Decimal(str(value))
        if number.is_finite() and 0 < number <= 10**6:
            return str(number)
    except InvalidOperation:
        pass
    return None


def contract_multiplier(row):
    """Interpret this provider's quotation fields before publishing standard evidence."""
    multiplier = None
    note = "报价和交易单位未能明确匹配，请核实后手动填写研究乘数。"
    if row["exchange"] == "CFFEX":
        if re.fullmatch(r"(?:IF|IH|IC|IM)[0-9]{4}\.CFX", row["symbol"]):
            multiplier = positive(row["multiplier"])
            note = "股指合约使用 fut_basic.multiplier；请确认每点每手金额。"
    elif row["trade_unit"] and row["quote_unit"] in {
        "元/" + row["trade_unit"],
        "人民币元/" + row["trade_unit"],
    }:
        multiplier = positive(row["per_unit"])
        note = "报价为元/交易单位，使用 fut_basic.per_unit 作为每手乘数；请确认单位。"
    return {"suggested_multiplier": multiplier, "multiplier_note": note}


def delivery_month(value):
    """Use the source full delivery year, never the displayed three/four digit code."""
    if value is None or value == "":
        return None
    if not isinstance(value, str) or not re.fullmatch(r"[0-9]{6}", value):
        raise ProviderError("交割月份必须提供完整 YYYYMM，不能从代码推断年份")
    try:
        return date.fromisoformat(value[:4] + "-" + value[4:] + "-01").strftime("%Y-%m")
    except ValueError:
        raise ProviderError("交割月份无效") from None
