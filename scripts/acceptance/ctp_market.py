"""Acceptance: live CTP market data against a provider-configured CTP front.

Runs the real Terminal core (dev bridge) with the packaged vendor SDK inside an
isolated node directory, connects, subscribes and waits for real quotes. The
password is read with getpass, passed only to the local core over its private
pipe, and never written to disk, logs or the report.

    python3 scripts/acceptance/ctp_market.py [--build build/Debug] \
        --front tcp://<host>:<port> --broker <broker-id> --user <user-id> \
        --instrument SHFE:rb2610 [--instrument DCE:m2609 ...]

Front addresses and broker ids come from your CTP service provider;
nothing is assumed here. Exits 0 when every instrument receives a quote.
"""
import argparse
import getpass
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
import time


def stop_test_agent(directory):
    """Stop only the Agent created in this run's private temporary directory."""
    pidfile = Path(directory) / "agent.pid"
    if not pidfile.exists():
        return
    pid = int(pidfile.read_text())
    if pid <= 1 or pid == os.getpid():
        raise RuntimeError("invalid isolated Agent process identity")
    if os.name == "nt":
        import ctypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        api.OpenProcess.restype = ctypes.c_void_p
        api.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        api.WaitForSingleObject.restype = ctypes.c_uint32
        api.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = api.OpenProcess(0x00100000, False, pid)
        if not handle:
            if ctypes.get_last_error() == 87:
                return
            raise OSError(ctypes.get_last_error(), "Cannot observe isolated Agent exit")
        try:
            try:
                os.kill(pid, signal.SIGTERM)
            except OSError:
                if api.WaitForSingleObject(handle, 0) != 0:
                    raise
            if api.WaitForSingleObject(handle, 15000) != 0:
                raise RuntimeError("isolated Agent did not exit")
        finally:
            api.CloseHandle(handle)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                os.kill(pid, 0)
                time.sleep(0.05)
        except ProcessLookupError:
            return
        raise RuntimeError("isolated Agent did not exit")

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--build", default=str(ROOT / "build/Debug"))
parser.add_argument("--front", required=True)
parser.add_argument("--broker", required=True)
parser.add_argument("--user", required=True)
parser.add_argument("--instrument", action="append", required=True, help="VENUE:SYMBOL")
parser.add_argument("--timeout", type=int, default=90, help="seconds to wait for quotes")
parser.add_argument("--report", default="", help="write a JSON report (no credentials)")
parser.add_argument("--sdk", default="", help="market SDK library (default: the packaged vendor SDK)")
parser.add_argument("--password-stdin", action="store_true",
                    help="read the password from standard input (automation only)")
args = parser.parse_args()

build = Path(args.build).resolve()
suffix = ".exe" if os.name == "nt" else ""
bridge = build / ("asterion_terminal_dev_bridge" + suffix)
sdk = Path(args.sdk).resolve() if args.sdk else build / (
    "ctp-md.dll" if os.name == "nt" else "ctp-md.dylib" if sys.platform == "darwin" else "ctp-md.so")
report = {"front": args.front, "broker": args.broker, "instruments": args.instrument,
          "sdk": sdk.name, "started_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
          "passed": False, "steps": []}
if args.report:
    # A startup or cleanup failure must never leave a previous passing report.
    Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
for required in (bridge, sdk):
    if not required.exists():
        sys.exit(f"missing {required}; build the project first")
instruments = []
for item in args.instrument:
    venue, _, symbol = item.partition(":")
    if not venue or not symbol:
        sys.exit(f"instrument must be VENUE:SYMBOL, got {item!r}")
    instruments.append({"venue": venue, "symbol": symbol})
password = (sys.stdin.readline().rstrip("\n") if args.password_stdin
            else getpass.getpass("Market password (not echoed, not stored): "))

with tempfile.TemporaryDirectory(prefix="asterion-ctp-", ignore_cleanup_errors=True) as folder:
    env = dict(os.environ, ASTERION_NODE_DIRECTORY=folder, ASTERION_TEST_NODE_ISOLATED="1",
               ASTERION_CTP_LIBRARY=str(sdk))
    core = subprocess.Popen([str(bridge)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True, env=env)

    def call(method, params=None):
        core.stdin.write(json.dumps({"version": 1, "method": method, "params": params or {}}) + "\n")
        core.stdin.flush()
        response = json.loads(core.stdout.readline())
        if "error" in response:
            raise RuntimeError(f"{method}: {response['error']['code']}: {response['error']['message']}")
        return response["result"]

    def step(name, ok, detail=""):
        report["steps"].append({"step": name, "ok": ok, "detail": detail,
                                "at": round(time.monotonic() - begin, 1)})
        print(f"[{'ok' if ok else 'FAIL'}] {name} {detail}")
        return ok

    begin = time.monotonic()
    passed = False
    try:
        state = call("market.local")
        step("local market service", bool(state.get("market")))
        state = call("market.connect", {"front": args.front, "broker": args.broker,
                                        "user": args.user, "password": password,
                                        "instruments": instruments})
        password = ""
        deadline = time.monotonic() + args.timeout
        seen = {}
        while time.monotonic() < deadline:
            market = call("runtime.snapshot")["market"] or {}
            phase = market.get("phase")
            if phase == "error":
                step("session", False, f"error_code {market.get('error_code')}")
                break
            for sub in market.get("subscriptions", []):
                key = f"{sub['venue']}:{sub['symbol']}"
                if sub.get("quote") and key not in seen:
                    seen[key] = sub["quote"]
                    step(f"first quote {key}", True, f"last={sub['quote'].get('last')} "
                         f"volume={sub['quote'].get('volume')}")
            if len(seen) == len(instruments):
                passed = step("all instruments quoted", True, f"phase={phase}")
                break
            time.sleep(1)
        else:
            step("quotes before timeout", False, f"received {sorted(seen)} of {args.instrument}")
        final = call("market.disconnect")["market"] or {}
        step("disconnect", final.get("phase") == "disconnected", final.get("phase", ""))
    except Exception as error:  # report, never the password
        step("run", False, str(error))
    finally:
        password = ""
        try:
            try:
                core.stdin.close()
                core.terminate()
                core.wait(timeout=15)
            finally:
                stop_test_agent(folder)
        except Exception as error:
            step("cleanup", False, str(error))
    passed = passed and all(item["ok"] for item in report["steps"])
    report["passed"] = passed
    if args.report:
        Path(args.report).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print("PASSED" if passed else "FAILED")
    sys.exit(0 if passed else 1)
