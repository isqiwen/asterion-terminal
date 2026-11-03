"""trading.proto requests in ProtoJSON, built as the trading page builds them."""
from decimal import Decimal
import time


def units(value):
    return {"units": str(int(Decimal(str(value)) * 100_000_000))}


def account_policy(catalog, symbol="rb2610", working_orders="2"):
    """A liveCreate with one contract from Market's CTP catalog (ProtoJSON):
    5 per order, 10 held, 2% deviation."""
    row = next(c for c in catalog["contracts"] if c["instrument"]["symbol"] == symbol)
    _, product, month = row["contractId"].split("/")
    return {"liveCreate": {
        "risk": {"maxOrderQuantity": units(5), "maxGrossQuantity": units(10),
                 "maxWorkingOrders": working_orders},
        "maxPriceDeviation": units("0.02"),
        "contracts": [{"venue": row["instrument"]["venue"], "symbol": symbol, "currency": "CNY",
                       "priceIncrement": units(row["priceTick"]), "quantityIncrement": units(1),
                       "multiplier": units(row["multiplier"]), "product": product,
                       "deliveryMonth": month}]}}


def market_catalog(call):
    """Market's CTP contract catalog through the Terminal relay, once it has loaded."""
    deadline = time.monotonic() + 20
    while True:
        catalog = call("market.call", {"request": {"snapshot": {}}})["market"]["snapshot"]["catalog"]
        if catalog.get("phase") == "ready":
            return catalog
        assert time.monotonic() < deadline, catalog
        time.sleep(0.2)


def submit(order_id, quantity, price="3500", symbol="rb2610"):
    return {"requestId": "submit." + order_id,
            "submit": {"orderId": order_id, "venue": "SHFE", "symbol": symbol, "side": "BUY",
                       "offset": "OPEN", "quantity": units(quantity), "price": units(price)}}
