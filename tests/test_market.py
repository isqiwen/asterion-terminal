import time
from datetime import datetime
from zoneinfo import ZoneInfo

import pytest
from fastapi.testclient import TestClient
from sqlalchemy import create_engine

from asterion.api.app import create_app
from asterion.market.models import MarketConfiguration, Subscription
from asterion.market.service import MarketService
from asterion.platform.config import Settings


class Feed:
    def __init__(self, configuration, password, emit):
        self.emit = emit
        self.closed = False

    def start(self):
        self.emit("connected", None)

    def close(self):
        self.closed = True


def configuration():
    return MarketConfiguration(
        front="tcp://localhost:12345",
        user_id="123456",
        subscriptions=[Subscription(exchange="SHFE", symbol="au2612")],
    )


def tick(now):
    stamp = datetime.fromtimestamp(now, ZoneInfo("Asia/Shanghai"))
    return {
        "InstrumentID": "au2612",
        "ExchangeID": "SHFE",
        "LastPrice": 100.0,
        "PreSettlementPrice": 80.0,
        "Volume": 10,
        "OpenInterest": 20.0,
        "TradingDay": "20260921",
        "ActionDay": stamp.strftime("%Y%m%d"),
        "UpdateTime": stamp.strftime("%H:%M:%S"),
        "UpdateMillisec": 0,
    }


def test_quotes_staleness_reconnect_rejection_and_password_not_saved(tmp_path):
    now = [time.time()]
    service = MarketService(tmp_path, Feed, lambda: now[0])
    service.configure(configuration())
    service.connect("do-not-persist")
    old = service.feed
    old.emit("tick", tick(now[0]))
    quote = service.snapshot().quotes[0]
    assert quote.change_percent == 25 and not quote.stale
    assert "do-not-persist" not in service.path.read_text()
    assert "do-not-persist" not in service.snapshot().model_dump_json()
    now[0] += 31
    assert service.snapshot().quotes[0].stale
    old.emit("tick", tick(now[0]))
    assert not service.snapshot().quotes[0].stale
    old.emit("reconnecting", None)
    assert service.snapshot().quotes[0].stale
    old.emit("connected", None)
    assert service.snapshot().quotes[0].stale  # Login alone cannot freshen a quote.
    old.emit("tick", tick(now[0]))
    service.disconnect()
    old.emit("connected", None)
    assert service.snapshot().state == "disconnected"
    service.connect("fresh")
    value = tick(now[0]) | {"ExchangeID": "DCE"}
    service.feed.emit("tick", value)
    assert "au2612" in service.snapshot().subscription_errors
    service.disconnect()
    restored = MarketService(tmp_path, Feed)
    assert restored.configuration == configuration()
    assert restored.feed is None


def test_invalid_prices_dates_out_of_order_and_subscription_errors(tmp_path):
    now = time.time()
    service = MarketService(tmp_path, Feed, lambda: now)
    service.configure(configuration())
    service.connect("test")
    service.feed.emit("tick", tick(now))
    service.feed.emit("tick", tick(now - 60) | {"LastPrice": 1})
    assert service.snapshot().quotes[0].last == 100
    service.feed.emit("tick", tick(now) | {"LastPrice": 1.7976931348623157e308, "ActionDay": ""})
    quote = service.snapshot().quotes[0]
    assert quote.last is None and quote.event_at is None and quote.stale
    service.feed.emit("subscription_error", ("au2612", "订阅被拒绝"))
    assert service.snapshot().subscription_errors["au2612"] == "订阅被拒绝"
    with pytest.raises(ValueError, match="先断开"):
        service.configure(configuration())
    service.disconnect()


def test_corrupt_configuration_no_writeback_and_month_codes(tmp_path):
    path = tmp_path / "market/simnow.json"
    path.parent.mkdir()
    path.write_text('{"version":99}')
    service = MarketService(tmp_path, Feed)
    assert service.snapshot().state == "error"
    with pytest.raises(ValueError):
        service.configure(configuration())
    with pytest.raises(ValueError):
        service.connect("test")
    assert path.read_text() == '{"version":99}'
    for symbol in ["au8888", "au0000", "au2613", "AU.CONT"]:
        with pytest.raises(ValueError):
            Subscription(exchange="SHFE", symbol=symbol)


def test_market_endpoints_authorization_and_secret_redaction(tmp_path):
    engine = create_engine(f"sqlite:///{tmp_path}/test.db")
    settings = Settings(
        token="market-test-token-long-enough", data_root=tmp_path / "data", require_account=False
    )
    with TestClient(create_app(settings, engine)) as client:
        assert client.get("/api/v1/market/state").status_code == 401
        headers = {"Authorization": f"Bearer {settings.token}"}
        assert client.get("/api/v1/market/state", headers=headers).json()["environment"] == "simnow"
        response = client.post(
            "/api/v1/market/connect",
            headers=headers,
            json={"password": "DO-NOT-ECHO", "unknown": True},
        )
        assert response.status_code == 422 and "DO-NOT-ECHO" not in response.text
        saved = client.post(
            "/api/v1/market/configuration", headers=headers, json=configuration().model_dump()
        )
        assert saved.status_code == 200
    engine.dispose()


def test_ctp_adapter_login_resubscribes_and_shutdown_releases(monkeypatch):
    from types import SimpleNamespace

    import openctp_ctp

    from asterion.market.ctp import CtpFeed

    calls = []
    api = SimpleNamespace(
        RegisterSpi=lambda spi: calls.append(("spi", spi)),
        RegisterFront=lambda front: calls.append(("front", front)),
        Init=lambda: calls.append(("init",)),
        ReqUserLogin=lambda body, request_id: (
            calls.append(("login", body.BrokerID, body.UserID)) or 0
        ),
        SubscribeMarketData=lambda symbols, count: calls.append(("subscribe", symbols, count)) or 0,
        Release=lambda: calls.append(("release",)),
    )
    monkeypatch.setattr(
        openctp_ctp,
        "mdapi",
        SimpleNamespace(
            CThostFtdcMdSpi=object,
            CThostFtdcMdApi=SimpleNamespace(CreateFtdcMdApi=lambda path: api),
            CThostFtdcReqUserLoginField=SimpleNamespace,
        ),
    )
    events = []
    feed = CtpFeed(configuration(), "secret", lambda *args: events.append(args))
    feed.start()
    feed.spi.OnFrontConnected()
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=0), 1, True)
    assert ("login", "9999", "123456") in calls
    assert ("subscribe", [b"au2612"], 1) in calls
    feed.spi.OnFrontDisconnected(1)
    feed.spi.OnFrontConnected()
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=0), 1, True)
    assert sum(c[0] == "subscribe" for c in calls) == 2
    feed.spi.OnRspUserLogin(None, SimpleNamespace(ErrorID=3), 1, True)
    assert events[-1][0] == "error"
    assert "secret" not in str(events)
    feed.close()
    assert feed.password == "" and calls[-1] == ("release",)
