"""trading.proto requests in ProtoJSON, built as the trading page builds them."""
import time


def decimal(value):
    return {"text": str(value)}


def account_policy(catalog, symbol="rb2610", working_orders=2):
    """An openAccount with one contract from Market's CTP catalog (ProtoJSON):
    5 per order, 10 held, 2% deviation."""
    row = next(c for c in catalog["contracts"] if c["instrument"]["symbol"] == symbol)
    _, product, month = row["contract"].split("/")
    return {"openAccount": {
        "limits": {"maxOrderLots": decimal(5), "maxGrossLots": decimal(10),
                   "maxWorkingOrders": working_orders},
        "maxPriceDeviation": decimal("0.02"),
        "contracts": [{"instrument": row["instrument"], "product": product, "deliveryMonth": month,
                       "multiplier": decimal(row["multiplier"]), "priceTick": row["priceTick"],
                       "lotSize": decimal(1)}]}}


def market_catalog(call):
    """Market's CTP contract catalog through the Terminal relay, once it has loaded."""
    deadline = time.monotonic() + 20
    while True:
        catalog = call("market.call", {"request": {"getBoard": {}}})["market"]["getBoard"].get("catalog", {})
        if catalog.get("status", {}).get("state") == "CATALOG_STATE_READY":
            return catalog
        assert time.monotonic() < deadline, catalog
        time.sleep(0.2)


def submit(order_id, quantity, price="3500", symbol="rb2610"):
    return {"requestId": "submit." + order_id,
            "submitOrder": {"orderId": order_id, "instrument": {"exchange": "SHFE", "symbol": symbol},
                            "side": "SIDE_BUY", "offset": "OFFSET_OPEN",
                            "lots": decimal(quantity), "price": decimal(price)}}
