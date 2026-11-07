"""Isolated, independently supervised services with explicit test-only credentials."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import time
import warehouse_tls

def install_program(binary, installed):
    binary, installed = Path(binary), Path(installed)
    # Debug binaries include DuckDB and can exceed 600 MiB. Parallel isolated
    # hosts share immutable program bytes without consuming the /tmp quota;
    # each host still owns its configuration, credentials, logs and data.
    if binary.stat().st_dev == installed.parent.stat().st_dev:
        os.link(binary, installed)
    else:
        shutil.copy2(binary, installed)

@contextmanager
def warehouse_host(build, root, *, exchanges=()):
    root = Path(root)
    for name in ('bin','config','logs','data','credentials'):
        (root / name).mkdir(parents=True)
    install_program(build / 'asterion_warehouse', root / 'bin/asterion_warehouse')
    if sys.platform == "linux":
        install_program(build / "asterion_crash_reporter", root / "bin/asterion_crash_reporter")
    shutil.copy2(Path(__file__).parents[2] / 'apps/services/warehouse/config/logging.json', root/'config/logging.json')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    tls = warehouse_tls.prepare(root)
    (root/'config/warehouse.json').write_text(json.dumps({'bind':'127.0.0.1','port':port, 'data':str(root/'data'),
        'clients':warehouse_tls.clients(tls),
        'acquisition':{'exchanges':list(exchanges),'tushare':None}}))
    with running(root, port, 'asterion_warehouse') as process:
        yield port
        assert process.poll() is None, "a client stopped the independent Warehouse"


@contextmanager
def market_host(build, root, *, vendor=False, user="steady", also_trust=None):
    root = Path(root)
    for name in ('bin','lib','config','logs','data','credentials'):
        (root / name).mkdir(parents=True)
    install_program(build / 'asterion_market', root / 'bin/asterion_market')
    if sys.platform == "linux":
        install_program(build / "asterion_crash_reporter", root / "bin/asterion_crash_reporter")
    suffix = '.dylib' if sys.platform == 'darwin' else '.so'
    for installed, fixture, real in [('ctp-md', 'libasterion_test_ctp', 'ctp-md'),
                                     ('ctp-trader', 'libasterion_test_ctp_trader', 'ctp-trader')]:
        shutil.copy2(build / ((real if vendor else fixture) + suffix), root / 'lib' / (installed+'.so'))
    shutil.copy2(Path(__file__).parents[2] / 'apps/services/market/config/logging.json', root/'config/logging.json')
    with socket.socket() as probe:
        probe.bind(('127.0.0.1',0));port=probe.getsockname()[1]
    tls = warehouse_tls.prepare(root)
    (root/'config/market.json').write_text(json.dumps({'bind':'127.0.0.1','port':port, 'data':str(root/'data'),
        'clients':[dict(fingerprint=c['fingerprint']) for c in warehouse_tls.clients(tls)]
                  + ([dict(fingerprint=c['fingerprint']) for c in warehouse_tls.clients(also_trust)] if also_trust else []),
        'counter':{'market_front':'tcp://127.0.0.1:1','trade_front':'tcp://127.0.0.1:1',
                   'broker':'test','user':user,'app_id':'app'}}))
    (root/'credentials/asterion.ctp.password').write_text('fixture-only-secret')
    (root/'credentials/asterion.ctp.auth_code').write_text('fixture-auth')
    with running(root, port, 'asterion_market') as process:
        yield port
        assert process.poll() is None, "a client stopped the independent Market"


@contextmanager
def running(root, port, program='asterion_warehouse'):
    """Restart an existing isolated installation; the caller may test abrupt exits."""
    log = (root/'stderr').open('a')
    process = subprocess.Popen([root/'bin'/program],
        env=dict(os.environ,CREDENTIALS_DIRECTORY=str(root/'credentials')),stdout=subprocess.DEVNULL,stderr=log)
    try:
        for _ in range(200):
            if process.poll() is not None:
                raise AssertionError((root/'stderr').read_text())
            try:
                with socket.create_connection(('127.0.0.1',port),timeout=.1):break
            except OSError:time.sleep(.05)
        else:raise AssertionError(program + ' did not listen')
        yield process
    finally:
        process.terminate()
        try:process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill();process.wait();raise
        log.close()


if __name__ == "__main__":
    # A desktop test owns these hosts, independently of the Electron application.
    root = Path(sys.argv[2])
    with warehouse_host(Path(sys.argv[1]), root / 'warehouse') as warehouse, market_host(Path(sys.argv[1]), root / 'market', also_trust=root / 'warehouse/config/tls') as market:
        print(warehouse, market, flush=True)
        sys.stdin.read()
