import json
import threading
from datetime import datetime, timedelta
from types import SimpleNamespace
from zoneinfo import ZoneInfo

import pytest
from connection_fakes import instrument, manager, save_body

from asterion.connector_ctp.query import query_instruments
from asterion.market.contracts import ContractChoicesService

NOW = datetime(2026, 9, 22, 12, tzinfo=ZoneInfo("Asia/Shanghai"))


def native_configuration():
    return SimpleNamespace(
        front="tcp://localhost:1234",
        trade_front="tcp://localhost:1235",
        user_id="fixture",
        broker_id="9999",
        app_id="simnow_client_test",
        auth_code="0000000000000000",
    )


def directory(root, rows=None, clock=lambda: NOW):
    connection = manager(root)
    key = connection.save(save_body()).connection_id
    connection.connect(key)
    connection.runtime[key].session.rows = rows if rows is not None else [instrument()]
    catalog = ContractChoicesService(
        root / "catalog", lambda cancel: connection.instruments(key, cancel), clock
    )
    catalog.request_refresh(connection.profile(key))
    catalog.thread.join(5)
    return catalog, connection, key


def native_fixture(monkeypatch, mode="success"):
    import openctp_ctp

    calls = []
    state = {}
    fields = {
        "ExchangeID": "SHFE",
        "InstrumentID": "au2612",
        "InstrumentName": "au2612" if mode in {"code_name", "missing_product"} else "黄金2612",
        "ProductID": "au",
        "ProductClass": "1",
        "OpenDate": "20260101",
        "ExpireDate": "20261215",
        "DeliveryYear": 2026,
        "DeliveryMonth": 12,
    }

    def authenticate(body, request_id):
        calls.append("authenticate")
        state["spi"].OnRspAuthenticate(
            None, SimpleNamespace(ErrorID=3 if mode == "auth" else 0), request_id, True
        )
        return 0

    def login(body, request_id, *extra):
        calls.append("login")
        state["spi"].OnRspUserLogin(None, None, request_id, True)
        return 0

    def products(body, request_id):
        calls.append("products")
        product = SimpleNamespace(
            ExchangeID="SHFE", ProductID="au", ProductName="黄金", ProductClass="1"
        )
        state["spi"].OnRspQryProduct(
            None if mode == "missing_product" else product,
            SimpleNamespace(ErrorID=7 if mode == "product_error" else 0),
            request_id,
            True,
        )
        return 0

    def query(body, request_id):
        calls.append("query")
        if mode == "timeout":
            return 0
        if mode == "send":
            return -3
        state["spi"].OnRspQryInstrument(SimpleNamespace(**fields), None, request_id, False)
        if mode == "partial":
            state["spi"].OnRspQryInstrument(None, SimpleNamespace(ErrorID=7), request_id, True)
        elif mode == "bad":
            state["spi"].OnRspQryInstrument(
                SimpleNamespace(**(fields | {"ExpireDate": ""})), None, request_id, True
            )
        else:
            state["spi"].OnRspQryInstrument(
                SimpleNamespace(**(fields | {"ProductClass": "2"})), None, request_id, True
            )
        return 0

    api = SimpleNamespace(
        RegisterSpi=lambda spi: state.update(spi=spi),
        RegisterFront=lambda front: calls.append(front),
        Init=lambda: state["spi"].OnFrontConnected(),
        ReqAuthenticate=authenticate,
        ReqUserLogin=login,
        ReqQryProduct=products,
        ReqQryInstrument=query,
        Release=lambda: calls.append("release"),
    )
    monkeypatch.setattr(
        openctp_ctp,
        "tdapi",
        SimpleNamespace(
            CThostFtdcTraderApi=SimpleNamespace(CreateFtdcTraderApi=lambda path: api),
            CThostFtdcTraderSpi=object,
            CThostFtdcReqAuthenticateField=SimpleNamespace,
            CThostFtdcReqUserLoginField=SimpleNamespace,
            CThostFtdcQryProductField=SimpleNamespace,
            CThostFtdcQryInstrumentField=SimpleNamespace,
            THOST_FTDC_PC_Futures="1",
        ),
    )
    return calls


@pytest.mark.parametrize(
    "mode",
    [
        "success",
        "code_name",
        "missing_product",
        "product_error",
        "auth",
        "partial",
        "bad",
        "timeout",
        "send",
    ],
)
def test_native_readonly_query_complete_or_rejected(monkeypatch, mode):
    calls = native_fixture(monkeypatch, mode)
    if mode in {"success", "code_name", "missing_product"}:
        rows = query_instruments(native_configuration(), "secret", threading.Event(), timeout=3)
        expected = {
            "success": "黄金2612",
            "code_name": "黄金 2026-12",
            "missing_product": "au2612",
        }[mode]
        assert rows == [instrument(name=expected)]
        assert calls[1:5] == ["authenticate", "login", "products", "query"]
    else:
        with pytest.raises(ValueError):
            query_instruments(
                native_configuration(),
                "secret",
                threading.Event(),
                timeout=0.01 if mode == "timeout" else 3,
            )
    assert calls[-1] == "release"


def test_cache_restore_expiry_and_source_names(tmp_path):
    clock = [NOW]
    catalog, connection, key = directory(
        tmp_path, [instrument(last_trade_on="2026-09-22")], lambda: clock[0]
    )
    assert catalog.choices("SHFE").contracts
    restored = ContractChoicesService(tmp_path / "catalog", catalog.query, lambda: clock[0])
    restored.bind(connection.profile(key))
    assert restored.names() == {"SHFE.au2612": "黄金2612"}
    clock[0] += timedelta(days=1)
    assert not restored.choices("SHFE").contracts
    assert restored.expired([]) == {("SHFE", "au2612")}


@pytest.mark.parametrize("damage", ["checksum", "fields", "future", "duplicate"])
def test_invalid_catalog_preserved(tmp_path, damage):
    import hashlib

    catalog, connection, key = directory(tmp_path)
    envelope = json.loads(catalog.path.read_text())
    payload = json.loads(envelope["payload"])
    if damage == "checksum":
        envelope["checksum"] = "bad"
    if damage == "fields":
        payload["contracts"][0].pop("last_trade_on")
    if damage == "future":
        payload["observed_at"] = (NOW + timedelta(days=1)).isoformat()
    if damage == "duplicate":
        payload["contracts"] *= 2
    if damage != "checksum":
        envelope["payload"] = json.dumps(payload)
        envelope["checksum"] = hashlib.sha256(envelope["payload"].encode()).hexdigest()
    catalog.path.write_text(json.dumps(envelope))
    original = catalog.path.read_bytes()
    restored = ContractChoicesService(tmp_path / "catalog", catalog.query, lambda: NOW)
    restored.bind(connection.profile(key))
    assert restored.choices("SHFE").state == "invalid" and restored.path.read_bytes() == original


@pytest.mark.parametrize(
    "name,expected", [("铸造铝合金期货 2026-10", "铸铝2610"), ("ad2610", "ad2610"), ("", "ad2610")]
)
def test_display_name_preserves_source(tmp_path, name, expected):
    row = instrument(symbol="ad2610", product="ad", delivery_month="2026-10", name=name)
    catalog, _, _ = directory(tmp_path, [row])
    assert catalog.choices("SHFE").contracts[0].name == expected
    assert catalog.cache.contracts[0] == row


def test_failed_or_cancelled_refresh_retains_last_complete(tmp_path, monkeypatch):
    catalog, connection, key = directory(tmp_path)
    original = catalog.path.read_bytes()

    def fail(cancel):
        raise ValueError("private")

    catalog.query = fail
    catalog.request_refresh(connection.profile(key))
    catalog.thread.join(5)
    assert catalog.state == "error" and catalog.path.read_bytes() == original
    entered = threading.Event()

    def wait(cancel):
        entered.set()
        cancel.wait(3)
        raise ValueError()

    catalog.query = wait
    catalog.request_refresh(connection.profile(key))
    assert entered.wait(2)
    catalog.stop()
    assert catalog.path.read_bytes() == original
    catalog.query = lambda cancel: connection.instruments(key, cancel)
    monkeypatch.setattr(
        "asterion.market.contracts.os.replace", lambda *a: (_ for _ in ()).throw(OSError())
    )
    catalog.request_refresh(connection.profile(key))
    catalog.thread.join(5)
    assert catalog.path.read_bytes() == original


def test_missing_from_next_batch_is_not_expired(tmp_path):
    catalog, connection, key = directory(tmp_path)
    connection.runtime[key].session.rows = [
        instrument(symbol="ag2612", product="ag", name="白银2612")
    ]
    catalog.request_refresh(connection.profile(key))
    catalog.thread.join(5)
    assert not catalog.expired([])
    assert len(catalog.cache.retained_lifecycles) == 1
