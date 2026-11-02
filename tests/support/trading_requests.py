"""trading.proto requests in ProtoJSON, built as the trading page builds them."""
from decimal import Decimal


def units(value):
    return {"units": str(int(Decimal(str(value)) * 100_000_000))}


def account_policy(catalog, symbol="rb2610", working_orders="2"):
    """A liveCreate with one contract from the CTP catalog: 5 per order, 10 held, 2% deviation."""
    row = next(c for c in catalog["contracts"] if c["symbol"] == symbol)
    _, product, month = row["contract_id"].split("/")
    return {"liveCreate": {
        "risk": {"maxOrderQuantity": units(5), "maxGrossQuantity": units(10),
                 "maxWorkingOrders": working_orders},
        "maxPriceDeviation": units("0.02"),
        "contracts": [{"venue": row["venue"], "symbol": symbol, "currency": "CNY",
                       "priceIncrement": units(row["price_tick"]), "quantityIncrement": units(1),
                       "multiplier": units(row["multiplier"]), "product": product,
                       "deliveryMonth": month}]}}


def submit(order_id, quantity, price="3500", symbol="rb2610"):
    return {"requestId": "submit." + order_id,
            "submit": {"orderId": order_id, "venue": "SHFE", "symbol": symbol, "side": "BUY",
                       "offset": "OPEN", "quantity": units(quantity), "price": units(price)}}
