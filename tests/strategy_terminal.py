from history_fixture import seed
"""Terminal orchestration uses an Agent-owned autonomous strategy, not a UI loop."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def launch():
    return subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8")


def call(process, method, params=None, error=False):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}})+"\n")
    process.stdin.flush()
    response=json.loads(process.stdout.readline())
    assert ("error" in response)==error, response
    return response if error else response["result"]


def close(process):
    process.kill();process.communicate(timeout=10)


with tempfile.TemporaryDirectory(prefix="asterion-strategy-terminal-", ignore_cleanup_errors=True) as folder:
    root=Path(folder);source=root/"ticks.csv";account=root/"account";account.mkdir()
    prices=[100,101,100,102,99,103]*10
    contents="timestamp_ns,price,quantity\n"+"".join(f"{i+1},{price},1\n" for i,price in enumerate(prices))
    source.write_text(contents)
    process=launch()
    try:
        seed(lambda method, params=None: call(process, method, params), prices, "fixture0")
        before=call(process,"paper.create",{"directory":str(account),"deposit":"10000","margin_per_lot":"100","open_fee":"2","close_today_fee":"3","close_yesterday_fee":"4","margin_rate":"0","open_fee_rate":"0","close_today_fee_rate":"0","close_yesterday_fee_rate":"0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})["paper"]
        call(process,"strategy.run",{"id":"invalid","fast":"4","slow":"2","quantity":"1"},error=True)
        assert call(process,"runtime.snapshot")["paper"]==before
        running=call(process,"strategy.run",{"id":"autonomous","fast":"1","slow":"2","quantity":"1"})
        assert running["strategy"]["state"]=="connected",running
        assert running["strategy"]["processed"]<60,running
        deadline=time.monotonic()+10
        while time.monotonic()<deadline:
            running=call(process,"runtime.snapshot")
            if running["strategy"]["processed"]>0:break
            time.sleep(.1)
        else:raise AssertionError(running["strategy"])
        public=json.dumps(running["strategy"])
        for private_field in ("tls_key","tls_cert","agent_endpoint",'"ticks"'):
            assert private_field not in public,public
    finally:
        close(process)
    source.unlink()
    # Actual autonomous progress while there is no Terminal process or source CSV.
    time.sleep(5)
    process=launch()
    try:
        call(process,"strategy.attach",{"id":"local","service":"strategy-autonomous"})
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            observed=call(process,"runtime.snapshot")["strategy"]
            assert observed["phase"]!="blocked",observed
            if observed["phase"]=="completed":break
            time.sleep(.1)
        else:raise AssertionError(observed)
        assert observed["processed"]==observed["total"]==60
        completed=call(process,"paper.open",{"directory":str(account)})["paper"]
        assert completed["cursor"]==60 and not completed["strategy"]["active"]
        assert completed["fills"] and completed["orders"]
        call(process,"node.action",{"id":"local","service":"strategy-autonomous","action":"restart"})
        time.sleep(.5)
        assert call(process,"runtime.snapshot")["paper"]==completed
        call(process,"strategy.revoke",{"grant_id":"grant.strategy-autonomous"}) # completed authorization is already inactive
        assert call(process,"runtime.snapshot")["paper"]==completed
        call(process,"node.action",{"id":"local","service":"strategy-autonomous","action":"stop"})
        call(process,"paper.close")
        source.write_text(contents);second=root/"second";second.mkdir()
        call(process,"research.local")
        call(process,"research.dataset.select",{"source_task_id":"fixture0-bars","settlement_task_id":"fixture0-settlement","begin_day":"","end_day":"","contract":{"venue":"SHFE","symbol":"rb2610","product":"rb","delivery_month":"2026-10","currency":"CNY","price_increment":"1","quantity_increment":"1","multiplier":"10"}})
        call(process,"paper.create",{"directory":str(second),"deposit":"10000","margin_per_lot":"100","open_fee":"2","close_today_fee":"3","close_yesterday_fee":"4","margin_rate":"0","open_fee_rate":"0","close_today_fee_rate":"0","close_yesterday_fee_rate":"0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        call(process,"strategy.run",{"id":"cancelled","fast":"1","slow":"2","quantity":"1"})
        revoked=call(process,"strategy.revoke",{"grant_id":"grant.strategy-cancelled"})["paper"]
        assert not revoked["strategy"]["active"]
        deadline=time.monotonic()+25
        while time.monotonic()<deadline:
            stopped=call(process,"runtime.snapshot")
            if stopped["strategy"]["phase"]=="blocked":break
            time.sleep(.1)
        else:raise AssertionError(stopped["strategy"])
        assert "revoked" in stopped["strategy"]["error"],stopped["strategy"]
        assert stopped["paper"]["fills"]==revoked["fills"]
        assert all(o["state"] in ("filled","cancelled") for o in stopped["paper"]["orders"])
        call(process,"node.action",{"id":"local","service":"strategy-cancelled","action":"stop"})
        # Revocation belongs to the account even if the strategy process is gone.
        call(process,"paper.close")
        third=root/"third";third.mkdir()
        call(process,"paper.create",{"directory":str(third),"deposit":"10000","margin_per_lot":"100","open_fee":"2","close_today_fee":"3","close_yesterday_fee":"4","margin_rate":"0","open_fee_rate":"0","close_today_fee_rate":"0","close_yesterday_fee_rate":"0", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100"})
        call(process,"strategy.run",{"id":"offline","fast":"1","slow":"2","quantity":"1"})
        call(process,"node.action",{"id":"local","service":"strategy-offline","action":"stop"})
        call(process,"strategy.revoke",{"grant_id":"unrelated"},error=True)
        offline=call(process,"strategy.revoke",{"grant_id":"grant.strategy-offline"})
        assert not offline["paper"]["strategy"]["active"]
        assert offline["strategy"]["state"]=="disconnected"
        before=offline["paper"]["cursor"]
        after=call(process,"paper.act",{"request_id":"manual-after-revoke","action":"advance"})
        assert after["paper"]["cursor"]==before+1

    finally:close(process)
print("Terminal -> local Agent -> automatic strategy -> paper account, UI-independent progress and revocation verified")
