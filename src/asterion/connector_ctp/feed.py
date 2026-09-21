"""Trusted CTP market-data adapter. Never imports or initializes TraderApi."""

import tempfile
import threading


class CtpFeed:
    def __init__(self, configuration, password, emit):
        from openctp_ctp import mdapi

        self.emit = emit
        self.configuration = configuration
        self.subscriptions = list(configuration.subscriptions)
        self.subscription_lock = threading.RLock()
        self.logged_in = False
        self.password = password
        self.flow = tempfile.TemporaryDirectory(prefix="asterion-md-")
        self.api = mdapi.CThostFtdcMdApi.CreateFtdcMdApi(self.flow.name + "/")
        owner = self

        class Spi(mdapi.CThostFtdcMdSpi):
            def OnFrontConnected(self):
                owner.emit("connecting", None)
                request = mdapi.CThostFtdcReqUserLoginField()
                request.BrokerID = configuration.broker_id
                request.UserID = configuration.user_id
                request.Password = owner.password
                code = owner.api.ReqUserLogin(request, 1)
                if code:
                    owner.emit("error", f"登录请求未发送（{code}），请重连")

            def OnFrontDisconnected(self, reason):
                with owner.subscription_lock:
                    owner.logged_in = False
                owner.emit("reconnecting", None)

            def OnRspUserLogin(self, response, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit("error", f"CTP 登录失败（{info.ErrorID}），请检查账号或服务时段")
                    return
                if last:
                    owner.emit("connected", None)
                    with owner.subscription_lock:
                        owner.logged_in = True
                        warnings = owner._subscribe(owner.subscriptions)
                    for warning in warnings:
                        owner.emit("subscription_warning", warning)

            def OnRspSubMarketData(self, instrument, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit(
                        "subscription_error",
                        (
                            instrument.InstrumentID if instrument else "未知代码",
                            f"订阅被拒绝（{info.ErrorID}）",
                        ),
                    )

            def OnRspUnSubMarketData(self, instrument, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit(
                        "subscription_warning", f"退订被拒绝（{info.ErrorID}）；请重新连接重试"
                    )

            def OnRspError(self, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit("error", f"行情接口错误（{info.ErrorID}），请重连")

            def OnRtnDepthMarketData(self, tick):
                # Copy callback-owned fields before the native object is released.
                owner.emit(
                    "tick",
                    {
                        name: getattr(tick, name)
                        for name in (
                            "InstrumentID",
                            "ExchangeID",
                            "LastPrice",
                            "PreSettlementPrice",
                            "HighestPrice",
                            "LowestPrice",
                            "Volume",
                            "OpenInterest",
                            "TradingDay",
                            "ActionDay",
                            "UpdateTime",
                            "UpdateMillisec",
                        )
                    },
                )

        self.spi = Spi()

    def _subscribe(self, subscriptions):
        symbols = [s.symbol.encode("ascii") for s in subscriptions]
        if symbols:
            code = self.api.SubscribeMarketData(symbols, len(symbols))
            if code:
                return [f"自选已保存，订阅请求未发送（{code}）；请重新连接重试"]
        return []

    def update_subscriptions(self, subscriptions):
        with self.subscription_lock:
            previous = {s.symbol for s in self.subscriptions}
            current = {s.symbol for s in subscriptions}
            self.subscriptions = list(subscriptions)
            if not self.logged_in:
                return
            warnings = []
            removed = [symbol.encode("ascii") for symbol in sorted(previous - current)]
            if removed:
                code = self.api.UnSubscribeMarketData(removed, len(removed))
                if code:
                    warnings.append(f"自选已保存，退订请求未发送（{code}）；请重新连接重试")
            warnings.extend(self._subscribe(subscriptions))
        for warning in warnings:
            self.emit("subscription_warning", warning)

    def start(self):
        self.api.RegisterSpi(self.spi)
        self.api.RegisterFront(self.configuration.front)
        self.api.Init()

    def close(self):
        self.api.RegisterSpi(None)
        self.api.Release()
        self.password = ""
        self.flow.cleanup()
