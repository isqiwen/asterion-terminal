"""Test-owned independent trading host for browser journeys; never uses systemd."""
import json
import shutil
from pathlib import Path
import sys
from trading_installation import install, Programs

root, build, fingerprint, mode = sys.argv[1:]
build = Path(build)
installation = install(root, Programs(str(build / "asterion_trading"),
    str(build / "asterion_test_certificates"), str(build / "libasterion_test_ctp_trader.so")))
if mode == 'strategy':
    source = Path(__file__).resolve().parents[2]
    (installation.root / 'lib/ctp-md.so').symlink_to(build / 'libasterion_test_ctp.so')
    code = installation.root / 'lib/strategy'
    (code / 'asterion').mkdir(parents=True)
    shutil.copy2(source / 'apps/services/trading/python/strategy_runner.py', code)
    shutil.copy2(source / 'apps/services/common/python/asterion_strategy.py', code)
    for name in ('strategy_pb2.py', 'common_pb2.py'):
        shutil.copy2(build / 'generated-python/asterion' / name, code / 'asterion')
    (installation.root / 'runtime').mkdir()
    (installation.root / 'runtime/python').symlink_to(source / 'build/electron-resources/python')
elif mode != 'account':
    raise ValueError('unknown test host mode')
port = installation.account("fixture", terminals=[fingerprint], user='strategy' if mode == 'strategy' else '000001')
process = installation.start("fixture")
try:
    print(json.dumps({"host": "127.0.0.1", "port": port, "instance": "fixture",
                      "fingerprint": installation.server}), flush=True)
    for line in sys.stdin:
        if line.strip() != "restart":
            raise ValueError("unsupported test command")
        process.terminate()
        process.wait(timeout=15)
        process = installation.start("fixture")
        print("{}", flush=True)
finally:
    if process.poll() is None:
        process.terminate()
        process.wait(timeout=15)
