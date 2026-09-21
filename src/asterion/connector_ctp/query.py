"""Read-only CTP queries. No settlement confirmation or order operations."""

import sys
import tempfile
import threading
import time
from datetime import date
from typing import cast

from asterion.connections.public import ConnectorError, SourceInstrument


def query_instruments(configuration, password, cancel, timeout=45):
    return cast(
        list[SourceInstrument], _query(configuration, password, cancel, "instruments", timeout)
    )


def query_account(configuration, password, cancel, timeout=45):
    return cast(dict, _query(configuration, password, cancel, "account", timeout))


def _query(configuration, password, cancel, kind, timeout):
    from openctp_ctp import tdapi

    done = threading.Event()
    rows, failures = [], []
    stage = [0]
    accounts, positions = [], []
    products = {}
    pending = []
    last_query = [0.0]
    started = time.monotonic()
    with tempfile.TemporaryDirectory(prefix="asterion-instruments-") as flow:
        api = tdapi.CThostFtdcTraderApi.CreateFtdcTraderApi(flow + "/")

        def fail(message):
            failures.append(message)
            done.set()

        def check(info, request_id, expected):
            if done.is_set() or cancel.is_set() or stage[0] != expected or request_id != expected:
                return False
            if info and info.ErrorID:
                fail(f"CTP 查询失败（{info.ErrorID}），请检查交易前置、账户或服务时段")
                return False
            return True

        def send(method, request, request_id):
            stage[0] = request_id
            if request_id >= 3:
                last_query[0] = time.monotonic()
            try:
                if method(request, request_id):
                    fail("CTP 查询请求未发送，请稍后重试")
            except Exception:  # noqa: BLE001 - never log native requests or passwords
                fail("CTP 查询接口调用失败，请检查本机 SDK 后重试")

        class Spi(tdapi.CThostFtdcTraderSpi):
            def OnFrontConnected(self):
                if done.is_set() or cancel.is_set():
                    return
                request = tdapi.CThostFtdcReqAuthenticateField()
                request.BrokerID = configuration.broker_id
                request.UserID = configuration.user_id
                request.AppID = configuration.app_id
                request.AuthCode = configuration.auth_code
                send(api.ReqAuthenticate, request, 1)

            def OnFrontDisconnected(self, reason):
                fail("CTP 查询连接中断，请重新获取")

            def OnRspAuthenticate(self, response, info, request_id, last):
                if check(info, request_id, 1) and last:
                    request = tdapi.CThostFtdcReqUserLoginField()
                    request.BrokerID = configuration.broker_id
                    request.UserID = configuration.user_id
                    request.Password = password
                    login = (
                        (lambda body, request_id: api.ReqUserLogin(body, request_id, 0, ""))
                        if sys.platform == "darwin"
                        else api.ReqUserLogin
                    )
                    send(login, request, 2)

            def OnRspUserLogin(self, response, info, request_id, last):
                if check(info, request_id, 2) and last:
                    if kind == "instruments":
                        send(api.ReqQryProduct, tdapi.CThostFtdcQryProductField(), 6)
                    else:
                        request = tdapi.CThostFtdcQryTradingAccountField()
                        request.BrokerID = configuration.broker_id
                        request.InvestorID = configuration.user_id
                        request.CurrencyID = "CNY"
                        pending.append((api.ReqQryTradingAccount, request, 4))

            def OnRspQryProduct(self, product, info, request_id, last):
                if not check(info, request_id, 6):
                    return
                try:
                    if product and product.ProductClass == tdapi.THOST_FTDC_PC_Futures:
                        key = (product.ExchangeID, product.ProductID)
                        name = product.ProductName.strip()
                        if key in products or len(products) >= 10000:
                            raise ValueError("duplicate or excessive product response")
                        products[key] = name
                    if last:
                        pending.append(
                            (api.ReqQryInstrument, tdapi.CThostFtdcQryInstrumentField(), 3)
                        )
                except (ValueError, TypeError, AttributeError):
                    fail("CTP 返回的品种名称无效，请重新获取")

            def OnRspQryInstrument(self, instrument, info, request_id, last):
                if not check(info, request_id, 3):
                    return
                try:
                    if instrument and instrument.ProductClass == tdapi.THOST_FTDC_PC_Futures:
                        exchange = instrument.ExchangeID
                        if exchange in {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"}:
                            # Copy native callback-owned fields; no guessed code or year.
                            listed = date.fromisoformat(instrument.OpenDate)
                            expiry = date.fromisoformat(instrument.ExpireDate)
                            name = instrument.InstrumentName.strip()
                            product_name = products.get((exchange, instrument.ProductID), "")
                            if (
                                (not name or name.casefold() == instrument.InstrumentID.casefold())
                                and product_name
                                and product_name.casefold() != instrument.ProductID.casefold()
                            ):
                                name = f"{product_name} {instrument.DeliveryYear:04d}-{instrument.DeliveryMonth:02d}"
                            rows.append(
                                SourceInstrument(
                                    exchange=exchange,
                                    symbol=instrument.InstrumentID,
                                    name=name or instrument.InstrumentID,
                                    product=instrument.ProductID,
                                    delivery_month=f"{instrument.DeliveryYear:04d}-{instrument.DeliveryMonth:02d}",
                                    listed_on=listed,
                                    last_trade_on=expiry,
                                )
                            )
                    if len(rows) > 10000:
                        raise ValueError("too many instruments")
                    if last:
                        done.set()
                except (ValueError, TypeError, AttributeError):
                    fail("CTP 返回的合约资料不完整或无效，请重新获取")

            def OnRspQryTradingAccount(self, account, info, request_id, last):
                if not check(info, request_id, 4):
                    return
                if account:
                    accounts.append(
                        {
                            key: getattr(account, key)
                            for key in (
                                "AccountID",
                                "CurrencyID",
                                "TradingDay",
                                "Balance",
                                "Available",
                                "CurrMargin",
                                "PositionProfit",
                                "CloseProfit",
                                "Commission",
                                "FrozenMargin",
                                "FrozenCommission",
                            )
                        }
                    )
                if len(accounts) > 10000:
                    fail("账户响应超出限制")
                    return
                if last:
                    request = tdapi.CThostFtdcQryInvestorPositionField()
                    request.BrokerID = configuration.broker_id
                    request.InvestorID = configuration.user_id
                    pending.append((api.ReqQryInvestorPosition, request, 5))

            def OnRspQryInvestorPosition(self, position, info, request_id, last):
                if not check(info, request_id, 5):
                    return
                if position:
                    positions.append(
                        {
                            key: getattr(position, key)
                            for key in (
                                "InstrumentID",
                                "ExchangeID",
                                "PosiDirection",
                                "HedgeFlag",
                                "PositionDate",
                                "Position",
                                "TodayPosition",
                                "PositionProfit",
                                "UseMargin",
                                "TradingDay",
                            )
                        }
                    )
                if len(positions) > 10000:
                    fail("CTP 持仓响应超出限制")
                if last:
                    done.set()

            def OnRspError(self, info, request_id, last):
                if info and info.ErrorID:
                    fail(f"CTP 查询接口错误（{info.ErrorID}），请稍后重试")

        spi = Spi()
        try:
            api.RegisterSpi(spi)
            api.RegisterFront(configuration.trade_front)
            api.Init()
            while not done.wait(0.1):
                if pending and time.monotonic() - last_query[0] >= 1.1:
                    method, request, request_id = pending.pop(0)
                    last_query[0] = time.monotonic()
                    send(method, request, request_id)
                if cancel.is_set():
                    raise ConnectorError("CTP 查询已取消", "session_invalid", False)
                if time.monotonic() - started > timeout:
                    raise ConnectorError("CTP 查询超时，请检查交易前置及服务时段后重试", "timeout")
            if cancel.is_set():
                raise ConnectorError("合约查询已取消", "session_invalid", False)
            if failures:
                raise ConnectorError(failures[0], "incomplete")
            if kind == "account":
                return {"accounts": accounts, "positions": positions}
            if not rows:
                raise ConnectorError("CTP 未返回期货合约，请检查所选环境后重试")
            return rows
        finally:
            api.RegisterSpi(None)
            api.Release()
