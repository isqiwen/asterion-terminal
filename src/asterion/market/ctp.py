"""Trusted CTP market-data adapter. Never imports or initializes TraderApi."""

import tempfile


class CtpFeed:
    def __init__(self, configuration, password, emit):
        from openctp_ctp import mdapi

        self.emit = emit
        self.configuration = configuration
        self.password = password
        self.flow = tempfile.TemporaryDirectory(prefix="asterion-md-")
        self.api = mdapi.CThostFtdcMdApi.CreateFtdcMdApi(self.flow.name + "/")
        owner = self

        class Spi(mdapi.CThostFtdcMdSpi):
            def OnFrontConnected(self):
                owner.emit("connecting", None)
                request = mdapi.CThostFtdcReqUserLoginField()
                request.BrokerID = "9999"
                request.UserID = configuration.user_id
                request.Password = owner.password
                code = owner.api.ReqUserLogin(request, 1)
                if code:
                    owner.emit("error", f"登录请求未发送（{code}），请重连")

            def OnFrontDisconnected(self, reason):
                owner.emit("reconnecting", None)

            def OnRspUserLogin(self, response, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit("error", f"SimNow 登录失败（{info.ErrorID}），请检查账号或服务时段")
                    return
                if last:
                    owner.emit("connected", None)
                    symbols = [s.symbol.encode("ascii") for s in configuration.subscriptions]
                    if symbols:
                        code = owner.api.SubscribeMarketData(symbols, len(symbols))
                        if code:
                            owner.emit("error", f"订阅请求未发送（{code}），请重连")

            def OnRspSubMarketData(self, instrument, info, request_id, last):
                if info and info.ErrorID:
                    owner.emit(
                        "subscription_error",
                        (
                            instrument.InstrumentID if instrument else "未知代码",
                            f"订阅被拒绝（{info.ErrorID}）",
                        ),
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

    def start(self):
        self.api.RegisterSpi(self.spi)
        self.api.RegisterFront(self.configuration.front)
        self.api.Init()

    def close(self):
        self.api.RegisterSpi(None)
        self.api.Release()
        self.password = ""
        self.flow.cleanup()
