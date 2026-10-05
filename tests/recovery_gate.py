"""Run the complete named recovery gate; missing or skipped checks fail the gate."""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

TESTS = (
    "Live.TradingDayRolloverRebuildsReportsAndRatesWithoutResending",
    "Live.SameDayReconnectDoesNotMistakeCachedOrdersForBrokerConfirmation",
    "Live.AutomaticReconnectRequiresNewAuthorizationEvenOnTheSameTradingDay",
    "CtpTrader.LateQueryRepliesCannotReplaceReconnectedAccountState",
    "TaskStore.DatedCostsArePinnedSwitchAtSettlementAndRejectForgedResults",
    "live_process",
    "task_agent_recovery",
    "RpcHost.PendingRepliesLeaveReadAndControlConnectionsAvailable",
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path("build/Release"))
    parser.add_argument("--check-selection", action="store_true")
    options = parser.parse_args()
    build = options.build.resolve()
    inventory = json.loads(subprocess.check_output(
        ["ctest", "--test-dir", str(build), "--show-only=json-v1"], text=True))
    names = [test["name"] for test in inventory["tests"]]
    missing = [name for name in TESTS if names.count(name) != 1]
    if missing:
        raise SystemExit("Recovery gate requires exactly one registration for: " + ", ".join(missing))
    print("Verified recovery checks: " + str(len(TESTS)), flush=True)
    for name in TESTS:
        print("  " + name, flush=True)
    if options.check_selection:
        return
    report = build / "recovery-repeat.xml"
    log = build / "recovery-repeat.log"
    pattern = "^(" + "|".join(re.escape(name) for name in TESTS) + ")$"
    command = ["ctest", "--test-dir", str(build), "-j", "3", "--repeat", "until-fail:3",
               "--no-tests=error", "-R", pattern, "--output-on-failure", "--output-junit", str(report)]
    with log.open("w") as output:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in process.stdout:
            output.write(line)
            print(line, end="", flush=True)
        code = process.wait()
    if code:
        raise SystemExit(code)
    cases = ET.parse(report).getroot().findall(".//testcase")
    passed = {case.attrib["name"] for case in cases
              if case.find("skipped") is None and case.find("failure") is None
              and case.find("error") is None and case.attrib.get("status") != "notrun"}
    if passed != set(TESTS):
        raise SystemExit("Recovery gate did not execute every required check successfully")
    print(f"Recovery gate passed; evidence: {log} and {report}")


if __name__ == "__main__":
    main()
