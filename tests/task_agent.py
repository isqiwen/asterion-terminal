from history_fixture import contracts, seed
"""Task survives a killed Terminal and an Agent-managed service restart."""
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
        tasks = [task for task in snapshot["task_service"]["tasks"] if task["id"] == task_id]
        if tasks and tasks[0]["state"] == "succeeded":
            return tasks[0]
        if tasks:
            assert tasks[0]["state"] in {"queued", "running"}, tasks
        time.sleep(.1)
    raise AssertionError(snapshot)


selections = {}
def select(process, identity):
    return call(process,"data.dataset.select",selections[identity])


with tempfile.TemporaryDirectory(prefix="asterion-backtest-factor-agent-", ignore_cleanup_errors=True):
    process=launch()
    try:
        invoke=lambda method,params=None:call(process,method,params)
        selections["short"] = {k:v for k,v in seed(invoke,[100,101,102,101,104,103,102,103],"short")["datasets"][0].items() if k in {"source_dataset_ids","settlement_dataset_ids","begin_day","end_day","price_increment","multiplier"}}
        selections["short"].update(price_increment="1",multiplier="10")
        selections["long"] = {k:v for k,v in seed(invoke,[100+i+i%3 for i in range(160)],"long")["datasets"][0].items() if k in {"source_dataset_ids","settlement_dataset_ids","begin_day","end_day"}}
        selections["long"].update(price_increment="1",multiplier="10")
        select(process,"short")
        call(process,"backtest.submit",{"id":"agent-recovery","fast":1,"slow":3,"quantity":"1","deposit":"10000","contracts": contracts(),"max_order_quantity":"100","max_gross_quantity":"100","max_working_orders":"100"})
        select(process,"long")
        call(process,"factor.submit",{"id":"factor-recovery","lookbacks":[2],"horizon":1,"evaluation":{"mode":"full_sample"}})
        call(process,"factor.submit",{"id":"rolling-recovery","lookbacks":[2,5,10],"horizon":1,"evaluation":{"mode":"walk_forward","training_events":80,"validation_events":40}})
    finally:close(process)
    # Service-owned immutable history and admitted tasks survive the submitting process.
    process=launch()
    try:
        call(process,"node.data_tasks.local.open")
        saved={}
        for identity,count in [("agent-recovery",8),("factor-recovery",160),("rolling-recovery",160)]:
            task=completed(process,identity)
            assert task["attempt"]==1,task
            result=call(process,"task.result",{"id":identity})["task_result"]
            assert result["task"]==task,result
            data=result["experiment"]["data"]  # backtests list one range per contract
            assert (data[0] if isinstance(data,list) else data)["count"]==count,result
            saved[identity]=(task,result)
        factor=saved["factor-recovery"][1]
        assert len(factor["result"]["samples"])==157,factor
        rolling=saved["rolling-recovery"][1]
        assert rolling["result"]["version"]==4,rolling
        assert len(rolling["result"]["folds"])==2 and len(rolling["result"]["samples"])==78,rolling
        call(process,"node.action",{"id":"local","service":"task","action":"stop"})
    finally:close(process)
    process=launch()
    try:
        call(process,"node.data_tasks.local.open")
        for identity,(task,result) in saved.items():
            assert completed(process,identity)==task
            assert call(process,"task.result",{"id":identity})["task_result"]==result
        selected,=select(process,"long")["datasets"]
        assert selected["count"]==160 and selected["revision"]==factor["experiment"]["dataset_revision"]
        call(process,"factor.submit",{"id":"repeated-factor","lookbacks":[2],"horizon":1,"evaluation":{"mode":"full_sample"}})
        completed(process,"repeated-factor")
        assert call(process,"task.result",{"id":"repeated-factor"})["task_result"]["result"]==factor["result"]
        call(process,"node.action",{"id":"local","service":"task","action":"stop"})
        call(process,"node.data_tasks.local.open")
        assert completed(process,"agent-recovery") == saved["agent-recovery"][0]
        assert call(process,"task.result",{"id":"agent-recovery"})["task_result"]==saved["agent-recovery"][1]
    finally:close(process)
print("Agent dispatch, killed Terminal, immutable historical sources, persisted task results and service restart verified")
