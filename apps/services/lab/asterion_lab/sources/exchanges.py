"""Official dated exchange settlement files; no broker account rates."""
from decimal import Decimal
import json
import ssl
import urllib.request
import urllib.error
import certifi
from .base import Source, Batch, SourceError, contract_of, decimal

THOUSANDTH = Decimal("0.001")
TEN_THOUSANDTH = Decimal("0.0001")
PERCENT = Decimal("0.01")

def fetch(url: str, timeout: float = 30) -> bytes:
    context = ssl.create_default_context(cafile=certifi.where())
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(request, timeout=timeout, context=context) as response:
        return response.read()


def official_settlement(exchange: str, day: str) -> dict:
    """Margin rate and fees by contract, from the exchange's own file."""
    compact = day.replace("-", "")
    rows = {}
    if exchange in ("SHFE", "INE"):
        host = "www.shfe.com.cn" if exchange == "SHFE" else "www.ine.cn"
        document = json.loads(fetch(f"https://{host}/data/tradedata/future/dailydata/js{compact}.dat"), parse_float=Decimal)
        for row in document["o_cursor"]:
            contract = contract_of(exchange, row["INSTRUMENTID"], day)
            if contract is None:
                continue
            # Margins are fractions; fee ratios are in thousandths of turnover.
            rows[contract] = {
                "long_margin_rate": decimal(row["SPECLONGMARGINRATIO"]),
                "short_margin_rate": decimal(row["SPECSHORTMARGINRATIO"]),
                "fee_rate": decimal(row["TRADEFEERATIO"]) * THOUSANDTH,
                "fee_per_lot": decimal(row["TRADEFEEUNIT"]),
                "close_today_fee_rate": decimal(row["TTRADEFEERATIO"]) * THOUSANDTH,
                "close_today_fee_per_lot": decimal(row["TTRADEFEEUNIT"]),
            }
        return rows
    if exchange == "CZCE":
        text = fetch(f"https://www.czce.com.cn/cn/DFSStaticFiles/Future/{day[:4]}/{compact}/"
                     "FutureDataClearParams.txt").decode("utf-8")
        lines = [line for line in text.splitlines() if "|" in line]
        header = [cell.strip() for cell in lines[0].split("|")]
        for line in lines[1:]:
            cells = dict(zip(header, (cell.strip() for cell in line.split("|"))))
            contract = contract_of("CZCE", cells["合约代码"], day)
            if contract is None:
                continue
            # Margin in percent; a fee is a ratio in ten-thousandths or yuan per lot.
            ratio = cells["手续费收取方式"] == "比例值"
            if not ratio and cells["手续费收取方式"] != "绝对值":
                raise ValueError(f"CZCE {cells['合约代码']}: unknown fee basis {cells['手续费收取方式']}")
            fee, today = decimal(cells["交易手续费"]), decimal(cells["日内平今仓交易手续费"])
            margin = decimal(cells["交易保证金率(%)"]) * PERCENT
            rows[contract] = {
                "long_margin_rate": margin, "short_margin_rate": margin,
                "fee_number": fee, "close_today_fee_number": today,
                "fee_rate": fee * TEN_THOUSANDTH if ratio else Decimal(0),
                "fee_per_lot": Decimal(0) if ratio else fee,
                "close_today_fee_rate": today * TEN_THOUSANDTH if ratio else Decimal(0),
                "close_today_fee_per_lot": Decimal(0) if ratio else today,
            }
        return rows
    raise ValueError(f"no official settlement file for {exchange}")


class ExchangeSource(Source):
    tables = ("settlement",)

    def read(self, query):
        self.check(query)
        self.requests += 1
        try:
            values = official_settlement(query.exchange, query.day)
        except urllib.error.HTTPError as error:
            # The exchanges publish no file for a closed day. The day stays
            # uncovered and is asked again next time, so this claims nothing.
            if error.code == 404:
                return Batch([])
            raise SourceError("unavailable", f"{self.identity}: settlement file unavailable (HTTP {error.code})") from None
        except (OSError, ValueError, KeyError, TypeError, IndexError):
            raise SourceError("unavailable", f"{self.identity}: cannot read the dated settlement file") from None
        self.check(query)
        rows = []
        for contract, values in values.items():
            if query.contract and contract != query.contract:
                continue
            rows.append({"exchange": query.exchange, "product": contract.split("/")[1],
                         "contract": contract, "day": query.day,
                         **{key: values.get(key) for key in (
                             "long_margin_rate", "short_margin_rate", "fee_rate", "fee_per_lot",
                             "close_today_fee_rate", "close_today_fee_per_lot")}})
        return Batch(rows)


class Shfe(ExchangeSource):
    identity, exchanges = "shfe", ("SHFE",)


class Ine(ExchangeSource):
    identity, exchanges = "ine", ("INE",)


class Czce(ExchangeSource):
    identity, exchanges = "czce", ("CZCE",)
