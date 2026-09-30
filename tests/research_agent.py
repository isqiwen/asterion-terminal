from history_fixture import seed
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


def select(process, identity):
    return call(process,"research.dataset.select",{
        "source_task_id":identity+"-bars","settlement_task_id":identity+"-settlement",
        "begin_day":"","end_day":"","contract":{"venue":"SHFE","symbol":"rb2610","product":"rb","delivery_month":"2026-10","currency":"CNY","price_increment":"1","quantity_increment":"1","multiplier":"10"}})

with tempfile.TemporaryDirectory(prefix="asterion-research-agent-", ignore_cleanup_errors=True):
    process=launch()
    try:
        invoke=lambda method,params=None:call(process,method,params)
        seed(invoke,[100,101,102,101,104,103,102,103],"short")
        seed(invoke,[100+i+i%3 for i in range(160)],"long")
        select(process,"short")
        call(process,"research.submit",{"id":"agent-recovery","fast":1,"slow":3,"quantity":"1","deposit":"10000","margin_per_lot":"100","open_fee":"2","close_today_fee":"3","close_yesterday_fee":"4","margin_rate":"0","open_fee_rate":"0","close_today_fee_rate":"0","close_yesterday_fee_rate":"0","max_order_quantity":"100","max_gross_quantity":"100","max_working_orders":"100"})
        select(process,"long")
        call(process,"research.factor.submit",{"id":"factor-recovery","lookbacks":[2],"horizon":1,"evaluation":{"mode":"full_sample"}})
        call(process,"research.factor.submit",{"id":"rolling-recovery","lookbacks":[2,5,10],"horizon":1,"evaluation":{"mode":"walk_forward","training_events":80,"validation_events":40}})
    finally:close(process)
    # Service-owned immutable history and admitted tasks survive the submitting process.
    process=launch()
    try:
        call(process,"research.local")
        saved={}
        for identity,count in [("agent-recovery",8),("factor-recovery",160),("rolling-recovery",160)]:
            task=completed(process,identity)
            assert task["attempt"]==1,task
            result=call(process,"research.result",{"id":identity})["research_result"]
            assert result["task"]==task,result
            assert result["experiment"]["data"]["count"]==count,result
            saved[identity]=(task,result)
        factor=saved["factor-recovery"][1]
        assert len(factor["result"]["samples"])==157,factor
        rolling=saved["rolling-recovery"][1]
        assert rolling["result"]["version"]==4,rolling
        assert len(rolling["result"]["folds"])==2 and len(rolling["result"]["samples"])==78,rolling
        call(process,"node.action",{"id":"local","service":"research","action":"stop"})
    finally:close(process)
    process=launch()
    try:
        call(process,"research.local")
        for identity,(task,result) in saved.items():
            assert completed(process,identity)==task
            assert call(process,"research.result",{"id":identity})["research_result"]==result
        selected=select(process,"long")["dataset"]
        assert selected["count"]==160 and selected["revision"]==factor["experiment"]["dataset_revision"]
        call(process,"research.factor.submit",{"id":"repeated-factor","lookbacks":[2],"horizon":1,"evaluation":{"mode":"full_sample"}})
        completed(process,"repeated-factor")
        assert call(process,"research.result",{"id":"repeated-factor"})["research_result"]["result"]==factor["result"]
        call(process,"node.action",{"id":"local","service":"research","action":"stop"})
        assert call(process,"research.local")["research"]["online"]
        assert call(process,"research.result",{"id":"agent-recovery"})["research_result"]==saved["agent-recovery"][1]
    finally:close(process)
print("Agent dispatch, killed Terminal, immutable historical sources, persisted research results and service restart verified")
