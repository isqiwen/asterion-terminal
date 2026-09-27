"""Research survives a killed Terminal and an Agent-managed service restart."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time


def launch():
    return subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, encoding="utf-8")


def call(process, method, params=None):
    process.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
    process.stdin.flush()
    response = json.loads(process.stdout.readline())
    assert "error" not in response, response
    return response["result"]


def close(process):
    process.kill()
    process.wait(timeout=10)
    process.stdin.close()
    process.stdout.close()


def completed(process, task_id="agent-recovery"):
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        snapshot = call(process, "runtime.snapshot")
        tasks = [task for task in snapshot["research"]["tasks"] if task["id"] == task_id]
        if tasks and tasks[0]["state"] == "succeeded":
            return tasks[0]
        if tasks:
            assert tasks[0]["state"] in {"queued", "running"}, tasks
        time.sleep(.1)
    raise AssertionError(snapshot)


with tempfile.TemporaryDirectory(prefix="asterion-research-agent-", ignore_cleanup_errors=True) as folder:
    csv = Path(folder) / "ticks.csv"
    prices = [100, 101, 102, 101, 104, 103, 102, 103]
    csv.write_text("timestamp_ns,price,quantity\n" + "".join(
        f"{1790298000000000000 + (0 if index < 4 else 259200000000000) + (index % 4) * 1000000000},{price},1\n"
        for index, price in enumerate(prices)), encoding="utf-8")
    process = launch()
    try:
        call(process, "research.local")
        call(process, "futures.inspect_csv", {
            "path": str(csv), "venue": "SHFE", "symbol": "rb2610", "product": "rb",
            "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1",
            "quantity_increment": "1", "multiplier": "10",
        })
        call(process, "research.submit", {"calendar_task":"",
            "id": "agent-recovery", "days":[{"trading_day": "2026-09-25", "schedule_source":"test fixture", "settlement_price":"105", "settlement_source":"test settlement", "sessions":[{"begin_ns":"1790298000000000000","end_ns":"1790298010000000000"}]},{"trading_day":"2026-09-28","schedule_source":"second fixture","settlement_price":"110","settlement_source":"second settlement","sessions":[{"begin_ns":"1790557200000000000","end_ns":"1790557210000000000"}]}], "fast": 1, "slow": 3,
            "quantity": "1", "deposit": "10000", "margin_per_lot": "100",
            "open_fee": "2", "close_today_fee": "3", "close_yesterday_fee": "4", "max_order_quantity":"100", "max_gross_quantity":"100", "max_working_orders":"100",
        })
        csv.write_text("timestamp_ns,price,quantity\n" + "".join(
            f"{1790298000000000000 + index * 1000000000},{100 + index + index % 3},1\n"
            for index in range(160)), encoding="utf-8")
        call(process, "futures.inspect_csv", {
            "path": str(csv), "venue": "SHFE", "symbol": "rb2610", "product": "rb",
            "delivery_month": "2026-10", "currency": "CNY", "price_increment": "1",
            "quantity_increment": "1", "multiplier": "10",
        })
        call(process, "research.factor.submit", {"id": "factor-recovery", "lookbacks": [2], "horizon": 1, "evaluation": {"mode": "full_sample"}})
        call(process, "research.factor.submit", {"id": "rolling-recovery", "lookbacks": [2,5,10], "horizon": 1, "evaluation": {"mode":"walk_forward","training_events":80,"validation_events":40}})
        call(process, "research.data.submit", {"id": "data-recovery"})
        calendar=Path(folder)/"calendar.csv"
        calendar.write_text("trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source\n2026-09-25,2026-09-25T09:00:00+08:00,2026-09-25T15:00:00+08:00,105,fixture,fixture\n",encoding="utf-8")
        call(process,"research.calendar.submit",{"id":"calendar-recovery","path":str(calendar)})
        calendar.unlink()
    finally:
        close(process)
    # Neither the Terminal process nor the source CSV is needed by the task.
    csv.unlink()
    process = launch()
    try:
        call(process, "research.local")
        task = completed(process)
        assert task["attempt"] == 1 and task["completed"] == 8, task
        result = call(process, "research.result", {"id": task["id"]})["research_result"]
        assert result["result"]["account"]["equity"] == "10014", result
        assert result["result"]["max_drawdown"] == "30", result
        assert result["result"]["settlements"][0]["equity"] == "10038"
        assert result["result"]["settlements"][1]["equity"] == "10014"
        assert len(result["experiment"]["days"]) == 2
        assert result["task"] == task
        assert result["experiment"]["sma"] == {"fast":1,"slow":3,"quantity":"1"}
        assert result["experiment"]["paper"]["deposit"] == "10000"
        assert result["experiment"]["data"]["count"] == 8
        assert "ticks" not in result["experiment"]["paper"]
        calendar_task=completed(process,"calendar-recovery")
        assert calendar_task["kind"]=="calendar_import" and calendar_task["attempt"]==1
        calendar_result=call(process,"research.result",{"id":"calendar-recovery"})["research_result"]
        assert calendar_result["result"]["calendar"]["days"][0]["settlement_price"]=="105"
        data_task = completed(process, "data-recovery")
        assert data_task["kind"] == "data_import" and data_task["attempt"] == 1, data_task
        publication = call(process, "research.result", {"id": "data-recovery"})["research_result"]
        assert publication["kind"] == "data_import" and len(publication["result"]["dataset"]["ticks"]) == 160, publication
        factor_task = completed(process, "factor-recovery")
        assert factor_task["kind"] == "factor" and factor_task["attempt"] == 1, factor_task
        factor_result = call(process, "research.result", {"id": "factor-recovery"})["research_result"]
        assert factor_result["kind"] == "factor" and len(factor_result["result"]["samples"]) == 157, factor_result
        assert factor_result["task"] == factor_task
        assert factor_result["experiment"]["lookbacks"] == [2]
        assert factor_result["experiment"]["evaluation"] == {"mode":"full_sample"}
        assert factor_result["experiment"]["data"]["count"] == 160
        assert factor_result["experiment"]["dataset_revision"] != result["experiment"]["dataset_revision"]
        rolling_task = completed(process, "rolling-recovery")
        rolling_result = call(process, "research.result", {"id":"rolling-recovery"})["research_result"]
        assert rolling_result["result"]["version"] == 4
        assert len(rolling_result["result"]["folds"]) == 2 and len(rolling_result["result"]["samples"]) == 78
        assert rolling_result["experiment"]["evaluation"] == {"mode":"walk_forward","training_events":80,"validation_events":40}
        call(process,"research.data.use",{"id":"data-recovery"})
        call(process,"research.submit",{"id":"calendar-backtest","calendar_task":"calendar-recovery","days":None,"fast":1,"slow":3,"quantity":"1","deposit":"10000","margin_per_lot":"100","open_fee":"2","close_today_fee":"3","close_yesterday_fee":"4","max_order_quantity":"100","max_gross_quantity":"100","max_working_orders":"100"})
        calendar_backtest_task=completed(process,"calendar-backtest")
        calendar_backtest=call(process,"research.result",{"id":"calendar-backtest"})["research_result"]
        assert calendar_backtest["experiment"]["calendar_publication"]==calendar_result["result"]
        assert calendar_backtest["experiment"]["version"]==5
        stopped=call(process,"node.action",{"id":"local","service":"research","action":"stop"})
        service=next(s for n in stopped["nodes"] for s in n["health"]["services"] if s["id"]=="research")
        call(process,"node.update",{"id":"local","service":"research","revision":service["revision"]})
        call(process,"node.action",{"id":"local","service":"research","action":"start"})
    finally:
        close(process)
    process = launch()
    try:
        call(process, "research.local")
        assert completed(process,"calendar-backtest")==calendar_backtest_task
        assert call(process,"research.result",{"id":"calendar-backtest"})["research_result"]==calendar_backtest
        assert completed(process,"calendar-recovery")==calendar_task
        assert call(process,"research.result",{"id":"calendar-recovery"})["research_result"]==calendar_result
        assert completed(process, "data-recovery") == data_task
        assert call(process, "research.result", {"id": "data-recovery"})["research_result"] == publication
        selected = call(process, "research.data.use", {"id": "data-recovery"})["dataset"]
        assert selected["persistent"] and selected["count"] == 160 and selected["revision"] == factor_result["result"]["dataset_revision"], selected
        call(process, "research.factor.submit", {"id": "published-factor", "lookbacks": [2], "horizon": 1, "evaluation": {"mode": "full_sample"}})
        completed(process, "published-factor")
        assert call(process, "research.result", {"id": "published-factor"})["research_result"]["result"] == factor_result["result"]
        assert completed(process, "factor-recovery") == factor_task
        assert call(process, "research.result", {"id": "factor-recovery"})["research_result"] == factor_result
        assert completed(process, "rolling-recovery") == rolling_task
        assert call(process, "research.result", {"id":"rolling-recovery"})["research_result"] == rolling_result
        recovered = completed(process)
        assert recovered == task, (recovered, task)
        assert call(process, "research.result", {"id": task["id"]})["research_result"] == result
        call(process, "node.action", {"id": "local", "service": "research", "action": "stop"})
        # Local reconnect must actually start the service, even with an existing client.
        connected = call(process, "research.local")["research"]
        assert connected["online"] and not connected["remote"] and connected["host"] == "localhost"
        assert call(process, "research.result", {"id": task["id"]})["research_result"] == result

    finally:
        close(process)
print("Agent dispatch, killed Terminal, deleted source, persisted results and service restart verified")
